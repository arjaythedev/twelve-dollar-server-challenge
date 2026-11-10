/* SQLite: the unmodified amalgamation plus the calls server.goose makes into it.
 *
 * Reads (GET /feed, GET /posts/:id) move SQLite's own B-tree cursors inside the pass's read
 * transaction, so only the SQL bytecode engine is skipped: pages come through SQLite's pager, WAL
 * and page cache. This file moves a cursor and copies the current row's record bytes into a
 * Goose builder; server.goose decodes the record and decides what to read next. Only functions
 * of SQLite's B-tree interface (btree.h) are called here, and no SQLite structure is written.
 * Writes, BEGIN/COMMIT and /health are ordinary prepared statements.
 *
 * Everything runs on the event-loop thread except WAL checkpoints, which a second thread runs
 * on its own connection when server.goose asks (design from rust-iouring-kensac #264), so
 * SQLite is built SQLITE_THREADSAFE=2: each connection is used by one thread. */
#include "sqlite3.c"
#include <pthread.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "ffi.h"

/* The trees a pass keeps a cursor on (the same numbers as in server.goose). */
enum { T_POSTS, T_USERS, T_FEED, T_LIKES, NT };

static sqlite3 *db;
static Btree *bt;
static BtCursor *cur[NT];
static Pgno root[NT];
static KeyInfo *keys[NT];
static sqlite3_stmt *q_begin, *q_commit, *q_rollback, *q_post, *q_like, *q_ping, *q_feed;
static int in_pass;      /* the pass's transaction is open, with its cursors */
static int pass_lost;    /* SQLite rolled it back by itself: nothing more may run in this pass */
static u8 *spill;        /* a record with overflow pages is assembled here (none with this schema) */
static u32 spill_cap;
static int wal_size;     /* frames in the log after the last commit that wrote */

/* ---- the checkpoint thread ---- */

/* On one vCPU a checkpoint at the loop's priority would take the CPU from it for as long as it
 * runs; at nice 19 it runs in the loop's idle time. */
#define CK_NICE 19
enum { CK_IDLE, CK_ASKED, CK_DONE_ALL, CK_DONE_SOME };
static sqlite3 *ck_db;
static int ck_state;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;

static void *ck_main(void *arg) {
  (void)arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), CK_NICE);
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (ck_state != CK_ASKED) pthread_cond_wait(&ck_cv, &ck_mu);
    pthread_mutex_unlock(&ck_mu);
    /* PASSIVE never waits on the loop's locks, and the loop never waits for this thread. */
    int in_log = -1, copied = -2;
    int rc = sqlite3_wal_checkpoint_v2(ck_db, "main", SQLITE_CHECKPOINT_PASSIVE, &in_log, &copied);
    __atomic_store_n(&ck_state, rc == SQLITE_OK && in_log == copied ? CK_DONE_ALL : CK_DONE_SOME, __ATOMIC_RELEASE);
  }
  return 0;
}

/* Asks the thread for one PASSIVE checkpoint. */
void db_ck_ask(void) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, CK_ASKED, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

/* 0 idle, 1 copying, 2 done with the whole log copied, 3 done with frames left. A finished
 * state is reported once. */
int32_t db_ck_poll(void) {
  int s = __atomic_load_n(&ck_state, __ATOMIC_ACQUIRE);
  if (s == CK_DONE_ALL || s == CK_DONE_SOME) __atomic_store_n(&ck_state, CK_IDLE, __ATOMIC_RELEASE);
  return s;
}

int64_t db_wal_frames(void) { return wal_size; }

/* The whole log is copied back: the next write starts it over, and the hook reports its size. */
void db_wal_restarts(void) { wal_size = 0; }

static int on_commit_frames(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_size = frames;
  return SQLITE_OK;
}

/* ---- opening ---- */

static void explain(gs_rref err, const char *what, sqlite3 *d) {
  const char *m = d ? sqlite3_errmsg(d) : "no connection";
  gs_bld_append(err, what, (int64_t)strlen(what));
  gs_bld_append(err, ": ", 2);
  gs_bld_append(err, m, (int64_t)strlen(m));
}

