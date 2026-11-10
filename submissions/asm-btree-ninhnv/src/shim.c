/* The only C in this server besides SQLite: a thin layer that #includes the unmodified SQLite
 * amalgamation, so the assembly can reach SQLite's B-tree functions (they are `static` inside
 * sqlite3.c and have no public API).
 *
 * Adapted from rust-iouring-kensac's csrc/shim.c (submissions/rust-iouring-kensac, PR #264): the
 * td_* interface, the cursor helpers, the checkpoint thread's states and the spill buffer come
 * from there. Changed or added here: the page view for the assembly's likes walk, INSERT without
 * RETURNING, the page-cache wrapper, the copy rounds and database sync before a write hold, the
 * second (tail) thread, the code lock, the once-a-minute log line.
 *
 * What lives here: opening the database, the per-pass transaction, B-tree cursor moves, the
 * record bytes of the current row, a view of the current page (so the assembly can walk the likes
 * index a leaf at a time), the three SQL statements (two INSERTs, SELECT 1), the reference feed
 * query used as a fallback, and the WAL checkpoint threads. Everything else (decoding records,
 * counting likes, JSON, HTTP, JWT, io_uring) is in the .asm files.
 *
 * SQLite's own state is only read here, never written: cursors move through sqlite3Btree*
 * calls, and the page cache is adjusted through its plugin interface (SQLITE_CONFIG_PCACHE2).
 *
 * SQLite runs on the event-loop thread, except WAL checkpoints, which two helper threads run on
 * their own connection; hence SQLITE_THREADSAFE=2 (each connection is used by one thread at a
 * time). */
#include "sqlite3.c"
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>

typedef long long i64;

static sqlite3 *db;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health, *st_feed;
static Pgno roots[4];
static KeyInfo *kis[4];
static int wal_frames;
static int in_tx;
/* SQLite ended the pass's transaction by itself (SQLITE_FULL, an I/O error): the rest of the
 * pass must not write in autocommit mode, and its COMMIT reports failure. */
static int tx_broken;

/* 0 posts, 1 users, 2 posts_created_at_id_idx, 3 likes_post_id_idx. Opened when the pass's
 * transaction starts, closed before it commits. */
static BtCursor *cur[4];
static int cur_open;

/* A record that spills onto overflow pages is copied here (no row of this schema does). */
static u8 *spill;
static u32 spill_cap;

/* Checkpoints. Past CKPT_FRAMES frames a helper thread copies the log back into the database,
 * then, in rounds, copies what was written meanwhile and syncs the database file, until at most
 * CKPT_LEFT frames were written during a round. Then writes are held (reads go on) while the other helper copies what was written
 * meanwhile, so the next write can restart the log from the top. A hold lasts at most HOLD_MS.
 * Past CKPT_FORCE frames (the helpers can't keep up) the loop checkpoints by itself. */
#ifndef CKPT_FRAMES /* overridable only to exercise holds in tests */
#define CKPT_FRAMES 4096
#endif
#define CKPT_FORCE 65536
#define CKPT_LEFT 128
#define CKPT_ROUNDS 5
#define HOLD_MS 250
#ifndef BULK_NICE
#define BULK_NICE 19
#endif
static sqlite3 *ckdb;
enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static int ck_state;
static int ck_full;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int held;
static struct timespec hold_t0;
/* For the log line: holds, holds that ran into HOLD_MS, checkpoints the loop made itself, the
 * most frames the log held. */
static unsigned n_holds, n_expired, n_forced, max_frames;

static int wal_hook(void *a, sqlite3 *d, const char *n, int frames) {
  (void)a; (void)d; (void)n;
  wal_frames = frames;
  return SQLITE_OK;
}

static int fail(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
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

/* The assembly decodes records by column position: refuse to start on any other layout. */
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
  return n < sizeof got && strcmp(got, want) == 0;
}

