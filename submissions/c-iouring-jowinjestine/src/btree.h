/* btree.h: GET /feed and GET /posts/:id on SQLite's own B-tree cursors, inside the batch's read
 * transaction. Compiled in the same translation unit as the unmodified amalgamation. Every page
 * comes through SQLite's pager, WAL and page cache; only the bytecode interpreter is skipped,
 * because both queries are known ahead of time. Anything unexpected returns -1 and the caller
 * runs the reference SQL instead. Nothing read here outlives the request. */
#ifndef BTREE_H
#define BTREE_H

#define FEED_N 20
#define LIKES_SPAN 512 /* count likes with one index walk when the feed's ids lie within this span */
static const int opt_tail = 1, opt_batch = 1; /* the two read-path variants (both measured; see README) */
static inline int cell_pid_rowid(MemPage *pg, int i, i64 *pid, i64 *rowid);

enum { CUR_POSTS = 0, CUR_USERS, CUR_FEED_IDX, CUR_LIKES_IDX, NCUR };

struct bt {
  Btree *bt;
  Pgno root[NCUR];
  KeyInfo *ki_feed, *ki_likes;
  BtCursor *cur[NCUR];
  int open; /* cursors are open in the current transaction */
  int ok;   /* the schema matches; plans are enabled */
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

/* The column layout the decoders were written for, checked at startup. */
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
  b->root[CUR_POSTS] = bt_rootpage(c, "posts");
  b->root[CUR_USERS] = bt_rootpage(c, "users");
  b->root[CUR_FEED_IDX] = bt_rootpage(c, "posts_created_at_id_idx");
  b->root[CUR_LIKES_IDX] = bt_rootpage(c, "likes_post_id_idx");
  if (!ok || ENC(c) != SQLITE_UTF8) return 0;
  for (int i = 0; i < NCUR; i++)
    if (!b->root[i]) return 0;
  b->ki_feed = bt_keyinfo(c, 2, 1, 1);
  b->ki_likes = bt_keyinfo(c, 1, 1, 0);
  if (!b->ki_feed || !b->ki_likes) return 0;
  for (int i = 0; i < NCUR; i++) {
    b->cur[i] = malloc((size_t)sqlite3BtreeCursorSize());
    if (!b->cur[i]) return 0;
  }
  b->ok = 1;
  return 1;
}

/* Cursors live inside one transaction: opened after it starts, closed before it ends. */
static int bt_open(struct bt *b) {
  if (!b->ok || b->open) return 0;
  if (sqlite3BtreeBeginTrans(b->bt, 0, 0) != SQLITE_OK) return -1;
  KeyInfo *ki[NCUR] = {NULL, NULL, b->ki_feed, b->ki_likes};
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(b->cur[i]);
    if (sqlite3BtreeCursor(b->bt, b->root[i], 0, ki[i], b->cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(b->cur[j]);
      return -1;
    }
  }
  b->open = 1;
  return 0;
}
static void bt_close(struct bt *b) {
  if (!b->open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(b->cur[i]);
  b->open = 0;
}

/* ---- records (https://sqlite.org/fileformat2.html#record_format) ---- */

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
/* The first ncol columns of a record. */
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
  case 6: { u64 v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return (i64)v; }
  case 8: return 0;
  case 9: return 1;
  default: return 0;
  }
}

/* The current row's record bytes (local payload, or copied if it spilled to overflow pages). */
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

/* ---- cursor moves: 0 on a row, 1 none, -1 error ---- */
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
/* Index cursor to the first entry whose key is > (or >= when after == 0) the given fields. */
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
  r.default_rc = after ? -1 : +1; /* +1: the key sorts before equal entries; -1: after them */
  int res = 0;
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 1;
  if (res < 0) return cur_next(c);
  return 0;
}

/* ---- like counts ----
 * Entries of likes_post_id_idx are (post_id, rowid), ordered. The cursor is positioned at the
 * first entry with post_id >= lo; the cells of its page are then decoded here, a page at a
 * time, instead of one cursor step per entry. The cursor's position is only read. When a leaf
 * is used up the cursor is moved by a seek past the leaf's last entry. 0 ok, 1 unexpected form
 * (the caller counts with cursor steps instead), -1 error. */
