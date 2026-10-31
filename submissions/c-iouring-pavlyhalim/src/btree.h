/* btree.h: the read plans on SQLite's own B-tree cursors. Compiled in the same translation
 * unit as the amalgamation. Every page is read through SQLite's pager, WAL and page cache,
 * inside the batch's transaction; only the bytecode interpreter is skipped, because the two
 * read queries are known ahead of time. Anything unexpected returns -1 and the caller runs
 * the SQL statement instead. */
#ifndef BTREE_H
#define BTREE_H

struct bt_cursor {
  BtCursor *c;
};

struct bt {
  Btree *bt;
  Pgno root_posts, root_users, root_likes_idx, root_posts_idx;
  KeyInfo *ki_likes_idx, *ki_posts_idx;
  struct bt_cursor posts, users, likes_idx, posts_idx;
  int open;        /* cursors are open in the current transaction */
  int ok;          /* the schema matches; plans are enabled */
  uint8_t *spill;  /* records that spill onto overflow pages */
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

/* "name:TYPE:pk,..." for a table, "col,col" for an index: the layout the plans were written for */
static int bt_table_layout(sqlite3 *c, const char *table, const char *expect) {
  char sql[128], got[512];
  snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", table);
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(c, sql, -1, &s, NULL) != SQLITE_OK) return 0;
  size_t n = 0;
  got[0] = 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got)
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s:%s:%d", n ? "," : "", sqlite3_column_text(s, 1),
                          sqlite3_column_text(s, 2), sqlite3_column_int(s, 5));
  sqlite3_finalize(s);
  if (strcmp(got, expect)) { fprintf(stderr, "table %s: layout %s, expected %s\n", table, got, expect); return 0; }
  return 1;
}
static int bt_index_layout(sqlite3 *c, const char *index, const char *expect) {
  char sql[128], got[256];
  snprintf(sql, sizeof sql, "PRAGMA index_xinfo(\"%s\")", index);
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(c, sql, -1, &s, NULL) != SQLITE_OK) return 0;
  size_t n = 0;
  got[0] = 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got) {
    if (!sqlite3_column_int(s, 5)) continue; /* key columns only */
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s:%d", n ? "," : "", sqlite3_column_text(s, 2), sqlite3_column_int(s, 3));
  }
  sqlite3_finalize(s);
  if (strcmp(got, expect)) { fprintf(stderr, "index %s: keys %s, expected %s\n", index, got, expect); return 0; }
  return 1;
}

static KeyInfo *bt_keyinfo(sqlite3 *c, int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(c, nkey, nextra);
  if (!k) return NULL;
  for (int i = 0; i < nkey + nextra; i++) {
    k->aColl[i] = c->pDfltColl;
    if (desc && i < nkey) k->aSortFlags[i] = KEYINFO_ORDER_DESC;
  }
  return k;
}

static int bt_init(struct bt *b, sqlite3 *c) {
  memset(b, 0, sizeof *b);
  b->bt = c->aDb[0].pBt;
  int ok = bt_table_layout(c, "users", "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0") &&
           bt_table_layout(c, "posts", "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0") &&
           bt_table_layout(c, "likes", "user_id:INTEGER:1,post_id:INTEGER:2,created_at:TEXT:0") &&
           bt_index_layout(c, "likes_post_id_idx", "post_id:0") &&
           bt_index_layout(c, "posts_created_at_id_idx", "created_at:1,id:1");
  b->root_posts = bt_rootpage(c, "posts");
  b->root_users = bt_rootpage(c, "users");
  b->root_likes_idx = bt_rootpage(c, "likes_post_id_idx");
  b->root_posts_idx = bt_rootpage(c, "posts_created_at_id_idx");
  if (!ok || !b->root_posts || !b->root_users || !b->root_likes_idx || !b->root_posts_idx) {
    fprintf(stderr, "btree: schema differs; reads use SQL\n");
    return 0;
  }
  b->ki_likes_idx = bt_keyinfo(c, 1, 1, 0);
  b->ki_posts_idx = bt_keyinfo(c, 2, 1, 1);
  size_t sz = (size_t)sqlite3BtreeCursorSize();
  b->posts.c = malloc(sz);
  b->users.c = malloc(sz);
  b->likes_idx.c = malloc(sz);
  b->posts_idx.c = malloc(sz);
  if (!b->ki_likes_idx || !b->ki_posts_idx || !b->posts.c || !b->users.c || !b->likes_idx.c || !b->posts_idx.c) return 0;
  b->ok = 1;
  return 1;
}

