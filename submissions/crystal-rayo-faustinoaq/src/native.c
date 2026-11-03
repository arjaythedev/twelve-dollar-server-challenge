/* SQLite with direct B-tree reads for the feed and single-post queries.
 *
 * This file includes the amalgamation, so it can call SQLite's B-tree layer
 * (cursors, seeks, payload fetch) instead of running prepared statements. Reads
 * still go through SQLite's pager, WAL and locks, inside the caller's transaction;
 * only the bytecode interpreter is skipped. Writes stay SQL.
 *
 * The feed shortcut relies on posts.created_at never decreasing in rowid order:
 * then ORDER BY created_at DESC, id DESC is reverse rowid order. The server checks
 * that at startup and after every insert, and calls nf_disable() (SQL from then on)
 * the first time it does not hold. */
#include "sqlite3.c"

typedef struct {
  i64 id, user_id, likes;
  u32 body_off, body_len, at_off, at_len, au_off, au_len;
} nf_row;

static Btree *nf_bt;
static Pgno nf_posts, nf_users, nf_likes;
static KeyInfo *nf_ki;
static int nf_on;
static u8 nf_scratch[8192];

int nf_init(sqlite3 *db) {
  Table *posts, *users;
  Index *likes;
  nf_on = 0;
  if (sqlite3_exec(db, "SELECT 1 FROM posts, users, likes LIMIT 0", 0, 0, 0) != SQLITE_OK) return 0;
  posts = sqlite3FindTable(db, "posts", "main");
  users = sqlite3FindTable(db, "users", "main");
  likes = sqlite3FindIndex(db, "likes_post_id_idx", "main");
  if (!posts || !users || !likes || !HasRowid(posts) || !HasRowid(users)) return 0;
  if (posts->nCol != 4 || users->nCol != 3 || likes->nKeyCol != 1) return 0;
  nf_ki = sqlite3KeyInfoAlloc(db, 1, 1);
  if (!nf_ki) return 0;
  nf_bt = db->aDb[0].pBt;
  nf_posts = posts->tnum;
  nf_users = users->tnum;
  nf_likes = likes->tnum;
  nf_on = 1;
  return 1;
}

void nf_disable(void) { nf_on = 0; }
int nf_enabled(void) { return nf_on; }

/* Read transaction for the caller's open (deferred) transaction; COMMIT ends it. */
static int nf_begin(void) {
  if (sqlite3BtreeTxnState(nf_bt) != SQLITE_TXN_NONE) return SQLITE_OK;
  return sqlite3BtreeBeginTrans(nf_bt, 0, 0);
}

/* The cursor's record, fetched in place, or copied when it spills to overflow pages. */
static const u8 *nf_record(BtCursor *c, u32 *n) {
  u32 size = sqlite3BtreePayloadSize(c), amt = 0;
  const u8 *p = sqlite3BtreePayloadFetch(c, &amt);
  *n = size;
  if (amt >= size) return p;
  if (size > sizeof(nf_scratch) || sqlite3BtreePayload(c, 0, size, nf_scratch) != SQLITE_OK) return 0;
  return nf_scratch;
}

static i64 nf_int(const u8 *p, u32 t) {
  static const u8 width[] = {0, 1, 2, 3, 4, 6, 8};
  i64 v;
  int i;
  if (t == 8) return 0;
  if (t == 9) return 1;
  v = (signed char)p[0];
  for (i = 1; i < width[t]; i++) v = (v << 8) | p[i];
  return v;
}

/* Column positions of a record: serial type and body offset of the first `n` columns. */
static int nf_columns(const u8 *rec, u32 size, int n, u32 *type, u32 *off) {
  u32 hdr, pos, body;
  int i;
  pos = getVarint32(rec, hdr);
  if (hdr > size) return 0;
  body = hdr;
  for (i = 0; i < n; i++) {
    if (pos >= hdr) return 0;
    pos += getVarint32(rec + pos, type[i]);
    off[i] = body;
    body += sqlite3VdbeSerialTypeLen(type[i]);
  }
  return body <= size;
}

static int nf_is_int(u32 t) { return (t >= 1 && t <= 6) || t == 8 || t == 9; }
static int nf_is_text(u32 t) { return t >= 13 && (t & 1); }

static int nf_put(u8 *buf, u32 cap, u32 *used, const u8 *src, u32 len, u32 *off) {
  if (*used + len > cap) return 0;
  memcpy(buf + *used, src, len);
  *off = *used;
  *used += len;
  return 1;
}

/* Fills body and created_at from the posts cursor's row. */
static int nf_post_row(BtCursor *c, nf_row *r, u8 *buf, u32 cap, u32 *used) {
  u32 size, type[4], off[4];
  const u8 *rec = nf_record(c, &size);
  if (!rec || !nf_columns(rec, size, 4, type, off)) return 0;
  if (!nf_is_int(type[1]) || !nf_is_text(type[2]) || !nf_is_text(type[3])) return 0;
  r->id = sqlite3BtreeIntegerKey(c);
  r->user_id = nf_int(rec + off[1], type[1]);
  r->body_len = (type[2] - 13) / 2;
  r->at_len = (type[3] - 13) / 2;
  r->likes = 0;
  return nf_put(buf, cap, used, rec + off[2], r->body_len, &r->body_off) &&
         nf_put(buf, cap, used, rec + off[3], r->at_len, &r->at_off);
}