static KeyInfo *keyinfo(int nkey, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, 1);
  if (!k) return 0;
  for (int i = 0; i <= nkey; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = 0;
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) ? 0 : s;
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void ck_set(int s) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, s, __ATOMIC_RELEASE);
  pthread_cond_broadcast(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

/* Syncs the database file. An unfinished copy leaves the pages it wrote unsynced (SQLite syncs the
 * file only when a copy brings in every frame), so the copy made during a write hold would have to
 * flush the whole cycle's pages while writes wait: on a slow disk the hold ran out first, the log
 * never restarted and grew until the loop had to copy it. Synced before the hold, the held copy
 * has only its own few pages to flush. (The approach of swift-iouring-akbashev, PR #376.) */
static void ck_sync_db(void) {
  sqlite3_file *f = 0;
  if (sqlite3_file_control(ckdb, "main", SQLITE_FCNTL_FILE_POINTER, &f) == SQLITE_OK && f && f->pMethods)
    f->pMethods->xSync(f, SQLITE_SYNC_NORMAL);
}

/* PASSIVE checkpoints, as the loop asks for them. PASSIVE never waits for the loop's locks and the
 * loop never waits for these threads. Two threads take turns on the one connection (the states
 * are strictly sequential). The bulk thread runs at nice BULK_NICE, so on one vCPU its copying and
 * its syncs stay out of the loop's way. The tail thread runs at normal priority because writes
 * wait for it; after the bulk thread's rounds it has a handful of frames to copy and sync. */
static void *ck_main(void *arg) {
  int serve = (int)(long)arg;
  if (serve == CK_BULK) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), BULK_NICE);
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (ck_state != serve) pthread_cond_wait(&ck_cv, &ck_mu);
    pthread_mutex_unlock(&ck_mu);
    int nlog = -1, ndone = -2, rc = SQLITE_OK;
    if (serve == CK_BULK) {
      /* The cycle's frames, then rounds of [copy what was written meanwhile, sync the database]
       * until little was written during a round: each round is shorter than the one before as
       * long as the disk is faster than the writes. A checkpoint reports the log as it was when
       * it began; what the loop wrote since is the loop's own count (wal_frames, set at each
       * commit) minus that. */
      rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
      for (int i = 0; i < CKPT_ROUNDS && rc == SQLITE_OK && nlog == ndone; i++) {
        rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
        ck_sync_db();
        if (__atomic_load_n(&wal_frames, __ATOMIC_RELAXED) - nlog <= CKPT_LEFT) break;
      }
    } else {
      rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    }
    pthread_mutex_lock(&ck_mu);
    ck_full = rc == SQLITE_OK && nlog == ndone;
    __atomic_store_n(&ck_state, serve + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ck_mu);
  }
  return 0;
}

/* ---- page cache --------------------------------------------------------------------------------
 * SQLite's default page cache behind its plugin interface (SQLITE_CONFIG_PCACHE2), with one
 * decision changed: a truncate that cannot find anything to drop is not passed on.
 *
 * Why: the default cache remembers the largest page number it has held and never lowers it. A
 * B-tree split (balance_nonroot) renumbers pages through a temporary number, the lock-byte
 * page's (1 GiB / page size + 1), far beyond the end of the file. From then on every
 * commit's truncate to the file size looks as if such pages might exist, and the cache walks
 * every page it holds (all of the 64 MB cache) to find none.
 *
 * The wrapper sees every page number that enters a cache (a fetch that may create, a rekey). It
 * keeps the largest, and counts the pages parked at the temporary number apart from it. With
 * nothing parked and the largest number below the limit, no page at or past the limit exists and
 * the truncate has nothing to do. Pages that leave only make the record an overestimate, which
 * errs toward passing the truncate on. (The same approach as the wrappers of c-iouring-pavlyhalim
 * #245 and wat-wasmedge-delneg #272; written for this shim.) */
static sqlite3_pcache_methods2 pc_base;
struct pc_wrap { sqlite3_pcache *base; unsigned maxkey, parked_at, nparked; };
#define PCW(p) ((struct pc_wrap *)(p))