/* Cursors live inside one transaction: opened after BEGIN, closed before COMMIT. */
static int bt_open(struct bt *b) {
  if (!b->ok || b->open) return 0;
  if (sqlite3BtreeBeginTrans(b->bt, 0, 0) != SQLITE_OK) return -1;
  sqlite3BtreeCursorZero(b->posts.c);
  sqlite3BtreeCursorZero(b->users.c);
  sqlite3BtreeCursorZero(b->likes_idx.c);
  sqlite3BtreeCursorZero(b->posts_idx.c);
  if (sqlite3BtreeCursor(b->bt, b->root_posts, 0, NULL, b->posts.c) != SQLITE_OK ||
      sqlite3BtreeCursor(b->bt, b->root_users, 0, NULL, b->users.c) != SQLITE_OK ||
      sqlite3BtreeCursor(b->bt, b->root_likes_idx, 0, b->ki_likes_idx, b->likes_idx.c) != SQLITE_OK ||
      sqlite3BtreeCursor(b->bt, b->root_posts_idx, 0, b->ki_posts_idx, b->posts_idx.c) != SQLITE_OK)
    return -1;
  b->open = 1;
  return 0;
}
static void bt_close(struct bt *b) {
  if (!b->open) return;
  sqlite3BtreeCloseCursor(b->posts.c);
  sqlite3BtreeCloseCursor(b->users.c);
  sqlite3BtreeCloseCursor(b->likes_idx.c);
  sqlite3BtreeCloseCursor(b->posts_idx.c);
  b->open = 0;
}

/* ---- records (https://sqlite.org/fileformat2.html#record_format) ---- */

struct rec {
  const u8 *p;
  u32 n;
};
struct col {
  u64 t;
  const u8 *p;
};

