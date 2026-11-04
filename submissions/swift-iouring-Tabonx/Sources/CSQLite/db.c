/* The SQLite side of the server: the unmodified amalgamation plus a thin layer over its B-tree.
 *
 * GET /feed and GET /posts/:id walk SQLite's own B-tree cursors inside the pass's transaction and
 * skip the bytecode engine; Swift decodes the record bytes. Writes, BEGIN/COMMIT and /health are
 * ordinary SQL. Everything runs on the event-loop thread except WAL checkpoints, which a second
 * thread runs on its own connection (SQLITE_THREADSAFE=2: each connection stays on one thread). */
#include "sqlite3.c"
#include "feeddb.h"
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>

typedef long long i64;

static sqlite3 *db;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health, *st_feed, *st_likes;
static Pgno roots[4];
static KeyInfo *keyinfos[4];
static int wal_frames;
static int in_tx;
/* SQLite ended the pass's transaction by itself (SQLITE_FULL, I/O errors): the rest of the pass
 * must not write in autocommit mode, and its commit reports failure. */
static int tx_broken;

#define NCUR 4
static BtCursor *cur[NCUR];
static int cur_open;

/* Records that spill onto overflow pages are copied here. */
static u8 *spill;
static u32 spill_cap;

static int fail(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  return SQLITE_OK;
}

static int exec(const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0); }

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

/* The Swift decoders assume this exact column order. Refuse to start on anything else. */
static int layout_is(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[512];
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

static KeyInfo *keyinfo(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) return 0;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

/* ---- Checkpoints ----------------------------------------------------------------------------
 * Past CKPT_FRAMES WAL frames the checkpoint thread copies the log back into the database at
 * nice 19, on its own connection, while the loop goes on. Under steady writes it can't copy the
 * frames written meanwhile, and the log only restarts once it is fully copied, so after that
 * first round writes are held (reads go on) while it copies the rest. A hold lasts at most
 * HOLD_MAX_MS. Past CKPT_FORCE frames the loop checkpoints by itself, as a last resort. */
#define CKPT_FRAMES 4096
#define CKPT_FORCE 65536
#define HOLD_MAX_MS 250
#define CKPT_NICE 19

static sqlite3 *ckdb;
enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static int ck_state;
static int ck_full;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int writes_held;
static struct timespec hold_start;

static void ck_set(int s) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, s, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

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
  /* The checkpoint copies frames straight from the log; this connection reads no tables. */
  if (sqlite3_exec(ckdb, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=16", 0, 0, 0))
    return -1;
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  int rc = pthread_create(&t, &a, ck_main, 0);
  pthread_attr_destroy(&a);
  return rc ? -1 : 0;
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

void fd_tick(void) {
  switch (__atomic_load_n(&ck_state, __ATOMIC_ACQUIRE)) {
  case CK_IDLE:
    if (wal_frames >= CKPT_FORCE) {
      int nlog = 0, ndone = 0;
      if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK &&
          nlog == ndone)
        wal_frames = 0;
    } else if (wal_frames >= CKPT_FRAMES) {
      ck_set(CK_BULK);
    }
    break;
  case CK_BULK_DONE:
    writes_held = 1;
    clock_gettime(CLOCK_MONOTONIC, &hold_start);
    ck_set(CK_TAIL);
    break;
  case CK_TAIL_DONE:
    /* Fully copied: the next write restarts the log. Otherwise the next tick starts another round. */
    if (ck_full) wal_frames = 0;
    writes_held = 0;
    ck_set(CK_IDLE);
    break;
  default:
    if (writes_held && ms_since(&hold_start) > HOLD_MAX_MS) writes_held = 0;
    break;
  }
}

int fd_writes_held(void) { return writes_held; }

/* ---- Opening --------------------------------------------------------------------------------- */

int fd_open(const char *path, char *err, int n) {
  if (sqlite3_initialize()) return fail(err, n, "init");
  /* unix-excl: the process takes an exclusive lock on the file and keeps it, so no other process can
   * open it, and the WAL index lives in heap memory shared by this process's two connections. */
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl"))
    return fail(err, n, "open");
  /* No mmap: a page in SQLite's own cache keeps its parsed header and needs no WAL-index lookup. */
  if (exec("PRAGMA journal_mode=WAL") || exec("PRAGMA synchronous=NORMAL") || exec("PRAGMA temp_store=MEMORY") ||
      exec("PRAGMA cache_size=-65536") || exec("PRAGMA mmap_size=0") || exec("PRAGMA wal_autocheckpoint=0") ||
      exec("SELECT count(*) FROM sqlite_schema"))
    return fail(err, n, "pragmas");
  sqlite3_wal_hook(db, wal_hook, 0);
  static const char *checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!layout_is(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  }
  if (ENC(db) != SQLITE_UTF8) {
    snprintf(err, n, "database text encoding is not UTF-8");
    return -1;
  }
  roots[FD_POSTS] = rootpage("posts");
  roots[FD_USERS] = rootpage("users");
  roots[FD_FEED_IDX] = rootpage("posts_created_at_id_idx");
  roots[FD_LIKES_IDX] = rootpage("likes_post_id_idx");
  for (int i = 0; i < NCUR; i++)
    if (!roots[i]) return fail(err, n, "root pages");
  keyinfos[FD_FEED_IDX] = keyinfo(2, 1, 1);
  keyinfos[FD_LIKES_IDX] = keyinfo(1, 1, 0);
  if (!keyinfos[FD_FEED_IDX] || !keyinfos[FD_LIKES_IDX]) return fail(err, n, "keyinfo");
  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_health = prep("SELECT 1");
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                 "FROM posts p JOIN users u ON u.id = p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  st_likes = prep("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id");
  if (!st_begin || !st_commit || !st_rollback || !st_post || !st_like || !st_health || !st_feed || !st_likes)
    return fail(err, n, "prepare");
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) return fail(err, n, "cursor alloc");
  }
  if (ck_start(path)) {
    snprintf(err, n, "checkpoint connection: %s", ckdb ? sqlite3_errmsg(ckdb) : "cannot open");
    return -1;
  }
  return 0;
}

