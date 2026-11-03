/* db.c: SQLite under app.goose. The unmodified amalgamation plus a thin layer over its B-tree.
 *
 * Reads (GET /feed, GET /posts/:id, the like handler's existence check) walk SQLite's own B-tree
 * cursors inside the pass's transaction, skipping the SQL bytecode engine: this file positions
 * cursors and copies a row's record bytes into a Goose buffer, and app.goose decodes them and
 * decides what to read next. Writes, BEGIN/COMMIT and /health are ordinary SQL statements.
 * Everything runs on the event-loop thread except WAL checkpoints, which a second thread runs on
 * its own connection (SQLITE_THREADSAFE=2: each connection is used by one thread). */
#include "sqlite3.c"
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include "goose.h"

typedef struct { const uint8_t *data; int64_t len; } sl_bytes;
typedef struct { uint32_t *data; int64_t len; } sl_u32;

static sqlite3 *db;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health, *st_feed;
static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;
static int wal_frames;
static int in_batch;
/* SQLite rolled the pass's transaction back on its own (e.g. SQLITE_FULL): the rest of the pass
 * must not write in autocommit mode, and its COMMIT reports failure. */
static int batch_broken;

/* Cursors: 0 posts, 1 users, 2 posts_created_at_id_idx, 3 likes_post_id_idx. Opened after the
 * pass's read transaction starts and closed before it commits. */
#define NCUR 4
static BtCursor *cur[NCUR];
static int cur_open;

/* Checkpoints. Past CKPT_FRAMES WAL frames the checkpoint thread copies the log back. Then writes
 * are held (reads go on) while it copies the frames written meanwhile, so the next write restarts
 * the log. A hold lasts at most HOLD_MAX_MS. Past CKPT_FORCE frames (the thread keeps failing to
 * catch up) the event loop checkpoints by itself, as a last resort. */
#define CKPT_FRAMES 2048
#define CKPT_HOLD 16384
#define CKPT_FORCE 32768
#define HOLD_MAX_MS 250
/* On one vCPU a checkpoint at the default priority takes the loop's CPU for its whole pass. */
#define CKPT_NICE 19

static sqlite3 *ckdb;
enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static int ck_state; /* written under ck_mu; the loop polls it with an atomic load */
static int ck_full;  /* the thread's last pass left nothing to copy back (set before *_DONE) */
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int writes_held;
static struct timespec hold_t0;

static void errmsg(gs_rref err, const char *what) {
  const char *m = db ? sqlite3_errmsg(db) : "no database";
  gs_bld_append(err, what, (int64_t)strlen(what));
  gs_bld_append(err, ": ", 2);
  gs_bld_append(err, m, (int64_t)strlen(m));
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  return SQLITE_OK;
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

/* The record decoders in app.goose assume this exact column order; refuse to start otherwise. */
static int layout(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *keyinfo(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) return 0;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0)) return 0;
  return s;
}

static int exec(const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0); }

static void ck_set(int s) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, s, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

/* The checkpoint thread: one PASSIVE checkpoint per request from the loop, on its own connection.
 * PASSIVE never waits on the loop's locks, and the loop never waits on the thread. */
static void *ck_main(void *arg) {
  (void)arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), CKPT_NICE);
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

static int ck_start(const char *path) {
  if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl")) return -1;
  /* A checkpoint copies frames straight from the WAL; this connection reads no tables. */
  if (sqlite3_exec(ckdb, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16",
                   0, 0, 0))
    return -1;
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  int rc = pthread_create(&t, &a, ck_main, 0);
  pthread_attr_destroy(&a);
  return rc ? -1 : 0;
}

