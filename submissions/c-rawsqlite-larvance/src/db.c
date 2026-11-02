#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

static sqlite3 *db;
static Btree *db_bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;
static sqlite3_stmt *Q_health, *Q_like_insert, *Q_post_insert, *Q_feed_sql, *Q_post_sql, *Q_post_exists;

#define CKPT_FRAMES 1000
#define WAL_HIGH 16384

static sqlite3 *ckpt_db;
static pthread_mutex_t ckpt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_cv = PTHREAD_COND_INITIALIZER;
static int ckpt_wanted;
static int wal_frames;

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  if (frames >= CKPT_FRAMES && !__atomic_load_n(&ckpt_wanted, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&ckpt_mu);
    ckpt_wanted = 1;
    pthread_cond_signal(&ckpt_cv);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return SQLITE_OK;
}

static void *ckpt_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&ckpt_mu);
    while (!ckpt_wanted) pthread_cond_wait(&ckpt_cv, &ckpt_mu);
    pthread_mutex_unlock(&ckpt_mu);
    int nlog = 0, ndone = 0;
    int rc = sqlite3_wal_checkpoint_v2(ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    if (rc != SQLITE_OK && rc != SQLITE_BUSY) fprintf(stderr, "checkpoint: %s\n", sqlite3_errmsg(ckpt_db));
    __atomic_store_n(&ckpt_wanted, 0, __ATOMIC_RELEASE);
  }
  return 0;
}

