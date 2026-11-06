// The SQLite side: the pinned, unmodified amalgamation plus a small layer over its B-tree API.
//
// Every function here is called from the Go event-loop thread, except ck_main, which runs WAL
// checkpoints on its own connection (so SQLite is built with SQLITE_THREADSAFE=2).
//
// Reads (feed, one post, a post's existence) walk SQLite's own B-tree cursors inside the pass's
// transaction and copy the columns they need into `td_out`, which Go turns into JSON. Nothing
// read here outlives the request that asked for it. Writes go through prepared SQL statements.
#include "sqlite/sqlite3.c"
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>

#include "shim.h"


td_out_t td_out;

static sqlite3 *db;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health, *st_feed, *st_one;
static Pgno root_posts, root_users, root_feed, root_likes;
static KeyInfo *ki_feed, *ki_likes;

// Cursors on posts, users, posts_created_at_id_idx and likes_post_id_idx. Opened when a pass's
// transaction starts and closed before it commits.
enum { C_POSTS, C_USERS, C_FEED, C_LIKES, NCUR };
static BtCursor *cur[NCUR];
static int in_tx;  // a transaction is open for this pass
static int broken; // SQLite ended the pass's transaction on its own: its COMMIT must fail

// Records that spill onto overflow pages are copied here (bodies are far below the limit).
static u8 *spill;
static u32 spill_cap;

static int wal_frames;

static int errf(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

static int wal_hook(void *a, sqlite3 *d, const char *name, int frames) {
  (void)a; (void)d; (void)name;
  wal_frames = frames;
  return SQLITE_OK;
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0)) return 0;
  return s;
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static Pgno rootpage(const char *name) {
  sqlite3_stmt *s;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0)) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