static int run(sqlite3_stmt *q) {
  int rc = sqlite3_step(q);
  sqlite3_reset(q);
  return rc;
}

static sqlite3_stmt *prepare(const char *sql) {
  sqlite3_stmt *q = 0;
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &q, 0) == SQLITE_OK ? q : 0;
}

/* Comma-joined first column of a query, for the schema checks. */
static int columns_are(const char *sql, const char *want) {
  sqlite3_stmt *q;
  char got[256];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &q, 0) != SQLITE_OK) return 0;
  while (sqlite3_step(q) == SQLITE_ROW && n < sizeof got) {
    const char *c = (const char *)sqlite3_column_text(q, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
  }
  sqlite3_finalize(q);
  return strcmp(got, want) == 0;
}

static Pgno root_of(const char *name) {
  sqlite3_stmt *q;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &q, 0) != SQLITE_OK) return 0;
  sqlite3_bind_text(q, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(q) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(q, 0);
  sqlite3_finalize(q);
  return r;
}

/* How an index compares its keys: every column with the database's default collation. */
static KeyInfo *key_info(int nkey, int descending) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, 1);
  if (!k) return 0;
  for (int i = 0; i <= nkey; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = descending ? KEYINFO_ORDER_DESC : 0;
  return k;
}

/* 0, or -1 with the reason appended to err. */
int32_t db_open(bytes_t path_b, gs_rref err) {
  char path[4096];
  if (path_b.len <= 0 || path_b.len >= (int64_t)sizeof path) {
    gs_bld_append(err, "bad SQLITE_PATH", 15);
    return -1;
  }
  memcpy(path, path_b.data, (size_t)path_b.len);
  path[path_b.len] = 0;
  if (sqlite3_initialize() != SQLITE_OK) { gs_bld_append(err, "sqlite3_initialize", 18); return -1; }
  /* unix-excl: this process locks the file once and keeps it, so no other process can open it,
   * and the WAL index is heap memory shared by the process's two connections (no -shm file, no
   * file locks per transaction). */
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) {
    explain(err, "open", db);
    return -1;
  }
  /* WAL with synchronous=NORMAL: a commit survives a process crash once COMMIT returns (rule 6).
   * No mmap: a page in SQLite's own cache keeps its parsed header and needs no WAL lookup. */
  if (sqlite3_exec(db,
                   "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;"
                   "PRAGMA cache_size = -65536; PRAGMA mmap_size = 0; PRAGMA wal_autocheckpoint = 0;"
                   "SELECT count(*) FROM sqlite_schema",
                   0, 0, 0) != SQLITE_OK) {
    explain(err, "pragmas", db);
    return -1;
  }
  sqlite3_wal_hook(db, on_commit_frames, 0);
  /* server.goose decodes records by position: refuse any other column order. */
  static const char *const checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT \"desc\" FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT \"desc\" FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "0"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!columns_are(checks[i][0], checks[i][1])) {
      gs_bld_append(err, "unexpected schema: ", 19);
      gs_bld_append(err, checks[i][0], (int64_t)strlen(checks[i][0]));
      return -1;
    }
  }
  if (ENC(db) != SQLITE_UTF8) { gs_bld_append(err, "the database is not UTF-8", 25); return -1; }
  root[T_POSTS] = root_of("posts");
  root[T_USERS] = root_of("users");
  root[T_FEED] = root_of("posts_created_at_id_idx");
  root[T_LIKES] = root_of("likes_post_id_idx");
  keys[T_FEED] = key_info(2, 1);
  keys[T_LIKES] = key_info(1, 0);
  for (int t = 0; t < NT; t++) {
    cur[t] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!root[t] || !cur[t] || (t >= T_FEED && !keys[t])) { explain(err, "tables", db); return -1; }
  }
  bt = db->aDb[0].pBt;
  q_begin = prepare("BEGIN");
  q_commit = prepare("COMMIT");
  q_rollback = prepare("ROLLBACK");
  q_post = prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  q_like = prepare("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  q_ping = prepare("SELECT 1");
  /* The reference query, for a feed the cursor plan cannot answer (a post with no user row). */
  q_feed = prepare("SELECT p.id, p.body, p.created_at, u.username,"
                   " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
                   " FROM posts p JOIN users u ON u.id = p.user_id"
                   " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  if (!q_begin || !q_commit || !q_rollback || !q_post || !q_like || !q_ping || !q_feed) {
    explain(err, "prepare", db);
    return -1;
  }
  /* The checkpoint connection copies frames from the log; it reads no tables. */
  if (sqlite3_open_v2(path, &ck_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK ||
      sqlite3_exec(ck_db, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16", 0, 0, 0) !=
        SQLITE_OK) {
    explain(err, "checkpoint connection", ck_db);
    return -1;
  }
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 512 << 10);
  if (pthread_create(&t, &a, ck_main, 0) != 0) { gs_bld_append(err, "checkpoint thread", 17); return -1; }
  pthread_attr_destroy(&a);
  return 0;
}

/* ---- the pass's transaction ---- */

static void close_cursors(void) {
  for (int t = 0; t < NT; t++) sqlite3BtreeCloseCursor(cur[t]);
}

/* Joins the pass's transaction, opening it (and a cursor on each tree) for the first request
 * that needs the database: its reads share one snapshot and its writes one COMMIT. 0, or -1. */
int32_t db_begin(void) {
  if (pass_lost) return -1;
  if (in_pass) {
    if (!sqlite3_get_autocommit(db)) return 0;
    /* SQLite ended the transaction itself (an I/O error, a full disk): the pass is lost. */
    close_cursors();
    in_pass = 0;
    pass_lost = 1;
    return -1;
  }
  if (run(q_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) { run(q_rollback); return -1; }
  for (int t = 0; t < NT; t++) {
    sqlite3BtreeCursorZero(cur[t]);
    if (sqlite3BtreeCursor(bt, root[t], 0, keys[t], cur[t]) != SQLITE_OK) {
      for (int k = 0; k < t; k++) sqlite3BtreeCloseCursor(cur[k]);
      run(q_rollback);
      return -1;
    }
  }
  in_pass = 1;
  return 0;
}

/* Ends the pass's transaction: true once it is committed (or there was none), false if it was
 * rolled back. No response of the pass is sent before this returns. */
bool db_commit(void) {
  if (pass_lost) {
    pass_lost = 0;
    if (!sqlite3_get_autocommit(db)) run(q_rollback);
    return false;
  }
  if (!in_pass) return true;
  in_pass = 0;
  close_cursors();
  if (run(q_commit) != SQLITE_DONE) {
    fprintf(stderr, "commit failed: %s\n", sqlite3_errmsg(db));
    if (!sqlite3_get_autocommit(db)) run(q_rollback);
    return false;
  }
  return true;
}

/* ---- cursors ---- */

/* Moves a table's cursor to rowid `key`: 1 there, 0 no such row, -1 error. */
int32_t db_seek(int32_t t, int64_t key) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur[t], key, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && !sqlite3BtreeEof(cur[t]);
}

/* Moves an index's cursor to its first entry whose leading integer column is >= key: 1 on an
 * entry, 0 none, -1 error. */
int32_t db_seek_ge(int32_t t, int64_t key) {
  BtCursor *c = cur[t];
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = key;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = keys[t];
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = 1; /* entries equal on the key sort after it, so this lands before the first */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

/* 1 on the first entry, 0 empty, -1 error. */
int32_t db_first(int32_t t) {
  int empty = 1;
  if (sqlite3BtreeFirst(cur[t], &empty) != SQLITE_OK) return -1;
  return !empty;
}

/* 1 on the next entry, 0 at the end, -1 error. */
int32_t db_next(int32_t t) {
  int rc = sqlite3BtreeNext(cur[t], 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* Appends the current entry's record bytes to a Goose builder: their count, or -1. */
int64_t db_row(int32_t t, gs_rref out) {
  BtCursor *c = cur[t];
  u32 total = sqlite3BtreePayloadSize(c);
  u32 local = 0;
  const void *p = sqlite3BtreePayloadFetch(c, &local);
  if (local >= total) {
    gs_bld_append(out, p, total);
    return total;
  }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) { spill_cap = 0; return -1; }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return -1;
  gs_bld_append(out, spill, total);
  return total;
}

/* Copies the first bytes of the current entry's record (at most 24) into a Goose array and
 * returns the record's whole length, or -1: enough to read an index entry's leading key. */
int64_t db_row_start(int32_t t, uint8_t *into) {
  BtCursor *c = cur[t];
  u32 total = sqlite3BtreePayloadSize(c);
  u32 local = 0;
  const void *p = sqlite3BtreePayloadFetch(c, &local);
  u32 n = total < 24 ? total : 24;
  if (local < n) return -1;
  memcpy(into, p, n);
  return total;
}

/* ---- writes and /health ---- */

/* A write failed: if SQLite ended the transaction with it, the pass is lost. */
static void after_failed_write(void) {
  if (in_pass && sqlite3_get_autocommit(db)) {
    close_cursors();
    in_pass = 0;
    pass_lost = 1;
  }
}

/* INSERT ... RETURNING id, created_at: 0 (id set, created_at appended), -1 error. */
int32_t db_add_post(int64_t user_id, bytes_t body, int64_t *id, gs_rref created) {
  sqlite3_bind_int64(q_post, 1, user_id);
  sqlite3_bind_text(q_post, 2, (const char *)body.data, (int)body.len, SQLITE_STATIC);
  int ok = sqlite3_step(q_post) == SQLITE_ROW;
  if (ok) {
    *id = sqlite3_column_int64(q_post, 0);
    const unsigned char *text = sqlite3_column_text(q_post, 1);
    int n = sqlite3_column_bytes(q_post, 1);
    ok = text && n > 0;
    if (ok) gs_bld_append(created, text, n);
    ok = ok && sqlite3_step(q_post) == SQLITE_DONE;
  }
  sqlite3_reset(q_post);
  sqlite3_clear_bindings(q_post);
  if (!ok) after_failed_write();
  return ok ? 0 : -1;
}

/* 1 a new like, 0 already liked, -1 error. */
int32_t db_add_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(q_like, 1, user_id);
  sqlite3_bind_int64(q_like, 2, post_id);
  if (run(q_like) != SQLITE_DONE) { after_failed_write(); return -1; }
  return sqlite3_changes(db) > 0;
}

/* SELECT 1: 0 ok, -1 with SQLite's message appended to err. */
int32_t db_ping(gs_rref err) {
  int ok = sqlite3_step(q_ping) == SQLITE_ROW && sqlite3_column_int(q_ping, 0) == 1;
  sqlite3_reset(q_ping);
  if (ok) return 0;
  const char *m = sqlite3_errmsg(db);
  gs_bld_append(err, m, (int64_t)strlen(m));
  return -1;
}

/* One row of the reference feed query: 1 (numbers in *row, the three strings appended to text
 * in the order body, created_at, author), 0 done, -1 error. */
int32_t db_feed_step(feed_row_t *row, gs_rref text) {
  int rc = sqlite3_step(q_feed);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(q_feed);
    return rc == SQLITE_DONE ? 0 : -1;
  }
  row->id = sqlite3_column_int64(q_feed, 0);
  row->likes = sqlite3_column_int64(q_feed, 4);
  int64_t *lens[3] = {&row->body_len, &row->created_len, &row->author_len};
  for (int k = 0; k < 3; k++) {
    const unsigned char *s = sqlite3_column_text(q_feed, k + 1);
    *lens[k] = s ? sqlite3_column_bytes(q_feed, k + 1) : 0;
    if (s) gs_bld_append(text, s, *lens[k]);
  }
  return 1;
}

/* Abandons the reference query before its last row. */
void db_feed_reset(void) { sqlite3_reset(q_feed); }