static void ckpt_start(const char *path) {
  if (sqlite3_open_v2(path, &ckpt_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) {
    fprintf(stderr, "ckpt open failed\n"); exit(1);
  }
  sqlite3_exec(ckpt_db, "PRAGMA synchronous = NORMAL", 0, 0, 0);
  pthread_t t;
  if (pthread_create(&t, 0, ckpt_main, 0)) { perror("pthread_create"); exit(1); }
  pthread_detach(t);
}

static void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) {
    fprintf(stderr, "open failed\n"); exit(1);
  }
  sqlite3_exec(db, "PRAGMA journal_mode = WAL", 0, 0, 0);
  sqlite3_exec(db, "PRAGMA synchronous = NORMAL", 0, 0, 0);
  sqlite3_exec(db, "PRAGMA temp_store = MEMORY", 0, 0, 0);
  sqlite3_exec(db, "PRAGMA cache_size = -4096", 0, 0, 0);
  sqlite3_exec(db, "PRAGMA mmap_size = 1073741824", 0, 0, 0);
  sqlite3_wal_hook(db, wal_hook, 0);
  db_bt = db->aDb[0].pBt;
  sqlite3_prepare_v3(db, "BEGIN", -1, SQLITE_PREPARE_PERSISTENT, &st_begin, 0);
  sqlite3_prepare_v3(db, "COMMIT", -1, SQLITE_PREPARE_PERSISTENT, &st_commit, 0);
  sqlite3_prepare_v3(db, "ROLLBACK", -1, SQLITE_PREPARE_PERSISTENT, &st_rollback, 0);
  sqlite3_prepare_v3(db, "SELECT 1", -1, SQLITE_PREPARE_PERSISTENT, &Q_health, 0);
  sqlite3_prepare_v3(db, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING", -1, SQLITE_PREPARE_PERSISTENT, &Q_like_insert, 0);
  sqlite3_prepare_v3(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at", -1, SQLITE_PREPARE_PERSISTENT, &Q_post_insert, 0);
  sqlite3_prepare_v3(db, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20", -1, SQLITE_PREPARE_PERSISTENT, &Q_feed_sql, 0);
  sqlite3_prepare_v3(db, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &Q_post_sql, 0);
  sqlite3_prepare_v3(db, "SELECT 1 FROM posts WHERE id = ?1", -1, SQLITE_PREPARE_PERSISTENT, &Q_post_exists, 0);
  ckpt_start(path);
}

static inline int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void db_wal_bound(void) {
  if (wal_frames < WAL_HIGH || __atomic_load_n(&ckpt_wanted, __ATOMIC_ACQUIRE)) return;
  int nlog = 0, ndone = 0;
  int rc = sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
  if (rc == SQLITE_OK) wal_frames = 0;
}

static void db_batch_begin(void) {
  if (step_reset(st_begin) != SQLITE_DONE) abort();
  if (sqlite3BtreeBeginTrans(db_bt, 0, 0) != SQLITE_OK) abort();
}

static int db_batch_end(void) {
  if (step_reset(st_commit) == SQLITE_DONE) {
    db_wal_bound();
    return 0;
  }
  step_reset(st_rollback);
  return -1;
}

typedef struct { BtCursor *c; } cursor;

static cursor cursor_new(Pgno root, KeyInfo *ki) {
  cursor r;
  r.c = malloc((size_t)sqlite3BtreeCursorSize());
  if (!r.c) abort();
  sqlite3BtreeCursorZero(r.c);
  if (sqlite3BtreeCursor(db_bt, root, 0, ki, r.c) != SQLITE_OK) abort();
  return r;
}

static void cursor_close(cursor *c) {
  sqlite3BtreeCloseCursor(c->c);
  free(c->c);
  c->c = 0;
}

typedef struct { const u8 *p; u32 n; } rec_t;
static u8 *rec_spill;
static u32 rec_spill_cap;

static int cur_record(cursor *c, rec_t *r) {
  u32 total = sqlite3BtreePayloadSize(c->c);
  u32 local;
  const u8 *p = sqlite3BtreePayloadFetch(c->c, &local);
  if (LIKELY(local == total)) { r->p = p; r->n = total; return 0; }
  if (total > rec_spill_cap) {
    free(rec_spill);
    rec_spill_cap = total + 4096;
    if (!(rec_spill = malloc(rec_spill_cap))) abort();
  }
  if (sqlite3BtreePayload(c->c, 0, total, rec_spill) != SQLITE_OK) return -1;
  r->p = rec_spill; r->n = total;
  return 0;
}

static inline i64 cur_rowid(cursor *c) { return sqlite3BtreeIntegerKey(c->c); }

static int cur_last(cursor *c) {
  int empty;
  if (sqlite3BtreeLast(c->c, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}

static int cur_prev(cursor *c) {
  int rc = sqlite3BtreePrevious(c->c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

static int cur_next(cursor *c) {
  int rc = sqlite3BtreeNext(c->c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

static int cur_seek(cursor *c, i64 rowid) {
  int res;
  if (sqlite3BtreeTableMoveto(c->c, rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0 ? 0 : 1;
}

static int idx_seek_ge(cursor *c, KeyInfo *ki, i64 k) {
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

typedef struct { u64 t; const u8 *p; } col_t;

static inline u32 serial_len(u64 t) {
  static const u8 n[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? n[t] : (u32)((t - 12) / 2);
}

static inline const u8 *rd_varint_f(const u8 *p, u64 *v) {
  u8 b = *p++;
  if (b < 0x80) { *v = b; return p; }
  u64 x = b & 0x7f;
  for (int i = 0; i < 7; i++) {
    b = *p++;
    x = (x << 7) | (b & 0x7f);
    if (!(b & 0x80)) { *v = x; return p; }
  }
  b = *p++;
  *v = (x << 8) | b;
  return p;
}

static inline int rec_cols(const rec_t *r, int ncol, col_t *c) {
  const u8 *end = r->p + r->n;
  u64 hsz;
  const u8 *h = rd_varint_f(r->p, &hsz);
  if (!h || hsz > r->n) return -1;
  const u8 *hend = r->p + hsz, *body = hend;
  for (int i = 0; i < ncol; i++) {
    if (h >= hend) return -1;
    h = rd_varint_f(h, &c[i].t);
    if (!h) return -1;
    c[i].p = body;
    body += serial_len(c[i].t);
  }
  return body <= end ? 0 : -1;
}

static inline i64 col_int(const col_t *c) {
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

static Pgno db_rootpage(const char *name) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) abort();
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  Pgno r = sqlite3_step(s) == SQLITE_ROW ? (Pgno)sqlite3_column_int64(s, 0) : 0;
  sqlite3_finalize(s);
  if (!r) { fprintf(stderr, "fatal: no btree for %s\n", name); exit(1); }
  return r;
}

static KeyInfo *db_keyinfo(int nkey, int nextra) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) abort();
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  return k;
}