// The record decoders below assume this exact layout; refuse to start on anything else.
static int same(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[256];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *keyinfo(int nkey, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, 1);
  if (!k) return 0;
  for (int i = 0; i <= nkey; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

// ---- checkpoints ------------------------------------------------------------------------------
//
// Automatic checkpoints are off. Past CK_FRAMES frames a low-priority thread runs a PASSIVE
// checkpoint on its own connection while the loop goes on. Under steady writes it can't copy the
// frames written meanwhile, and the log only restarts once fully copied, so afterwards writes are
// held (between passes, at most HOLD_MS) while the thread copies the tail. Past CK_FORCE frames
// the loop checkpoints itself. (Same scheme as rust-iouring-kensac #264; see README.)
#define CK_FRAMES 4096
#define CK_FORCE 65536
#define HOLD_MS 250

static sqlite3 *ckdb;
enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static int ck_state, ck_full;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int held;
static struct timespec held_at;

static void ck_set(int s) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, s, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

static void *ck_main(void *a) {
  (void)a;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (ck_state != CK_BULK && ck_state != CK_TAIL) pthread_cond_wait(&ck_cv, &ck_mu);
    int st = ck_state;
    pthread_mutex_unlock(&ck_mu);
    int nlog = -1, ndone = -2;
    int rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    pthread_mutex_lock(&ck_mu);
    ck_full = rc == SQLITE_OK && nlog == ndone;
    __atomic_store_n(&ck_state, st + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ck_mu);
  }
  return 0;
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

// Between passes (no transaction open).
void td_tick(void) {
  switch (__atomic_load_n(&ck_state, __ATOMIC_ACQUIRE)) {
  case CK_IDLE:
    if (wal_frames >= CK_FORCE) {
      int nlog = 0, ndone = 0;
      if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK &&
          nlog == ndone)
        wal_frames = 0;
    } else if (wal_frames >= CK_FRAMES) {
      ck_set(CK_BULK);
    }
    break;
  case CK_BULK_DONE:
    held = 1;
    clock_gettime(CLOCK_MONOTONIC, &held_at);
    ck_set(CK_TAIL);
    break;
  case CK_TAIL_DONE:
    if (ck_full) wal_frames = 0;
    held = 0;
    ck_set(CK_IDLE);
    break;
  default:
    if (held && ms_since(&held_at) > HOLD_MS) held = 0;
    break;
  }
}

int td_writes_held(void) { return held; }

// ---- open -------------------------------------------------------------------------------------

int td_open(const char *path, char *err, int n) {
  if (sqlite3_initialize()) return errf(err, n, "init");
  // unix-excl: one process owns the file; the WAL index lives in heap memory shared by this
  // process's two connections (no -shm file, no file locks per transaction).
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl"))
    return errf(err, n, "open");
  if (sqlite3_exec(db,
                   "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;"
                   "PRAGMA cache_size = -98304; PRAGMA mmap_size = 0; PRAGMA wal_autocheckpoint = 0;"
                   "SELECT count(*) FROM sqlite_schema",
                   0, 0, 0))
    return errf(err, n, "pragmas");
  sqlite3_wal_hook(db, wal_hook, 0);

  static const char *checks[][2] = {
      {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
      {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
      {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
      {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
      {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
      {"SELECT pk FROM pragma_table_info('posts') WHERE name = 'id'", "1"},
      {"SELECT pk FROM pragma_table_info('users') WHERE name = 'id'", "1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++)
    if (!same(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  if (ENC(db) != SQLITE_UTF8) {
    snprintf(err, n, "database text encoding is not UTF-8");
    return -1;
  }
  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_feed = rootpage("posts_created_at_id_idx");
  root_likes = rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed || !root_likes) return errf(err, n, "root pages");
  ki_feed = keyinfo(2, 1);
  ki_likes = keyinfo(1, 0);
  if (!ki_feed || !ki_likes) return errf(err, n, "keyinfo");

  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_health = prep("SELECT 1");
  // The reference queries: answer when the data isn't in the form the B-tree plan expects.
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                 "FROM posts p JOIN users u ON u.id = p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  st_one = prep("SELECT p.id, p.body, p.created_at, u.username, "
                "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1");
  if (!st_begin || !st_commit || !st_rollback || !st_post || !st_like || !st_health || !st_feed || !st_one)
    return errf(err, n, "prepare");
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++)
    if (!(cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize()))) return errf(err, n, "cursor");

  if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") ||
      sqlite3_exec(ckdb, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16", 0, 0, 0)) {
    snprintf(err, n, "checkpoint connection: %s", ckdb ? sqlite3_errmsg(ckdb) : "cannot open");
    return -1;
  }
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  if (pthread_create(&t, &a, ck_main, 0)) {
    snprintf(err, n, "checkpoint thread");
    return -1;
  }
  pthread_attr_destroy(&a);
  return 0;
}

// ---- transactions -----------------------------------------------------------------------------

static void close_cursors(void) {
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
}

// Joins the pass's transaction, starting it on first use. 0 ok, -1 the pass is lost.
static int tx(void) {
  if (in_tx) {
    if (!sqlite3_get_autocommit(db)) return 0;
    // SQLite ended the transaction itself (an I/O error, SQLITE_FULL): the pass is lost.
    close_cursors();
    in_tx = 0;
    broken = 1;
    return -1;
  }
  if (broken) return -1;
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) {
    step_reset(st_rollback);
    return -1;
  }
  static const Pgno *roots[NCUR] = {&root_posts, &root_users, &root_feed, &root_likes};
  KeyInfo *kis[NCUR] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, *roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      step_reset(st_rollback);
      return -1;
    }
  }
  in_tx = 1;
  return 0;
}

// 0 committed (or nothing to commit), -1 the pass's writes are lost.
int td_commit(void) {
  if (broken) {
    broken = 0;
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  if (!in_tx) return 0;
  in_tx = 0;
  close_cursors();
  if (step_reset(st_commit) != SQLITE_DONE) {
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  return 0;
}

// A write failed: if SQLite ended the transaction itself, the pass is lost.
static void check_tx(void) {
  if (in_tx && sqlite3_get_autocommit(db)) {
    close_cursors();
    in_tx = 0;
    broken = 1;
  }
}

// ---- records ----------------------------------------------------------------------------------

// The current entry's record. NULL on error.
static const u8 *record(BtCursor *c, u32 *len) {
  u32 amt = 0;
  u32 total = sqlite3BtreePayloadSize(c); // parses the cell (PayloadFetch expects that done)
  const u8 *p = sqlite3BtreePayloadFetch(c, &amt);
  if (amt >= total) {
    *len = total;
    return p;
  }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    if (!(spill = sqlite3_malloc(spill_cap))) {
      spill_cap = 0;
      return 0;
    }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return spill;
}

typedef struct {
  u32 type;
  const u8 *p;
  u32 n;
} col_t;

static const u8 fixed_len[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};

// The first `ncol` columns of a record. 0 ok, -1 malformed.
static int columns(const u8 *rec, u32 len, int ncol, col_t *out) {
  u32 hdr, i, body;
  i = getVarint32(rec, hdr);
  if (hdr > len) return -1;
  body = hdr;
  for (int k = 0; k < ncol; k++) {
    if (i >= hdr) return -1;
    u32 t;
    i += getVarint32(rec + i, t);
    u32 n = t < 12 ? fixed_len[t] : (t - 12) / 2;
    if (body + n > len) return -1;
    out[k].type = t;
    out[k].p = rec + body;
    out[k].n = n;
    body += n;
  }
  return 0;
}

static int as_int(const col_t *c, i64 *v) {
  if (c->type == 8 || c->type == 9) {
    *v = c->type - 8;
    return 0;
  }
  if (c->type < 1 || c->type > 6) return -1;
  i64 x = (signed char)c->p[0];
  for (u32 j = 1; j < c->n; j++) x = x << 8 | c->p[j];
  *v = x;
  return 0;
}

static int is_text(const col_t *c) { return c->type >= 13 && (c->type & 1); }

// Appends text to td_out.text. -1 if it doesn't fit.
static int put(const col_t *c, u32 *off, u32 *len) {
  if (td_out.ntext + c->n > TD_TEXT) return -1;
  memcpy(td_out.text + td_out.ntext, c->p, c->n);
  *off = td_out.ntext;
  *len = c->n;
  td_out.ntext += c->n;
  return 0;
}

static int put_raw(const unsigned char *p, int n, u32 *off, u32 *len) {
  col_t c = {13, p, (u32)(n < 0 ? 0 : n)};
  return put(&c, off, len);
}

// Table cursor to rowid id: 1 found, 0 absent, -1 error.
static int seek(BtCursor *c, i64 id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && sqlite3BtreeCursorIsValidNN(c);
}

// Index cursor to the first entry whose leading integer column is >= k: 1 on one, 0 none, -1 error.
static int seek_ge(BtCursor *c, i64 k) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = c->pKeyInfo;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1; // entries equal on the key sort after it: land on the first of them
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

// The leading integer of the current likes_post_id_idx entry.
static int like_key(BtCursor *c, i64 *v) {
  u32 len;
  const u8 *rec = record(c, &len);
  col_t k;
  if (!rec || columns(rec, len, 1, &k)) return -1;
  return as_int(&k, v);
}

// Likes for lo <= post_id <= hi in one walk of likes_post_id_idx: n[post_id - lo]++ per entry.
static int count_likes(i64 lo, i64 hi, u32 *n) {
  BtCursor *c = cur[C_LIKES];
  int r = seek_ge(c, lo);
  while (r == 1) {
    i64 v;
    if (like_key(c, &v)) return -1;
    if (v > hi) return 0;
    if (v >= lo) n[v - lo]++;
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return r;
}

// users.username for user_id into the row. 1 found, 0 no such user, -1 error.
static int author(i64 uid, td_row_t *row) {
  BtCursor *c = cur[C_USERS];
  int r = seek(c, uid);
  if (r <= 0) return r;
  u32 len;
  const u8 *rec = record(c, &len);
  col_t cols[2];
  if (!rec || columns(rec, len, 2, cols) || !is_text(&cols[1])) return -1;
  return put(&cols[1], &row->author_off, &row->author_len) ? -1 : 1;
}

// ---- the SQL fallbacks --------------------------------------------------------------------------

static int sql_row(sqlite3_stmt *s, td_row_t *row) {
  row->id = sqlite3_column_int64(s, 0);
  row->likes = sqlite3_column_int64(s, 4);
  if (put_raw(sqlite3_column_text(s, 1), sqlite3_column_bytes(s, 1), &row->body_off, &row->body_len) ||
      put_raw(sqlite3_column_text(s, 2), sqlite3_column_bytes(s, 2), &row->created_off, &row->created_len) ||
      put_raw(sqlite3_column_text(s, 3), sqlite3_column_bytes(s, 3), &row->author_off, &row->author_len))
    return -1;
  return 0;
}

static int feed_sql(void) {
  td_out.nrows = 0;
  td_out.ntext = 0;
  int rc;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW && td_out.nrows < TD_ROWS)
    if (sql_row(st_feed, &td_out.rows[td_out.nrows++])) break;
  sqlite3_reset(st_feed);
  return rc == SQLITE_DONE || rc == SQLITE_ROW ? td_out.nrows : -1;
}

static int post_sql(i64 id) {
  td_out.nrows = 0;
  td_out.ntext = 0;
  sqlite3_bind_int64(st_one, 1, id);
  int rc = sqlite3_step(st_one), found = 0;
  if (rc == SQLITE_ROW) found = sql_row(st_one, &td_out.rows[0]) ? -1 : 1;
  else if (rc != SQLITE_DONE) found = -1;
  sqlite3_reset(st_one);
  td_out.nrows = found > 0;
  return found;
}

// ---- reads ------------------------------------------------------------------------------------

// The 20 newest posts by (created_at DESC, id DESC) into td_out. Returns the row count or -1.
int td_feed(void) {
  if (tx()) return -1;
  td_row_t *rows = td_out.rows;
  td_out.nrows = 0;
  td_out.ntext = 0;
  int n = 0;
  // 1. The index, newest first: (created_at, id, rowid).
  BtCursor *ci = cur[C_FEED];
  int empty = 1;
  if (sqlite3BtreeFirst(ci, &empty) != SQLITE_OK) return -1;
  if (!empty) {
    for (;;) {
      u32 len;
      const u8 *rec = record(ci, &len);
      col_t cols[2];
      if (!rec || columns(rec, len, 2, cols) || !is_text(&cols[0]) || as_int(&cols[1], &rows[n].id)) return feed_sql();
      if (put(&cols[0], &rows[n].created_off, &rows[n].created_len)) return -1;
      if (++n == TD_ROWS) break;
      int rc = sqlite3BtreeNext(ci, 0);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_OK) return -1;
    }
  }
  // 2. The posts, in ascending id order (newest first is usually descending id), so consecutive
  //    ids are one step along a leaf (SQLite's own TableMoveto fast path).
  i64 uid[TD_ROWS];
  i64 lo = 0, hi = 0;
  for (int k = n - 1; k >= 0; k--) {
    td_row_t *row = &rows[k];
    int r = seek(cur[C_POSTS], row->id);
    if (r < 0) return -1;
    if (r == 0) return feed_sql(); // the index and the table disagree
    u32 len;
    const u8 *rec = record(cur[C_POSTS], &len);
    col_t cols[3];
    if (!rec || columns(rec, len, 3, cols) || as_int(&cols[1], &uid[k]) || !is_text(&cols[2])) return feed_sql();
    if (put(&cols[2], &row->body_off, &row->body_len)) return -1;
    if (k == n - 1 || row->id < lo) lo = row->id;
    if (k == n - 1 || row->id > hi) hi = row->id;
  }
  // 3. Authors.
  for (int k = 0; k < n; k++) {
    int r = author(uid[k], &rows[k]);
    if (r < 0) return -1;
    if (r == 0) return feed_sql(); // a post without a user: the JOIN drops it
  }
  // 4. Like counts: one walk of likes_post_id_idx when the ids are close together.
  if (n > 0 && hi - lo < 1024) {
    u32 cnt[1024];
    memset(cnt, 0, (size_t)(hi - lo + 1) * sizeof cnt[0]);
    if (count_likes(lo, hi, cnt) < 0) return -1;
    for (int k = 0; k < n; k++) rows[k].likes = cnt[rows[k].id - lo];
  } else {
    for (int k = 0; k < n; k++) {
      u32 c1 = 0;
      if (count_likes(rows[k].id, rows[k].id, &c1) < 0) return -1;
      rows[k].likes = c1;
    }
  }
  td_out.nrows = n;
  return n;
}

// One post into td_out.rows[0]. 1 found, 0 missing, -1 error.
int td_post(i64 id) {
  if (tx()) return -1;
  td_out.nrows = 0;
  td_out.ntext = 0;
  td_row_t *row = &td_out.rows[0];
  int r = seek(cur[C_POSTS], id);
  if (r <= 0) return r;
  u32 len;
  const u8 *rec = record(cur[C_POSTS], &len);
  col_t cols[4];
  i64 uid;
  if (!rec || columns(rec, len, 4, cols) || as_int(&cols[1], &uid) || !is_text(&cols[2]) || !is_text(&cols[3]))
    return post_sql(id);
  row->id = id;
  if (put(&cols[2], &row->body_off, &row->body_len) || put(&cols[3], &row->created_off, &row->created_len)) return -1;
  r = author(uid, row);
  if (r < 0) return -1;
  if (r == 0) return 0; // the JOIN finds no row
  u32 c1 = 0;
  if (count_likes(id, id, &c1) < 0) return -1;
  row->likes = c1;
  td_out.nrows = 1;
  return 1;
}

// ---- writes -----------------------------------------------------------------------------------

// INSERT a like. 2 new, 1 already liked, 0 no such post, -1 error.
int td_like(i64 uid, i64 pid) {
  if (tx()) return -1;
  int r = seek(cur[C_POSTS], pid);
  if (r <= 0) return r;
  sqlite3_bind_int64(st_like, 1, uid);
  sqlite3_bind_int64(st_like, 2, pid);
  if (step_reset(st_like) != SQLITE_DONE) {
    check_tx();
    return -1;
  }
  return sqlite3_changes(db) > 0 ? 2 : 1;
}

// INSERT a post; its id and created_at go to td_out (created_at as rows[0].created). 0 ok, -1 error.
int td_insert_post(i64 uid, const char *body, int len) {
  if (tx()) return -1;
  td_out.ntext = 0;
  sqlite3_bind_int64(st_post, 1, uid);
  sqlite3_bind_text(st_post, 2, body, len, SQLITE_STATIC);
  int rc = sqlite3_step(st_post);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st_post);
    sqlite3_clear_bindings(st_post);
    check_tx();
    return -1;
  }
  td_row_t *row = &td_out.rows[0];
  row->id = sqlite3_column_int64(st_post, 0);
  int bad = put_raw(sqlite3_column_text(st_post, 1), sqlite3_column_bytes(st_post, 1), &row->created_off,
                    &row->created_len);
  rc = sqlite3_step(st_post);
  sqlite3_reset(st_post);
  sqlite3_clear_bindings(st_post);
  if (rc != SQLITE_DONE) check_tx();
  return bad || rc != SQLITE_DONE ? -1 : 0;
}

// SELECT 1: 0 ok, -1 with the message in td_out.text (ntext bytes).
int td_health(void) {
  td_out.ntext = 0;
  if (tx()) {
    td_out.ntext = (u32)snprintf((char *)td_out.text, 128, "cannot begin transaction");
    return -1;
  }
  int rc = sqlite3_step(st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
  sqlite3_reset(st_health);
  if (!ok) {
    td_out.ntext = (u32)snprintf((char *)td_out.text, 256, "%s", sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}
