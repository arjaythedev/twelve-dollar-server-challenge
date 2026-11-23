/* btree.h: GET /feed and GET /posts/:id on SQLite's B-tree cursors.
 *
 * Combines:
 * - Speculative reverse-rowid posts tail walk + O(1) cell-19 rank_twenty certificate
 *   with 2-leaf page pinning + dedicated CUR_POST_PREV 2nd-leaf cursor
 * - Dedicated CUR_FEED_TAIL and CUR_LIKES_LAST cursors parked at AtLast (no-op rechecks)
 * - O(1) CUR_LIKES_LAST in-leaf binary search for GET /posts/:id on recent posts
 * - 32-slot hash-indexed CUR_USER_BASE[uid & 31] cursor bank shared by /feed and /posts/:id
 * - Page-at-a-time likes_post_id_idx scan with +8 lookahead & binary run-length subtraction
 *
 * Unmodified SQLite 3.54.1 amalgamation. Any unexpected layout, missing author, or
 * overflow record falls back to the reference SQL statement. */
#ifndef BTREE_H
#define BTREE_H

#define FEED_N 20
#define LIKES_SPAN 512
#define TAIL_GAP 64
#define USER_SLOTS 32

static inline int cell_pid_rowid(MemPage *pg, int i, i64 *pid, i64 *rowid);

enum {
  CUR_POSTS = 0,
  CUR_FEED_IDX,
  CUR_LIKES_IDX,
  CUR_FEED_TAIL,
  CUR_POST_PREV,
  CUR_LIKES_LAST,
  CUR_USER_BASE, /* 32 hash-indexed user cursors: CUR_USER_BASE + (uid & 31) */
  NCUR = CUR_USER_BASE + USER_SLOTS
};

struct bt {
  Btree *bt;
  Pgno root[4]; /* 0: posts, 1: users, 2: feed_idx, 3: likes_idx */
  KeyInfo *ki_feed, *ki_likes;
  BtCursor *cur[NCUR];
  uint64_t open_mask; /* bit i = cur[i] is open in the current transaction */
  int open;           /* transaction open */
  int ok;             /* schema validated */
  uint8_t *spill;
  uint32_t spill_cap;
};

static Pgno bt_rootpage(sqlite3 *c, const char *name) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(c, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  Pgno r = sqlite3_step(s) == SQLITE_ROW ? (Pgno)sqlite3_column_int64(s, 0) : 0;
  sqlite3_finalize(s);
  return r;
}

static int bt_layout(sqlite3 *c, const char *sql, const char *expect) {
  sqlite3_stmt *s;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(c, sql, -1, &s, NULL) != SQLITE_OK) return 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got - 1) {
    const char *t = (const char *)sqlite3_column_text(s, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", t ? t : "");
  }
  sqlite3_finalize(s);
  if (strcmp(got, expect)) { fprintf(stderr, "schema: %s gave %s, expected %s\n", sql, got, expect); return 0; }
  return 1;
}

