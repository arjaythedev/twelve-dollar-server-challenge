/* Storage primitives over SQLite. Compiled in the same translation unit as the amalgamation
 * (server.c includes sqlite3.c first), so reads can use SQLite's B-tree cursors directly: the
 * pager, WAL, locking and page cache are SQLite's own, only the bytecode engine is skipped.
 * Writes are prepared SQL statements.
 *
 * Every primitive is called from Lean (Feed/Store.lean) and has a one-line contract there,
 * stated over the list model of the tables (Feed/Model.lean). The primitives decide nothing
 * about the API: which rows a response needs, how rows are joined, counted and rendered is
 * Lean code. */
#include "rt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/prctl.h>

static sqlite3 *db;
static Btree *db_bt;
static const char *db_vfs;   /* "feedwal" (runtime/walvfs.c), or SQLite's default */
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_health, *st_like, *st_post;
static Pgno root_posts, root_users, root_feed_idx, root_likes_post_idx;
static KeyInfo *ki_feed_idx, *ki_likes_post_idx;
static BtCursor *c_posts, *c_users, *c_feed_idx, *c_likes, *c_exists;
static int reads_open;
static void pins_release(void);
LEAN_EXPORT lean_obj_res feed_db_commit(void);

static void db_die(const char *what) {
  fprintf(stderr, "fatal: %s: %s\n", what, db ? sqlite3_errmsg(db) : "no database");
  exit(1);
}
static sqlite3_stmt *db_prepare(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) db_die(sql);
  return s;
}
static void db_exec(const char *sql) {
  char *err = 0;
  if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "fatal: %s: %s\n", sql, err); exit(1); }
}
static Pgno db_root(const char *name) {
  sqlite3_stmt *s = db_prepare("SELECT rootpage FROM sqlite_schema WHERE name = ?1");
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  Pgno r = sqlite3_step(s) == SQLITE_ROW ? (Pgno)sqlite3_column_int64(s, 0) : 0;
  sqlite3_finalize(s);
  if (!r) { fprintf(stderr, "fatal: no b-tree for %s\n", name); exit(1); }
  return r;
}
static KeyInfo *db_keyinfo(int nkey, int nextra) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) db_die("keyinfo");
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  return k;
}

/* The schema these primitives read is fixed by the challenge (schema.sql). Check the column
 * layout once at startup so a different file is refused rather than misread. */
static void db_check_layout(void) {
  static const struct { const char *sql, *want; } checks[] = {
      {"SELECT group_concat(name || ':' || type || ':' || pk, ',') FROM pragma_table_info('posts')",
       "id:INTEGER:1,user_id:INTEGER:0,body:TEXT:0,created_at:TEXT:0"},
      {"SELECT group_concat(name || ':' || type || ':' || pk, ',') FROM pragma_table_info('users')",
       "id:INTEGER:1,username:TEXT:0,created_at:TEXT:0"},
      {"SELECT group_concat(name || ':' || desc, ',') FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key",
       "created_at:1,id:1"},
      {"SELECT group_concat(name, ',') FROM pragma_index_xinfo('likes_post_id_idx')", "post_id"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    sqlite3_stmt *s = db_prepare(checks[i].sql);
    const char *got = sqlite3_step(s) == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 0) : 0;
    if (!got || strcmp(got, checks[i].want)) {
      fprintf(stderr, "fatal: schema layout: got '%s', expected '%s'\n", got ? got : "(null)", checks[i].want);
      exit(1);
    }
    sqlite3_finalize(s);
  }
}

/* ---- WAL checkpoints on a second thread ------------------------------------------------
 * A checkpoint copies committed pages into the database file and fsyncs it. On the request
 * thread that would stall every connection for the length of an fsync, so a second thread with
 * its own connection runs PASSIVE checkpoints. Durability does not depend on them (a commit is
 * in the WAL, synchronous=NORMAL). The WAL restarts from the beginning only when a writer finds
 * it fully checkpointed, so after a checkpoint that copied everything the request thread's next
 * commit starts the log over and it stays small. */
static int CKPT_FRAMES = 500;      /* FEED_CKPT_FRAMES overrides it (for measurements) */
static int WAL_PAUSE_FRAMES = 8192;   /* see db_writes_ok; FEED_WAL_PAUSE_FRAMES overrides it (for measurements) */
static int pause_at = 8192;
/* On one vCPU a checkpoint pass at the request thread's priority takes the CPU from the loop for
 * the length of the pass (c-iouring-pavlyhalim #245 measured 300-430 ms on the benchmark box); at
 * nice 19 it runs while the loop sleeps between batches. FEED_CKPT_NICE overrides it. */
static int ckpt_nice = 19;
static u64_t WAL_PAUSE_NS = 300000000ull;  /* the longest write pause (below); FEED_PAUSE_MS overrides it */
static sqlite3 *ckpt_db;
static pthread_mutex_t ckpt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_cv = PTHREAD_COND_INITIALIZER;
static int ckpt_wanted;
static volatile int wal_frames;
static int ckpt_full;          /* the last checkpoint copied every frame of the log */

static void ckpt_wake(void) {
  if (__atomic_load_n(&ckpt_wanted, __ATOMIC_RELAXED)) return;
  pthread_mutex_lock(&ckpt_mu);
  ckpt_wanted = 1;
  pthread_cond_signal(&ckpt_cv);
  pthread_mutex_unlock(&ckpt_mu);
}

