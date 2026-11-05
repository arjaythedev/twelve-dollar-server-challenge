/* SQLite amalgamation plus td_feed_read: the feed's read path straight on the B-tree layer.
** It reads the same tables as the SQL it replaces, in the caller's read transaction, and keeps
** no state between calls except root page numbers and key descriptions (schema facts). */
#include "build/sqlite3.c"
#include "td_feed.h"

static struct {
  sqlite3 *db;
  int posts_root, users_root, top_root, likes_root;
  KeyInfo *likes_ki, *top_ki;
  UnpackedRecord *likes_key;
  void *cur[4];
} F;

static int root_of(sqlite3 *db, const char *name) {
  sqlite3_stmt *st;
  int root = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?", -1, &st, 0)) return 0;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW) root = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return root;
}

int td_feed_init(sqlite3 *db) {
  memset(&F, 0, sizeof F);
  F.db = db;
  F.posts_root = root_of(db, "posts");
  F.users_root = root_of(db, "users");
  F.top_root = root_of(db, "posts_created_at_id_idx");
  F.likes_root = root_of(db, "likes_post_id_idx");
  if (!F.posts_root || !F.users_root || !F.top_root || !F.likes_root) return 0;
  F.top_ki = sqlite3KeyInfoAlloc(db, 2, 0);
  F.likes_ki = sqlite3KeyInfoAlloc(db, 2, 0);
  if (!F.top_ki || !F.likes_ki) return 0;
  F.likes_key = sqlite3VdbeAllocUnpackedRecord(F.likes_ki);
  if (!F.likes_key) return 0;
  for (int i = 0; i < 4; i++) {
    F.cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!F.cur[i]) return 0;
  }
  return 1;
}

/* Decoded record: pointers into the cell payload (valid until the cursor moves). */
typedef struct { const unsigned char *col[4]; i64 val[4]; int len[4]; } Rec;

/* Decode up to n columns of a record held entirely in the page. Returns 0 on overflow/corruption. */
static int rec_parse(BtCursor *c, int n, Rec *r) {
  u32 amt, size = sqlite3BtreePayloadSize(c);   /* size first: it parses the cell for Fetch */
  const unsigned char *p = sqlite3BtreePayloadFetch(c, &amt);
  if (amt != size || amt < 2) return 0;
  u32 hdr; int h = getVarint32(p, hdr);
  if (hdr > amt) return 0;
  const unsigned char *data = p + hdr;
  const unsigned char *end = p + amt;
  for (int i = 0; i < n; i++) {
    u32 t;
    if (h >= (int)hdr) return 0;
    h += getVarint32(p + h, t);
    int len; i64 v = 0;
    if (t < 12) {
      static const unsigned char sz[] = {0, 1, 2, 3, 4, 6, 8, 0, 0, 0, 0, 0};
      len = sz[t];
      if (t == 8) v = 0; else if (t == 9) v = 1;
      else if (t >= 10) return 0;
      else if (len) {
        if (data + len > end) return 0;
        v = (signed char)data[0];
        for (int k = 1; k < len; k++) v = (v << 8) | data[k];
      }
    } else {
      len = (t - 12) / 2;
    }
    if (data + len > end) return 0;
    r->col[i] = data; r->val[i] = v; r->len[i] = len;
    data += len;
  }
  return 1;
}

/* row_out.h writes JSON; the amalgamation's own build flags already target the host CPU. */
#include "row_out.h"