/* ---- Transactions ---------------------------------------------------------------------------- */

static void close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}

int fd_begin(void) {
  if (tx_broken) return -1;
  if (in_tx) {
    if (!sqlite3_get_autocommit(db)) return 0;
    in_tx = 0;
    tx_broken = 1;
    return -1;
  }
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) {
    step_reset(st_rollback);
    return -1;
  }
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, roots[i], 0, keyinfos[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      step_reset(st_rollback);
      return -1;
    }
  }
  cur_open = 1;
  in_tx = 1;
  return 0;
}

int fd_commit(void) {
  if (tx_broken) {
    tx_broken = 0;
    close_cursors();
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  if (!in_tx) return 0;
  in_tx = 0;
  close_cursors();
  if (step_reset(st_commit) != SQLITE_DONE) {
    step_reset(st_rollback);
    return -1;
  }
  return 0;
}

/* After a failed write: if SQLite ended the transaction itself, the pass is lost. */
static void check_tx(void) {
  if (in_tx && sqlite3_get_autocommit(db)) {
    in_tx = 0;
    tx_broken = 1;
  }
}

/* ---- Cursors --------------------------------------------------------------------------------- */

int fd_seek(int c, int64_t id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur[c], id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && cur[c]->eState == CURSOR_VALID;
}

int fd_first(int c) {
  int empty = 1;
  if (sqlite3BtreeFirst(cur[c], &empty) != SQLITE_OK) return -1;
  return !empty;
}

int fd_next(int c) {
  int rc = sqlite3BtreeNext(cur[c], 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

const uint8_t *fd_payload(int c, uint32_t *len) {
  BtCursor *k = cur[c];
  getCellInfo(k);
  u32 total = k->info.nPayload;
  if (k->info.nLocal == total) {
    *len = total;
    return k->info.pPayload;
  }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) {
      spill_cap = 0;
      return 0;
    }
  }
  if (sqlite3BtreePayload(k, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return spill;
}

/* Index cursor to the first entry whose leading integer column is >= key: 1 on an entry, 0 none. */
static int seek_ge(BtCursor *c, i64 key) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = key;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = c->pKeyInfo;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1; /* entries equal to the key sort after it: land on the first of them */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

/* The leading integer of an index record that lies wholly on its page: 1 ok, 0 not that form. */
static inline int leading_int(const u8 *cell, const u8 *end, i64 *out) {
  u32 len = cell[0];
  const u8 *p = cell + 1;
  if (len >= 0x80 || len < 3 || p + len > end) return 0;
  u32 h = p[0], t = p[1];
  if (h >= len || h < 3 || t >= 0x80) return 0;
  if (t >= 1 && t <= 6) {
    static const u8 size[7] = {0, 1, 2, 3, 4, 6, 8};
    u32 k = size[t];
    if (h + k > len) return 0;
    const u8 *q = p + h;
    i64 v = (i8)q[0];
    for (u32 j = 1; j < k; j++) v = v << 8 | q[j];
    *out = v;
    return 1;
  }
  if (t == 8 || t == 9) {
    *out = t - 8;
    return 1;
  }
  return 0;
}

/* The fallback for cells the fast walk doesn't expect: the same counts through SQL. */
static int count_likes_sql(i64 lo, i64 hi, uint32_t *counts) {
  sqlite3_bind_int64(st_likes, 1, lo);
  sqlite3_bind_int64(st_likes, 2, hi);
  int rc;
  while ((rc = sqlite3_step(st_likes)) == SQLITE_ROW) {
    i64 id = sqlite3_column_int64(st_likes, 0);
    if (id >= lo && id <= hi) counts[id - lo] = (uint32_t)sqlite3_column_int64(st_likes, 1);
  }
  sqlite3_reset(st_likes);
  if (rc != SQLITE_DONE) {
    check_tx();
    return -1;
  }
  return 0;
}

int fd_count_likes(int64_t lo, int64_t hi, uint32_t *counts) {
  BtCursor *c = cur[FD_LIKES_IDX];
  int r = seek_ge(c, lo);
  if (r <= 0) return r;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1; /* an interior entry: just this one */
    for (; i < last; i++) {
      i64 v;
      if (!leading_int(findCell(pg, i) + pg->childPtrSize, pg->aDataEnd, &v)) {
        memset(counts, 0, (size_t)(hi - lo + 1) * sizeof *counts);
        return count_likes_sql(lo, hi, counts);
      }
      if (v > hi) return 0;
      if (v >= lo) counts[v - lo]++;
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

/* ---- Writes and SQL -------------------------------------------------------------------------- */

int fd_insert_post(int64_t user_id, const uint8_t *body, int len, int64_t *id, uint8_t *created, int *created_len) {
  sqlite3_bind_int64(st_post, 1, user_id);
  sqlite3_bind_text(st_post, 2, (const char *)body, len, SQLITE_STATIC);
  int rc = sqlite3_step(st_post);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st_post);
    check_tx();
    return -1;
  }
  *id = sqlite3_column_int64(st_post, 0);
  int n = sqlite3_column_bytes(st_post, 1);
  const unsigned char *t = sqlite3_column_text(st_post, 1);
  if (!t || n > 63) {
    sqlite3_reset(st_post);
    check_tx();
    return -1;
  }
  memcpy(created, t, n);
  *created_len = n;
  rc = sqlite3_step(st_post);
  sqlite3_reset(st_post);
  sqlite3_clear_bindings(st_post);
  if (rc != SQLITE_DONE) {
    check_tx();
    return -1;
  }
  return 0;
}

int fd_insert_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(st_like, 1, user_id);
  sqlite3_bind_int64(st_like, 2, post_id);
  if (step_reset(st_like) != SQLITE_DONE) {
    check_tx();
    return -1;
  }
  return sqlite3_changes(db) > 0;
}

int fd_health(char *err, int n) {
  int rc = sqlite3_step(st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
  sqlite3_reset(st_health);
  if (!ok) {
    snprintf(err, n, "%s", sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int fd_feed_sql_step(int64_t *id, const uint8_t **body, int *body_len, const uint8_t **created, int *created_len,
                     const uint8_t **author, int *author_len, int64_t *likes) {
  int rc = sqlite3_step(st_feed);
  if (rc == SQLITE_DONE) {
    sqlite3_reset(st_feed);
    return 0;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st_feed);
    return -1;
  }
  *id = sqlite3_column_int64(st_feed, 0);
  *body = sqlite3_column_text(st_feed, 1);
  *body_len = sqlite3_column_bytes(st_feed, 1);
  *created = sqlite3_column_text(st_feed, 2);
  *created_len = sqlite3_column_bytes(st_feed, 2);
  *author = sqlite3_column_text(st_feed, 3);
  *author_len = sqlite3_column_bytes(st_feed, 3);
  *likes = sqlite3_column_int64(st_feed, 4);
  return 1;
}
