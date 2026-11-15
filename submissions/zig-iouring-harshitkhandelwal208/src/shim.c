/* shim.c: Direct SQLite B-tree cursor operations and low-priority background checkpoint worker */
#include "../deps/sqlite-amalgamation-3540100/sqlite3.c"
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>

typedef long long i64;
typedef uint8_t u8;
typedef uint32_t u32;

static sqlite3 *db;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_unlike, *st_check, *st_health, *st_feed;
static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;
static int wal_frames;
static int in_batch;
static int batch_broken;

#define NCUR 4
static BtCursor *cur[NCUR];
static int cur_open;

static u8 *spill;
static u32 spill_cap;

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
static struct timespec hold_t0;

void td_close(void);

static int fail(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  td_close();
  return -1;
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

static int layout(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
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

static int ck_start(const char *path, char *err, int n) {
  if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0)) {
    snprintf(err, n, "ck sqlite3_open_v2: %s", sqlite3_errmsg(ckdb));
    if (ckdb) { sqlite3_close(ckdb); ckdb = NULL; }
    return -1;
  }
  sqlite3_busy_timeout(ckdb, 5000);
  if (sqlite3_exec(ckdb, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16", 0, 0, 0)) {
    snprintf(err, n, "ck sqlite3_exec: %s", sqlite3_errmsg(ckdb));
    sqlite3_close(ckdb);
    ckdb = NULL;
    return -1;
  }
  pthread_t t;
  int rc = pthread_create(&t, NULL, ck_main, 0);
  if (rc != 0) {
    snprintf(err, n, "ck pthread_create: %d (%s)", rc, strerror(rc));
    sqlite3_close(ckdb);
    ckdb = NULL;
    return -1;
  }
  return 0;
}

int td_open(const char *path, char *err, int n) {
  if (sqlite3_initialize()) return fail(err, n, "init");
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0))
    return fail(err, n, "open");
  if (exec("PRAGMA journal_mode = WAL") ||
      exec("PRAGMA synchronous = NORMAL") || exec("PRAGMA temp_store = MEMORY") ||
      exec("PRAGMA cache_size = -8192") || exec("PRAGMA mmap_size = 0") ||
      exec("PRAGMA busy_timeout = 5000") ||
      exec("PRAGMA wal_autocheckpoint = 0") || exec("SELECT count(*) FROM sqlite_schema"))
    return fail(err, n, "pragmas");
  sqlite3_wal_hook(db, wal_hook, 0);
  if (ck_start(path, err, n)) {
    return -1;
  }
  static const char *checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!layout(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  }
  if (ENC(db) != SQLITE_UTF8) {
    snprintf(err, n, "database text encoding is not UTF-8");
    return -1;
  }
  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_feed_idx = rootpage("posts_created_at_id_idx");
  root_likes_idx = rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed_idx || !root_likes_idx) return fail(err, n, "root pages");
  ki_feed = keyinfo(2, 1, 1);
  ki_likes = keyinfo(1, 1, 0);
  if (!ki_feed || !ki_likes) return fail(err, n, "keyinfo");
  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_unlike = prep("DELETE FROM likes WHERE user_id = ?1 AND post_id = ?2");
  st_check = prep("SELECT 1 FROM posts WHERE id = ?1");
  st_health = prep("SELECT 1");
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                 "FROM posts p JOIN users u ON u.id = p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  if (!st_begin || !st_commit || !st_rollback || !st_post || !st_like || !st_unlike || !st_check || !st_health || !st_feed)
    return fail(err, n, "prepare");
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) return fail(err, n, "cursor alloc");
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