/* Background checkpoints run each time the log has grown by CKPT_FRAMES since the last one. */
static int ckpt_next = 500;
static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  if (frames < ckpt_next - CKPT_FRAMES) ckpt_next = CKPT_FRAMES;   /* the log restarted */
  if (frames >= ckpt_next) { ckpt_next = frames + CKPT_FRAMES; ckpt_wake(); }
  return SQLITE_OK;
}
static void *ckpt_main(void *arg) {
  (void)arg;
  if (ckpt_nice && nice(ckpt_nice) == -1) {}
  for (;;) {
    pthread_mutex_lock(&ckpt_mu);
    while (!ckpt_wanted) pthread_cond_wait(&ckpt_cv, &ckpt_mu);
    pthread_mutex_unlock(&ckpt_mu);
    int nlog = 0, ndone = 0;
    int rc = sqlite3_wal_checkpoint_v2(ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    if (rc != SQLITE_OK && rc != SQLITE_BUSY) fprintf(stderr, "checkpoint: %s\n", sqlite3_errmsg(ckpt_db));
    if (rc == SQLITE_OK && nlog >= 0 && ndone == nlog) __atomic_store_n(&ckpt_full, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&ckpt_wanted, 0, __ATOMIC_RELEASE);
  }
  return 0;
}
static void ckpt_start(const char *path) {
  if (getenv("FEED_CKPT_FRAMES")) CKPT_FRAMES = atoi(getenv("FEED_CKPT_FRAMES"));
  if (getenv("FEED_CKPT_NICE")) ckpt_nice = atoi(getenv("FEED_CKPT_NICE"));
  if (getenv("FEED_PAUSE_MS")) WAL_PAUSE_NS = (u64_t)atoll(getenv("FEED_PAUSE_MS")) * 1000000ull;
  if (getenv("FEED_WAL_PAUSE_FRAMES")) pause_at = WAL_PAUSE_FRAMES = atoi(getenv("FEED_WAL_PAUSE_FRAMES"));
  ckpt_next = CKPT_FRAMES;
  if (sqlite3_open_v2(path, &ckpt_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, db_vfs) != SQLITE_OK) db_die("open (checkpointer)");
  sqlite3_exec(ckpt_db, "PRAGMA synchronous = NORMAL; PRAGMA cache_size = -2048; SELECT count(*) FROM users WHERE id = 1", 0, 0, 0);
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 1 << 20);
  if (pthread_create(&t, &at, ckpt_main, 0)) { perror("pthread_create"); exit(1); }
  pthread_detach(t);
}

/* ---- page cache: SQLite's own, behind its plugin interface (SQLITE_CONFIG_PCACHE2) ----------
 * With one change: at commit SQLite drops the cached pages past the end of the database
 * (xTruncate). The default cache scans its whole hash table for them whenever a key at or past
 * that limit was ever seen, and balance_nonroot parks a page under the lock-byte page's number
 * for a moment when it reorders new pages, so after every page split the next commit would scan
 * the whole cache. This wrapper tracks the largest key that a page of the cache actually holds
 * (the parked key is not one: the page moves on at once) and passes xTruncate on only when that
 * key is at or past the limit. */
typedef struct { sqlite3_pcache *in; unsigned top, parked; } pc_t;
static sqlite3_pcache_methods2 pc_in;
static sqlite3_pcache *pc_create(int szPage, int szExtra, int bPurgeable) {
  pc_t *c = malloc(sizeof *c);
  if (!c) return 0;
  if (!(c->in = pc_in.xCreate(szPage, szExtra, bPurgeable))) { free(c); return 0; }
  c->top = 0;
  c->parked = (unsigned)(PENDING_BYTE / szPage) + 1;   /* the lock-byte page's number */
  return (sqlite3_pcache *)c;
}
static void pc_cachesize(sqlite3_pcache *p, int n) { pc_in.xCachesize(((pc_t *)p)->in, n); }
static int pc_pagecount(sqlite3_pcache *p) { return pc_in.xPagecount(((pc_t *)p)->in); }
static sqlite3_pcache_page *pc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  pc_t *c = (pc_t *)p;
  sqlite3_pcache_page *r = pc_in.xFetch(c->in, key, create);
  if (r && key > c->top) c->top = key;
  return r;
}
static void pc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) { pc_in.xUnpin(((pc_t *)p)->in, pg, discard); }
static void pc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned old, unsigned new) {
  pc_t *c = (pc_t *)p;
  pc_in.xRekey(c->in, pg, old, new);
  if (new > c->top && new != c->parked) c->top = new;
}
static void pc_truncate(sqlite3_pcache *p, unsigned limit) {
  pc_t *c = (pc_t *)p;
  if (limit > c->top) return;   /* no page of the cache has a key at or past the limit */
  pc_in.xTruncate(c->in, limit);
  c->top = limit ? limit - 1 : 0;
}
static void pc_destroy(sqlite3_pcache *p) { pc_in.xDestroy(((pc_t *)p)->in); free(p); }
static void pc_shrink(sqlite3_pcache *p) { pc_in.xShrink(((pc_t *)p)->in); }
static void pc_install(void) {
  if (getenv("FEED_PLAIN_PCACHE")) return;
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pc_in) != SQLITE_OK) return;
  static sqlite3_pcache_methods2 m;
  m = pc_in;
  m.xCreate = pc_create;
  m.xCachesize = pc_cachesize;
  m.xPagecount = pc_pagecount;
  m.xFetch = pc_fetch;
  m.xUnpin = pc_unpin;
  m.xRekey = pc_rekey;
  m.xTruncate = pc_truncate;
  m.xDestroy = pc_destroy;
  m.xShrink = pc_shrink;
  sqlite3_config(SQLITE_CONFIG_PCACHE2, &m);
}

static void db_open(const char *path) {
  /* no transparent huge pages for this process: Lean's allocator advises its 1 GiB arena for them,
   * and a fault there may then compact memory synchronously, stalling the event loop for as long
   * as that takes when memory is fragmented (it mostly is: the kernel's socket objects fill it) */
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  pc_install();
  /* the request thread is not the process's main thread, so glibc gives it an arena that it
   * grows and shrinks with mprotect as SQLite allocates and frees pages: keep what it got */
  mallopt(M_TRIM_THRESHOLD, 512 << 20);
  mallopt(M_TOP_PAD, 16 << 20);
  if (!getenv("FEED_PLAIN_VFS")) { walvfs_register(); db_vfs = "feedwal"; }
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, db_vfs) != SQLITE_OK) db_die("open");
  db_exec("PRAGMA journal_mode = WAL");
  db_exec("PRAGMA synchronous = NORMAL");
  db_exec("PRAGMA temp_store = MEMORY");
  /* SQLite's page cache is memory the kernel cannot take back. On a 2 GB machine the kernel's
   * memory per open connection (3.6 KB) sets a wall near 350,000 connections, and every megabyte
   * here is about 290 connections; a page the cache misses is a pread from the kernel's page
   * cache. FEED_CACHE_KB overrides the size (for measurements). */
  {
    char q[64];
    snprintf(q, sizeof q, "PRAGMA cache_size = -%ld", getenv("FEED_CACHE_KB") ? atol(getenv("FEED_CACHE_KB")) : 8192L);
    db_exec(q);
  }
  if (getenv("FEED_MMAP")) { char q[64]; snprintf(q, sizeof q, "PRAGMA mmap_size = %s", getenv("FEED_MMAP")); db_exec(q); }
  else db_exec("PRAGMA mmap_size = 0");      /* page-cache hits are cheaper than mapped pages in WAL mode */
  db_exec("PRAGMA wal_autocheckpoint = 0");  /* checkpoints run on their own thread */
  sqlite3_wal_hook(db, wal_hook, 0);
  db_check_layout();
  db_bt = db->aDb[0].pBt;
  root_posts = db_root("posts");
  root_users = db_root("users");
  root_feed_idx = db_root("posts_created_at_id_idx");
  root_likes_post_idx = db_root("likes_post_id_idx");
  ki_feed_idx = db_keyinfo(2, 1);
  ki_feed_idx->aSortFlags[0] = KEYINFO_ORDER_DESC;
  ki_feed_idx->aSortFlags[1] = KEYINFO_ORDER_DESC;
  ki_likes_post_idx = db_keyinfo(1, 1);
  st_begin = db_prepare("BEGIN");
  st_commit = db_prepare("COMMIT");
  st_rollback = db_prepare("ROLLBACK");
  st_health = db_prepare("SELECT 1");
  /* OR IGNORE: for these values (never NULL; a body the handler checked) the only constraint that
   * can fail is the like's primary key, which it ignores as ON CONFLICT DO NOTHING would; and no
   * constraint can abort the statement, so SQLite needs no statement journal. created_at is bound
   * in the schema default's format (db_now). */
  st_like = db_prepare("INSERT OR IGNORE INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3)");
  st_post = db_prepare("INSERT OR IGNORE INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)");
  c_posts = malloc((size_t)sqlite3BtreeCursorSize());
  c_users = malloc((size_t)sqlite3BtreeCursorSize());
  c_feed_idx = malloc((size_t)sqlite3BtreeCursorSize());
  c_likes = malloc((size_t)sqlite3BtreeCursorSize());
  c_exists = malloc((size_t)sqlite3BtreeCursorSize());
  if (!c_posts || !c_users || !c_feed_idx || !c_likes || !c_exists) abort();
  ckpt_start(path);
}