/* Counts likes per post_id in [lo, hi] into cnt[post_id - lo]. Returns 0 on success. */
static int likes_range(Btree *bt, i64 lo, i64 hi, i64 *cnt) {
  BtCursor *c = F.cur[3];
  UnpackedRecord *key = F.likes_key;
  int res, rc, bad = 0;
  sqlite3BtreeCursorZero(c);
  if (sqlite3BtreeCursor(bt, F.likes_root, 0, F.likes_ki, c)) return 1;
  sqlite3VdbeMemInit(&key->aMem[0], F.db, MEM_Null);
  sqlite3VdbeMemSetInt64(&key->aMem[0], lo);
  key->nField = 1; key->default_rc = 1; key->errCode = 0; key->r1 = 0; key->r2 = 0; key->eqSeen = 0;
  if (sqlite3BtreeIndexMoveto(c, key, &res)) bad = 1;
  else if (res < 0 && (rc = sqlite3BtreeNext(c, 0)) && rc != SQLITE_DONE) bad = 1;
  while (!bad && !sqlite3BtreeEof(c)) {
    Rec r;
    if (!rec_parse(c, 1, &r)) { bad = 1; break; }
    if (r.val[0] > hi) break;
    if (r.val[0] >= lo) cnt[r.val[0] - lo]++;
    rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) break;
    if (rc) bad = 1;
  }
  sqlite3BtreeCloseCursor(c);
  return bad;
}

/* Key of cell idx on a table b-tree page (the rowid of a leaf cell, the divider of an interior one). */
static inline i64 cell_key(MemPage *pg, int idx) {
  u8 *c = findCellPastPtr(pg, idx);
  if (pg->intKeyLeaf) while (0x80 <= *(c++)) {}
  return sqlite3VarintValue(c);
}

/* sqlite3BtreeTableMoveto with a different probe order. Ids grow densely, so the first probe on
** each page interpolates between its first and last key and, when it misses, the search gallops
** towards the key before it falls back to bisection. Result and cursor state are the same. */
static int table_moveto(BtCursor *cur, i64 key, int *res) {
  int rc = moveToRoot(cur);
  if (rc) { if (rc == SQLITE_EMPTY) { *res = -1; return SQLITE_OK; } return rc; }
  for (;;) {
    MemPage *pg = cur->pPage;
    int lo = 0, hi = pg->nCell - 1, idx, step = 1, c;
    i64 first = cell_key(pg, 0), last = cell_key(pg, hi);
    if (key <= first) idx = 0;
    else if (key >= last) idx = hi;
    else idx = (int)(((unsigned long long)(key - first) * (unsigned)hi) / (unsigned long long)(last - first));
    for (int moved_lo = 0, moved_hi = 0;;) {
      i64 k = cell_key(pg, idx);
      if (k < key) {
        lo = idx + 1; moved_lo = 1;
        if (lo > hi) { c = -1; break; }
        if (moved_hi) idx = (lo + hi) >> 1;
        else { idx = lo + step - 1; step <<= 1; if (idx > hi) idx = hi; }
      } else if (k > key) {
        hi = idx - 1; moved_hi = 1;
        if (lo > hi) { c = 1; break; }
        if (moved_lo) idx = (lo + hi) >> 1;
        else { idx = hi - step + 1; step <<= 1; if (idx < lo) idx = lo; }
      } else {
        cur->ix = (u16)idx;
        if (pg->leaf) {
          cur->curFlags |= BTCF_ValidNKey;
          cur->info.nKey = k;
          cur->info.nSize = 0;
          *res = 0;
          return SQLITE_OK;
        }
        lo = idx;
        goto child;
      }
    }
    if (pg->leaf) {
      cur->ix = (u16)idx;
      cur->info.nSize = 0;
      *res = c;
      return SQLITE_OK;
    }
child:
    {
      Pgno child = lo >= pg->nCell ? get4byte(&pg->aData[pg->hdrOffset + 8]) : get4byte(findCell(pg, lo));
      cur->ix = (u16)lo;
      if ((rc = moveToChild(cur, child))) return rc;
    }
  }
}

/* Writes the post under `posts` with its author as one JSON row at d, straight from the pages.
** Returns the end, or NULL when the row does not fit the fast path (overflow pages, odd
** created_at, not enough room before dend). */
static unsigned char *put_row(BtCursor *posts, BtCursor *users, i64 id, i64 likes, unsigned char *d, unsigned char *dend) {
  Rec r, u;
  int res;
  if (!rec_parse(posts, 4, &r) || r.len[3] != 24) return 0;        /* user_id, body, created_at */
  if (table_moveto(users, r.val[1], &res) || res != 0) return 0;
  if (!rec_parse(users, 2, &u)) return 0;                          /* username */
  if ((r.len[2] + u.len[1]) * 6 + 160 > dend - d) return 0;       /* worst case, see td_put_row */
  td_feed_row o = { id, likes, (unsigned char *)r.col[2], (unsigned char *)r.col[3], (unsigned char *)u.col[1],
                    r.len[2], u.len[1] };
  return td_put_row(d, &o);
}

