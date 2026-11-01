#include "rt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *db;
static Btree *db_bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;
static sqlite3_stmt *st_feed, *st_post, *st_exists, *st_health, *st_like, *st_insert;
static Pgno root_posts, root_users, root_likes_idx;
static KeyInfo *ki_likes;
static BtCursor *c_posts, *c_users, *c_likes;
static int cursor_bytes;
static int native_ok, inv_ok;
static int64_t last_id;
static uint8_t last_created[40];
static uint32_t last_created_len;

static uint8_t *rec_spill;
static uint32_t rec_spill_cap;

static void db_die(const char *what) {
  fprintf(stderr, "fatal: %s: %s\n", what, db ? sqlite3_errmsg(db) : "no database");
  exit(1);
}
static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) db_die(sql);
  return s;
}
static void exec_sql(const char *sql) {
  char *err = 0;
  if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
    fprintf(stderr, "fatal: %s: %s\n", sql, err ? err : "");
    exit(1);
  }
}
static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}
static Pgno rootpage(const char *name) {
  sqlite3_stmt *s = prep("SELECT rootpage FROM sqlite_schema WHERE name=?1");
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  Pgno r = 0;
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  if (!r) { fprintf(stderr, "fatal: no root page for %s\n", name); exit(1); }
  return r;
}

/* ---- checkpoint thread: does not run on the request loop ---------------------------- */
#define CKPT_FRAMES 1000
#define WAL_HIGH 8192
static sqlite3 *ckpt_db;
static pthread_mutex_t ckpt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_cv = PTHREAD_COND_INITIALIZER;
static int ckpt_wanted;
static uint32_t wal_frames;

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = (uint32_t)frames;
  if (frames >= CKPT_FRAMES && !__atomic_load_n(&ckpt_wanted, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&ckpt_mu);
    ckpt_wanted = 1;
    pthread_cond_signal(&ckpt_cv);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return 0;
}
static void *ckpt_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&ckpt_mu);
    while (!ckpt_wanted) pthread_cond_wait(&ckpt_cv, &ckpt_mu);
    pthread_mutex_unlock(&ckpt_mu);
    int nlog = 0, ndone = 0;
    int rc = sqlite3_wal_checkpoint_v2(ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    if (rc != SQLITE_OK && rc != SQLITE_BUSY)
      fprintf(stderr, "checkpoint: %s\n", sqlite3_errmsg(ckpt_db));
    __atomic_store_n(&ckpt_wanted, 0, __ATOMIC_RELEASE);
  }
  return 0;
}

static void db_wal_bound(void) {
  if (wal_frames < WAL_HIGH || __atomic_load_n(&ckpt_wanted, __ATOMIC_ACQUIRE)) return;
  int nlog = 0, ndone = 0;
  int rc = sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_RESTART, &nlog, &ndone);
  if (rc == SQLITE_OK) wal_frames = 0;
}