static int pc_init(void *a) { (void)a; return pc_base.xInit(pc_base.pArg); }
static void pc_shutdown(void *a) { (void)a; pc_base.xShutdown(pc_base.pArg); }
static sqlite3_pcache *pc_create(int szPage, int szExtra, int purgeable) {
  struct pc_wrap *w = sqlite3_malloc(sizeof *w);
  if (!w) return 0;
  if (!(w->base = pc_base.xCreate(szPage, szExtra, purgeable))) { sqlite3_free(w); return 0; }
  w->maxkey = 0;
  w->parked_at = 0x40000000u / (unsigned)szPage + 1; /* the lock-byte page's number */
  w->nparked = 0;
  return (sqlite3_pcache *)w;
}
static void pc_cachesize(sqlite3_pcache *p, int n) { pc_base.xCachesize(PCW(p)->base, n); }
static int pc_pagecount(sqlite3_pcache *p) { return pc_base.xPagecount(PCW(p)->base); }
static sqlite3_pcache_page *pc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  struct pc_wrap *w = PCW(p);
  sqlite3_pcache_page *pg = pc_base.xFetch(w->base, key, create);
  if (create && pg) {
    if (key == w->parked_at) w->nparked++;
    else if (key > w->maxkey) w->maxkey = key;
  }
  return pg;
}
static void pc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) {
  pc_base.xUnpin(PCW(p)->base, pg, discard);
}
static void pc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned from, unsigned to) {
  struct pc_wrap *w = PCW(p);
  pc_base.xRekey(w->base, pg, from, to);
  if (from == w->parked_at && w->nparked) w->nparked--;
  if (to == w->parked_at) w->nparked++;
  else if (to > w->maxkey) w->maxkey = to;
}
static void pc_truncate(sqlite3_pcache *p, unsigned limit) {
  struct pc_wrap *w = PCW(p);
  if (!w->nparked && w->maxkey < limit) return;
  pc_base.xTruncate(w->base, limit);
  if (limit && w->maxkey >= limit) w->maxkey = limit - 1;
  if (w->parked_at >= limit) w->nparked = 0;
}
static void pc_destroy(sqlite3_pcache *p) {
  pc_base.xDestroy(PCW(p)->base);
  sqlite3_free(p);
}
static void pc_shrink(sqlite3_pcache *p) { pc_base.xShrink(PCW(p)->base); }

/* Before sqlite3_initialize. */
static int pc_install(void) {
  static sqlite3_pcache_methods2 m;
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pc_base) != SQLITE_OK) return -1;
  m = pc_base;
  m.pArg = 0;
  m.xInit = pc_init;
  m.xShutdown = pc_shutdown;
  m.xCreate = pc_create;
  m.xCachesize = pc_cachesize;
  m.xPagecount = pc_pagecount;
  m.xFetch = pc_fetch;
  m.xUnpin = pc_unpin;
  m.xRekey = pc_rekey;
  m.xTruncate = pc_truncate;
  m.xDestroy = pc_destroy;
  m.xShrink = pc_shrink;
  return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) == SQLITE_OK ? 0 : -1;
}

/* ---- code kept in RAM -------------------------------------------------------------------------
 * Locks the program's code and read-only data (this binary first, then the libraries it loads)
 * into memory. With no swap the kernel reclaims file-backed pages first when memory runs short,
 * which on the 2 GB box is where a few hundred thousand sockets take it; without the lock the
 * loop then stalls on major faults reading its own code back from disk. A few MB, inside the
 * 8 MB RLIMIT_MEMLOCK a systemd service gets; whatever doesn't fit stays unlocked.
 * td_unlock_code undoes it (the loop calls it if the kernel refuses a ring: some kernels count a
 * ring's memory against the same limit). Returns the KB locked. */
static struct { unsigned long a, n; } locked[64];
static int nlocked;

long td_lock_code(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  char line[512];
  long kb = 0;
  if (!f) return 0;
  while (fgets(line, sizeof line, f) && nlocked < 64) {
    unsigned long a, b, ino;
    char perm[8];
    if (sscanf(line, "%lx-%lx %7s %*s %*s %lu", &a, &b, perm, &ino) != 4) continue;
    if (!ino || perm[1] == 'w' || perm[3] != 'p' || !strchr(line, '/')) continue;
    if (mlock((void *)a, b - a)) break;
    locked[nlocked].a = a;
    locked[nlocked++].n = b - a;
    kb += (long)((b - a) >> 10);
  }
  fclose(f);
  return kb;
}

void td_unlock_code(void) {
  while (nlocked) { nlocked--; munlock((void *)locked[nlocked].a, locked[nlocked].n); }
}

/* A crash leaves one line in the log (the signal, the faulting address, where the code was) and
 * then takes its default course, so the service manager still sees the signal. */