static int open_post_cursors(Btree *bt) {
  BtCursor *posts = F.cur[1], *users = F.cur[2];
  sqlite3BtreeCursorZero(posts);
  if (sqlite3BtreeCursor(bt, F.posts_root, 0, 0, posts)) return 0;
  sqlite3BtreeCursorZero(users);
  if (sqlite3BtreeCursor(bt, F.users_root, 0, 0, users)) { sqlite3BtreeCloseCursor(posts); return 0; }
  return 1;
}

/* Writes the newest 20 posts (created_at DESC, id DESC) with author and like count as
** comma-separated JSON rows at d. Returns the end, or NULL when the caller must use the SQL path. */
unsigned char *td_feed_read(int span, unsigned char *d, unsigned char *dend) {
  Btree *bt = F.db->aDb[0].pBt;
  int n = 0, res, rc;
  i64 ids[20], lo, hi, cnt[256];
  BtCursor *top = F.cur[0], *posts = F.cur[1], *users = F.cur[2];

  if (span > 256 || sqlite3BtreeBeginTrans(bt, 0, 0)) return 0;
  sqlite3BtreeCursorZero(top);
  if (sqlite3BtreeCursor(bt, F.top_root, 0, F.top_ki, top)) return 0;
  rc = sqlite3BtreeFirst(top, &res);
  while (rc == SQLITE_OK && !res && n < 20) {
    Rec r;
    if (!rec_parse(top, 2, &r)) goto top_done;   /* created_at, id */
    ids[n++] = r.val[1];
    rc = sqlite3BtreeNext(top, 0);
  }
  if (rc != SQLITE_OK && rc != SQLITE_DONE) n = 0;
top_done:
  sqlite3BtreeCloseCursor(top);
  if (n == 0) return 0;
  lo = hi = ids[0];
  for (int k = 1; k < n; k++) { if (ids[k] < lo) lo = ids[k]; if (ids[k] > hi) hi = ids[k]; }
  if (hi - lo >= span) return 0;
  memset(cnt, 0, sizeof(i64) * (hi - lo + 1));
  if (likes_range(bt, lo, hi, cnt)) return 0;
  if (!open_post_cursors(bt)) return 0;
  for (int k = 0; k < n && d; k++) {
    if (!(k > 0 && ids[k] == ids[k - 1] - 1 && sqlite3BtreePrevious(posts, 0) == SQLITE_OK
          && sqlite3BtreeIntegerKey(posts) == ids[k])) {
      if (table_moveto(posts, ids[k], &res) || res != 0) { d = 0; break; }
    }
    if (k > 0) *d++ = ',';
    d = put_row(posts, users, ids[k], cnt[ids[k] - lo], d, dend);
  }
  sqlite3BtreeCloseCursor(users);
  sqlite3BtreeCloseCursor(posts);
  return d;
}

/* Writes one post by id as a JSON row at d. Returns the end, or NULL when the SQL path must
** answer (unknown id, or a row the fast path does not cover). */
unsigned char *td_post_read(long long id, unsigned char *d, unsigned char *dend) {
  Btree *bt = F.db->aDb[0].pBt;
  int res;
  i64 cnt[1] = {0};
  if (sqlite3BtreeBeginTrans(bt, 0, 0) || likes_range(bt, id, id, cnt) || !open_post_cursors(bt)) return 0;
  if (table_moveto(F.cur[1], id, &res) || res != 0) d = 0;
  else d = put_row(F.cur[1], F.cur[2], id, cnt[0], d, dend);
  sqlite3BtreeCloseCursor(F.cur[2]);
  sqlite3BtreeCloseCursor(F.cur[1]);
  return d;
}