static int bt_count_likes_fast(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  BtCursor *c = b->cur[CUR_LIKES_IDX];
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
      if (pid >= lo) n[pid - lo]++;
    }
    if (!cell_pid_rowid(pg, last - 1, &pid, &rowid)) return 1;
    if (!pg->leaf) { /* an interior entry: the next entries are in the child subtree after it */
      r = cur_next(c);
      if (r) return r < 0 ? -1 : 0;
      continue;
    }
    if (last == 0) return 1;
    i64 k[2] = {pid, rowid};
    r = idx_seek(c, b->ki_likes, k, 2, 1);
    if (r) return r < 0 ? -1 : 0;
  }
}
/* The same with one cursor step per entry. */
static int bt_count_likes_slow(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  BtCursor *c = b->cur[CUR_LIKES_IDX];
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
static int bt_count_likes_tail(struct bt *b, i64 lo, i64 hi, uint32_t *n);
static int bt_count_likes(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  memset(n, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
  /* a range (the feed) from the tail; a single post seeks (the tail scan would pass the likes of every newer post) */
  int r = opt_tail && hi - lo >= 4 ? bt_count_likes_tail(b, lo, hi, n) : 1;
  if (r <= 0) return r;
  memset(n, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
  r = bt_count_likes_fast(b, lo, hi, n);
  if (r <= 0) return r;
  memset(n, 0, sizeof(uint32_t) * (size_t)(hi - lo + 1));
  return bt_count_likes_slow(b, lo, hi, n);
}

/* ---- like counts off the index's tail ----
 * The likes of the newest posts are the last entries of likes_post_id_idx (it is ordered by
 * post_id). When hi is within TAIL_GAP ids of the last entry's post_id, the counts are read
 * backwards from the end: sqlite3BtreeLast costs nothing while the cursor already sits there
 * (no like was inserted since), and no record comparison is needed. 0 ok, 1 not applicable or
 * unexpected (the caller seeks instead), -1 error. */
#define TAIL_GAP 64
/* The (post_id, rowid) of an index cell. rowid is decoded only when asked for (the continuation key). */
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
static int bt_count_likes_tail(struct bt *b, i64 lo, i64 hi, uint32_t *n) {
  BtCursor *c = b->cur[CUR_LIKES_IDX];
  int empty = 1;
  if (sqlite3BtreeLast(c, &empty) != SQLITE_OK) return -1;
  if (empty) return 0;
  if (c->eState != CURSOR_VALID || !c->pPage->leaf) return 1;
  i64 pid, rowid;
  if (!cell_pid_rowid(c->pPage, c->ix, &pid, NULL)) return 1;
  if (pid - hi > TAIL_GAP) return 1;
  for (;;) {
    if (c->eState != CURSOR_VALID) return 1;
    MemPage *pg = c->pPage;
    int i = c->ix, first = pg->leaf ? 0 : i;
    for (; i >= first; i--) {
      if (!cell_pid_rowid(pg, i, &pid, NULL)) return 1;
      if (pid < lo) return 0;
      if (pid <= hi) n[pid - lo]++;
    }
    if (!cell_pid_rowid(pg, first, &pid, &rowid)) return 1;
    int r;
    if (!pg->leaf) { /* an interior entry: the entries before it are in the child subtree */
      r = sqlite3BtreePrevious(c, 0);
      if (r == SQLITE_DONE) return 0;
      if (r != SQLITE_OK) return -1;
      continue;
    }
    /* the leaf is used up: back to its first entry, then one step back */
    i64 k[2] = {pid, rowid};
    r = idx_seek(c, b->ki_likes, k, 2, 0);
    if (r) return r < 0 ? -1 : 0;
    r = sqlite3BtreePrevious(c, 0);
    if (r == SQLITE_DONE) return 0;
    if (r != SQLITE_OK) return -1;
  }
}

/* ---- the authors: several rowid lookups in users, one level at a time ----
 * The same descent as sqlite3BtreeTableMoveto, through the same pager calls (getAndInitPage,
 * findCell, releasePage), for up to FEED_N keys together: every key's child page is chosen on
 * one level before any page of the next level is fetched, and a page shared by several keys is
 * fetched and bounded once. Inside a page the search starts where a straight line through the
 * page's first and last rowids puts the key (rowids are dense), so it takes 2-3 probes instead of
 * a binary search's 7-9, each on its own cache line. (The idea is from c-iouring-pavlyhalim #245.)
 * Anything not in the expected form returns -1 with no pages held, and the caller uses the
 * cursor instead. The hits hold references to the leaf pages the names point into. */
struct bt_hit {
  MemPage *pg;
  int ix;
};
static inline i64 bt_cell_key(MemPage *pg, int i) {
  const u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf) { /* skip the payload-size varint */
    while (*(c++) & 0x80)
      if (c >= pg->aDataEnd) return INT64_MAX;
  }
  if (c[0] < 0x80) return c[0];
  if (c[1] < 0x80) return (i64)(c[0] & 0x7f) << 7 | c[1];
  u64 v;
  sqlite3GetVarint(c, &v);
  return (i64)v;
}
struct bt_pageinfo {
  MemPage *pg;
  i64 k0, kn;
};
/* the first cell whose key is >= k (nCell if none) */
static int bt_lower_bound(const struct bt_pageinfo *pi, i64 k) {
  MemPage *pg = pi->pg;
  int n = pg->nCell;
  if (k <= pi->k0) return 0;
  if (k > pi->kn) return n;
  int lo = 1, hi = n - 1; /* the answer is in [lo, hi]: key(lo-1) < k <= key(hi) */
  int g = lo + (int)((k - pi->k0) * (i64)(n - 1) / (pi->kn - pi->k0));
  if (g < lo) g = lo;
  if (g > hi) g = hi;
  for (int probe = 0; lo < hi; probe++) {
    if (bt_cell_key(pg, g) >= k) hi = g;
    else lo = g + 1;
    if (probe < 2 && lo < hi) g = hi == g ? g - 1 : g + 1; /* two steps next to the guess, then halve */
    else g = lo + ((hi - lo) >> 1);
    if (g < lo) g = lo;
    if (g > hi) g = hi;
  }
  return lo;
}
static void bt_release_hits(struct bt_hit *h, int n) {
  for (int i = 0; i < n; i++)
    if (h[i].pg) releasePageNotNull(h[i].pg);
}
static int bt_seek_many(struct bt *b, Pgno root, const i64 *key, int n, struct bt_hit *hit) {
  BtShared *bs = b->bt->pBt;
  struct bt_pageinfo pages[FEED_N];
  int page_of[FEED_N]; /* each key's page on this level */
  Pgno next[FEED_N];
  if (n < 1 || n > FEED_N) return -1;
  for (int i = 0; i < n; i++) next[i] = root;
  for (int depth = 0;; depth++) {
    int np = 0, leaf = -1;
    for (int i = 0; i < n; i++) {
      int p = 0;
      while (p < np && pages[p].pg->pgno != next[i]) p++;
      if (p == np) {
        MemPage *pg;
        if (getAndInitPage(bs, next[i], &pg, PAGER_GET_READONLY) != SQLITE_OK) pg = NULL;
        if (!pg || !pg->intKey || pg->nCell == 0 || pg->nOverflow || depth >= BTCURSOR_MAX_DEPTH || (leaf >= 0 && pg->leaf != leaf)) {
          if (pg) releasePageNotNull(pg);
          for (int k = 0; k < np; k++) releasePageNotNull(pages[k].pg);
          return -1;
        }
        leaf = pg->leaf;
        pages[np].pg = pg;
        pages[np].k0 = bt_cell_key(pg, 0);
        pages[np].kn = bt_cell_key(pg, pg->nCell - 1);
        np++;
      }
      page_of[i] = p;
    }
    if (leaf) {
      for (int i = 0; i < n; i++) {
        struct bt_pageinfo *pi = &pages[page_of[i]];
        int ix = bt_lower_bound(pi, key[i]);
        if (ix < pi->pg->nCell && bt_cell_key(pi->pg, ix) == key[i]) {
          hit[i].pg = pi->pg;
          hit[i].ix = ix;
          sqlite3PagerRef(pi->pg->pDbPage);
        } else {
          hit[i].pg = NULL;
        }
      }
      for (int k = 0; k < np; k++) releasePageNotNull(pages[k].pg);
      return 0;
    }
    for (int i = 0; i < n; i++) {
      struct bt_pageinfo *pi = &pages[page_of[i]];
      int lw = bt_lower_bound(pi, key[i]); /* key <= cell key: that cell's left child */
      next[i] = lw >= pi->pg->nCell ? get4byte(&pi->pg->aData[pi->pg->hdrOffset + 8]) : get4byte(findCell(pi->pg, lw));
    }
    for (int k = 0; k < np; k++) releasePageNotNull(pages[k].pg);
  }
}
/* usernames for n user ids: 0 with every name found (release the hits after use), 1 if one is
 * absent, -1 if the caller must use the cursor */
static int bt_user_names(struct bt *b, const i64 *uid, int n, struct bt_hit *hit, const u8 **name, u32 *len) {
  if (!opt_batch || bt_seek_many(b, b->root[CUR_USERS], uid, n, hit)) return -1;
  for (int i = 0; i < n; i++) {
    int rc = 1;
    if (hit[i].pg) {
      CellInfo info;
      hit[i].pg->xParseCell(hit[i].pg, findCell(hit[i].pg, hit[i].ix), &info);
      struct col c[2];
      rc = info.nLocal != info.nPayload || rec_cols(info.pPayload, info.nPayload, 2, c) || !col_is_text(&c[1]) ? -1 : 0;
      if (!rc) {
        name[i] = c[1].p;
        len[i] = col_len(&c[1]);
      }
    }
    if (rc) {
      bt_release_hits(hit, n);
      return rc;
    }
  }
  return 0;
}

/* username by rowid: 0 found (name points into the page: use it before the cursor moves),
 * 1 absent, -1 error */
static int bt_user_name(struct bt *b, i64 uid, const u8 **name, u32 *len) {
  BtCursor *c = b->cur[CUR_USERS];
  int rc = cur_seek(c, uid);
  if (rc) return rc;
  u32 n;
  const u8 *rec = bt_record(b, c, &n);
  struct col col[2];
  if (!rec || rec_cols(rec, n, 2, col) || !col_is_text(&col[1])) return -1;
  *name = col[1].p;
  *len = col_len(&col[1]);
  return 0;
}

/* One post object. The literals are written unchecked after one room check for all of them;
 * the strings check their own room (their escaped form may be up to six times longer). */
#define OB_PUT(o, lit) (memcpy((o)->p + (o)->n, lit, sizeof(lit) - 1), (o)->n += sizeof(lit) - 1)
static inline void ob_post_obj(struct ob *o, i64 id, const u8 *body, u32 body_n, const u8 *created, u32 created_n,
                               const u8 *author, u32 author_n, i64 likes) {
  if (!ob_room(o, 96)) return;
  OB_PUT(o, "{\"id\":");
  o->n += u64_to_dec((uint64_t)id, (char *)o->p + o->n);
  OB_PUT(o, ",\"body\":");
  ob_json_str(o, body, body_n);
  if (!ob_room(o, 96)) return;
  OB_PUT(o, ",\"created_at\":");
  ob_json_str(o, created, created_n);
  if (!ob_room(o, 96)) return;
  OB_PUT(o, ",\"author\":");
  ob_json_str(o, author, author_n);
  if (!ob_room(o, 96)) return;
  OB_PUT(o, ",\"like_count\":");
  o->n += u64_to_dec((uint64_t)likes, (char *)o->p + o->n);
  o->p[o->n++] = '}';
}

/* The posts cursor's row: its columns, and the page it is on gets one more reference so the
 * columns stay valid while the cursor moves on (released by the caller). */
struct post_row {
  const u8 *body, *created;
  u32 body_n, created_n;
  i64 uid;
};
static int bt_post_row(struct bt *b, struct post_row *r, MemPage **pinned, int *npinned) {
  BtCursor *cp = b->cur[CUR_POSTS];
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

/* GET /feed: the 20 newest ids off posts_created_at_id_idx (a DESC,DESC index, so the tree's
 * first entries), the like counts with one walk of likes_post_id_idx, the rows in feed order
 * (an id one below the previous is one cursor step back; their pages stay referenced), the 20
 * authors by rowid, then the JSON straight from the pages. 0 ok, -1: the reference query
 * answers instead. */
static int bt_feed(struct bt *b, struct ob *o) {
  i64 ids[FEED_N];
  int n = 0;
  BtCursor *ci = b->cur[CUR_FEED_IDX];
  int rc = cur_first(ci);
  while (rc == 0 && n < FEED_N) {
    u32 len;
    const u8 *rec = bt_record(b, ci, &len);
    struct col c[2];
    if (!rec || rec_cols(rec, len, 2, c) || !col_is_text(&c[0]) || !col_is_int(&c[1])) return -1;
    ids[n++] = col_int(&c[1]);
    rc = cur_next(ci);
  }
  if (rc < 0) return -1;
  if (n == 0) { ob_lit(o, "{\"posts\":[]}"); return 0; }
  i64 lo = ids[0], hi = ids[0];
  for (int i = 1; i < n; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }
  i64 likes[FEED_N];
  if (hi - lo < LIKES_SPAN) {
    uint32_t cnt[LIKES_SPAN];
    if (bt_count_likes(b, lo, hi, cnt)) return -1;
    for (int i = 0; i < n; i++) likes[i] = cnt[ids[i] - lo];
  } else {
    for (int i = 0; i < n; i++) {
      uint32_t cnt;
      if (bt_count_likes(b, ids[i], ids[i], &cnt)) return -1;
      likes[i] = cnt;
    }
  }
  struct post_row row[FEED_N];
  MemPage *pinned[FEED_N];
  int npinned = 0;
  i64 uid[FEED_N];
  BtCursor *cp = b->cur[CUR_POSTS];
  for (int i = 0; i < n; i++) {
    int found = 0;
    if (i > 0 && ids[i] == ids[i - 1] - 1 && cp->eState == CURSOR_VALID) {
      int r = sqlite3BtreePrevious(cp, 0);
      if (r == SQLITE_OK) found = sqlite3BtreeIntegerKey(cp) == ids[i];
      else if (r != SQLITE_DONE) { bt_unpin(pinned, npinned); return -1; }
    }
    if ((!found && cur_seek(cp, ids[i])) || bt_post_row(b, &row[i], pinned, &npinned)) {
      bt_unpin(pinned, npinned); /* the index names a row that is not there, or an odd record */
      return -1;
    }
    uid[i] = row[i].uid;
  }
  /* the authors: all 20 in one descent, or each with the cursor as it is written */
  struct bt_hit hits[FEED_N];
  const u8 *name[FEED_N];
  u32 name_n[FEED_N];
  rc = bt_user_names(b, uid, n, hits, name, name_n);
  if (rc > 0) { bt_unpin(pinned, npinned); return -1; } /* an author is missing: the JOIN drops the post; SQL decides */
  ob_lit(o, "{\"posts\":[");
  for (int i = 0; i < n; i++) {
    if (rc < 0 && bt_user_name(b, uid[i], &name[i], &name_n[i])) { bt_unpin(pinned, npinned); return -1; }
    if (i) ob_char(o, ',');
    ob_post_obj(o, ids[i], row[i].body, row[i].body_n, row[i].created, row[i].created_n, name[i], name_n[i], likes[i]);
  }
  ob_lit(o, "]}");
  if (rc == 0) bt_release_hits(hits, n);
  bt_unpin(pinned, npinned);
  return 0;
}

/* GET /posts/:id. 1 ok (written), 0 absent, -1 error. */
static int bt_post(struct bt *b, struct ob *o, i64 id) {
  int rc = cur_seek(b->cur[CUR_POSTS], id);
  if (rc) return rc < 0 ? -1 : 0;
  uint32_t likes;
  if (bt_count_likes(b, id, id, &likes)) return -1;
  struct post_row row;
  MemPage *pinned[FEED_N];
  int npinned = 0;
  if (bt_post_row(b, &row, pinned, &npinned)) { bt_unpin(pinned, npinned); return -1; }
  const u8 *name;
  u32 name_n;
  if (bt_user_name(b, row.uid, &name, &name_n)) { bt_unpin(pinned, npinned); return -1; } /* an absent author: let SQL decide */
  ob_lit(o, "{\"post\":");
  ob_post_obj(o, id, row.body, row.body_n, row.created, row.created_n, name, name_n, likes);
  ob_char(o, '}');
  bt_unpin(pinned, npinned);
  return 1;
}

/* 1 exists, 0 absent, -1 error */
static int bt_post_exists(struct bt *b, i64 id) {
  int rc = cur_seek(b->cur[CUR_POSTS], id);
  return rc < 0 ? -1 : rc == 0;
}

#endif
