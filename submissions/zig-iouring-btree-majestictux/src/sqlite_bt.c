// The SQLite amalgamation (unmodified) plus read helpers that use SQLite's own B-tree cursor API for
// GET /feed and GET /posts/:id. Writes and every fallback stay on plain SQL. Any surprise returns -1
// and the caller runs the SQL path instead.
//
// Adapted from submissions/c-iouring-shivanshthapliyal/src/sqlite_bt.c (MIT, this repo). Changes:
//   * the custom VFS / page-cache wrappers are dropped (the server keeps the stock unix VFS)
//   * bt_feed follows the #1 entry's plan (rust-iouring-kensac's shim): collect the 20 ids from the
//     feed index, look posts and authors up in ASCENDING id order so SQLite's TableMoveto fast
//     path keeps each cursor on its leaf instead of descending root-to-leaf per row
//   * like counting inside a narrow id range decodes index leaf cells directly, a leaf at a time
//     (also the #1 entry's technique), with the plain cursor walk kept as a fallback
#include "sqlite3.c"

#include "sqlite_bt.h"

static Btree *bt;
static Pgno root_posts, root_users, root_feed, root_likes;
static KeyInfo *ki_feed, *ki_likes;
static UnpackedRecord *key_likes;
static BtCursor *c_posts, *c_users, *c_feed, *c_likes;
static int open_now;

int bt_init(sqlite3 *db) {
  Table *tp = sqlite3FindTable(db, "posts", "main"), *tu = sqlite3FindTable(db, "users", "main");
  Index *ixf = sqlite3FindIndex(db, "posts_created_at_id_idx", "main"), *ixl = sqlite3FindIndex(db, "likes_post_id_idx", "main");
  if (!tp || !tu || !ixf || !ixl) return -1;
  // The record decoders below assume this exact column order (the challenge schema is fixed).
  if (tp->nCol != 4 || tu->nCol != 3 || ixf->nColumn != 3 || ixl->nColumn != 2) return -1;
  root_posts = tp->tnum, root_users = tu->tnum, root_feed = ixf->tnum, root_likes = ixl->tnum;
  bt = db->aDb[0].pBt;
  ki_feed = sqlite3KeyInfoAlloc(db, 2, 1);
  ki_likes = sqlite3KeyInfoAlloc(db, 1, 1);
  if (!ki_feed || !ki_likes) return -1;
  ki_feed->aSortFlags[0] = ki_feed->aSortFlags[1] = KEYINFO_ORDER_DESC;
  key_likes = sqlite3VdbeAllocUnpackedRecord(ki_likes);
  if (!key_likes) return -1;
  // The allocator leaves aMem uninitialised; MemSetInt64 reads the flags, so they must start as MEM_Null.
  memset(key_likes->aMem, 0, sizeof(Mem) * key_likes->nField);
  for (int i = 0; i < key_likes->nField; i++) key_likes->aMem[i].flags = MEM_Null, key_likes->aMem[i].db = db;
  int sz = sqlite3BtreeCursorSize();
  c_posts = sqlite3MallocZero(sz), c_users = sqlite3MallocZero(sz), c_feed = sqlite3MallocZero(sz), c_likes = sqlite3MallocZero(sz);
  return key_likes && c_posts && c_users && c_feed && c_likes ? 0 : -1;
}

// Inside the caller's BEGIN ... COMMIT: opens the read transaction and the four cursors.
// COMMIT ends the read transaction (vdbeCommit commits read-only btree transactions too).
int bt_begin(void) {
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return -1;
  BtCursor *cs[4] = {c_posts, c_users, c_feed, c_likes};
  Pgno rs[4] = {root_posts, root_users, root_feed, root_likes};
  KeyInfo *ks[4] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(cs[i]);
    if (sqlite3BtreeCursor(bt, rs[i], 0, ks[i], cs[i]) != SQLITE_OK) {
      while (i--) sqlite3BtreeCloseCursor(cs[i]);
      return -1;
    }
  }
  open_now = 1;
  return 0;
}