void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK)
    db_die("open");
  exec_sql("PRAGMA journal_mode=WAL");
  exec_sql("PRAGMA synchronous=NORMAL");
  exec_sql("PRAGMA temp_store=MEMORY");
  exec_sql("PRAGMA cache_size=-65536");
  exec_sql("PRAGMA mmap_size=0");
  exec_sql("PRAGMA wal_autocheckpoint=0");
  exec_sql("PRAGMA busy_timeout=2000");
  sqlite3_wal_hook(db, wal_hook, 0);
  db_bt = db->aDb[0].pBt;
  if (!db_bt) db_die("btree");

  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_health = prep("SELECT 1");
  st_feed = prep(
    "SELECT p.id, p.body, p.created_at, u.username,"
    " (SELECT count(*) FROM likes l WHERE l.post_id=p.id)"
    " FROM posts p JOIN users u ON u.id=p.user_id"
    " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  st_post = prep(
    "SELECT p.id, p.body, p.created_at, u.username,"
    " (SELECT count(*) FROM likes l WHERE l.post_id=p.id)"
    " FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1");
  st_exists = prep("SELECT 1 FROM posts WHERE id=?1");
  st_like = prep("INSERT INTO likes(user_id, post_id) VALUES(?1, ?2) ON CONFLICT(user_id, post_id) DO NOTHING");
  st_insert = prep("INSERT INTO posts(user_id, body) VALUES(?1, ?2) RETURNING id, created_at");

  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_likes_idx = rootpage("likes_post_id_idx");
  ki_likes = sqlite3KeyInfoAlloc(db, 1, 1);
  if (!ki_likes || !db->pDfltColl) db_die("keyinfo");
  for (int i = 0; i < 2; i++) ki_likes->aColl[i] = db->pDfltColl;

  cursor_bytes = sqlite3BtreeCursorSize();
  c_posts = calloc(1, (size_t)cursor_bytes);
  c_users = calloc(1, (size_t)cursor_bytes);
  c_likes = calloc(1, (size_t)cursor_bytes);
  if (!c_posts || !c_users || !c_likes) abort();
  native_ok = 1;

  if (sqlite3_open_v2(path, &ckpt_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK)
    db_die("open checkpoint");
  sqlite3_exec(ckpt_db, "PRAGMA synchronous=NORMAL", 0, 0, 0);
  sqlite3_exec(ckpt_db, "PRAGMA busy_timeout=2000", 0, 0, 0);
  sqlite3_exec(ckpt_db, "SELECT 1 FROM sqlite_schema LIMIT 1", 0, 0, 0);
  pthread_t th;
  if (pthread_create(&th, 0, ckpt_main, 0)) { perror("pthread_create"); exit(1); }
  pthread_detach(th);
  fprintf(stderr, "sqlite: posts root %u, users root %u, likes_post_id_idx root %u\n",
          (unsigned)root_posts, (unsigned)root_users, (unsigned)root_likes_idx);
}

const char *db_errmsg(void) { return db ? sqlite3_errmsg(db) : "no database"; }
int db_fast_feed(void) { return native_ok && inv_ok; }
int db_health(void) { return step_reset(st_health) == SQLITE_ROW; }

int db_batch_begin(void) {
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(db_bt, 0, 0) != SQLITE_OK) return -1;
  return 0;
}
int db_batch_end(void) {
  if (step_reset(st_commit) == SQLITE_DONE) {
    db_wal_bound();
    return 0;
  }
  step_reset(st_rollback);
  return -1;
}

static int cursors_open(void) {
  sqlite3BtreeCursorZero(c_posts);
  sqlite3BtreeCursorZero(c_users);
  sqlite3BtreeCursorZero(c_likes);
  if (sqlite3BtreeCursor(db_bt, root_posts, 0, 0, c_posts) != SQLITE_OK) return -1;
  if (sqlite3BtreeCursor(db_bt, root_users, 0, 0, c_users) != SQLITE_OK) return -1;
  if (sqlite3BtreeCursor(db_bt, root_likes_idx, 0, ki_likes, c_likes) != SQLITE_OK) return -1;
  return 0;
}
static void cursors_close(void) {
  sqlite3BtreeCloseCursor(c_posts);
  sqlite3BtreeCloseCursor(c_users);
  sqlite3BtreeCloseCursor(c_likes);
}

typedef struct { const uint8_t *p; uint32_t n; } rec_t;
typedef struct { uint64_t t; const uint8_t *p; } col_t;