static int nf_author(BtCursor *c, nf_row *r, u8 *buf, u32 cap, u32 *used) {
  int res = 0;
  u32 size, type[2], off[2];
  const u8 *rec;
  if (sqlite3BtreeTableMoveto(c, r->user_id, 0, &res) != SQLITE_OK || res != 0) return 0;
  rec = nf_record(c, &size);
  if (!rec || !nf_columns(rec, size, 2, type, off) || !nf_is_text(type[1])) return 0;
  r->au_len = (type[1] - 13) / 2;
  return nf_put(buf, cap, used, rec + off[1], r->au_len, &r->au_off);
}

/* Like counts of rows[0..n) from one scan of likes_post_id_idx over [lo, hi]. */
static int nf_count_likes(BtCursor *c, nf_row *rows, int n, i64 lo, i64 hi) {
  UnpackedRecord key;
  Mem m;
  int res = 0, rc, i;
  memset(&key, 0, sizeof(key));
  memset(&m, 0, sizeof(m));
  m.flags = MEM_Int;
  m.u.i = lo;
  key.pKeyInfo = nf_ki;
  key.aMem = &m;
  key.nField = 1;
  key.default_rc = 1; /* lands on the first entry >= lo */
  if (sqlite3BtreeIndexMoveto(c, &key, &res) != SQLITE_OK) return 0;
  if (sqlite3BtreeEof(c)) return 1;
  if (res < 0 && (rc = sqlite3BtreeNext(c, 0)) != SQLITE_OK) return rc == SQLITE_DONE;
  for (;;) {
    u32 size, type[1], off[1];
    const u8 *rec = nf_record(c, &size);
    i64 id;
    if (!rec || !nf_columns(rec, size, 1, type, off) || !nf_is_int(type[0])) return 0;
    id = nf_int(rec + off[0], type[0]);
    if (id > hi) return 1;
    for (i = 0; i < n; i++)
      if (rows[i].id == id) { rows[i].likes++; break; }
    rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 1;
    if (rc != SQLITE_OK) return 0;
  }
}

typedef struct {
  BtCursor *posts, *users, *likes;
  u8 mem[3][512];
} nf_cursors;

static int nf_open(nf_cursors *k) {
  if ((u32)sqlite3BtreeCursorSize() > sizeof(k->mem[0])) return 0;
  k->posts = (BtCursor *)k->mem[0];
  k->users = (BtCursor *)k->mem[1];
  k->likes = (BtCursor *)k->mem[2];
  sqlite3BtreeCursorZero(k->posts);
  sqlite3BtreeCursorZero(k->users);
  sqlite3BtreeCursorZero(k->likes);
  if (sqlite3BtreeCursor(nf_bt, nf_posts, 0, 0, k->posts) != SQLITE_OK) return 0;
  if (sqlite3BtreeCursor(nf_bt, nf_users, 0, 0, k->users) != SQLITE_OK) return 0;
  return sqlite3BtreeCursor(nf_bt, nf_likes, 0, nf_ki, k->likes) == SQLITE_OK;
}

static void nf_close(nf_cursors *k) {
  sqlite3BtreeCloseCursor(k->posts);
  sqlite3BtreeCloseCursor(k->users);
  sqlite3BtreeCloseCursor(k->likes);
}

/* Newest `max` posts with author and like count. Returns the row count, or -1 when
 * the caller must use SQL. Text fields point into buf. */
int nf_feed(nf_row *rows, int max, u8 *buf, u32 cap) {
  nf_cursors k;
  int n = 0, res = 0, ok = 0, i;
  u32 used = 0;
  i64 lo, hi;
  if (!nf_on || nf_begin() != SQLITE_OK) return -1;
  if (!nf_open(&k)) { nf_close(&k); return -1; }
  if (sqlite3BtreeLast(k.posts, &res) != SQLITE_OK) goto done;
  if (res) { ok = 1; goto done; } /* empty table */
  for (;;) {
    int rc;
    if (!nf_post_row(k.posts, &rows[n], buf, cap, &used)) goto done;
    n++;
    if (n == max) break;
    rc = sqlite3BtreePrevious(k.posts, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) goto done;
  }
  for (i = 0; i < n; i++)
    if (!nf_author(k.users, &rows[i], buf, cap, &used)) goto done;
  lo = rows[n - 1].id;
  hi = rows[0].id;
  ok = nf_count_likes(k.likes, rows, n, lo, hi);
done:
  nf_close(&k);
  return ok ? n : -1;
}

/* One post by id: 1 found, 0 not found, -1 use SQL. */
int nf_post(i64 id, nf_row *row, u8 *buf, u32 cap) {
  nf_cursors k;
  int res = 0, ok = 0, found = 0;
  u32 used = 0;
  if (!nf_on || nf_begin() != SQLITE_OK) return -1;
  if (!nf_open(&k)) { nf_close(&k); return -1; }
  if (sqlite3BtreeTableMoveto(k.posts, id, 0, &res) != SQLITE_OK) goto done;
  if (res != 0 || sqlite3BtreeEof(k.posts)) { ok = 1; goto done; }
  if (!nf_post_row(k.posts, row, buf, cap, &used)) goto done;
  if (!nf_author(k.users, row, buf, cap, &used)) goto done;
  ok = nf_count_likes(k.likes, row, 1, id, id);
  found = 1;
done:
  nf_close(&k);
  return ok ? found : -1;
}