void bt_end(void) {
  if (!open_now) return;
  sqlite3BtreeCloseCursor(c_posts), sqlite3BtreeCloseCursor(c_users);
  sqlite3BtreeCloseCursor(c_feed), sqlite3BtreeCloseCursor(c_likes);
  open_now = 0;
}

// ---------------------------------------------------------------- record format

typedef struct { u64 type; const u8 *p; u32 n; } col_t;

static u64 serial_len(u64 t) {
  static const u8 fixed[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? fixed[t] : (t - 12) / 2;
}

// Splits a record into its first `max` columns; returns the count or -1 if malformed.
static int rec_split(const u8 *p, u32 sz, col_t *c, int max) {
  if (sz == 0) return -1;
  u64 hdr;
  u32 h = sqlite3GetVarint(p, &hdr);
  if (hdr < h || hdr > sz) return -1;
  const u8 *hp = p + h, *he = p + hdr, *dp = p + hdr, *end = p + sz;
  int k = 0;
  while (hp < he && k < max) {
    if (he - hp < 9 && (hp[he - hp - 1] & 0x80)) return -1;  // a varint must end inside the header
    u64 t;
    hp += sqlite3GetVarint(hp, &t);
    if (hp > he) return -1;
    u64 len = serial_len(t);
    if (t == 10 || t == 11 || len > (u64)(end - dp)) return -1;
    c[k].type = t, c[k].p = dp, c[k].n = (u32)len;
    dp += len, k++;
  }
  return k;
}

static int col_int(const col_t *c, i64 *v) {
  if (c->type == 8) { *v = 0; return 0; }
  if (c->type == 9) { *v = 1; return 0; }
  if (c->type < 1 || c->type > 6) return -1;
  u64 x = 0;
  for (u32 i = 0; i < c->n; i++) x = (x << 8) | c->p[i];
  if (c->n < 8 && (c->p[0] & 0x80)) x |= ~(u64)0 << (8 * c->n);  // sign-extend
  *v = (i64)x;
  return 0;
}

static int col_text(const col_t *c) { return c->type >= 13 && (c->type & 1); }

// The current cell's full payload, local to the page. Returns NULL for overflow payloads (caller falls back).
static const u8 *cell(BtCursor *cur, u32 *sz) {
  u32 amt;
  *sz = sqlite3BtreePayloadSize(cur);  // first: it parses the cell info that PayloadFetch reads
  const u8 *p = sqlite3BtreePayloadFetch(cur, &amt);
  return amt >= *sz ? p : NULL;
}

// ---------------------------------------------------------------- row loads

static char *put(char **w, char *end, const u8 *s, u32 n) {
  if (*w + n > end) return NULL;
  char *d = *w;
  memcpy(d, s, n);
  *w += n;
  return d;
}

// Loads post row `id` (body, created_at, user_id). 1 found, 0 no such post, -1 fall back to SQL.
static int load_row(i64 id, bt_post *o, i64 *uid, char **w, char *end) {
  int res;
  u32 sz;
  if (sqlite3BtreeTableMoveto(c_posts, id, 0, &res) != SQLITE_OK) return -1;
  if (res != 0 || sqlite3BtreeEof(c_posts)) return 0;
  const u8 *p = cell(c_posts, &sz);
  col_t c[4];
  // posts(id INTEGER PRIMARY KEY, user_id, body, created_at): id is the rowid, stored as NULL.
  if (!p || rec_split(p, sz, c, 4) != 4 || !col_text(&c[2]) || !col_text(&c[3])) return -1;
  if (col_int(&c[1], uid)) return -1;
  o->id = id;
  if (!(o->body = put(w, end, c[2].p, c[2].n)) || !(o->ca = put(w, end, c[3].p, c[3].n))) return -1;
  o->bl = (int)c[2].n, o->cl = (int)c[3].n;
  return 1;
}

// Loads username for user `uid` into the post. 1 found, 0 no such user, -1 fall back.
static int load_user(i64 uid, bt_post *o, char **w, char *end) {
  int res;
  u32 sz;
  if (sqlite3BtreeTableMoveto(c_users, uid, 0, &res) != SQLITE_OK) return -1;
  if (res != 0 || sqlite3BtreeEof(c_users)) return 0;
  const u8 *q = cell(c_users, &sz);
  col_t u[3];
  // users(id INTEGER PRIMARY KEY, username, created_at): username is column 1.
  if (!q || rec_split(q, sz, u, 3) < 2 || !col_text(&u[1])) return -1;
  if (!(o->au = put(w, end, u[1].p, u[1].n))) return -1;
  o->al = (int)u[1].n;
  return 1;
}

// ---------------------------------------------------------------- likes

// Positions the likes index at the first entry with post_id >= v. 1 if positioned, 0 at end, -1 error.
static int likes_seek(i64 v) {
  sqlite3VdbeMemSetInt64(&key_likes->aMem[0], v);
  key_likes->nField = 1;
  key_likes->default_rc = 1;
  key_likes->eqSeen = 0;
  key_likes->errCode = 0;
  int res;
  if (sqlite3BtreeIndexMoveto(c_likes, key_likes, &res) != SQLITE_OK || key_likes->errCode) return -1;
  if (sqlite3BtreeEof(c_likes)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c_likes, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

static int likes_post_id(i64 *v) {
  u32 sz;
  const u8 *p = cell(c_likes, &sz);
  col_t c[1];
  return p && rec_split(p, sz, c, 1) == 1 ? col_int(&c[0], v) : -1;
}

// Counts likes for the posts in o[0..n) whose ids lie in [lo, hi] (hi - lo < 512), decoding the
// index's leaf cells directly — the record's first column is all a cell needs to give up.
// Returns 0 ok, 1 a cell not in the expected form (caller decodes entries itself), -1 error.
// (Leaf-cell walk adapted from rust-iouring-kensac's td_count_likes, MIT, this repo.)
static int count_range_fast(bt_post *o, int n, i64 lo, i64 hi, const short *slot) {
  int r = likes_seek(lo);
  if (r <= 0) return r;
  BtCursor *c = c_likes;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1;  // an interior entry: just this one
    for (; i < last; i++) {
      u8 *cl = findCell(pg, i) + pg->childPtrSize;
      u32 len = cl[0];
      const u8 *p = cl + 1;
      // One-byte sizes only, so the record is all on this page (maxLocal is far larger).
      if (len >= 0x80 || len < 3 || p + len > pg->aDataEnd) return 1;
      u32 h = p[0], t = p[1];
      if (h >= len || h < 3 || t >= 0x80) return 1;
      i64 v;
      if (t >= 1 && t <= 6) {
        static const u8 sz[7] = {0, 1, 2, 3, 4, 6, 8};
        u32 k = sz[t];
        if (h + k > len) return 1;
        const u8 *q = p + h;
        v = (i8)q[0];
        for (u32 j = 1; j < k; j++) v = v << 8 | q[j];
      } else if (t == 8 || t == 9) {
        v = t - 8;
      } else {
        return 1;
      }
      if (v > hi) return 0;
      if (v >= lo) {
        short s = slot[v - lo];
        if (s >= 0) o[s].likes++;
      }
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

// The same count, one cursor step per index entry. Fallback for unusual cells.
static int count_range_slow(bt_post *o, int n, i64 lo, i64 hi, const short *slot) {
  (void)n;
  int s = likes_seek(lo);
  if (s <= 0) return s;
  for (;;) {
    i64 pid;
    if (likes_post_id(&pid)) return -1;
    if (pid > hi) return 0;
    short k = slot[pid - lo];
    if (k >= 0) o[k].likes++;
    int rc = sqlite3BtreeNext(c_likes, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

// Adds the like counts for every post in o[0..n) with ids within [lo, hi] (hi - lo < 512),
// scanning the index range once.
static int count_range(bt_post *o, int n, i64 lo, i64 hi) {
  for (int k = 0; k < n; k++) o[k].likes = 0;
  static short slot[512];
  for (i64 v = lo; v <= hi; v++) slot[v - lo] = -1;
  for (int k = 0; k < n; k++) slot[o[k].id - lo] = (short)k;
  int r = count_range_fast(o, n, lo, hi, slot);
  if (r == 1) {  // unexpected cell shape: zero and recount the plain way
    for (int k = 0; k < n; k++) o[k].likes = 0;
    r = count_range_slow(o, n, lo, hi, slot);
  }
  return r;
}

static int count_one(bt_post *o) {
  o->likes = 0;
  int s = likes_seek(o->id);
  if (s <= 0) return s;
  for (;;) {
    i64 pid;
    if (likes_post_id(&pid)) return -1;
    if (pid != o->id) return 0;
    o->likes++;
    int rc = sqlite3BtreeNext(c_likes, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

// ---------------------------------------------------------------- entry points

int bt_feed(bt_post *o, int *n_out, char *buf, size_t cap) {
  char *w = buf, *end = buf + cap;
  int res;
  *n_out = 0;
  if (sqlite3BtreeFirst(c_feed, &res) != SQLITE_OK) return -1;
  if (res) return 0;
  // Pass 1: the index is (created_at DESC, id DESC), so its b-tree order is the feed's order.
  // Collect the 20 newest ids. (created_at is re-read from the post row in pass 2.)
  i64 ids[20];
  int n = 0;
  while (n < 20) {
    u32 sz;
    const u8 *p = cell(c_feed, &sz);
    col_t c[3];
    int nc = p ? rec_split(p, sz, c, 3) : -1;
    // The entry ends with the rowid (posts.id), however many columns precede it.
    if (nc < 2 || col_int(&c[nc - 1], &ids[n])) return -1;
    n++;
    int rc = sqlite3BtreeNext(c_feed, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) return -1;
  }
  // Pass 2: post rows in ASCENDING id order — consecutive ids land on the next cell of the
  // same leaf (SQLite's TableMoveto fast path), so there is no root-to-leaf descent per row.
  int ord[20];
  for (int i = 0; i < n; i++) ord[i] = i;
  for (int i = 1; i < n; i++) {  // insertion sort, n <= 20
    int t = ord[i], j = i - 1;
    while (j >= 0 && ids[ord[j]] > ids[t]) { ord[j + 1] = ord[j]; j--; }
    ord[j + 1] = t;
  }
  i64 uids[20];
  for (int i = 0; i < n; i++) {
    int k = ord[i];
    int r = load_row(ids[k], &o[k], &uids[k], &w, end);
    if (r <= 0) return -1;  // missing post/author: the SQL JOIN plan sorts it out (SQL fallback)
  }
  // Pass 3: authors in ascending user-id order, same leaf-locality reason.
  for (int i = 1; i < n; i++) {
    int t = ord[i], j = i - 1;
    while (j >= 0 && uids[ord[j]] > uids[t]) { ord[j + 1] = ord[j]; j--; }
    ord[j + 1] = t;
  }
  for (int i = 0; i < n; i++) {
    int k = ord[i];
    if (load_user(uids[k], &o[k], &w, end) <= 0) return -1;
  }
  *n_out = n;
  if (!n) return 0;
  i64 lo = ids[0], hi = ids[0];
  for (int k = 1; k < n; k++) lo = ids[k] < lo ? ids[k] : lo, hi = ids[k] > hi ? ids[k] : hi;
  if (hi - lo < 512) return count_range(o, n, lo, hi);
  for (int k = 0; k < n; k++)
    if (count_one(&o[k])) return -1;
  return 0;
}

int bt_post_get(long long id, bt_post *o, char *buf, size_t cap) {
  char *w = buf;
  i64 uid;
  int k = load_row(id, o, &uid, &w, buf + cap);
  if (k <= 0) return k;
  if (load_user(uid, o, &w, buf + cap) <= 0) return -1;  // no author row: SQL decides (JOIN drops it -> 404)
  return count_one(o) ? -1 : 1;
}