static void on_crash(int sig, siginfo_t *si, void *uc) {
  greg_t *r = ((ucontext_t *)uc)->uc_mcontext.gregs;
  dprintf(2, "fatal signal %d: address %p, rip %p, rsp %p, rbp %p, loop pid %d, thread %ld\n", sig, si->si_addr,
          (void *)r[REG_RIP], (void *)r[REG_RSP], (void *)r[REG_RBP], (int)getpid(), (long)syscall(SYS_gettid));
  signal(sig, SIG_DFL);
  raise(sig);
}

static void crash_log(void) {
  static char stack[32768];
  stack_t ss = {.ss_sp = stack, .ss_size = sizeof stack};
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sigaltstack(&ss, 0);
  sa.sa_sigaction = on_crash;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, 0);
}

int td_open(const char *path, char *err, int n) {
  crash_log();
  if (pc_install() || sqlite3_initialize()) return fail(err, n, "init");
  /* unix-excl: the process keeps an exclusive lock on the file, so the WAL index lives in heap
   * memory shared by the two connections (no -shm file, no file locks per transaction). */
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl"))
    return fail(err, n, "open");
  /* WAL + synchronous=NORMAL (rule 6). No mmap: a page in SQLite's own cache keeps its parsed
   * header and needs no WAL-index lookup; the pages the requests touch fit in 64 MB. */
  if (sqlite3_exec(db,
                   "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;"
                   "PRAGMA cache_size = -65536; PRAGMA mmap_size = 0; PRAGMA wal_autocheckpoint = 0;"
                   "SELECT count(*) FROM sqlite_schema",
                   0, 0, 0))
    return fail(err, n, "pragmas");
  sqlite3_wal_hook(db, wal_hook, 0);

  if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") ||
      sqlite3_exec(ckdb, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16", 0, 0, 0)) {
    snprintf(err, n, "checkpoint connection: %s", ckdb ? sqlite3_errmsg(ckdb) : "cannot open");
    return -1;
  }
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  int rc = pthread_create(&t, &a, ck_main, (void *)(long)CK_BULK);
  if (!rc) rc = pthread_create(&t, &a, ck_main, (void *)(long)CK_TAIL);
  pthread_attr_destroy(&a);
  if (rc) { snprintf(err, n, "checkpoint thread"); return -1; }

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
    if (!same(checks[i][0], checks[i][1])) { snprintf(err, n, "unexpected schema (%s)", checks[i][0]); return -1; }
  if (ENC(db) != SQLITE_UTF8) { snprintf(err, n, "database encoding is not UTF-8"); return -1; }

  roots[0] = rootpage("posts");
  roots[1] = rootpage("users");
  roots[2] = rootpage("posts_created_at_id_idx");
  roots[3] = rootpage("likes_post_id_idx");
  if (!roots[0] || !roots[1] || !roots[2] || !roots[3]) return fail(err, n, "root pages");
  kis[2] = keyinfo(2, 1);
  kis[3] = keyinfo(1, 0);
  if (!kis[2] || !kis[3]) return fail(err, n, "keyinfo");

  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2)");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_health = prep("SELECT 1");
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                 "FROM posts p JOIN users u ON u.id = p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  if (!st_begin || !st_commit || !st_rollback || !st_post || !st_like || !st_health || !st_feed)
    return fail(err, n, "prepare");
  bt = db->aDb[0].pBt;
  for (int i = 0; i < 4; i++)
    if (!(cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize()))) return fail(err, n, "cursor");
  return 0;
}

static void close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < 4; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}

/* Joins the pass's transaction, starting it (and its read snapshot and cursors) if needed.
 * 0 ok, -1 the pass's transaction is lost. */
int td_begin(void) {
  if (tx_broken) return -1;
  if (in_tx) {
    if (!sqlite3_get_autocommit(db)) return 0;
    in_tx = 0; /* SQLite ended it by itself */
    tx_broken = 1;
    return -1;
  }
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) { step_reset(st_rollback); return -1; }
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      step_reset(st_rollback);
      return -1;
    }
  }
  cur_open = 1;
  in_tx = 1;
  return 0;
}