/* Keeping the log bounded. SQLite starts the WAL over only when a write transaction begins
 * after every frame was copied into the database; with a write in every batch that moment never
 * comes and the log (and its page cache) grows without bound. So once the log passes
 * WAL_PAUSE_FRAMES, new writes wait (reads go on) until a checkpoint has copied the whole log;
 * the next write then restarts it. Checkpoints run every CKPT_FRAMES frames, so little is left
 * to copy when a pause begins. A pause ends when that checkpoint's fsync does, at most after
 * WAL_PAUSE_NS; after a pause that timed out the next one waits for another WAL_PAUSE_FRAMES / 4
 * frames. On a cloud volume capped at 3,000 writes per second, at 320,000 users a checkpoint's fsync
 * took longer than 30 ms: with a 30 ms limit every pause timed out, the log never started over
 * and grew to 105,000 frames during a 5-minute hold, and its page cache with it. With writes
 * limited the same way in the local harness, a 300 ms limit kept the log under 9,000 frames and
 * no pause reached it. On a fast disk a pause takes a few milliseconds. */
static int writes_paused;
static u64_t pause_start;
static u64_t st_pauses, st_pause_timeouts;
static int db_writes_ok(u64_t now_ns) {
  if (!writes_paused) {
    if (wal_frames < WAL_PAUSE_FRAMES) pause_at = WAL_PAUSE_FRAMES;
    if (wal_frames >= pause_at) {
      writes_paused = 1;
      pause_start = now_ns;
      __atomic_store_n(&ckpt_full, 0, __ATOMIC_RELEASE);
      ckpt_wake();
      st_pauses++;
    }
  } else if (__atomic_load_n(&ckpt_full, __ATOMIC_ACQUIRE)) {
    writes_paused = 0;
    wal_frames = 0;   /* the next commit reports the size of the (restarted) log */
  } else if (now_ns - pause_start > WAL_PAUSE_NS) {
    writes_paused = 0;
    pause_at = wal_frames + WAL_PAUSE_FRAMES / 4;
    st_pause_timeouts++;
  } else ckpt_wake();
  return !writes_paused;
}