/* 0 ok; -1 with the reason appended to err. */
int32_t db_open(sl_bytes path_s, gs_rref err) {
  char path[4096];
  if (path_s.len <= 0 || path_s.len >= (int64_t)sizeof path) { errmsg(err, "path"); return -1; }
  memcpy(path, path_s.data, (size_t)path_s.len);
  path[path_s.len] = 0;
  if (sqlite3_initialize()) { errmsg(err, "init"); return -1; }
  /* unix-excl: this process takes an exclusive lock on the file and keeps it, and the WAL index
   * lives in heap memory shared by this process's two connections (no -shm file, no file locks
   * per transaction). synchronous=NORMAL in WAL: a commit is durable against process crashes
   * when COMMIT returns (rule 6). */
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl")) {
    errmsg(err, "open");
    return -1;
  }
  /* No mmap: a page held in SQLite's own cache keeps its parsed B-tree header and is found
   * without a WAL-index lookup, while an mmap page pays both on every fetch. */
  if (exec("PRAGMA journal_mode = WAL") || exec("PRAGMA synchronous = NORMAL") ||
      exec("PRAGMA temp_store = MEMORY") || exec("PRAGMA cache_size = -32768") ||
      exec("PRAGMA mmap_size = 0") || exec("PRAGMA wal_autocheckpoint = 0") ||
      exec("SELECT count(*) FROM sqlite_schema")) {
    errmsg(err, "pragmas");
    return -1;
  }
  sqlite3_wal_hook(db, wal_hook, 0);
  if (ck_start(path)) { errmsg(err, "checkpoint connection"); return -1; }
  static const char *checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!layout(checks[i][0], checks[i][1])) { errmsg(err, "unexpected schema"); return -1; }
  }
  if (ENC(db) != SQLITE_UTF8) { errmsg(err, "database text encoding is not UTF-8"); return -1; }
  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_feed_idx = rootpage("posts_created_at_id_idx");
  root_likes_idx = rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed_idx || !root_likes_idx) { errmsg(err, "root pages"); return -1; }
  ki_feed = keyinfo(2, 1, 1);
  ki_likes = keyinfo(1, 1, 0);
  if (!ki_feed || !ki_likes) { errmsg(err, "keyinfo"); return -1; }
  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_health = prep("SELECT 1");
  /* The reference query, for data the B-tree plan does not expect. */
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                 "FROM posts p JOIN users u ON u.id = p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  if (!st_begin || !st_commit || !st_rollback || !st_post || !st_like || !st_health || !st_feed) {
    errmsg(err, "prepare");
    return -1;
  }
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) { errmsg(err, "cursor alloc"); return -1; }
  }
  return 0;
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}

/* One transaction per event-loop pass: reads share its snapshot, writes are group-committed.
 * Every request that touches the database calls this first. 0 ok, -1 the pass is lost. */
int32_t db_begin(void) {
  if (batch_broken) return -1;
  if (in_batch) {
    if (!sqlite3_get_autocommit(db)) return 0;
    /* SQLite ended the transaction itself (e.g. an I/O error in a read): the pass is lost. */
    in_batch = 0;
    batch_broken = 1;
    return -1;
  }
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) { step_reset(st_rollback); return -1; }
  static const Pgno *roots[NCUR] = {&root_posts, &root_users, &root_feed_idx, &root_likes_idx};
  KeyInfo *kis[NCUR] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, *roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      step_reset(st_rollback);
      return -1;
    }
  }
  cur_open = 1;
  in_batch = 1;
  return 0;
}