static KeyInfo *bt_keyinfo(sqlite3 *c, int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(c, nkey, nextra);
  if (!k) return NULL;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = c->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

static int bt_init(struct bt *b, sqlite3 *c) {
  memset(b, 0, sizeof *b);
  b->bt = c->aDb[0].pBt;
  int ok = bt_layout(c, "SELECT name || ':' || type || ':' || pk FROM pragma_table_info('users')", "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0") &&
           bt_layout(c, "SELECT name || ':' || type || ':' || pk FROM pragma_table_info('posts')",
                     "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0") &&
           bt_layout(c, "SELECT name || ':' || desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "created_at:1,id:1") &&
           bt_layout(c, "SELECT name || ':' || desc FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "post_id:0");
  b->root[0] = bt_rootpage(c, "posts");
  b->root[1] = bt_rootpage(c, "users");
  b->root[2] = bt_rootpage(c, "posts_created_at_id_idx");
  b->root[3] = bt_rootpage(c, "likes_post_id_idx");
  if (!ok || ENC(c) != SQLITE_UTF8) return 0;
  for (int i = 0; i < 4; i++)
    if (!b->root[i]) return 0;
  b->ki_feed = bt_keyinfo(c, 2, 1, 1);
  b->ki_likes = bt_keyinfo(c, 1, 1, 0);
  if (!b->ki_feed || !b->ki_likes) return 0;
  size_t csz = (size_t)sqlite3BtreeCursorSize();
  for (int i = 0; i < NCUR; i++) {
    b->cur[i] = malloc(csz);
    if (!b->cur[i]) return 0;
  }
  b->ok = 1;
  return 1;
}

static inline Pgno cur_root(const struct bt *b, int idx) {
  if (idx == CUR_POSTS || idx == CUR_FEED_TAIL || idx == CUR_POST_PREV) return b->root[0];
  if (idx >= CUR_USER_BASE) return b->root[1];
  if (idx == CUR_FEED_IDX) return b->root[2];
  return b->root[3];
}
static inline KeyInfo *cur_ki(const struct bt *b, int idx) {
  if (idx == CUR_FEED_IDX) return b->ki_feed;
  if (idx == CUR_LIKES_IDX || idx == CUR_LIKES_LAST) return b->ki_likes;
  return NULL;
}

static inline BtCursor *bt_cur(struct bt *b, int idx) {
  uint64_t bit = 1ULL << idx;
  if (likely(b->open_mask & bit)) return b->cur[idx];
  sqlite3BtreeCursorZero(b->cur[idx]);
  if (unlikely(sqlite3BtreeCursor(b->bt, cur_root(b, idx), 0, cur_ki(b, idx), b->cur[idx]) != SQLITE_OK)) return NULL;
  b->open_mask |= bit;
  return b->cur[idx];
}

static int bt_open(struct bt *b) {
  if (!b->ok || b->open) return 0;
  if (sqlite3BtreeBeginTrans(b->bt, 0, 0) != SQLITE_OK) return -1;
  b->open = 1;
  return 0;
}

static inline void bt_close_mask(struct bt *b, uint64_t mask) {
  uint64_t m = b->open_mask & mask;
  while (m) {
    int i = __builtin_ctzll(m);
    sqlite3BtreeCloseCursor(b->cur[i]);
    m &= m - 1;
  }
  b->open_mask &= ~mask;
}

static void bt_close(struct bt *b) {
  if (!b->open) return;
  uint64_t m = b->open_mask;
  while (m) {
    int i = __builtin_ctzll(m);
    sqlite3BtreeCloseCursor(b->cur[i]);
    m &= m - 1;
  }
  b->open_mask = 0;
  b->open = 0;
}

/* ---- records ---- */

struct col {
  u64 t;
  const u8 *p;
};

static inline const u8 *rd_varint(const u8 *p, const u8 *end, u64 *v) {
  u64 x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= end) return NULL;
    x = x << 7 | (p[0] & 0x7f);
    if (!(*p++ & 0x80)) { *v = x; return p; }
  }
  if (p >= end) return NULL;
  *v = x << 8 | *p++;
  return p;
}
static inline u32 serial_len(u64 t) {
  static const u8 n[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? n[t] : (u32)((t - 12) / 2);
}
static inline int rec_cols(const u8 *r, u32 n, int ncol, struct col *c) {
  const u8 *end = r + n;
  u64 hsz;
  const u8 *h = rd_varint(r, end, &hsz);
  if (!h || hsz > n) return -1;
  const u8 *hend = r + hsz, *body = hend;
  for (int i = 0; i < ncol; i++) {
    if (h >= hend) return -1;
    h = rd_varint(h, hend, &c[i].t);
    if (!h) return -1;
    c[i].p = body;
    body += serial_len(c[i].t);
  }
  return body <= end ? 0 : -1;
}
static inline int col_is_int(const struct col *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static inline int col_is_text(const struct col *c) { return c->t >= 13 && (c->t & 1); }
static inline u32 col_len(const struct col *c) { return serial_len(c->t); }
static inline i64 col_int(const struct col *c) {
  const u8 *p = c->p;
  switch (c->t) {
  case 1: return (int8_t)p[0];
  case 2: return (int16_t)((u16)p[0] << 8 | p[1]);
  case 3: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8;
  case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
  case 5: return (i64)((u64)(int16_t)((u16)p[0] << 8 | p[1]) << 32 | (u64)((u32)p[2] << 24 | (u32)p[3] << 16 | (u32)p[4] << 8 | p[5]));
  case 6: { u64 v; memcpy(&v, p, 8); return (i64)__builtin_bswap64(v); }
  case 8: return 0;
  case 9: return 1;
  default: return 0;
  }
}

static const u8 *bt_record(struct bt *b, BtCursor *c, u32 *len) {
  u32 total = sqlite3BtreePayloadSize(c), local;
  const u8 *p = sqlite3BtreePayloadFetch(c, &local);
  if (local == total) { *len = total; return p; }
  if (total > b->spill_cap) {
    free(b->spill);
    b->spill_cap = total + 4096;
    b->spill = malloc(b->spill_cap);
    if (!b->spill) { b->spill_cap = 0; return NULL; }
  }
  if (sqlite3BtreePayload(c, 0, total, b->spill) != SQLITE_OK) return NULL;
  *len = total;
  return b->spill;
}

/* ---- cursor moves: 0 on row, 1 none, -1 error ---- */
static inline int cur_first(BtCursor *c) {
  int empty = 1;
  return sqlite3BtreeFirst(c, &empty) != SQLITE_OK ? -1 : empty ? 1 : 0;
}
static inline int cur_next(BtCursor *c) {
  int rc = sqlite3BtreeNext(c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static inline int cur_seek(BtCursor *c, i64 rowid) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID ? 0 : 1;
}
static int idx_seek(BtCursor *c, KeyInfo *ki, const i64 *keys, int nkeys, int after) {
  Mem m[2];
  UnpackedRecord r;
  memset(m, 0, sizeof m);
  for (int i = 0; i < nkeys; i++) {
    m[i].flags = MEM_Int;
    m[i].u.i = keys[i];
  }
  memset(&r, 0, sizeof r);
  r.pKeyInfo = ki;
  r.aMem = m;
  r.nField = (u16)nkeys;
  r.default_rc = after ? -1 : +1;
  int res = 0;
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 1;
  if (res < 0) return cur_next(c);
  return 0;
}

/* ---- like counts ---- */

static inline int cell_pid_rowid(MemPage *pg, int i, i64 *pid, i64 *rowid) {
  const u8 *cell = findCell(pg, i) + pg->childPtrSize;
  u32 len = cell[0];
  const u8 *p = cell + 1;
  if (len >= 0x80 || len < 3 || p + len > pg->aDataEnd) return 0;
  u32 h = p[0], t0 = p[1], t1 = p[2];
  if (h != 3 || t0 >= 0x80 || t1 >= 0x80) return 0;
  struct col c0 = {t0, p + 3};
  if (!col_is_int(&c0) || p + 3 + serial_len(t0) + serial_len(t1) > p + len) return 0;
  *pid = col_int(&c0);
  if (rowid) {
    struct col c1 = {t1, p + 3 + serial_len(t0)};
    if (!col_is_int(&c1)) return 0;
    *rowid = col_int(&c1);
  }
  return 1;
}

static __attribute__((noinline)) int bt_count_likes_fast(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  BtCursor *c = bt_cur(b, CUR_LIKES_IDX);
  if (!c) return -1;
  int r = idx_seek(c, b->ki_likes, &lo, 1, 0);
  if (r) return r < 0 ? -1 : 0;
  for (;;) {
    if (c->eState != CURSOR_VALID) return 1;
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1;
    i64 pid = 0, rowid = 0;
    for (; i < last; i++) {
      if (!cell_pid_rowid(pg, i, &pid, NULL)) return 1;
      if (pid > hi) return 0;
      if (pid >= lo) {
        if (pg->leaf && i + 8 < last) {
          i64 ahead;
          if (!cell_pid_rowid(pg, i + 8, &ahead, NULL)) return 1;
          if (ahead == pid) {
            int blo = i + 9, bhi = last;
            while (blo < bhi) {
              int mid = blo + ((bhi - blo) >> 1);
              if (!cell_pid_rowid(pg, mid, &ahead, NULL)) return 1;
              if (ahead == pid) blo = mid + 1;
              else bhi = mid;
            }
            n[pid - lo] += (uint32_t)(blo - i);
            i = blo - 1;
            continue;
          }
        }
        n[pid - lo]++;
      }
    }
    if (last == 0 || !cell_pid_rowid(pg, last - 1, &pid, &rowid)) return 1;
    if (!pg->leaf) {
      r = cur_next(c);
      if (r) return r < 0 ? -1 : 0;
      continue;
    }
    i64 k[2] = {pid, rowid};
    r = idx_seek(c, b->ki_likes, k, 2, 1);
    if (r) return r < 0 ? -1 : 0;
  }
}

static __attribute__((noinline)) int bt_count_likes_slow(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  BtCursor *c = bt_cur(b, CUR_LIKES_IDX);
  if (!c) return -1;
  int r = idx_seek(c, b->ki_likes, &lo, 1, 0);
  while (r == 0) {
    u32 len;
    const u8 *rec = bt_record(b, c, &len);
    struct col col[1];
    if (!rec || rec_cols(rec, len, 1, col) || !col_is_int(&col[0])) return -1;
    i64 pid = col_int(&col[0]);
    if (pid > hi) return 0;
    if (pid >= lo) n[pid - lo]++;
    r = cur_next(c);
  }
  return r < 0 ? -1 : 0;
}

static __attribute__((noinline)) int bt_count_likes_tail(struct bt *b, i64 lo, i64 hi, i64 last_pid, uint32_t *n) {
  if (last_pid - hi > TAIL_GAP) return 1;
  BtCursor *c = bt_cur(b, CUR_LIKES_IDX);
  if (!c) return -1;
  int empty = 1;
  if (sqlite3BtreeLast(c, &empty) != SQLITE_OK) return -1;
  if (empty) return 0;
  for (;;) {
    if (c->eState != CURSOR_VALID) return 1;
    MemPage *pg = c->pPage;
    int i = c->ix, first = pg->leaf ? 0 : i;
    i64 pid = 0, rowid = 0;
    for (; i >= first; i--) {
      if (!cell_pid_rowid(pg, i, &pid, NULL)) return 1;
      if (pid < lo) return 0;
      if (pid <= hi) {
        if (pg->leaf && i >= first + 8) {
          i64 ahead;
          if (!cell_pid_rowid(pg, i - 8, &ahead, NULL)) return 1;
          if (ahead == pid) {
            int blo = first, bhi = i - 8;
            while (blo < bhi) {
              int mid = blo + ((bhi - blo) >> 1);
              if (!cell_pid_rowid(pg, mid, &ahead, NULL)) return 1;
              if (ahead == pid) bhi = mid;
              else blo = mid + 1;
            }
            n[pid - lo] += (uint32_t)(i - blo + 1);
            i = blo;
            continue;
          }
        }
        n[pid - lo]++;
      }
    }
    if (!cell_pid_rowid(pg, first, &pid, &rowid)) return 1;
    int r;
    if (!pg->leaf) {
      r = sqlite3BtreePrevious(c, 0);
      if (r == SQLITE_DONE) return 0;
      if (r != SQLITE_OK) return -1;
      continue;
    }
    i64 k[2] = {pid, rowid};
    r = idx_seek(c, b->ki_likes, k, 2, 0);
    if (r) return r < 0 ? -1 : 0;
    r = sqlite3BtreePrevious(c, 0);
    if (r == SQLITE_DONE) return 0;
    if (r != SQLITE_OK) return -1;
  }
}

static inline BtCursor *bt_ensure_likes_last(struct bt *b, int *empty_out) {
  uint64_t bit = 1ULL << CUR_LIKES_LAST;
  if (likely(b->open_mask & bit)) {
    BtCursor *cl = b->cur[CUR_LIKES_LAST];
    if (likely(cl->eState == CURSOR_VALID && (cl->curFlags & BTCF_AtLast))) {
      *empty_out = 0;
      return cl;
    }
  }
  BtCursor *cl = bt_cur(b, CUR_LIKES_LAST);
  if (unlikely(!cl)) return NULL;
  int empty = 1;
  if (unlikely(sqlite3BtreeLast(cl, &empty) != SQLITE_OK)) return NULL;
  *empty_out = empty;
  return cl;
}

static int bt_count_likes(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  memset(n, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
  int empty = 1;
  BtCursor *cl = bt_ensure_likes_last(b, &empty);
  if (unlikely(!cl)) return -1;
  if (unlikely(empty)) return 0;

  if (lo == hi) {
    /* Point lookup (GET /posts/:id): O(1) max-pid bound check and in-leaf binary search. */
    if (likely(cl->eState == CURSOR_VALID && cl->pPage->leaf && cl->ix >= 0 && cl->ix + 1 == cl->pPage->nCell)) {
      MemPage *lpg = cl->pPage;
      i64 last_pid = 0, first_pid = 0;
      if (likely(cell_pid_rowid(lpg, cl->ix, &last_pid, NULL))) {
        if (lo > last_pid) return 0;
        if (likely(cell_pid_rowid(lpg, 0, &first_pid, NULL)) &&
            (lo > first_pid || (cl->iPage == 0 && lo >= first_pid))) {
          int end = cl->ix + 1;
          int a = (lo == first_pid) ? 0 : 1, bhi = end;
          while (a < bhi) {
            int mid = a + ((bhi - a) >> 1);
            i64 mpid;
            if (unlikely(!cell_pid_rowid(lpg, mid, &mpid, NULL))) goto slow_scan;
            if (mpid < lo) a = mid + 1;
            else bhi = mid;
          }
          int lb = a;
          if (lb == end) return 0;
          i64 lpid;
          if (unlikely(!cell_pid_rowid(lpg, lb, &lpid, NULL))) goto slow_scan;
          if (lpid != lo) return 0;
          int u_lo = lb + 1, u_hi = end;
          while (u_lo < u_hi) {
            int mid = u_lo + ((u_hi - u_lo) >> 1);
            i64 mpid;
            if (unlikely(!cell_pid_rowid(lpg, mid, &mpid, NULL))) goto slow_scan;
            if (mpid <= lo) u_lo = mid + 1;
            else u_hi = mid;
          }
          n[0] = (uint32_t)(u_lo - lb);
          return 0;
        }
      }
    }
  } else if (hi - lo >= 4) {
    /* Range query (GET /feed): rightmost leaf scan or bounded tail reverse scan. */
    i64 last_pid = 0;
    int have_last = cl->eState == CURSOR_VALID && cl->pPage->leaf && cl->ix >= 0 &&
                    cell_pid_rowid(cl->pPage, cl->ix, &last_pid, NULL);
    if (have_last) {
      if (lo > last_pid) return 0;
      if (hi > last_pid) hi = last_pid;
      MemPage *lpg = cl->pPage;
      i64 first_pid = 0;
      if (cell_pid_rowid(lpg, 0, &first_pid, NULL) && (cl->iPage == 0 || lo > first_pid)) {
        for (int i = cl->ix; i >= 0; i--) {
          i64 pid;
          if (!cell_pid_rowid(lpg, i, &pid, NULL)) goto tail_multi;
          if (pid < lo) return 0;
          if (pid <= hi) {
            if (i >= 8) {
              i64 ahead;
              if (!cell_pid_rowid(lpg, i - 8, &ahead, NULL)) goto tail_multi;
              if (ahead == pid) {
                int blo = 0, bhi = i - 8;
                while (blo < bhi) {
                  int mid = blo + ((bhi - blo) >> 1);
                  if (!cell_pid_rowid(lpg, mid, &ahead, NULL)) goto tail_multi;
                  if (ahead == pid) bhi = mid;
                  else blo = mid + 1;
                }
                n[pid - lo] += (uint32_t)(i - blo + 1);
                i = blo;
                continue;
              }
            }
            n[pid - lo]++;
          }
        }
        return 0;
      }
    tail_multi:;
      if (last_pid - hi <= TAIL_GAP) {
        int r = bt_count_likes_tail(b, lo, hi, last_pid, n);
        if (r <= 0) return r;
        memset(n, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
      }
    }
  }

slow_scan:;
  int r = bt_count_likes_fast(b, lo, hi, n);
  if (r <= 0) return r;
  memset(n, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
  return bt_count_likes_slow(b, lo, hi, n);
}

static inline i64 bt_cell_key(MemPage *pg, int i) {
  const u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf) {
    while (*(c++) & 0x80)
      if (c >= pg->aDataEnd) return INT64_MAX;
  }
  if (c[0] < 0x80) return c[0];
  if (c[1] < 0x80) return (i64)(c[0] & 0x7f) << 7 | c[1];
  u64 v;
  sqlite3GetVarint(c, &v);
  return (i64)v;
}

static inline int cur_at_rowid(BtCursor *c, i64 rowid) {
  return c->eState == CURSOR_VALID && c->pPage->intKeyLeaf &&
         (c->curFlags & BTCF_ValidNKey) && c->info.nKey == rowid;
}

static int bt_user_name(struct bt *b, BtCursor *c, i64 uid, const u8 **name, u32 *len) {
  if (!cur_at_rowid(c, uid)) {
    int rc = cur_seek(c, uid);
    if (rc) return rc;
  }
  if (c->eState == CURSOR_VALID && c->pPage->intKeyLeaf) {
    const u8 *p = findCellPastPtr(c->pPage, c->ix);
    const u8 *end = c->pPage->aDataEnd;
    if (p + 2 < end && p[0] < 0x80) {
      u32 plen = p[0];
      const u8 *q = p + 1;
      if (*q < 0x80) q++;
      else if (q + 1 < end && q[1] < 0x80) q += 2;
      else if (q + 2 < end && q[2] < 0x80) q += 3;
      else { u64 dummy; q = rd_varint(q, end, &dummy); if (!q) goto slow_user; }
      if (q + plen <= end && plen >= 4 && q[0] == 4 && q[1] == 0 &&
          q[2] >= 13 && q[2] < 0x80 && (q[2] & 1) && q[3] < 0x80) {
        u32 ulen = (u32)((q[2] - 13) >> 1);
        if (4u + ulen <= plen) {
          *name = q + 4;
          *len = ulen;
          return 0;
        }
      }
    }
  }
slow_user:;
  u32 n;
  const u8 *rec = bt_record(b, c, &n);
  struct col col[2];
  if (!rec || rec == b->spill || rec_cols(rec, n, 2, col) || !col_is_text(&col[1])) return -1;
  *name = col[1].p;
  *len = col_len(&col[1]);
  return 0;
}

/* ---- JSON object writer ---- */

#define OB_PUT(o, lit) (memcpy((o)->p + (o)->n, lit, sizeof(lit) - 1), (o)->n += sizeof(lit) - 1)
static __attribute__((noinline)) void ob_post_obj(struct ob *o, i64 id, const u8 *body, u32 body_n,
                                                  const u8 *created, u32 created_n,
                                                  const u8 *author, u32 author_n, i64 likes) {
  if (!ob_room(o, 64)) return;
  OB_PUT(o, "{\"id\":");
  o->n += u64_to_dec((uint64_t)id, (char *)o->p + o->n);
  OB_PUT(o, ",\"body\":");
  ob_json_str(o, body, body_n);
  if (!ob_room(o, 20)) return;
  OB_PUT(o, ",\"created_at\":");
  ob_json_str(o, created, created_n);
  if (!ob_room(o, 16)) return;
  OB_PUT(o, ",\"author\":");
  ob_json_str(o, author, author_n);
  if (!ob_room(o, 40)) return;
  OB_PUT(o, ",\"like_count\":");
  o->n += u64_to_dec((uint64_t)likes, (char *)o->p + o->n);
  o->p[o->n++] = '}';
}

static inline int parse_post_leaf_cell(MemPage *pg, int idx, i64 *id_out, struct col pc[4]) {
  const u8 *cell = findCell(pg, idx);
  const u8 *p = cell;
  const u8 *end = pg->aDataEnd;
  if (likely(p + 2 < end)) {
    u32 plen = p[0];
    if (plen < 0x80) {
      p += 1;
    } else if (p[1] < 0x80) {
      plen = ((plen & 0x7fu) << 7) | p[1];
      p += 2;
    } else {
      goto fallback;
    }
    u64 key = 0;
    p = rd_varint(p, end, &key);
    if (likely(p != NULL && plen <= (u32)pg->maxLocal && p + plen <= end)) {
      if (likely(rec_cols(p, plen, 4, pc) == 0 && col_is_int(&pc[1]) && col_is_text(&pc[2]) && col_is_text(&pc[3]))) {
        *id_out = (i64)key;
        return 1;
      }
      return 0;
    }
  }
fallback:;
  CellInfo info;
  pg->xParseCell(pg, (u8 *)cell, &info);
  if (info.nLocal != info.nPayload || rec_cols(info.pPayload, info.nLocal, 4, pc) != 0 ||
      !col_is_int(&pc[1]) || !col_is_text(&pc[2]) || !col_is_text(&pc[3]))
    return 0;
  *id_out = info.nKey;
  return 1;
}

/* ---- feed & point reads ---- */

struct feed_item {
  i64 id, uid;
  const u8 *body, *created;
  u32 body_n, created_n, likes;
};

static inline int ts_cmp(const u8 *a, u32 an, const u8 *b, u32 bn) {
  u32 m = an < bn ? an : bn;
  int v = memcmp(a, b, m);
  return v ? v : (an > bn) - (an < bn);
}

static int bt_post_row_from_cur(struct bt *b, BtCursor *cp, struct feed_item *r, MemPage **pinned, int *npinned) {
  u32 len;
  const u8 *rec = bt_record(b, cp, &len);
  struct col c[4];
  if (!rec || rec == b->spill || rec_cols(rec, len, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) || !col_is_text(&c[3])) return -1;
  if (cp->eState != CURSOR_VALID) return -1;
  MemPage *pg = cp->pPage;
  if (*npinned == 0 || pinned[*npinned - 1] != pg) {
    if (*npinned == FEED_N) return -1;
    sqlite3PagerRef(pg->pDbPage);
    pinned[(*npinned)++] = pg;
  }
  r->body = c[2].p;
  r->body_n = col_len(&c[2]);
  r->created = c[3].p;
  r->created_n = col_len(&c[3]);
  r->uid = col_int(&c[1]);
  return 0;
}

static void bt_unpin(MemPage **pinned, int n) {
  for (int i = 0; i < n; i++) releasePageNotNull(pinned[i]);
}

/* Resolves the 20 authors using hash-indexed CUR_USER_BASE[uid & 31] cursors
 * shared across /feed and /posts/:id, with intra-feed uid deduplication. */
static int bt_resolve_and_emit(struct bt *b, struct ob *o, const struct feed_item *items, int n,
                               MemPage **pinned, int npinned) {
  const u8 *name[FEED_N];
  u32 name_n[FEED_N];
  for (int i = 0; i < n; i++) {
    int dup = -1;
    for (int j = 0; j < i; j++) {
      if (items[j].uid == items[i].uid) { dup = j; break; }
    }
    if (dup >= 0) {
      name[i] = name[dup];
      name_n[i] = name_n[dup];
      continue;
    }
    BtCursor *uc = bt_cur(b, CUR_USER_BASE + (int)((uint64_t)items[i].uid & (USER_SLOTS - 1u)));
    if (!uc || bt_user_name(b, uc, items[i].uid, &name[i], &name_n[i])) {
      bt_unpin(pinned, npinned);
      return -1;
    }
  }
  ob_lit(o, "{\"posts\":[");
  for (int i = 0; i < n; i++) {
    if (i) ob_char(o, ',');
    ob_post_obj(o, items[i].id, items[i].body, items[i].body_n, items[i].created, items[i].created_n,
                name[i], name_n[i], items[i].likes);
  }
  ob_lit(o, "]}");
  bt_unpin(pinned, npinned);
  return 0;
}

/* Speculative reverse-rowid tail plan with O(1) cell-19 rank_twenty certificate.
 * Returns 0 on success, 1 to fall back to bt_feed_index, -1 on error. */
static int bt_feed_tail(struct bt *b, struct ob *o) {
  BtCursor *ci = bt_cur(b, CUR_FEED_IDX);
  BtCursor *ct = bt_cur(b, CUR_FEED_TAIL);
  if (!ci || !ct) return -1;
  int rc = cur_first(ci);
  if (rc != 0) return rc;
  if (ci->eState != CURSOR_VALID || !ci->pPage->leaf || ci->pPage->nCell < FEED_N) return 1;
  CellInfo cinfo;
  ci->pPage->xParseCell(ci->pPage, findCell(ci->pPage, FEED_N - 1), &cinfo);
  struct col ic[2];
  if (cinfo.nLocal != cinfo.nPayload || rec_cols(cinfo.pPayload, cinfo.nPayload, 2, ic) ||
      !col_is_text(&ic[0]) || !col_is_int(&ic[1]))
    return 1;
  i64 boundary_id = col_int(&ic[1]);

  int empty = 1;
  if (sqlite3BtreeLast(ct, &empty) != SQLITE_OK) return -1;
  if (empty) return 1;
  if (ct->eState != CURSOR_VALID || !ct->pPage->intKeyLeaf) return 1;

  struct feed_item items[FEED_N];
  MemPage *pinned[2];
  int npinned = 0, n = 0, ordered = 1;
  MemPage *pg = ct->pPage;
  sqlite3PagerRef(pg->pDbPage);
  pinned[npinned++] = pg;

  for (int i = ct->ix; i >= 0 && n < FEED_N; i--) {
    struct col pc[4];
    i64 pid = 0;
    if (!parse_post_leaf_cell(pg, i, &pid, pc)) {
      bt_unpin(pinned, npinned);
      return 1;
    }
    items[n].id = pid;
    items[n].uid = col_int(&pc[1]);
    items[n].body = pc[2].p;
    items[n].body_n = col_len(&pc[2]);
    items[n].created = pc[3].p;
    items[n].created_n = col_len(&pc[3]);
    if (n > 0 && ordered) {
      if (ts_cmp(items[n - 1].created, items[n - 1].created_n, items[n].created, items[n].created_n) < 0)
        ordered = 0;
    }
    n++;
  }

  /* If rightmost leaf had < 20 cells, use CUR_POST_PREV to seek items[n-1].id and step left once
   * so CUR_FEED_TAIL stays AtLast and CUR_POST_PREV remains parked on the 2nd-to-last leaf! */
  if (n < FEED_N) {
    BtCursor *cp2 = bt_cur(b, CUR_POST_PREV);
    if (!cp2 || cur_seek(cp2, items[n - 1].id) != 0) { bt_unpin(pinned, npinned); return 1; }
    int pr = sqlite3BtreePrevious(cp2, 0);
    if (pr == SQLITE_DONE) { bt_unpin(pinned, npinned); return 1; }
    if (pr != SQLITE_OK || cp2->eState != CURSOR_VALID || !cp2->pPage->intKeyLeaf) {
      bt_unpin(pinned, npinned);
      return 1;
    }
    MemPage *pg2 = cp2->pPage;
    sqlite3PagerRef(pg2->pDbPage);
    pinned[npinned++] = pg2;
    for (int i = cp2->ix; i >= 0 && n < FEED_N; i--) {
      struct col pc[4];
      i64 pid = 0;
      if (!parse_post_leaf_cell(pg2, i, &pid, pc)) {
        bt_unpin(pinned, npinned);
        return 1;
      }
      items[n].id = pid;
      items[n].uid = col_int(&pc[1]);
      items[n].body = pc[2].p;
      items[n].body_n = col_len(&pc[2]);
      items[n].created = pc[3].p;
      items[n].created_n = col_len(&pc[3]);
      if (ordered && ts_cmp(items[n - 1].created, items[n - 1].created_n, items[n].created, items[n].created_n) < 0)
        ordered = 0;
      n++;
    }
  }

  if (n < FEED_N) { bt_unpin(pinned, npinned); return 1; }

  int worst = FEED_N - 1;
  if (!ordered) {
    worst = 0;
    for (int i = 1; i < FEED_N; i++) {
      int c = ts_cmp(items[i].created, items[i].created_n, items[worst].created, items[worst].created_n);
      if (c < 0 || (c == 0 && items[i].id < items[worst].id)) worst = i;
    }
  }
  if (items[worst].id != boundary_id) {
    bt_unpin(pinned, npinned);
    return 1;
  }

  /* items[0..19] are in descending id order: lo = items[19].id, hi = items[0].id */
  i64 lo = items[FEED_N - 1].id, hi = items[0].id;
  if (hi - lo < LIKES_SPAN) {
    uint32_t cnt[LIKES_SPAN];
    if (bt_count_likes(b, lo, hi, cnt)) { bt_unpin(pinned, npinned); return -1; }
    for (int i = 0; i < FEED_N; i++) items[i].likes = cnt[items[i].id - lo];
  } else {
    for (int i = 0; i < FEED_N; i++) {
      uint32_t cnt;
      if (bt_count_likes(b, items[i].id, items[i].id, &cnt)) { bt_unpin(pinned, npinned); return -1; }
      items[i].likes = cnt;
    }
  }

  if (!ordered) {
    for (int i = 1; i < FEED_N; i++) {
      struct feed_item tmp = items[i];
      int j = i - 1;
      while (j >= 0) {
        int c = ts_cmp(items[j].created, items[j].created_n, tmp.created, tmp.created_n);
        if (c > 0 || (c == 0 && items[j].id > tmp.id)) break;
        items[j + 1] = items[j];
        j--;
      }
      items[j + 1] = tmp;
    }
  }

  return bt_resolve_and_emit(b, o, items, FEED_N, pinned, npinned);
}

static __attribute__((noinline)) int bt_feed_index(struct bt *b, struct ob *o) {
  BtCursor *ci = bt_cur(b, CUR_FEED_IDX);
  BtCursor *cp = bt_cur(b, CUR_POSTS);
  if (!ci || !cp) return -1;
  struct feed_item items[FEED_N];
  int n = 0;
  int rc = cur_first(ci);
  while (rc == 0 && n < FEED_N) {
    u32 len;
    const u8 *rec = bt_record(b, ci, &len);
    struct col c[2];
    if (!rec || rec_cols(rec, len, 2, c) || !col_is_text(&c[0]) || !col_is_int(&c[1])) return -1;
    items[n++].id = col_int(&c[1]);
    rc = cur_next(ci);
  }
  if (rc < 0) return -1;
  if (n == 0) { ob_lit(o, "{\"posts\":[]}"); return 0; }
  i64 lo = items[0].id, hi = items[0].id;
  for (int i = 1; i < n; i++) {
    if (items[i].id < lo) lo = items[i].id;
    if (items[i].id > hi) hi = items[i].id;
  }
  if (hi - lo < LIKES_SPAN) {
    uint32_t cnt[LIKES_SPAN];
    if (bt_count_likes(b, lo, hi, cnt)) return -1;
    for (int i = 0; i < n; i++) items[i].likes = cnt[items[i].id - lo];
  } else {
    for (int i = 0; i < n; i++) {
      uint32_t cnt;
      if (bt_count_likes(b, items[i].id, items[i].id, &cnt)) return -1;
      items[i].likes = cnt;
    }
  }
  MemPage *pinned[FEED_N];
  int npinned = 0;
  for (int i = 0; i < n; i++) {
    int found = 0;
    if (i > 0 && items[i].id == items[i - 1].id - 1 && cp->eState == CURSOR_VALID) {
      int r = sqlite3BtreePrevious(cp, 0);
      if (r == SQLITE_OK) found = sqlite3BtreeIntegerKey(cp) == items[i].id;
      else if (r != SQLITE_DONE) { bt_unpin(pinned, npinned); return -1; }
    }
    if ((!found && cur_seek(cp, items[i].id)) || bt_post_row_from_cur(b, cp, &items[i], pinned, &npinned)) {
      bt_unpin(pinned, npinned);
      return -1;
    }
  }
  return bt_resolve_and_emit(b, o, items, n, pinned, npinned);
}

static int bt_feed(struct bt *b, struct ob *o) {
  int r = bt_feed_tail(b, o);
  if (r <= 0) return r;
  return bt_feed_index(b, o);
}

static inline int bt_tail_leaf_hit(BtCursor *c, i64 id, struct feed_item *row, MemPage **pinned, int *npinned) {
  if (c->eState != CURSOR_VALID || !c->pPage->intKeyLeaf || c->ix < 0) return 0;
  i64 last_id = bt_cell_key(c->pPage, c->ix);
  if (id > last_id) return (c->curFlags & BTCF_AtLast) ? -1 : 0;
  if (last_id - id <= c->ix) {
    int idx = c->ix - (int)(last_id - id);
    struct col pc[4];
    i64 got_id = 0;
    if (parse_post_leaf_cell(c->pPage, idx, &got_id, pc) && got_id == id) {
      sqlite3PagerRef(c->pPage->pDbPage);
      pinned[0] = c->pPage;
      *npinned = 1;
      row->body = pc[2].p;
      row->body_n = col_len(&pc[2]);
      row->created = pc[3].p;
      row->created_n = col_len(&pc[3]);
      row->uid = col_int(&pc[1]);
      return 1;
    }
  }
  return 0;
}

static int bt_post(struct bt *b, struct ob *o, i64 id) {
  struct feed_item row;
  MemPage *pinned[1];
  int npinned = 0;
  if (b->open_mask & (1ULL << CUR_FEED_TAIL)) {
    int r = bt_tail_leaf_hit(b->cur[CUR_FEED_TAIL], id, &row, pinned, &npinned);
    if (r < 0) return 0;
    if (r > 0) goto got_post_row;
  }
  if (b->open_mask & (1ULL << CUR_POST_PREV)) {
    int r = bt_tail_leaf_hit(b->cur[CUR_POST_PREV], id, &row, pinned, &npinned);
    if (r > 0) goto got_post_row;
  }
  BtCursor *cp = bt_cur(b, CUR_POSTS);
  if (!cp) return -1;
  int rc = cur_seek(cp, id);
  if (rc) return rc < 0 ? -1 : 0;
  if (bt_post_row_from_cur(b, cp, &row, pinned, &npinned)) { bt_unpin(pinned, npinned); return -1; }
got_post_row:;
  uint32_t likes;
  if (bt_count_likes(b, id, id, &likes)) { bt_unpin(pinned, npinned); return -1; }
  const u8 *name;
  u32 name_n;
  BtCursor *cu = bt_cur(b, CUR_USER_BASE + (int)((uint64_t)row.uid & (USER_SLOTS - 1u)));
  if (!cu || bt_user_name(b, cu, row.uid, &name, &name_n)) { bt_unpin(pinned, npinned); return -1; }
  ob_lit(o, "{\"post\":");
  ob_post_obj(o, id, row.body, row.body_n, row.created, row.created_n, name, name_n, likes);
  ob_char(o, '}');
  bt_unpin(pinned, npinned);
  return 1;
}

static int bt_post_exists(struct bt *b, i64 id) {
  if (b->open_mask & (1ULL << CUR_FEED_TAIL)) {
    BtCursor *ct = b->cur[CUR_FEED_TAIL];
    if (ct->eState == CURSOR_VALID && ct->pPage->intKeyLeaf && ct->ix >= 0) {
      i64 last_id = bt_cell_key(ct->pPage, ct->ix);
      if (id > last_id && (ct->curFlags & BTCF_AtLast)) return 0;
      if (id <= last_id && last_id - id <= ct->ix) {
        int idx = ct->ix - (int)(last_id - id);
        if (bt_cell_key(ct->pPage, idx) == id) return 1;
      }
    }
  }
  if (b->open_mask & (1ULL << CUR_POST_PREV)) {
    BtCursor *cp2 = b->cur[CUR_POST_PREV];
    if (cp2->eState == CURSOR_VALID && cp2->pPage->intKeyLeaf && cp2->ix >= 0) {
      i64 last_id = bt_cell_key(cp2->pPage, cp2->ix);
      if (id <= last_id && last_id - id <= cp2->ix) {
        int idx = cp2->ix - (int)(last_id - id);
        if (bt_cell_key(cp2->pPage, idx) == id) return 1;
      }
    }
  }
  BtCursor *cp = bt_cur(b, CUR_POSTS);
  if (!cp) return -1;
  int rc = cur_seek(cp, id);
  return rc < 0 ? -1 : rc == 0;
}

#endif