/* Ends the pass's transaction. 0 committed (or nothing to commit), -1 rolled back. */
int td_commit(void) {
  if (tx_broken) {
    tx_broken = 0;
    close_cursors();
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  if (!in_tx) return 0;
  in_tx = 0;
  close_cursors();
  if (step_reset(st_commit) != SQLITE_DONE) { step_reset(st_rollback); return -1; }
  return 0;
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Between passes (no transaction open): drives the checkpoint threads. Holds start and end only
 * here. Returns whether writes are held. Once a minute, a line for the log. */
int td_tick(void) {
  static unsigned ticks;
  static struct timespec log_t0;
  if ((unsigned)wal_frames > max_frames) max_frames = (unsigned)wal_frames;
  if (!(++ticks & 1023)) {
    if (!log_t0.tv_sec) clock_gettime(CLOCK_MONOTONIC, &log_t0);
    else if (ms_since(&log_t0) >= 60000) {
      clock_gettime(CLOCK_MONOTONIC, &log_t0);
      dprintf(2, "wal: %u holds, %u ran out, %u copies on the loop, at most %u frames\n", n_holds, n_expired,
              n_forced, max_frames);
      n_holds = n_expired = n_forced = max_frames = 0;
    }
  }
  switch (__atomic_load_n(&ck_state, __ATOMIC_ACQUIRE)) {
  case CK_IDLE:
    if (wal_frames >= CKPT_FORCE) {
      int nlog = 0, ndone = 0;
      n_forced++;
      if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK && nlog == ndone)
        wal_frames = 0;
    } else if (wal_frames >= CKPT_FRAMES) {
      ck_set(CK_BULK);
    }
    break;
  case CK_BULK_DONE:
    held = 1;
    n_holds++;
    clock_gettime(CLOCK_MONOTONIC, &hold_t0);
    ck_set(CK_TAIL);
    break;
  case CK_TAIL_DONE:
    if (ck_full) wal_frames = 0;
    held = 0;
    ck_set(CK_IDLE);
    break;
  default:
    if (held && ms_since(&hold_t0) > HOLD_MS) { held = 0; n_expired++; }
    break;
  }
  return held;
}

int td_writes_held(void) { return held; }

void *td_cursor(int i) { return cur[i]; }

/* Table cursor to rowid id: 1 found, 0 absent, -1 error. */
int td_seek(BtCursor *c, i64 id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

/* Index cursor to the first entry whose leading integer column is >= k: 1 on it, 0 none, -1 error. */
int td_seek_ge(BtCursor *c, i64 k) {
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
  r.default_rc = +1; /* entries equal on the key sort after it: land before the first of them */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

int td_first(BtCursor *c) {
  int empty = 1;
  if (sqlite3BtreeFirst(c, &empty) != SQLITE_OK) return -1;
  return !empty;
}

int td_next(BtCursor *c) {
  int rc = sqlite3BtreeNext(c, 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* The current row's record bytes (valid until the cursor moves), or 0 on error. */
const u8 *td_payload(BtCursor *c, u32 *len) {
  getCellInfo(c);
  u32 total = c->info.nPayload;
  if (c->info.nLocal == total) { *len = total; return c->info.pPayload; }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    if (!(spill = sqlite3_malloc(spill_cap))) { spill_cap = 0; return 0; }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return spill;
}

/* A view of the page under an index cursor, for walking its cells in assembly. On a leaf the
 * cells from ix to ncell-1 are the next entries in order; on an interior page only cell ix is
 * (the next entry is down a child), so ncell is ix+1 there. A cell's bytes start at
 * data + (mask & big-endian u16 at cellidx + 2*i) + hdr. */
struct td_pv { const u8 *data, *cellidx, *end; u32 ix, ncell, hdr, mask; };

void td_pv(BtCursor *c, struct td_pv *v) {
  MemPage *pg = c->pPage;
  v->data = pg->aData;
  v->cellidx = pg->aCellIdx;
  v->end = pg->aDataEnd;
  v->ix = c->ix;
  v->ncell = pg->leaf ? pg->nCell : (u32)c->ix + 1;
  v->hdr = pg->childPtrSize;
  v->mask = pg->maskPage;
}

/* Moves past entry `ix` of the current page (the last one the assembly read from the view): 1 on
 * the next entry, 0 at the end, -1 error. The cursor is only moved by sqlite3BtreeNext (within a
 * leaf each call is an index step), and not at all when the page is the index's last leaf and
 * the assembly read it to its end, which is how a feed's like counts usually finish: every
 * ancestor was entered through its right-most child, so nothing follows. */
int td_next_after(BtCursor *c, u32 ix) {
  MemPage *pg = c->pPage;
  if (pg->leaf && ix + 1 >= pg->nCell) {
    int i = 0;
    while (i < c->iPage && c->aiIdx[i] == c->apPage[i]->nCell) i++;
    if (i == c->iPage) return 0;
  }
  while (c->ix < ix) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc != SQLITE_OK) return rc == SQLITE_DONE ? 0 : -1;
  }
  return td_next(c);
}

i64 td_rowid(BtCursor *c) { return sqlite3BtreeIntegerKey(c); }

static void check_tx(void) {
  if (in_tx && sqlite3_get_autocommit(db)) { in_tx = 0; tx_broken = 1; }
}

/* Text column `col` of a record: 0 ok, -1 not text or malformed. */
static int rec_text(const u8 *rec, u32 n, int col, const u8 **p, u32 *len) {
  u32 hdr, t, i, off;
  if (!n) return -1;
  i = getVarint32(rec, hdr);
  if (hdr > n) return -1;
  off = hdr;
  for (int c = 0;; c++) {
    if (i >= hdr) return -1;
    i += getVarint32(rec + i, t);
    u32 sz = sqlite3VdbeSerialTypeLen(t);
    if (off + sz > n) return -1;
    if (c == col) {
      if (t < 13 || !(t & 1)) return -1;
      *p = rec + off;
      *len = sz;
      return 0;
    }
    off += sz;
  }
}

/* INSERT, then the new row's created_at (the column default) read back through the posts cursor
 * in the same transaction. Same result as INSERT ... RETURNING, which buffers its rows in a
 * temporary B-tree (its own pager and page cache) built and torn down on every call.
 * 0 ok (id and created_at filled, created_at <= 63 bytes). */
int td_insert_post(i64 uid, const char *body, int len, i64 *id, char *created, int *clen) {
  sqlite3_bind_int64(st_post, 1, uid);
  sqlite3_bind_text(st_post, 2, body, len, SQLITE_STATIC);
  int rc = sqlite3_step(st_post);
  sqlite3_reset(st_post);
  sqlite3_clear_bindings(st_post);
  if (rc != SQLITE_DONE) { check_tx(); return -1; }
  *id = sqlite3_last_insert_rowid(db);
  const u8 *rec, *t;
  u32 n, tn;
  if (td_seek(cur[0], *id) != 1 || !(rec = td_payload(cur[0], &n)) || rec_text(rec, n, 3, &t, &tn) || tn > 63)
    return -1;
  memcpy(created, t, tn);
  *clen = (int)tn;
  return 0;
}

/* 1 inserted, 0 already liked, -1 error. */
int td_insert_like(i64 uid, i64 pid) {
  sqlite3_bind_int64(st_like, 1, uid);
  sqlite3_bind_int64(st_like, 2, pid);
  if (step_reset(st_like) != SQLITE_DONE) { check_tx(); return -1; }
  return sqlite3_changes(db) > 0;
}

/* SELECT 1: 0 ok, -1 with SQLite's message in err. */
int td_health(char *err, int n) {
  int rc = sqlite3_step(st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
  sqlite3_reset(st_health);
  if (!ok) { snprintf(err, n, "%s", sqlite3_errmsg(db)); return -1; }
  return 0;
}

/* The reference feed query, one row per call, for data the B-tree plan doesn't expect.
 * Row layout filled: id, likes, body/len, created/len, author/len. 1 row, 0 done, -1 error. */
struct td_row { i64 id, likes; const u8 *body; i64 blen; const u8 *ca; i64 clen; const u8 *au; i64 alen; };

int td_feed_sql_step(struct td_row *r) {
  int rc = sqlite3_step(st_feed);
  if (rc != SQLITE_ROW) { sqlite3_reset(st_feed); return rc == SQLITE_DONE ? 0 : -1; }
  r->id = sqlite3_column_int64(st_feed, 0);
  r->body = sqlite3_column_text(st_feed, 1); r->blen = sqlite3_column_bytes(st_feed, 1);
  r->ca = sqlite3_column_text(st_feed, 2); r->clen = sqlite3_column_bytes(st_feed, 2);
  r->au = sqlite3_column_text(st_feed, 3); r->alen = sqlite3_column_bytes(st_feed, 3);
  r->likes = sqlite3_column_int64(st_feed, 4);
  return 1;
}