/* Ends the pass's transaction: true once committed (or if there was none), false if rolled back. */
bool db_commit(void) {
  if (batch_broken) {
    batch_broken = 0;
    close_cursors();
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return false;
  }
  if (!in_batch) return true;
  in_batch = 0;
  close_cursors();
  if (step_reset(st_commit) != SQLITE_DONE) {
    step_reset(st_rollback);
    return false;
  }
  return true;
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Between passes (no transaction open): drives the checkpoint thread. A hold starts and ends only
 * here, so the tail checkpoint starts after the last write committed, and the pass after the hold
 * begins with the log fully copied back, which lets its first write restart the log. */
void db_tick(void) {
  switch (__atomic_load_n(&ck_state, __ATOMIC_ACQUIRE)) {
  case CK_IDLE:
    if (wal_frames >= CKPT_FORCE) {
      /* Last resort: with the thread idle there is no other reader, so this copies the whole log. */
      int nlog = 0, ndone = 0;
      if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK &&
          nlog == ndone)
        wal_frames = 0;
    } else if (wal_frames >= CKPT_FRAMES) {
      ck_set(CK_BULK);
    }
    break;
  case CK_BULK_DONE:
    if (!writes_held) clock_gettime(CLOCK_MONOTONIC, &hold_t0);
    writes_held = 1;
    ck_set(CK_TAIL);
    break;
  case CK_TAIL_DONE:
    /* Fully copied: the next write restarts the log and the WAL hook reports its new size.
     * Otherwise (the hold timed out and writes went on) the next tick starts another round. */
    if (ck_full) wal_frames = 0;
    writes_held = 0;
    ck_set(CK_IDLE);
    break;
  case CK_BULK:
    /* The bulk pass is still running and the log keeps growing (the loop leaves the thread
     * little CPU): hold writes so the pass can catch up. */
    if (!writes_held && wal_frames >= CKPT_HOLD) {
      writes_held = 1;
      clock_gettime(CLOCK_MONOTONIC, &hold_t0);
    } else if (writes_held && ms_since(&hold_t0) > HOLD_MAX_MS) {
      writes_held = 0;
    }
    break;
  default:
    if (writes_held && ms_since(&hold_t0) > HOLD_MAX_MS) writes_held = 0;
    break;
  }
}

/* Writes wait, unread, while the checkpoint thread copies the log's last frames. */
bool db_writes_held(void) { return writes_held != 0; }

/* ---- B-tree cursors ---- */