static int bt_record(struct bt *b, struct bt_cursor *c, struct rec *r) {
  u32 total = sqlite3BtreePayloadSize(c->c), local;
  const u8 *p = sqlite3BtreePayloadFetch(c->c, &local);
  if (local == total) { r->p = p; r->n = total; return 0; }
  if (total > b->spill_cap) {
    free(b->spill);
    b->spill_cap = total + 4096;
    b->spill = malloc(b->spill_cap);
    if (!b->spill) return -1;
  }
  if (sqlite3BtreePayload(c->c, 0, total, b->spill) != SQLITE_OK) return -1;
  r->p = b->spill;
  r->n = total;
  return 0;
}

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
/* Decodes the first ncol columns. */
static inline int rec_cols(const struct rec *r, int ncol, struct col *c) {
  const u8 *end = r->p + r->n;
  u64 hsz;
  const u8 *h = rd_varint(r->p, end, &hsz);
  if (!h || hsz > r->n) return -1;
  const u8 *hend = r->p + hsz, *body = hend;
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

/* ---- cursor steps: 0 on a row, 1 past the end, -1 on error ---- */
static inline int cur_first(struct bt_cursor *c) {
  int empty;
  return sqlite3BtreeFirst(c->c, &empty) != SQLITE_OK ? -1 : empty ? 1 : 0;
}
static inline int cur_next(struct bt_cursor *c) {
  int rc = sqlite3BtreeNext(c->c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static inline int cur_seek(struct bt_cursor *c, i64 rowid) {
  int res;
  if (sqlite3BtreeTableMoveto(c->c, rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0 ? 0 : 1;
}
/* First index entry whose leading integer key is >= k. */
static int idx_seek_ge(struct bt_cursor *c, KeyInfo *ki, i64 k) {
  Mem m;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  UnpackedRecord r;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = ki;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1;
  int res;
  if (sqlite3BtreeIndexMoveto(c->c, &r, &res) != SQLITE_OK) return -1;
  if (res < 0) return cur_next(c);
  return sqlite3BtreeEof(c->c) ? 1 : 0;
}

/* ---- the plans ---- */

/* like counts for post ids lo..hi from one range scan of likes_post_id_idx */
static int bt_range_counts(struct bt *b, i64 lo, i64 hi, i64 *counts) {
  memset(counts, 0, sizeof(i64) * (size_t)(hi - lo + 1));
  int rc = idx_seek_ge(&b->likes_idx, b->ki_likes_idx, lo);
  while (rc == 0) {
    struct rec r;
    struct col c[1];
    if (bt_record(b, &b->likes_idx, &r) || rec_cols(&r, 1, c) || !col_is_int(&c[0])) return -1;
    i64 pid = col_int(&c[0]);
    if (pid > hi) return 0;
    counts[pid - lo]++;
    rc = cur_next(&b->likes_idx);
  }
  return rc < 0 ? -1 : 0;
}

/* username by rowid: 0 found, 1 absent, -1 error */
static int bt_user_name(struct bt *b, i64 uid, const u8 **name, u32 *len) {
  int rc = cur_seek(&b->users, uid);
  if (rc) return rc;
  struct rec r;
  struct col c[2];
  if (bt_record(b, &b->users, &r) || rec_cols(&r, 2, c) || !col_is_text(&c[1])) return -1;
  *name = c[1].p;
  *len = col_len(&c[1]);
  return 0;
}

/* ---- several rowid lookups in one table, one level at a time ----
 * The same descent as sqlite3BtreeTableMoveto, through the same pager calls, for up to FEED_N keys
 * together: every key's child page is chosen on one level before any page of the next level is
 * fetched, and the pages are prefetched as they are fetched, so the cache misses of different
 * keys overlap instead of queueing; keys that share a page fetch it once. Inside a page the search
 * starts at an interpolated guess: table rowids are dense (users 1..N), so the key is usually at
 * or next to where a straight line through the page's first and last keys puts it, where a plain
 * binary search reads 7-9 cells, each on its own cache line. */
struct bt_hit {
  MemPage *pg; /* the leaf holding the key, referenced; NULL if the key is absent */
  int ix;
};

static inline i64 bt_cell_key(MemPage *pg, int i) {
  u8 *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf)
    while (0x80 <= *(c++))
      if (c >= pg->aDataEnd) return INT64_MAX; /* corrupt: steers the search; the caller rechecks */
  return sqlite3VarintValue(c);
}
/* the first cell whose key is >= k (nCell if none) */
static int bt_lower_bound(MemPage *pg, i64 k) {
  int n = pg->nCell;
  i64 k0 = bt_cell_key(pg, 0), kn = bt_cell_key(pg, n - 1);
  if (k <= k0) return 0;
  if (k > kn) return n;
  int lo = 1, hi = n - 1; /* the answer is in [lo, hi]: key(lo-1) < k <= key(hi) */
  int g = lo + (int)((double)(k - k0) / (double)(kn - k0) * (double)(n - 1) - 0.5);
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
/* 0 with every hit set (pg NULL for an absent key), -1 on an error (no pages held) */
static int bt_seek_many(struct bt *b, Pgno root, const i64 *key, int n, struct bt_hit *hit) {
  BtShared *bs = b->bt->pBt;
  MemPage *pg[FEED_N];
  Pgno next[FEED_N];
  if (n < 1 || n > FEED_N) return -1;
  for (int i = 0; i < n; i++) next[i] = root;
  for (int depth = 0;; depth++) {
    /* fetch this level's pages, once each, and start loading their cell pointers */
    for (int i = 0; i < n; i++) {
      int j = i - 1;
      while (j >= 0 && next[j] != next[i]) j--;
      if (j >= 0) {
        pg[i] = pg[j];
        sqlite3PagerRef(pg[i]->pDbPage);
      } else if (getAndInitPage(bs, next[i], &pg[i], PAGER_GET_READONLY) != SQLITE_OK) {
        pg[i] = NULL;
      } else {
        __builtin_prefetch(pg[i]->aCellIdx);
        __builtin_prefetch(pg[i]->aCellIdx + 2 * (pg[i]->nCell - 1));
      }
      /* what moveToRoot and moveToChild check before they search a page */
      if (!pg[i] || !pg[i]->intKey || pg[i]->nCell == 0 || pg[i]->nOverflow || depth >= BTCURSOR_MAX_DEPTH ||
          (i > 0 && pg[i]->leaf != pg[0]->leaf)) {
        for (int k = 0; k <= i; k++)
          if (pg[k]) releasePageNotNull(pg[k]);
        return -1;
      }
    }
    if (pg[0]->leaf) break;
    for (int i = 0; i < n; i++) {
      int lw = bt_lower_bound(pg[i], key[i]); /* key <= cell key: that cell's left child */
      next[i] = lw >= pg[i]->nCell ? get4byte(&pg[i]->aData[pg[i]->hdrOffset + 8]) : get4byte(findCell(pg[i], lw));
    }
    for (int i = 0; i < n; i++) releasePageNotNull(pg[i]);
  }
  for (int i = 0; i < n; i++) {
    int ix = bt_lower_bound(pg[i], key[i]);
    if (ix < pg[i]->nCell && bt_cell_key(pg[i], ix) == key[i]) {
      hit[i].pg = pg[i];
      hit[i].ix = ix;
    } else {
      releasePageNotNull(pg[i]);
      hit[i].pg = NULL;
    }
  }
  return 0;
}
/* the record of a hit's cell, when it fits on its page (users rows always do) */
static int bt_hit_record(struct bt_hit *h, struct rec *r) {
  CellInfo info;
  h->pg->xParseCell(h->pg, findCell(h->pg, h->ix), &info);
  if (info.nLocal != info.nPayload) return -1;
  r->p = info.pPayload;
  r->n = info.nPayload;
  return 0;
}
/* usernames for n user ids: 0 with every name found (the hits hold the pages the names point
 * into: release them after use), 1 if one is absent, -1 if the caller must use the cursors */
static int bt_user_names(struct bt *b, const i64 *uid, int n, struct bt_hit *hit, const u8 **name, u32 *len) {
  if (bt_seek_many(b, b->root_users, uid, n, hit)) return -1;
  for (int i = 0; i < n; i++) {
    struct rec r;
    struct col c[2];
    int rc = !hit[i].pg ? 1 : bt_hit_record(&hit[i], &r) || rec_cols(&r, 2, c) || !col_is_text(&c[1]) ? -1 : 0;
    if (rc) {
      bt_release_hits(hit, n);
      return rc;
    }
    name[i] = c[1].p;
    len[i] = col_len(&c[1]);
  }
  return 0;
}

/* One post row by rowid, written as a post object. 0 ok, 1 absent, -1 error. */
static int bt_post_obj(struct bt *b, struct ob *o, i64 id, i64 likes) {
  int rc = cur_seek(&b->posts, id);
  if (rc) return rc;
  struct rec r;
  struct col c[4];
  if (bt_record(b, &b->posts, &r) || rec_cols(&r, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) || !col_is_text(&c[3]))
    return -1;
  /* the record is a pointer into a page; the users seek below may move pages, so copy first */
  u32 bl = col_len(&c[2]), cl = col_len(&c[3]);
  u8 body[2048], created[64];
  if (bl > sizeof body || cl > sizeof created) return -1;
  memcpy(body, c[2].p, bl);
  memcpy(created, c[3].p, cl);
  const u8 *name;
  u32 nl;
  i64 uid = col_int(&c[1]);
  struct bt_hit h;
  rc = bt_user_names(b, &uid, 1, &h, &name, &nl);
  if (rc > 0) return -1;
  if (rc < 0) {
    if (bt_user_name(b, uid, &name, &nl)) return -1;
    h.pg = NULL;
  }
  ob_post_obj(o, id, body, (int)bl, created, (int)cl, name, (int)nl, likes);
  bt_release_hits(&h, 1);
  return 0;
}

/* First row of a table whose rowid is >= key: 0 on a row, 1 past the end, -1 on error.
 * The B-tree equivalent of the bytecode engine's OP_SeekGE on a rowid. */
static int table_seek_ge(struct bt_cursor *c, i64 key) {
  int res;
  if (sqlite3BtreeTableMoveto(c->c, key, 0, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c->c)) return 1;
  if (res < 0) return cur_next(c); /* landed on the row just below key */
  return 0;
}

/* GET /feed: the 20 newest ids off posts_created_at_id_idx (a DESC,DESC index, so the tree's
 * first entries). When they lie within FEED_SPAN ids of each other (they do: ids and timestamps
 * both grow), the rows are read the way the bytecode engine runs `WHERE id BETWEEN lo AND hi`:
 * one range scan of likes_post_id_idx for the counts, one range scan of posts (a single seek,
 * then sqlite3BtreeNext) for the rows, keeping the ones in the feed. Then each author by rowid. */
static int bt_feed(struct bt *b, struct ob *o) {
  i64 ids[FEED_N];
  int n = 0;
  int rc = cur_first(&b->posts_idx);
  while (rc == 0 && n < FEED_N) {
    struct rec r;
    struct col c[2];
    if (bt_record(b, &b->posts_idx, &r) || rec_cols(&r, 2, c) || !col_is_text(&c[0]) || !col_is_int(&c[1])) return -1;
    ids[n++] = col_int(&c[1]);
    rc = cur_next(&b->posts_idx);
  }
  if (rc < 0) return -1;
  if (n == 0) { ob_lit(o, "{\"posts\":[]}"); return 0; }
  i64 lo = ids[0], hi = ids[0];
  for (int i = 1; i < n; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }
  if (hi - lo >= FEED_SPAN) return -1;
  i64 counts[FEED_SPAN];
  if (bt_range_counts(b, lo, hi, counts)) return -1;

  /* the rows: one range scan over [lo, hi]. Each kept row is copied out of its page, because the
   * cursor moves on (and the author seeks below use another cursor). */
  int8_t slot[FEED_SPAN];
  memset(slot, -1, sizeof slot);
  for (int i = 0; i < n; i++) slot[ids[i] - lo] = (int8_t)i;
  static u8 bodies[FEED_N][2048], created[FEED_N][64];
  u32 body_len[FEED_N], created_len[FEED_N];
  i64 uids[FEED_N];
  int found = 0;
  rc = table_seek_ge(&b->posts, lo);
  while (rc == 0) {
    i64 id = sqlite3BtreeIntegerKey(b->posts.c);
    if (id > hi) break;
    int i = slot[id - lo];
    if (i >= 0) {
      struct rec r;
      struct col c[4];
      if (bt_record(b, &b->posts, &r) || rec_cols(&r, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) || !col_is_text(&c[3]))
        return -1;
      body_len[i] = col_len(&c[2]);
      created_len[i] = col_len(&c[3]);
      if (body_len[i] > sizeof bodies[i] || created_len[i] > sizeof created[i]) return -1;
      memcpy(bodies[i], c[2].p, body_len[i]);
      memcpy(created[i], c[3].p, created_len[i]);
      uids[i] = col_int(&c[1]);
      if (++found == n) break;
    }
    rc = cur_next(&b->posts);
  }
  if (rc < 0 || found != n) return -1; /* a row the index named is missing: the SQL path decides */

  /* the authors, all together, and the JSON in feed order */
  struct bt_hit hits[FEED_N];
  const u8 *names[FEED_N];
  u32 name_len[FEED_N];
  rc = bt_user_names(b, uids, n, hits, names, name_len);
  if (rc > 0) return -1; /* an author is missing: the SQL path decides */
  ob_lit(o, "{\"posts\":[");
  for (int i = 0; i < n; i++) {
    if (rc < 0) { /* the batched seek declined: one cursor seek each */
      hits[i].pg = NULL;
      if (bt_user_name(b, uids[i], &names[i], &name_len[i])) return -1;
    }
    if (i) ob_char(o, ',');
    ob_post_obj(o, ids[i], bodies[i], (int)body_len[i], created[i], (int)created_len[i], names[i], (int)name_len[i],
                counts[ids[i] - lo]);
  }
  ob_lit(o, "]}");
  if (rc == 0) bt_release_hits(hits, n);
  return 0;
}

/* GET /posts/:id. 1 ok, 0 absent, -1 error (the caller's convention). */
static int bt_post(struct bt *b, struct ob *o, i64 id) {
  i64 likes;
  if (bt_range_counts(b, id, id, &likes)) return -1;
  ob_lit(o, "{\"post\":");
  int rc = bt_post_obj(b, o, id, likes);
  if (rc < 0) return -1;
  if (rc == 1) return 0;
  ob_char(o, '}');
  return 1;
}

static int bt_post_exists(struct bt *b, i64 id) { return cur_seek(&b->posts, id); }

#endif