static void db_log_error(const char *what) {
  static time_t last;
  static int n;
  time_t t = time(0);
  if (t != last) { last = t; n = 0; }
  if (n++ < 5) fprintf(stderr, "sqlite: %s: %s (%d)\n", what, sqlite3_errmsg(db), sqlite3_extended_errcode(db));
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

/* ---- record decoding (https://sqlite.org/fileformat2.html#record_format) ---------------- */
typedef struct { u64_t t; const u8_t *p; } col_t;
static inline const u8_t *rd_varint(const u8_t *p, const u8_t *end, u64_t *v) {
  u64_t x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= end) return 0;
    x = x << 7 | (p[0] & 0x7f);
    if (!(*p++ & 0x80)) { *v = x; return p; }
  }
  if (p >= end) return 0;
  *v = x << 8 | *p++;
  return p;
}
static inline u32_t serial_len(u64_t t) {
  static const u8_t n[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? n[t] : (u32_t)((t - 12) / 2);
}
static int rec_cols(const u8_t *rec, u32_t n, int ncol, col_t *c) {
  const u8_t *end = rec + n;
  u64_t hsz;
  const u8_t *h = rd_varint(rec, end, &hsz);
  if (!h || hsz > n) return -1;
  const u8_t *hend = rec + hsz, *body = hend;
  for (int i = 0; i < ncol; i++) {
    if (h >= hend) return -1;
    h = rd_varint(h, hend, &c[i].t);
    if (!h) return -1;
    c[i].p = body;
    body += serial_len(c[i].t);
  }
  return body <= end ? 0 : -1;
}
static inline int col_is_int(const col_t *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static inline int col_is_text(const col_t *c) { return c->t >= 13 && (c->t & 1); }
static inline i64_t col_int(const col_t *c) {
  const u8_t *p = c->p;
  switch (c->t) {
  case 1: return (int8_t)p[0];
  case 2: return (int16_t)((u16_t)p[0] << 8 | p[1]);
  case 3: return (int32_t)((u32_t)p[0] << 24 | (u32_t)p[1] << 16 | (u32_t)p[2] << 8) >> 8;
  case 4: return (int32_t)((u32_t)p[0] << 24 | (u32_t)p[1] << 16 | (u32_t)p[2] << 8 | p[3]);
  case 5: return (i64_t)((u64_t)(int16_t)((u16_t)p[0] << 8 | p[1]) << 32 | (u64_t)((u32_t)p[2] << 24 | (u32_t)p[3] << 16 | (u32_t)p[4] << 8 | p[5]));
  case 6: { u64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return (i64_t)v; }
  case 9: return 1;
  default: return 0;
  }
}

/* Payload of the cursor's current cell. Records that spill onto overflow pages are copied. */
static u8_t *spill;
static u32_t spill_cap;
static const u8_t *cur_payload(BtCursor *c, u32_t *n) {
  u32_t total = sqlite3BtreePayloadSize(c), local;
  const u8_t *p = sqlite3BtreePayloadFetch(c, &local);
  if (LIKELY(local == total)) { *n = total; return p; }
  if (total > spill_cap) {
    free(spill);
    spill_cap = total + 4096;
    if (!(spill = malloc(spill_cap))) abort();
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *n = total;
  return spill;
}

/* ---- Lean objects built here ------------------------------------------------------------- */
static inline lean_obj_res io_ok(lean_obj_arg v) { return lean_io_result_mk_ok(v); }
static lean_obj_res mk_pair(lean_obj_arg a, lean_obj_arg b) {
  lean_object *o = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(o, 0, a);
  lean_ctor_set(o, 1, b);
  return o;
}
static inline lean_obj_res nat_array(size_t cap) { return lean_alloc_array(0, cap); }
static inline void nat_push(lean_object *a, u64_t v) {
  lean_array_object *o = lean_to_array(a);
  o->m_data[o->m_size++] = u64_nat(v);
}
/* ---- read primitives ----------------------------------------------------------------------
 * All reads of one batch happen inside one read transaction (feed_db_begin .. feed_db_end_reads):
 * they see the same snapshot, the state before the batch's writes. */

static void cursor_open(BtCursor *c, Pgno root, KeyInfo *ki) {
  sqlite3BtreeCursorZero(c);
  if (sqlite3BtreeCursor(db_bt, root, 0, ki, c) != SQLITE_OK) db_die("cursor");
}

LEAN_EXPORT lean_obj_res feed_db_begin(void) {

  if (step_reset(st_begin) != SQLITE_DONE) return io_ok(lean_box(0));
  if (sqlite3BtreeBeginTrans(db_bt, 0, 0) != SQLITE_OK) { step_reset(st_rollback); return io_ok(lean_box(0)); }
  cursor_open(c_posts, root_posts, 0);
  cursor_open(c_users, root_users, 0);
  cursor_open(c_feed_idx, root_feed_idx, ki_feed_idx);
  cursor_open(c_likes, root_likes_post_idx, ki_likes_post_idx);
  reads_open = 1;
  return io_ok(lean_box(1));
}

LEAN_EXPORT lean_obj_res feed_db_end_reads(void) {

  pins_release();
  if (reads_open) {
    sqlite3BtreeCloseCursor(c_posts);
    sqlite3BtreeCloseCursor(c_users);
    sqlite3BtreeCloseCursor(c_feed_idx);
    sqlite3BtreeCloseCursor(c_likes);
    reads_open = 0;
  }
  return io_ok(lean_box(0));
}

/* Position the likes_post_id_idx cursor on the first entry with post_id >= k. 0 on an entry. */
static int likes_seek_ge(i64_t k) {
  Mem m;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  UnpackedRecord r;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = ki_likes_post_idx;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1;
  int res;
  if (sqlite3BtreeIndexMoveto(c_likes, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c_likes)) return 1;
  if (res < 0) { int rc = sqlite3BtreeNext(c_likes, 0); return rc == SQLITE_OK ? 0 : 1; }
  return 0;
}
static inline int likes_key(i64_t *k) {
  u32_t len;
  const u8_t *rec = cur_payload(c_likes, &len);
  col_t c[1];
  if (!rec || rec_cols(rec, len, 1, c) || !col_is_int(&c[0])) return -1;
  *k = col_int(&c[0]);
  return 0;
}

/* The number of likes rows with this post id: a range of likes_post_id_idx. */
static u64_t like_count_one(u64_t id) {
  u64_t c = 0;
  if (reads_open && id <= (u64_t)INT64_MAX && likes_seek_ge((i64_t)id) == 0) {
    for (;;) {
      i64_t k;
      if (likes_key(&k) || k != (i64_t)id) break;
      c++;
      if (sqlite3BtreeNext(c_likes, 0) != SQLITE_OK) break;
    }
  }
  return c;
}
/* ---- pages: posts with their authors and like counts ---------------------------------------
 * Row k of a page is cols[8k .. 8k+8) = id, b, c, e, found, s, t, likes: the body is blob[b, c),
 * created_at blob[c, e), the author's username blob[s, t) when found is 1. */
typedef struct { i64_t id, uid; u32_t b, c, e; } prow_t;

/* Append one posts record to blob and describe it in *r. 0 if the record is not the expected layout. */
static int page_post(lean_object **blob, prow_t *r, i64_t rowid, const u8_t *rec, u32_t n) {
  col_t c[4];
  if (rec_cols(rec, n, 4, c) || !col_is_int(&c[1]) || !col_is_text(&c[2]) || !col_is_text(&c[3])) return 0;
  u32_t bl = serial_len(c[2].t), cl = serial_len(c[3].t);
  *blob = ba_reserve(*blob, bl + cl + 32);   /* slack: leaves may load 32 bytes past a field */
  u32_t bs = (u32_t)lean_sarray_size(*blob);
  u8_t *d = ba_end(*blob);
  memcpy(d, c[2].p, bl);
  memcpy(d + bl, c[3].p, cl);
  ba_grow(*blob, bl + cl);
  r->id = rowid; r->uid = col_int(&c[1]); r->b = bs; r->c = bs + bl; r->e = bs + bl + cl;
  return 1;
}

/* ---- rowid lookups of several keys of one table, a level at a time ------------------------
 * The search of sqlite3BtreeTableMoveto for up to MANY keys together (sorted, distinct): on each
 * level the child page of every key is chosen before any page of the next level is fetched, each
 * page once, and a fetched page's cell pointers are prefetched, so that the cache misses of
 * different keys overlap. Within a page the search starts where a line through the page's first
 * and last keys puts the key (rowids are dense), then bisects. */
#define MANY 64
static inline i64_t page_key(MemPage *pg, int i) {
  u8_t *c = findCellPastPtr(pg, i);
  if (pg->intKeyLeaf)
    while (*c++ & 0x80)
      if (c >= pg->aDataEnd) return INT64_MAX;   /* corrupt: the caller checks the cell it finds */
  return sqlite3VarintValue(c);
}
/* The first cell of pg whose key is >= k (nCell if there is none), given its first key k0 and its
 * last key *kn (read here if *kn is INT64_MIN). The keys of a page are increasing. With dense
 * rowids the key is at k - k0; otherwise the guess is where a line through k0 and kn puts it,
 * then its neighbour, then bisection. */
static int page_lower_bound(MemPage *pg, i64_t k, i64_t k0, i64_t *knp) {
  int n = pg->nCell;
  if (k <= k0) return 0;
  u64_t off = (u64_t)(k - k0);
  if (pg->leaf && off < (u64_t)n && page_key(pg, (int)off) == k) return (int)off;
  if (*knp == INT64_MIN) *knp = page_key(pg, n - 1);
  i64_t kn = *knp;
  if (k > kn) return n;
  int lo = 1, hi = n - 1;   /* key(lo - 1) < k <= key(hi) */
  u64_t span = (u64_t)(kn - k0);
  int g = span < (1ull << 32) ? (int)(off * (u64_t)(n - 1) / span) : (int)((double)off / (double)span * (double)(n - 1));
  if (g < lo) g = lo;
  if (g > hi) g = hi;
  if (page_key(pg, g) >= k) {
    if (g == lo || page_key(pg, g - 1) < k) return g;
    hi = g - 1;
  } else {
    if (g + 1 > hi) return hi;
    if (page_key(pg, g + 1) >= k) return g + 1;
    lo = g + 2;
  }
  while (lo < hi) {
    int m = lo + ((hi - lo) >> 1);
    if (page_key(pg, m) >= k) hi = m; else lo = m + 1;
  }
  return lo;
}

/* Interior pages found by lookup_many stay referenced until the batch's reads end, as the pages
 * on an open cursor's path do. */
#define NPIN 16
static MemPage *pinned[NPIN];
static int npinned;
static MemPage *pin_find(Pgno no) {
  for (int i = 0; i < npinned; i++) if (pinned[i]->pgno == no) return pinned[i];
  return 0;
}
static void pins_release(void) {
  for (int i = 0; i < npinned; i++) releasePageNotNull(pinned[i]);
  npinned = 0;
}

/* For sorted distinct keys key[0, n): hit[i] the leaf page holding key[i] (referenced; release
 * it) and cell[i] its cell, or hit[i] = 0 if the table has no such row. -1 if the tree is not
 * what the search expects (nothing is held then: use the cursor). */
static int lookup_many(Pgno root, const i64_t *key, int n, MemPage **hit, int *cell) {
  BtShared *bs = db_bt->pBt;
  MemPage *pg[MANY];
  Pgno next[MANY];
  i64_t k0[MANY], kn[MANY];
  u8_t own[MANY];
  if (n < 1 || n > MANY) return -1;
  for (int i = 0; i < n; i++) next[i] = root;
  for (int depth = 0;; depth++) {
    for (int i = 0; i < n; i++) {
      own[i] = 0;
      if (i > 0 && next[i] == next[i - 1]) pg[i] = pg[i - 1];
      else if (!(pg[i] = pin_find(next[i]))) {
        if (getAndInitPage(bs, next[i], &pg[i], PAGER_GET_READONLY) != SQLITE_OK) pg[i] = 0;
        else {
          own[i] = 1;
          __builtin_prefetch(pg[i]->aCellIdx);
          __builtin_prefetch(pg[i]->aCellIdx + 2 * (pg[i]->nCell - 1));
        }
      }
      /* what moveToRoot and moveToChild check before they search a page */
      if (!pg[i] || !pg[i]->intKey || pg[i]->nCell == 0 || pg[i]->nOverflow || depth >= BTCURSOR_MAX_DEPTH ||
          pg[i]->leaf != pg[0]->leaf) {
        for (int j = 0; j <= i; j++)
          if (own[j]) releasePageNotNull(pg[j]);
        return -1;
      }
    }
    for (int i = 0; i < n; i++) {
      if (i == 0 || pg[i] != pg[i - 1]) { k0[i] = page_key(pg[i], 0); kn[i] = INT64_MIN; }
      else { k0[i] = k0[i - 1]; kn[i] = kn[i - 1]; }
    }
    if (pg[0]->leaf) break;
    for (int i = 0; i < n; i++) {
      int lw = page_lower_bound(pg[i], key[i], k0[i], &kn[i]);   /* key <= the cell's key: its left child */
      if (i + 1 < n && pg[i + 1] == pg[i]) kn[i + 1] = kn[i];
      next[i] = lw >= pg[i]->nCell ? get4byte(&pg[i]->aData[pg[i]->hdrOffset + 8]) : get4byte(findCell(pg[i], lw));
    }
    for (int i = 0; i < n; i++)
      if (own[i]) {
        if (npinned < NPIN) pinned[npinned++] = pg[i];
        else releasePageNotNull(pg[i]);
      }
  }
  /* each hit holds its own reference: take them while the first key of a page still holds its
   * own, then drop the references no hit took over */
  for (int i = 0; i < n; i++) {
    int ix = page_lower_bound(pg[i], key[i], k0[i], &kn[i]);
    if (i + 1 < n && pg[i + 1] == pg[i]) kn[i + 1] = kn[i];
    if (ix < pg[i]->nCell && page_key(pg[i], ix) == key[i]) {
      if (!own[i]) sqlite3PagerRef(pg[i]->pDbPage);
      hit[i] = pg[i];
      cell[i] = ix;
    } else hit[i] = 0;
  }
  for (int i = 0; i < n; i++)
    if (own[i] && !hit[i]) releasePageNotNull(pg[i]);
  return 0;
}

/* The users row of a hit: its record, when it lies on the page (0 otherwise). */
static const u8_t *hit_record(MemPage *pg, int ix, u32_t *n) {
  CellInfo info;
  pg->xParseCell(pg, findCell(pg, ix), &info);
  if (info.nLocal != info.nPayload) return 0;
  *n = info.nPayload;
  return info.pPayload;
}

/* Append user uid's username to blob: 1 found (name at blob[*s, *t)), 0 not found, by the cursor. */
static int user_name_cursor(lean_object **blob, i64_t uid, u32_t *s, u32_t *t) {
  int res = 1;
  *s = *t = (u32_t)lean_sarray_size(*blob);
  if (!reads_open || sqlite3BtreeTableMoveto(c_users, uid, 0, &res) != SQLITE_OK || res != 0) return 0;
  u32_t len;
  const u8_t *rec = cur_payload(c_users, &len);
  col_t c[2];
  if (!rec || rec_cols(rec, len, 2, c) || !col_is_text(&c[1])) return 0;
  u32_t nl = serial_len(c[1].t);
  *blob = ba_reserve(*blob, nl + 32);
  *s = (u32_t)lean_sarray_size(*blob);
  memcpy(ba_end(*blob), c[1].p, nl);
  ba_grow(*blob, nl);
  *t = *s + nl;
  return 1;
}

/* The usernames of users uid[0, k): found[q], and the name at blob[s[q], t[q]). */
static void user_names(lean_object **blob, const i64_t *uid, size_t k, u8_t *found, u32_t *s, u32_t *t) {
  if (k == 0) return;
  if (k > MANY || !reads_open) goto by_cursor;
  /* the rows by key (insertion sort), then the distinct keys */
  int ord[MANY];
  for (int q = 0; q < (int)k; q++) {
    int i = q;
    while (i > 0 && uid[ord[i - 1]] > uid[q]) { ord[i] = ord[i - 1]; i--; }
    ord[i] = q;
  }
  i64_t key[MANY];
  int nk = 0, of[MANY];
  for (int i = 0; i < (int)k; i++) {
    if (nk == 0 || key[nk - 1] != uid[ord[i]]) key[nk++] = uid[ord[i]];
    of[ord[i]] = nk - 1;
  }
  MemPage *hit[MANY];
  int cell[MANY];
  if (lookup_many(root_users, key, nk, hit, cell)) goto by_cursor;
  /* each key's name once; rows with the same author share it */
  u32_t ks[MANY], kt[MANY];
  u8_t kf[MANY];
  int bad = 0;
  for (int i = 0; i < nk; i++) {
    kf[i] = 0;
    ks[i] = kt[i] = (u32_t)lean_sarray_size(*blob);
    if (!hit[i]) continue;
    u32_t len;
    const u8_t *rec = hit_record(hit[i], cell[i], &len);
    col_t c[2];
    if (!rec || rec_cols(rec, len, 2, c) || !col_is_text(&c[1])) { bad = 1; continue; }
    u32_t nl = serial_len(c[1].t);
    *blob = ba_reserve(*blob, nl + 32);
    ks[i] = (u32_t)lean_sarray_size(*blob);
    copy_small(ba_end(*blob), c[1].p, nl);
    ba_grow(*blob, nl);
    kt[i] = ks[i] + nl;
    kf[i] = 1;
  }
  for (int i = 0; i < nk; i++) if (hit[i]) releasePageNotNull(hit[i]);
  if (bad) goto by_cursor;
  for (size_t q = 0; q < k; q++) { found[q] = kf[of[q]]; s[q] = ks[of[q]]; t[q] = kt[of[q]]; }
  return;
by_cursor:
  for (size_t q = 0; q < k; q++) found[q] = (u8_t)user_name_cursor(blob, uid[q], &s[q], &t[q]);
}

/* The authors and like counts of rows[0, k), then the page's columns. */
static lean_obj_res page_finish(lean_object *blob, const prow_t *rows, size_t k) {
  u64_t cbuf[64], *cnt = k <= 64 ? cbuf : malloc(k * sizeof(u64_t));
  if (!cnt) abort();
  /* like counts: one ordered scan of likes_post_id_idx when the ids are close together */
  i64_t lo = INT64_MAX, hi = 0;
  for (size_t q = 0; q < k; q++) { if (rows[q].id < lo) lo = rows[q].id; if (rows[q].id > hi) hi = rows[q].id; }
  if (k > 1 && (u64_t)(hi - lo) < 4 * k + 64) {
    size_t w = (size_t)(hi - lo + 1);
    u32_t wbuf[512], *win = w <= 512 ? wbuf : calloc(w, sizeof(u32_t));
    if (!win) abort();
    if (w <= 512) memset(win, 0, w * sizeof(u32_t));
    if (reads_open && likes_seek_ge(lo) == 0) {
      for (;;) {
        i64_t key;
        if (likes_key(&key) || key > hi) break;
        if (key >= lo) win[key - lo]++;
        if (sqlite3BtreeNext(c_likes, 0) != SQLITE_OK) break;
      }
    }
    for (size_t q = 0; q < k; q++) cnt[q] = win[rows[q].id - lo];
    if (win != wbuf) free(win);
  } else {
    for (size_t q = 0; q < k; q++) cnt[q] = like_count_one((u64_t)rows[q].id);
  }
  i64_t ubuf[64], *uid = k <= 64 ? ubuf : malloc(k * sizeof(i64_t));
  u8_t fbuf[64], *found = k <= 64 ? fbuf : malloc(k);
  u32_t sbuf[64], tbuf[64], *s = k <= 64 ? sbuf : malloc(k * sizeof(u32_t)), *t = k <= 64 ? tbuf : malloc(k * sizeof(u32_t));
  if (!uid || !found || !s || !t) abort();
  for (size_t q = 0; q < k; q++) uid[q] = rows[q].uid;
  user_names(&blob, uid, k, found, s, t);
  lean_object *cols = nat_array(8 * k);
  for (size_t q = 0; q < k; q++) {
    nat_push(cols, (u64_t)rows[q].id);
    nat_push(cols, rows[q].b);
    nat_push(cols, rows[q].c);
    nat_push(cols, rows[q].e);
    nat_push(cols, (u64_t)found[q]);
    nat_push(cols, s[q]);
    nat_push(cols, t[q]);
    nat_push(cols, cnt[q]);
  }
  if (cnt != cbuf) free(cnt);
  if (uid != ubuf) { free(uid); free(found); free(s); free(t); }
  return io_ok(mk_pair(blob, cols));
}

/* feedPage n: the first n posts in (created_at DESC, id DESC) order, with their authors and like
 * counts. The ids come from posts_created_at_id_idx; when they are consecutive (the newest posts
 * got increasing ids) the rows are read by one backward scan of posts, each checked against its
 * id; otherwise, or from the first mismatch on, by one lookup per id. */
LEAN_EXPORT lean_obj_res feed_db_feed_page(b_lean_obj_arg nn) {
  u64_t n = nat_u64(nn);
  i64_t idbuf[64], *ids = idbuf;
  size_t cap = 64, k = 0;
  int empty = 1;
  if (reads_open && sqlite3BtreeFirst(c_feed_idx, &empty) == SQLITE_OK) {
    while (!empty && k < n) {
      u32_t len;
      const u8_t *rec = cur_payload(c_feed_idx, &len);
      col_t c[2];
      if (!rec || rec_cols(rec, len, 2, c) || !col_is_int(&c[1])) break;
      if (k == cap) {
        i64_t *m = malloc(2 * cap * sizeof *m);
        if (!m) abort();
        memcpy(m, ids, k * sizeof *m);
        if (ids != idbuf) free(ids);
        ids = m;
        cap *= 2;
      }
      ids[k++] = col_int(&c[1]);
      if (sqlite3BtreeNext(c_feed_idx, 0) != SQLITE_OK) break;
    }
  }
  prow_t rbuf[64], *rows = k <= 64 ? rbuf : malloc(k * sizeof(prow_t));
  if (!rows) abort();
  lean_object *blob = lean_alloc_sarray(1, 0, (k < 64 ? k : 64) * 400 + 256);
  size_t j = 0, nr = 0;
  int consecutive = k > 1;
  for (size_t q = 1; consecutive && q < k; q++) consecutive = ids[q] + 1 == ids[q - 1];
  int res = 1;
  if (consecutive && sqlite3BtreeTableMoveto(c_posts, ids[0], 0, &res) == SQLITE_OK && res == 0) {
    for (j = 0; j < k; j++) {
      if (j > 0 && (sqlite3BtreePrevious(c_posts, 0) != SQLITE_OK || sqlite3BtreeIntegerKey(c_posts) != ids[j])) break;
      u32_t len;
      const u8_t *rec = cur_payload(c_posts, &len);
      if (!rec || !page_post(&blob, &rows[nr], ids[j], rec, len)) break;
      nr++;
    }
  }
  for (; j < k; j++) {
    res = 1;
    if (sqlite3BtreeTableMoveto(c_posts, ids[j], 0, &res) != SQLITE_OK || res != 0) continue;
    u32_t len;
    const u8_t *rec = cur_payload(c_posts, &len);
    if (rec && page_post(&blob, &rows[nr], ids[j], rec, len)) nr++;
  }
  if (ids != idbuf) free(ids);
  lean_obj_res r = page_finish(blob, rows, nr);
  if (rows != rbuf) free(rows);
  return r;
}

/* postPage id: the post with this id with its author and like count; no row if there is none. */
LEAN_EXPORT lean_obj_res feed_db_post_page(b_lean_obj_arg idn) {
  u64_t id = nat_u64(idn);
  prow_t row;
  size_t nr = 0;
  lean_object *blob = lean_alloc_sarray(1, 0, 1024);
  int res = 1;
  if (reads_open && id <= (u64_t)INT64_MAX && sqlite3BtreeTableMoveto(c_posts, (i64_t)id, 0, &res) == SQLITE_OK && res == 0) {
    u32_t len;
    const u8_t *rec = cur_payload(c_posts, &len);
    if (rec && page_post(&blob, &row, (i64_t)id, rec, len)) nr = 1;
  }
  return page_finish(blob, &row, nr);
}

/* ---- write primitives (inside the batch transaction, after the reads) -------------------- */

/* postExists id: whether the posts table has a row with this id (as of the earlier writes of the
 * batch). 0 no, 1 yes, 2 error. */
LEAN_EXPORT lean_obj_res feed_db_post_exists(b_lean_obj_arg idn) {
  u64_t id = nat_u64(idn);
  if (id > (u64_t)INT64_MAX) return io_ok(lean_box(0));
  /* a cursor of the batch's transaction, so it sees the batch's earlier writes */
  int res = 1, rc;
  sqlite3BtreeCursorZero(c_exists);
  if ((rc = sqlite3BtreeCursor(db_bt, root_posts, 0, 0, c_exists)) == SQLITE_OK) {
    rc = sqlite3BtreeTableMoveto(c_exists, (i64_t)id, 0, &res);
    sqlite3BtreeCloseCursor(c_exists);
  }
  if (rc != SQLITE_OK) { db_log_error("post exists"); return io_ok(lean_box(2)); }
  return io_ok(lean_box(res == 0 ? 1 : 0));
}

/* The schema default of created_at, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'): the current UTC time
 * in milliseconds, as SQLite's 'now' (the unix VFS's clock, milliseconds truncated). */
static int db_now(char *out) {
  static time_t sec_cached = -1;
  static char head[64];   /* "YYYY-MM-DDTHH:MM:SS" of sec_cached */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  if (ts.tv_sec != sec_cached) {
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(head, sizeof head, "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    sec_cached = ts.tv_sec;
  }
  int ms = (int)(ts.tv_nsec / 1000000);
  memcpy(out, head, 19);
  out[19] = '.';
  out[20] = (char)('0' + ms / 100);
  out[21] = (char)('0' + ms / 10 % 10);
  out[22] = (char)('0' + ms % 10);
  out[23] = 'Z';
  out[24] = 0;
  return 24;
}

/* insertLike uid pid: insert the like unless present. 1 inserted, 0 already there, 2 error. */
LEAN_EXPORT lean_obj_res feed_db_insert_like(b_lean_obj_arg uidn, b_lean_obj_arg pidn) {
  u64_t uid = nat_u64(uidn), pid = nat_u64(pidn);
  if (uid > (u64_t)INT64_MAX || pid > (u64_t)INT64_MAX) return io_ok(lean_box(2));
  char now[32];
  int nl = db_now(now);
  sqlite3_bind_int64(st_like, 1, (i64_t)uid);
  sqlite3_bind_int64(st_like, 2, (i64_t)pid);
  sqlite3_bind_text(st_like, 3, now, nl, SQLITE_STATIC);
  int rc = sqlite3_step(st_like);
  int changed = sqlite3_changes(db);
  if (rc != SQLITE_DONE) db_log_error("insert like");
  sqlite3_reset(st_like);
  return io_ok(lean_box(rc != SQLITE_DONE ? 2 : changed ? 1 : 0));
}

/* insertPost uid body: insert the post; its created_at and id (id 0 on error). */
LEAN_EXPORT lean_obj_res feed_db_insert_post(b_lean_obj_arg uidn, b_lean_obj_arg body) {
  u64_t uid = nat_u64(uidn), id = 0;
  lean_object *created = lean_alloc_sarray(1, 0, 32);
  if (uid <= (u64_t)INT64_MAX) {
    char now[32];
    int nl = db_now(now);
    sqlite3_bind_int64(st_post, 1, (i64_t)uid);
    sqlite3_bind_text(st_post, 2, (const char *)lean_sarray_cptr(body), (int)lean_sarray_size(body), SQLITE_STATIC);
    sqlite3_bind_text(st_post, 3, now, nl, SQLITE_STATIC);
    int rc = sqlite3_step(st_post);
    if (rc != SQLITE_DONE) db_log_error("insert post");
    else if (sqlite3_changes(db) == 1) {
      i64_t v = sqlite3_last_insert_rowid(db);
      if (v > 0) {
        id = (u64_t)v;
        memcpy(lean_sarray_cptr(created), now, (size_t)nl);
        lean_to_sarray(created)->m_size = (size_t)nl;
      }
    }
    sqlite3_reset(st_post);
    sqlite3_clear_bindings(st_post);
  }
  return io_ok(mk_pair(created, u64_nat(id)));
}

/* For the self-test: lookup_many against sqlite3BtreeTableMoveto on the users table, for sets of
 * sorted distinct keys (every key up to past the largest, in runs of up to MANY; and random sets),
 * in one read transaction so that the pinned interior pages are reused. 1 if they agree. */
static int check_keys(const i64_t *key, int n) {
  MemPage *hit[MANY];
  int cell[MANY], ok = 1;
  if (lookup_many(root_users, key, n, hit, cell)) return 0;
  for (int i = 0; i < n; i++) {
    int res = 1;
    if (sqlite3BtreeTableMoveto(c_users, key[i], 0, &res) != SQLITE_OK) ok = 0;
    else if ((res == 0) != (hit[i] != 0)) ok = 0;
    else if (hit[i]) {
      u32_t a, b;
      const u8_t *ra = cur_payload(c_users, &a), *rb = hit_record(hit[i], cell[i], &b);
      if (!ra || !rb || a != b || memcmp(ra, rb, a)) ok = 0;
    }
  }
  for (int i = 0; i < n; i++) if (hit[i]) releasePageNotNull(hit[i]);
  return ok;
}
LEAN_EXPORT lean_obj_res feed_db_check_lookups(void) {
  lean_dec(feed_db_begin());
  int ok = reads_open, empty = 1;
  i64_t top = 0;
  if (ok && sqlite3BtreeLast(c_users, &empty) == SQLITE_OK && !empty) top = sqlite3BtreeIntegerKey(c_users);
  i64_t key[MANY];
  for (i64_t k = 0; ok && k <= top + 70; k += MANY) {
    for (int i = 0; i < MANY; i++) key[i] = k + i;
    ok = check_keys(key, MANY);
  }
  u64_t r = 0x853c49e6748fea9bull;
  for (int round = 0; ok && round < 4000; round++) {
    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
    int n = 1 + (int)(r % MANY), nk = 0;
    i64_t base = (i64_t)((r >> 8) % (u64_t)(top + 100)), span = round % 3 == 0 ? 3 * MANY : top + 100;
    for (int i = 0; i < n; i++) {
      r ^= r << 13; r ^= r >> 7; r ^= r << 17;
      i64_t v = round % 3 == 0 ? base + (i64_t)(r % (u64_t)span) : (i64_t)(r % (u64_t)span);
      int j = nk;
      while (j > 0 && key[j - 1] > v) j--;
      if (j > 0 && key[j - 1] == v) continue;
      memmove(key + j + 1, key + j, (size_t)(nk - j) * sizeof *key);
      key[j] = v;
      nk++;
    }
    ok = check_keys(key, nk);
  }
  lean_dec(feed_db_end_reads());
  lean_dec(feed_db_commit());
  return io_ok(lean_box(ok));
}

/* For the self-test: a transaction that grows the database and rolls back, then one that commits,
 * through the page cache wrapper (which drops the pages past the end of the file at the end of a
 * transaction); then SQLite's integrity check. 1 if every count and the check come out right. */
static int integrity_ok(void) {
  sqlite3_stmt *s = db_prepare("PRAGMA integrity_check");
  int ok = sqlite3_step(s) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(s, 0), "ok");
  sqlite3_finalize(s);
  return ok;
}
static i64_t q_int(const char *sql) {
  sqlite3_stmt *s = db_prepare(sql);
  i64_t v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
  sqlite3_finalize(s);
  return v;
}
LEAN_EXPORT lean_obj_res feed_db_check_rollback(void) {
  i64_t n0 = q_int("SELECT count(*) FROM posts"), pages0 = q_int("PRAGMA page_count");
  char big[401];
  memset(big, 'R', 400); big[400] = 0;
  char sql[600];
  db_exec("BEGIN");
  for (int i = 0; i < 3000; i++) {
    snprintf(sql, sizeof sql, "INSERT INTO posts (user_id, body) VALUES (%d, '%s')", 1 + i % 50, big);
    db_exec(sql);
  }
  int ok = q_int("PRAGMA page_count") > pages0 + 100;   /* the file grew */
  db_exec("ROLLBACK");

  ok = ok && q_int("SELECT count(*) FROM posts") == n0 && q_int("PRAGMA page_count") == pages0;
  memset(big, 'C', 400);
  db_exec("BEGIN");
  for (int i = 0; i < 2000; i++) {
    snprintf(sql, sizeof sql, "INSERT INTO posts (user_id, body) VALUES (%d, '%s')", 1 + i % 50, big);
    db_exec(sql);
  }
  db_exec("COMMIT");
  ok = ok && q_int("SELECT count(*) FROM posts") == n0 + 2000 &&
       q_int("SELECT count(*) FROM posts WHERE body GLOB 'R*'") == 0 &&
       q_int("SELECT count(*) FROM posts WHERE body GLOB 'C*'") == 2000;
  ok = ok && integrity_ok();
  db_exec("DELETE FROM posts WHERE body GLOB 'C*'");
  return io_ok(lean_box(ok));
}
/* For the self-test: PRAGMA integrity_check says ok. */
LEAN_EXPORT lean_obj_res feed_db_integrity(void) { return io_ok(lean_box(integrity_ok())); }

/* For the self-test (Feed/Selftest.lean): create a database file from an SQL script. */
LEAN_EXPORT lean_obj_res feed_db_create(b_lean_obj_arg path, b_lean_obj_arg script) {
  sqlite3 *d = 0;
  int rc = sqlite3_open_v2(lean_string_cstr(path), &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
  char *err = 0;
  if (rc == SQLITE_OK) rc = sqlite3_exec(d, lean_string_cstr(script), 0, 0, &err);
  if (rc != SQLITE_OK) fprintf(stderr, "selftest: %s\n", err ? err : sqlite3_errmsg(d));
  sqlite3_free(err);
  sqlite3_close(d);
  return io_ok(lean_box(rc == SQLITE_OK));
}

/* commit: 1 if the batch's transaction committed. */
LEAN_EXPORT lean_obj_res feed_db_commit(void) {

  if (reads_open) lean_dec(feed_db_end_reads());
  if (sqlite3_get_autocommit(db)) return io_ok(lean_box(1));
  if (step_reset(st_commit) == SQLITE_DONE) return io_ok(lean_box(1));
  db_log_error("commit");
  step_reset(st_rollback);
  return io_ok(lean_box(0));
}

/* health: SELECT 1 succeeded. */
LEAN_EXPORT lean_obj_res feed_db_health(void) {

  int rc = sqlite3_step(st_health);
  sqlite3_reset(st_health);
  return io_ok(lean_box(rc == SQLITE_ROW ? 1 : 0));
}