/* Table cursor to rowid `id`: 1 found, 0 absent, -1 error. */
int32_t bt_seek(int32_t i, int64_t id) {
  BtCursor *c = cur[i];
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

/* Index cursor to the first entry whose leading integer column is >= k: 1 on an entry, 0 none. */
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
  r.default_rc = +1; /* entries equal on the key sort after it: land on the first of them */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

int32_t bt_seek_ge(int32_t i, int64_t k) { return seek_ge(cur[i], k); }

/* Likes per post for lo <= post_id <= hi off likes_post_id_idx (post_id, rowid), in one walk:
 * n[post_id - lo] counts the entries. The cells are decoded here a leaf at a time instead of one
 * call per entry. 0 ok, -1 error, 1 a cell not in the expected form (the caller then decodes the
 * entries itself, which reports it). */
int32_t bt_count_likes(int64_t lo, int64_t hi, sl_u32 n) {
  BtCursor *c = cur[3];
  if (hi < lo || hi - lo >= n.len) return 1;
  int r = seek_ge(c, lo);
  if (r <= 0) return r;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1; /* an interior entry: just this one */
    for (; i < last; i++) {
      u8 *cell = findCell(pg, i) + pg->childPtrSize;
      u32 len = cell[0];
      const u8 *p = cell + 1;
      /* One-byte sizes only, so the record is all on this page (maxLocal is far larger). */
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
      if (v >= lo) n.data[v - lo]++;
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

/* 1 on a row, 0 empty, -1 error. */
int32_t bt_first(int32_t i) {
  int empty = 1;
  if (sqlite3BtreeFirst(cur[i], &empty) != SQLITE_OK) return -1;
  return !empty;
}

/* 1 on the next row, 0 at the end, -1 error. */
int32_t bt_next(int32_t i) {
  int rc = sqlite3BtreeNext(cur[i], 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

/* Appends the current row's record bytes to `out`; their length, or -1. */
int64_t bt_payload(int32_t i, gs_rref out) {
  BtCursor *c = cur[i];
  getCellInfo(c);
  u32 total = c->info.nPayload;
  if (c->info.nLocal == total) {
    gs_bld_append(out, c->info.pPayload, total);
    return total;
  }
  /* Spills onto overflow pages (not expected with this schema): read it through the pager. */
  if (sqlite3BtreePayload(c, 0, total, out.stk->top) != SQLITE_OK) return -1;
  out.stk->top += total;
  out.hdr->len += total;
  return total;
}

/* ---- writes and /health (SQL) ---- */

/* After a failed write: if SQLite ended the transaction itself, the pass is lost. */
static void check_tx(void) {
  if (in_batch && sqlite3_get_autocommit(db)) {
    in_batch = 0;
    batch_broken = 1;
  }
}

/* INSERT ... RETURNING id, created_at. 0 ok (id set, created_at appended), -1 error. */
int32_t db_insert_post(int64_t user_id, sl_bytes body, int64_t *id, gs_rref created) {
  sqlite3_bind_int64(st_post, 1, user_id);
  sqlite3_bind_text(st_post, 2, (const char *)body.data, (int)body.len, SQLITE_STATIC);
  int rc = sqlite3_step(st_post);
  if (rc != SQLITE_ROW) { sqlite3_reset(st_post); check_tx(); return -1; }
  *id = sqlite3_column_int64(st_post, 0);
  int n = sqlite3_column_bytes(st_post, 1);
  const unsigned char *t = sqlite3_column_text(st_post, 1);
  if (!t || n > 63) { sqlite3_reset(st_post); return -1; }
  gs_bld_append(created, t, n);
  rc = sqlite3_step(st_post);
  sqlite3_reset(st_post);
  sqlite3_clear_bindings(st_post);
  return rc == SQLITE_DONE ? 0 : -1;
}

/* 1 inserted, 0 already liked, -1 error. */
int32_t db_insert_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(st_like, 1, user_id);
  sqlite3_bind_int64(st_like, 2, post_id);
  int rc = step_reset(st_like);
  if (rc != SQLITE_DONE) { check_tx(); return -1; }
  return sqlite3_changes(db) > 0;
}

/* SELECT 1: 0 ok, -1 with the error message appended to err. */
int32_t db_health(gs_rref err) {
  int rc = sqlite3_step(st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
  sqlite3_reset(st_health);
  if (!ok) {
    const char *m = sqlite3_errmsg(db);
    gs_bld_append(err, m, (int64_t)strlen(m));
    return -1;
  }
  return 0;
}

/* The reference feed query, one row per call: 1 row (body, created_at and author appended to
 * text, their lengths in lens), 0 done, -1 error. */
#pragma pack(push, 1)
typedef struct { int64_t id, likes, blen, clen, alen; } SqlRow;
#pragma pack(pop)
int32_t db_feed_sql_step(SqlRow *row, gs_rref text) {
  int rc = sqlite3_step(st_feed);
  if (rc == SQLITE_DONE) { sqlite3_reset(st_feed); return 0; }
  if (rc != SQLITE_ROW) { sqlite3_reset(st_feed); return -1; }
  row->id = sqlite3_column_int64(st_feed, 0);
  row->likes = sqlite3_column_int64(st_feed, 4);
  const unsigned char *b = sqlite3_column_text(st_feed, 1);
  row->blen = sqlite3_column_bytes(st_feed, 1);
  gs_bld_append(text, b, row->blen);
  const unsigned char *c = sqlite3_column_text(st_feed, 2);
  row->clen = sqlite3_column_bytes(st_feed, 2);
  gs_bld_append(text, c, row->clen);
  const unsigned char *a = sqlite3_column_text(st_feed, 3);
  row->alen = sqlite3_column_bytes(st_feed, 3);
  gs_bld_append(text, a, row->alen);
  return 1;
}

/* ---- the process environment ---- */

bool env_get(sl_bytes name_s, gs_rref out) {
  char name[128];
  if (name_s.len <= 0 || name_s.len >= (int64_t)sizeof name) return false;
  memcpy(name, name_s.data, (size_t)name_s.len);
  name[name_s.len] = 0;
  const char *v = getenv(name);
  if (!v) return false;
  gs_bld_append(out, v, (int64_t)strlen(v));
  return true;
}

/* Unix time in seconds (for JWT exp) and a monotonic clock in ms (for uptime). */
double now_unix(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int64_t now_mono_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void log_line(sl_bytes s) {
  fwrite(s.data, 1, (size_t)s.len, stderr);
  fputc('\n', stderr);
}