int td_begin(void) {
  if (batch_broken) return -1;
  if (in_batch) {
    if (!sqlite3_get_autocommit(db)) return 0;
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

int td_commit(void) {
  if (batch_broken) {
    batch_broken = 0;
    close_cursors();
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  if (!in_batch) return 0;
  in_batch = 0;
  close_cursors();
  if (step_reset(st_commit) != SQLITE_DONE) {
    step_reset(st_rollback);
    return -1;
  }
  return 0;
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

void td_tick(void) {
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
    clock_gettime(CLOCK_MONOTONIC, &hold_t0);
    ck_set(CK_TAIL);
    break;
  case CK_TAIL_DONE:
    if (ck_full) wal_frames = 0;
    writes_held = 0;
    ck_set(CK_IDLE);
    break;
  default:
    if (writes_held && ms_since(&hold_t0) > HOLD_MAX_MS) {
      writes_held = 0;
      ck_set(CK_IDLE);
    }
    break;
  }
}

int td_writes_held(void) { return writes_held; }
int td_in_batch(void) { return in_batch; }

void td_rollback(void) {
  if (!in_batch) return;
  in_batch = 0;
  close_cursors();
  step_reset(st_rollback);
}

void *td_cursor(int i) { return cur[i]; }

int td_seek(BtCursor *c, i64 id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

static Mem seek_m;
static UnpackedRecord seek_r;
static int seek_inited;

int td_seek_ge(BtCursor *c, i64 k) {
  if (__builtin_expect(!seek_inited, 0)) {
    memset(&seek_m, 0, sizeof seek_m);
    memset(&seek_r, 0, sizeof seek_r);
    seek_m.flags = MEM_Int;
    seek_r.aMem = &seek_m;
    seek_r.nField = 1;
    seek_r.default_rc = +1;
    seek_inited = 1;
  }
  seek_m.u.i = k;
  seek_r.pKeyInfo = c->pKeyInfo;
  seek_r.eqSeen = 0;
  seek_r.errCode = 0;
  int res = 0;
  if (sqlite3BtreeIndexMoveto(c, &seek_r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

int td_count_likes(BtCursor *c, i64 lo, i64 hi, unsigned *n) {
  if (hi < lo || (hi - lo) > 1000) return 1;
  int r = td_seek_ge(c, lo);
  if (r <= 0) return r;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1;
    for (; i < last; i++) {
      u8 *cell = findCell(pg, i) + pg->childPtrSize;
      u32 len = cell[0];
      const u8 *p = cell + 1;
      if (len >= 0x80 || len < 3 || p + len > pg->aDataEnd) return 1;
      u32 h = p[0], t = p[1];
      if (h >= len || h < 3 || t >= 0x80) return 1;
      i64 v;
      if (t >= 1 && t <= 6) {
        static const u8 sz[7] = {0, 1, 2, 3, 4, 6, 8};
        u32 k = sz[t];
        if (h + k > len) return 1;
        const u8 *q = p + h;
        if (t == 4) {
          uint32_t raw;
          memcpy(&raw, q, 4);
          v = (int32_t)__builtin_bswap32(raw);
        } else if (t == 2) {
          uint16_t raw;
          memcpy(&raw, q, 2);
          v = (int16_t)__builtin_bswap16(raw);
        } else if (t == 1) {
          v = (int8_t)q[0];
        } else if (t == 3) {
          uint32_t raw = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8);
          v = ((int32_t)raw) >> 8;
        } else if (t == 6) {
          uint64_t raw;
          memcpy(&raw, q, 8);
          v = (int64_t)__builtin_bswap64(raw);
        } else {
          uint64_t raw = ((uint64_t)q[0] << 56) | ((uint64_t)q[1] << 48) | ((uint64_t)q[2] << 40) |
                         ((uint64_t)q[3] << 32) | ((uint64_t)q[4] << 24) | ((uint64_t)q[5] << 16);
          v = ((int64_t)raw) >> 16;
        }
      } else if (t == 8 || t == 9) {
        v = t - 8;
      } else {
        return 1;
      }
      if (v > hi) return 0;
      if (v >= lo && (v - lo) <= (hi - lo)) n[v - lo]++;
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

int td_first(BtCursor *c) {
  int empty = 1;
  if (sqlite3BtreeFirst(c, &empty) != SQLITE_OK) return -1;
  return !empty;
}

int td_next(BtCursor *c) {
  int rc = sqlite3BtreeNext(c, 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

const u8 *td_payload(BtCursor *c, u32 *len) {
  getCellInfo(c);
  u32 total = c->info.nPayload;
  if (c->info.nLocal == total) { *len = total; return c->info.pPayload; }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) { spill_cap = 0; return 0; }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return spill;
}

i64 td_rowid(BtCursor *c) { return sqlite3BtreeIntegerKey(c); }

static void check_tx(void) {
  if (in_batch && sqlite3_get_autocommit(db)) {
    in_batch = 0;
    batch_broken = 1;
  }
}

int td_insert_post(i64 user_id, const char *body, int len, i64 *id, char *created, int *created_len) {
  sqlite3_bind_int64(st_post, 1, user_id);
  sqlite3_bind_text(st_post, 2, body, len, SQLITE_STATIC);
  int rc = sqlite3_step(st_post);
  if (rc != SQLITE_ROW) { sqlite3_reset(st_post); check_tx(); return -1; }
  *id = sqlite3_column_int64(st_post, 0);
  int n = sqlite3_column_bytes(st_post, 1);
  const unsigned char *t = sqlite3_column_text(st_post, 1);
  if (!t || n > 63) { sqlite3_reset(st_post); return -1; }
  memcpy(created, t, n);
  *created_len = n;
  rc = sqlite3_step(st_post);
  sqlite3_reset(st_post);
  sqlite3_clear_bindings(st_post);
  return rc == SQLITE_DONE ? 0 : -1;
}

int td_insert_like(i64 user_id, i64 post_id) {
  sqlite3_bind_int64(st_like, 1, user_id);
  sqlite3_bind_int64(st_like, 2, post_id);
  int rc = step_reset(st_like);
  if (rc != SQLITE_DONE) { check_tx(); return -1; }
  return sqlite3_changes(db) > 0;
}

int td_delete_like(i64 user_id, i64 post_id) {
  sqlite3_bind_int64(st_unlike, 1, user_id);
  sqlite3_bind_int64(st_unlike, 2, post_id);
  int rc = step_reset(st_unlike);
  if (rc != SQLITE_DONE) { check_tx(); return -1; }
  return sqlite3_changes(db) > 0;
}

int td_health(char *err, int n) {
  int rc = sqlite3_step(st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
  sqlite3_reset(st_health);
  if (!ok) { snprintf(err, n, "%s", sqlite3_errmsg(db)); return -1; }
  return 0;
}

int td_feed_sql_step(i64 *id, const u8 **body, int *blen, const u8 **ca, int *clen,
                     const u8 **author, int *alen, i64 *likes) {
  int rc = sqlite3_step(st_feed);
  if (rc == SQLITE_DONE) { sqlite3_reset(st_feed); return 0; }
  if (rc != SQLITE_ROW) { sqlite3_reset(st_feed); return -1; }
  *id = sqlite3_column_int64(st_feed, 0);
  *body = sqlite3_column_text(st_feed, 1); *blen = sqlite3_column_bytes(st_feed, 1);
  *ca = sqlite3_column_text(st_feed, 2); *clen = sqlite3_column_bytes(st_feed, 2);
  *author = sqlite3_column_text(st_feed, 3); *alen = sqlite3_column_bytes(st_feed, 3);
  *likes = sqlite3_column_int64(st_feed, 4);
  return 1;
}

void td_feed_sql_reset(void) {
  sqlite3_reset(st_feed);
}

void td_close(void) {
  close_cursors();
  for (int i = 0; i < NCUR; i++) {
    if (cur[i]) { sqlite3_free(cur[i]); cur[i] = NULL; }
  }
  if (st_begin) { sqlite3_finalize(st_begin); st_begin = NULL; }
  if (st_commit) { sqlite3_finalize(st_commit); st_commit = NULL; }
  if (st_rollback) { sqlite3_finalize(st_rollback); st_rollback = NULL; }
  if (st_post) { sqlite3_finalize(st_post); st_post = NULL; }
  if (st_like) { sqlite3_finalize(st_like); st_like = NULL; }
  if (st_unlike) { sqlite3_finalize(st_unlike); st_unlike = NULL; }
  if (st_check) { sqlite3_finalize(st_check); st_check = NULL; }
  if (st_health) { sqlite3_finalize(st_health); st_health = NULL; }
  if (st_feed) { sqlite3_finalize(st_feed); st_feed = NULL; }
  if (ki_feed) { sqlite3KeyInfoUnref(ki_feed); ki_feed = NULL; }
  if (ki_likes) { sqlite3KeyInfoUnref(ki_likes); ki_likes = NULL; }
  if (spill) { sqlite3_free(spill); spill = NULL; spill_cap = 0; }
  if (ckdb) { sqlite3_close(ckdb); ckdb = NULL; }
  if (db) { sqlite3_close(db); db = NULL; }
  in_batch = 0;
  batch_broken = 0;
  writes_held = 0;
}