static const uint8_t *rd_varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
  uint64_t x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= end) return 0;
    uint8_t b = *p++;
    x = (x << 7) | (uint64_t)(b & 0x7f);
    if (!(b & 0x80)) { *v = x; return p; }
  }
  if (p >= end) return 0;
  *v = (x << 8) | *p++;
  return p;
}
static uint32_t serial_len(uint64_t t) {
  static const uint8_t n[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? n[t] : (uint32_t)((t - 12) / 2);
}
static int cur_record(BtCursor *c, rec_t *r) {
  uint32_t total = sqlite3BtreePayloadSize(c);
  uint32_t local = 0;
  const uint8_t *p = sqlite3BtreePayloadFetch(c, &local);
  if (local == total && p) { r->p = p; r->n = total; return 0; }
  if (total > rec_spill_cap) {
    free(rec_spill);
    rec_spill_cap = total + 256;
    rec_spill = malloc(rec_spill_cap);
    if (!rec_spill) return -1;
  }
  if (sqlite3BtreePayload(c, 0, total, rec_spill) != SQLITE_OK) return -1;
  r->p = rec_spill;
  r->n = total;
  return 0;
}
static int rec_cols(const rec_t *r, int ncol, col_t *c) {
  const uint8_t *end = r->p + r->n;
  uint64_t hsz = 0;
  const uint8_t *h = rd_varint(r->p, end, &hsz);
  if (!h || hsz > r->n) return -1;
  const uint8_t *hend = r->p + hsz;
  const uint8_t *body = hend;
  for (int i = 0; i < ncol; i++) {
    if (h >= hend) return -1;
    h = rd_varint(h, hend, &c[i].t);
    if (!h) return -1;
    c[i].p = body;
    uint32_t n = serial_len(c[i].t);
    if (body + n > end) return -1;
    body += n;
  }
  return 0;
}
static int col_is_int(const col_t *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static int col_is_text(const col_t *c) { return c->t >= 13 && (c->t & 1); }
static int64_t col_int(const col_t *c) {
  const uint8_t *p = c->p;
  switch (c->t) {
  case 1: return (int8_t)p[0];
  case 2: return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
  case 3: return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)) >> 8;
  case 4: return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
  case 5: {
    uint64_t hi = ((uint64_t)p[0] << 8) | p[1];
    uint64_t lo = ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 8) | p[5];
    int64_t v = (int64_t)((hi << 32) | lo);
    if (hi & 0x8000) v |= (int64_t)((~0ULL) << 48);
    return v;
  }
  case 6: {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return (int64_t)v;
  }
  case 8: return 0;
  case 9: return 1;
  default: return 0;
  }
}
static int cur_last(BtCursor *c) {
  int empty = 0;
  if (sqlite3BtreeLast(c, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}
static int cur_first(BtCursor *c) {
  int empty = 0;
  if (sqlite3BtreeFirst(c, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}
static int cur_prev(BtCursor *c) {
  int rc = sqlite3BtreePrevious(c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static int cur_next(BtCursor *c) {
  int rc = sqlite3BtreeNext(c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static int cur_seek(BtCursor *c, int64_t rowid) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0 ? 0 : 1;
}
static int idx_seek_ge(int64_t k) {
  Mem m;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  UnpackedRecord r;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = ki_likes;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = 1;
  int res = 0;
  if (sqlite3BtreeIndexMoveto(c_likes, &r, &res) != SQLITE_OK) return -1;
  if (res < 0) return cur_next(c_likes);
  return sqlite3BtreeEof(c_likes) ? 1 : 0;
}
static int text_cmp(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn) {
  uint32_t m = an < bn ? an : bn;
  int c = memcmp(a, b, m);
  if (c) return c;
  return an < bn ? -1 : an > bn ? 1 : 0;
}

typedef struct {
  int64_t id, uid, likes;
  uint32_t body_len, created_len, author_len;
  uint8_t body[2048];
  uint8_t created[40];
  uint8_t author[256];
} row_t;

static int copy_text(const col_t *c, uint8_t *dst, uint32_t cap, uint32_t *out) {
  if (!col_is_text(c)) return -1;
  uint32_t n = serial_len(c->t);
  if (n > cap) return -1;
  memcpy(dst, c->p, n);
  *out = n;
  return 0;
}
static int load_post_at_cursor(row_t *row) {
  rec_t r;
  col_t c[4];
  if (cur_record(c_posts, &r) || rec_cols(&r, 4, c)) return -1;
  if (!col_is_int(&c[1])) return -1;
  row->id = sqlite3BtreeIntegerKey(c_posts);
  row->uid = col_int(&c[1]);
  if (copy_text(&c[2], row->body, sizeof row->body, &row->body_len)) return -1;
  if (copy_text(&c[3], row->created, sizeof row->created, &row->created_len)) return -1;
  return 0;
}
static int load_author(row_t *row) {
  int rc = cur_seek(c_users, row->uid);
  if (rc) return rc ? -1 : -1;
  rec_t r;
  col_t c[3];
  if (cur_record(c_users, &r) || rec_cols(&r, 3, c) || copy_text(&c[1], row->author, sizeof row->author, &row->author_len))
    return -1;
  return 0;
}
static int range_counts(int64_t lo, int64_t hi, int64_t *counts) {
  int64_t span = hi - lo + 1;
  if (span <= 0 || span > 64) return -1;
  memset(counts, 0, sizeof(int64_t) * (size_t)span);
  int rc = idx_seek_ge(lo);
  while (rc == 0) {
    rec_t r;
    col_t c[1];
    if (cur_record(c_likes, &r) || rec_cols(&r, 1, c) || !col_is_int(&c[0])) return -1;
    int64_t pid = col_int(&c[0]);
    if (pid > hi) return 0;
    if (pid >= lo) counts[pid - lo]++;
    rc = cur_next(c_likes);
  }
  return rc < 0 ? -1 : 0;
}

static int feed_tail(out_t *o) {
  row_t rows[20];
  int rc = cur_last(c_posts);
  if (rc < 0) return -1;
  out_lit(o, "{\"posts\":[", 10);
  if (rc == 0) {
    int n = 0;
    for (;;) {
      if (load_post_at_cursor(&rows[n])) return -1;
      n++;
      if (n == 20) break;
      int64_t prev = rows[n - 1].id;
      int st = cur_prev(c_posts);
      if (st < 0) return -1;
      if (st == 1) break;
      if (sqlite3BtreeIntegerKey(c_posts) != prev - 1) return -1;
    }
    for (int i = 1; i < n; i++)
      if (text_cmp(rows[i - 1].created, rows[i - 1].created_len, rows[i].created, rows[i].created_len) < 0)
        return -1;
    int64_t hi = rows[0].id, lo = rows[n - 1].id;
    if (hi - lo + 1 != n) return -1;
    int64_t counts[20];
    if (range_counts(lo, hi, counts)) return -1;
    for (int i = 0; i < n; i++) {
      rows[i].likes = counts[rows[i].id - lo];
      if (load_author(&rows[i])) return -1;
      if (i) out_lit(o, ",", 1);
      put_post(o, rows[i].id, rows[i].body, rows[i].body_len, rows[i].created, rows[i].created_len,
               rows[i].author, rows[i].author_len, rows[i].likes);
    }
  }
  out_lit(o, "]}", 2);
  return 0;
}
static int emit_sql_row(out_t *o, sqlite3_stmt *s) {
  const uint8_t *body = sqlite3_column_text(s, 1);
  const uint8_t *created = sqlite3_column_text(s, 2);
  const uint8_t *author = sqlite3_column_text(s, 3);
  if (!body || !created || !author) return -1;
  put_post(o, sqlite3_column_int64(s, 0), body, (uint32_t)sqlite3_column_bytes(s, 1),
           created, (uint32_t)sqlite3_column_bytes(s, 2),
           author, (uint32_t)sqlite3_column_bytes(s, 3),
           sqlite3_column_int64(s, 4));
  return 0;
}
static int feed_sql(out_t *o) {
  out_lit(o, "{\"posts\":[", 10);
  int i = 0, rc;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
    if (i++) out_lit(o, ",", 1);
    if (emit_sql_row(o, st_feed)) { sqlite3_reset(st_feed); return -1; }
  }
  sqlite3_reset(st_feed);
  if (rc != SQLITE_DONE) return -1;
  out_lit(o, "]}", 2);
  return 0;
}
int db_write_feed(out_t *o) {
  if (native_ok && inv_ok) {
    out_begin(o);
    if (feed_tail(o) == 0) return 0;
    out_abort(o);
  }
  out_begin(o);
  if (feed_sql(o) == 0) return 0;
  out_abort(o);
  return -1;
}

static int post_native(out_t *o, int64_t id) {
  int rc = cur_seek(c_posts, id);
  if (rc) return rc;
  row_t row;
  if (load_post_at_cursor(&row)) return -1;
  if (load_author(&row)) return -1;
  int64_t likes = 0;
  if (range_counts(id, id, &likes)) return -1;
  out_lit(o, "{\"post\":", 8);
  put_post(o, row.id, row.body, row.body_len, row.created, row.created_len, row.author, row.author_len, likes);
  out_lit(o, "}", 1);
  return 0;
}
static int post_sql(out_t *o, int64_t id) {
  sqlite3_reset(st_post);
  sqlite3_bind_int64(st_post, 1, id);
  int rc = sqlite3_step(st_post);
  if (rc == SQLITE_ROW) {
    out_lit(o, "{\"post\":", 8);
    if (emit_sql_row(o, st_post)) { sqlite3_reset(st_post); return -1; }
    out_lit(o, "}", 1);
  }
  sqlite3_reset(st_post);
  return rc == SQLITE_ROW ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
int db_write_post(out_t *o, int64_t id) {
  if (native_ok) {
    out_begin(o);
    int rc = post_native(o, id);
    if (rc == 0) return 0;
    out_abort(o);
    if (rc > 0) return 1;
  }
  out_begin(o);
  int rc = post_sql(o, id);
  if (rc == 0) return 0;
  out_abort(o);
  return rc;
}

int db_post_exists(int64_t id) {
  sqlite3_reset(st_exists);
  sqlite3_bind_int64(st_exists, 1, id);
  int rc = sqlite3_step(st_exists);
  sqlite3_reset(st_exists);
  return rc == SQLITE_ROW ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
int db_insert_post(int64_t uid, const uint8_t *body, uint32_t blen, int64_t *id,
                   uint8_t *created, uint32_t *clen) {
  sqlite3_reset(st_insert);
  sqlite3_bind_int64(st_insert, 1, uid);
  sqlite3_bind_text(st_insert, 2, (const char *)body, (int)blen, SQLITE_STATIC);
  int rc = sqlite3_step(st_insert);
  if (rc != SQLITE_ROW) { sqlite3_reset(st_insert); return -1; }
  *id = sqlite3_column_int64(st_insert, 0);
  const uint8_t *ct = sqlite3_column_text(st_insert, 1);
  int n = sqlite3_column_bytes(st_insert, 1);
  if (!ct || n <= 0 || n > 39) { sqlite3_reset(st_insert); return -1; }
  memcpy(created, ct, (size_t)n);
  created[n] = 0;
  *clen = (uint32_t)n;
  sqlite3_reset(st_insert);
  if (inv_ok) {
    if (*id != last_id + 1 || text_cmp(created, *clen, last_created, last_created_len) < 0) inv_ok = 0;
    else {
      last_id = *id;
      memcpy(last_created, created, (size_t)n);
      last_created_len = (uint32_t)n;
    }
  }
  return 0;
}
int db_insert_like(int64_t uid, int64_t post_id, int *already) {
  sqlite3_reset(st_like);
  sqlite3_bind_int64(st_like, 1, uid);
  sqlite3_bind_int64(st_like, 2, post_id);
  int rc = sqlite3_step(st_like);
  int ch = sqlite3_changes(db);
  sqlite3_reset(st_like);
  if (rc != SQLITE_DONE) return -1;
  *already = ch != 1;
  return 0;
}

/* Cursors for one batch. Reads call these; app_batch brackets them. */
int db_cursors_open(void) { return native_ok ? cursors_open() : 0; }
void db_cursors_close(void) { if (native_ok) cursors_close(); }

void db_native_off(void) { native_ok = inv_ok = 0; }

void db_check_inv(void) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (db_batch_begin()) { native_ok = inv_ok = 0; return; }
  if (cursors_open()) {
    cursors_close();
    db_batch_end();
    native_ok = inv_ok = 0;
    return;
  }
  int ok = 1;
  int64_t expect = 1;
  uint8_t prev[40];
  uint32_t prev_n = 0;
  int have = 0;
  int rc = cur_first(c_posts);
  while (ok && rc == 0) {
    row_t row;
    if (sqlite3BtreeIntegerKey(c_posts) != expect || load_post_at_cursor(&row)) { ok = 0; break; }
    if (have && text_cmp(prev, prev_n, row.created, row.created_len) > 0) { ok = 0; break; }
    memcpy(prev, row.created, row.created_len);
    prev_n = row.created_len;
    have = 1;
    expect++;
    rc = cur_next(c_posts);
  }
  if (rc < 0) ok = 0;
  cursors_close();
  db_batch_end();
  inv_ok = ok;
  if (ok && have) {
    last_id = expect - 1;
    memcpy(last_created, prev, prev_n);
    last_created_len = prev_n;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
  fprintf(stderr, "invariant: %s (%lld posts, %.0f ms)\n", ok ? "holds" : "off, feed uses SQL",
          (long long)(expect - 1), ms);
}
