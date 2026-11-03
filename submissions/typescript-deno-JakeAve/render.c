/* SQLite plus one helper, compiled as one translation unit by build.sh: the helper reuses SQLite's own JSON
** escape table (so the output is byte-identical to json_object) and PGO trains both together.
** json_object + group_concat was 41% of the feed query's time (6.2 of 15.1 µs); this replaces it with one
** FFI call that steps the rows and writes the JSON into a buffer the caller owns. */
#include "vendor/sqlite3.c"

/* Appends z[0..n) as a JSON string, escaped like SQLite's jsonAppendString. 0 when out of room. */
static unsigned char *jsonStr(unsigned char *p, unsigned char *end, const unsigned char *z, int n){
  static const char special[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 'b', 't', 'n', 0, 'f', 'r', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,   0,   0,   0, 0,   0,   0, 0, 0
  };
  int i = 0, k;
  if( z==0 ){
    if( end-p<4 ) return 0;
    memcpy(p, "null", 4);
    return p+4;
  }
  if( p>=end ) return 0;
  *p++ = '"';
  for(;;){
    for(k=i; k<n && jsonIsOk[z[k]]; k++){}
    if( end-p<k-i+7 ) return 0; /* the run, one escape and the closing quote */
    memcpy(p, z+i, k-i);
    p += k-i;
    if( k==n ) break;
    unsigned char c = z[k];
    i = k+1;
    if( c=='\'' ){
      *p++ = c;
    }else if( c=='"' || c=='\\' ){
      *p++ = '\\';
      *p++ = c;
    }else if( special[c] ){
      *p++ = '\\';
      *p++ = special[c];
    }else{
      memcpy(p, "\\u00", 4);
      p[4] = "0123456789abcdef"[c>>4];
      p[5] = "0123456789abcdef"[c&0xf];
      p += 6;
    }
  }
  *p++ = '"';
  return p;
}

static unsigned char *jsonInt(unsigned char *p, unsigned char *end, sqlite3_int64 v){
  char tmp[24];
  int n = 0;
  sqlite3_uint64 u = v<0 ? -(sqlite3_uint64)v : (sqlite3_uint64)v;
  do{ tmp[n++] = '0' + u%10; u /= 10; }while( u );
  if( v<0 ) tmp[n++] = '-';
  if( end-p<n ) return 0;
  while( n ) *p++ = tmp[--n];
  return p;
}

static unsigned char *lit(unsigned char *p, unsigned char *end, const char *s, int n){
  if( end-p<n ) return 0;
  memcpy(p, s, n);
  return p+n;
}

/* A TEXT column as a JSON string. column_text before column_bytes, as SQLite requires. */
static unsigned char *jsonCol(unsigned char *p, unsigned char *end, sqlite3_stmt *s, int col){
  const unsigned char *z = sqlite3_column_text(s, col);
  return jsonStr(p, end, z, sqlite3_column_bytes(s, col));
}

/* Steps s, a bound statement whose rows are (id, body, created_at, author, like_count), and writes the JSON
** body into out: {"posts":[...]} over every row, or with single, {"post":{...}} from the first row.
** Returns the bytes written; 0 when single finds no row; -1 when cap is too small; -2 on an SQLite error
** (sqlite3_errmsg has it). Always resets s. */
int render_posts(sqlite3_stmt *s, unsigned char *out, int cap, int single){
  unsigned char *p = out, *end = out+cap;
  int rc = SQLITE_DONE, rows = 0;
  p = single ? lit(p, end, "{\"post\":", 8) : lit(p, end, "{\"posts\":[", 10);
  while( p && (rc = sqlite3_step(s))==SQLITE_ROW ){
    if( rows++ && !(p = lit(p, end, ",", 1)) ) break;
    if( (p = lit(p, end, "{\"id\":", 6))==0
     || (p = jsonInt(p, end, sqlite3_column_int64(s, 0)))==0
     || (p = lit(p, end, ",\"body\":", 8))==0
     || (p = jsonCol(p, end, s, 1))==0
     || (p = lit(p, end, ",\"created_at\":", 14))==0
     || (p = jsonCol(p, end, s, 2))==0
     || (p = lit(p, end, ",\"author\":", 10))==0
     || (p = jsonCol(p, end, s, 3))==0
     || (p = lit(p, end, ",\"like_count\":", 14))==0
     || (p = jsonInt(p, end, sqlite3_column_int64(s, 4)))==0
     || (p = lit(p, end, "}", 1))==0 ) break;
    if( single ) break;
  }
  if( p==0 ){ sqlite3_reset(s); return -1; }
  if( rc!=SQLITE_ROW && rc!=SQLITE_DONE ){ sqlite3_reset(s); return -2; }
  sqlite3_reset(s);
  if( single && rows==0 ) return 0;
  p = single ? lit(p, end, "}", 1) : lit(p, end, "]}", 2);
  return p ? (int)(p-out) : -1;
}

/* The off-thread checkpointer: a pthread with its own connection that runs a checkpoint every interval_ms, so
** the request thread never pays for one: PASSIVE, or TRUNCATE once the WAL is past one 4096-frame hash segment
** (reads walk every segment: bench/results.md "does a long WAL cost anything?"). TRUNCATE takes the WAL write
** lock (waiting, up to the busy timeout, for the commit in flight), backfills, then waits for readers on old
** snapshots and resets the WAL. counters[4] is 1 throughout, and the request thread, seeing it, sends writes to
** its queue, ends its idle write-connection snapshot and refreshes its read connection every batch, so the lock
** is never contended on the request thread and the reader wait drains. Counters [runs, worst_ms, log, ckpt,
** waiting]: checkpoints run, the slowest one in ms, the last call's WAL frame count and frames backfilled
** (equal when it caught up), and the TRUNCATE flag. Written only by the
** thread (ckpt_stats may zero worst_ms), read over FFI with __atomic loads. ckpt_start pins this library
** (RTLD_NODELETE) so the detached thread never wakes into unmapped code when a short-lived process exits and the
** runtime dlcloses it (it segfaulted). */
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static sqlite3_int64 ckptCounters[5];
static int ckptStarted, ckptIntervalMs;

static sqlite3_int64 nowMs(void){
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (sqlite3_int64)t.tv_sec*1000 + t.tv_nsec/1000000;
}

static void *ckptLoop(void *arg){
  sqlite3 *db = arg;
  struct timespec iv = { ckptIntervalMs/1000, (ckptIntervalMs%1000)*1000000L };
  for(;;){
    int log = -1, ckpt = -1;
    nanosleep(&iv, 0);
    int mode = __atomic_load_n(&ckptCounters[2], __ATOMIC_RELAXED)>4096 ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE;
    sqlite3_int64 t = nowMs();
    if( mode==SQLITE_CHECKPOINT_TRUNCATE ) __atomic_store_n(&ckptCounters[4], 1, __ATOMIC_RELEASE);
    sqlite3_wal_checkpoint_v2(db, 0, mode, &log, &ckpt);
    __atomic_store_n(&ckptCounters[4], 0, __ATOMIC_RELEASE);
    t = nowMs()-t;
    sqlite3_int64 w = __atomic_load_n(&ckptCounters[1], __ATOMIC_RELAXED);
    /* CAS, not load-store, so a reset from ckpt_stats in between is never overwritten by a stale max. */
    while( t>w && !__atomic_compare_exchange_n(&ckptCounters[1], &w, t, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ){}
    __atomic_store_n(&ckptCounters[2], log, __ATOMIC_RELAXED);
    __atomic_store_n(&ckptCounters[3], ckpt, __ATOMIC_RELAXED);
    __atomic_store_n(&ckptCounters[0], ckptCounters[0]+1, __ATOMIC_RELEASE);
  }
  return 0;
}

/* Opens path on a new connection (journal_mode=WAL, synchronous=sync as the caller's, no autocheckpoint) and
** starts the checkpointer on it. Returns 0, or an SQLite error code: SQLITE_MISUSE on a second call (one per
** process), SQLITE_ERROR if the library cannot be pinned, else whatever open, the pragmas or pthread_create
** failed with. */
int ckpt_start(const char *path, int interval_ms, int sync){
  sqlite3 *db = 0;
  pthread_t th;
  char sql[96];
  Dl_info info;
  static void *self; /* the pin: never dlclosed */
  int rc;
  if( __atomic_exchange_n(&ckptStarted, 1, __ATOMIC_ACQ_REL) ) return SQLITE_MISUSE;
  if( !self && dladdr((void*)ckpt_start, &info) ) self = dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
  if( !self ){
    __atomic_store_n(&ckptStarted, 0, __ATOMIC_RELEASE);
    return SQLITE_ERROR;
  }
  sqlite3_snprintf(sizeof sql, sql,
    "PRAGMA journal_mode = WAL; PRAGMA synchronous = %d; PRAGMA wal_autocheckpoint = 0;", sync);
  rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, 0);
  if( rc==SQLITE_OK ) rc = sqlite3_exec(db, sql, 0, 0, 0);
  if( rc==SQLITE_OK ) sqlite3_busy_timeout(db, 50); /* TRUNCATE's wait for the write lock and for readers */
  ckptIntervalMs = interval_ms; /* read by the thread after pthread_create, which orders it */
  if( rc==SQLITE_OK && pthread_create(&th, 0, ckptLoop, db) ) rc = SQLITE_ERROR;
  if( rc!=SQLITE_OK ){
    sqlite3_close(db);
    __atomic_store_n(&ckptStarted, 0, __ATOMIC_RELEASE);
    return rc;
  }
  pthread_detach(th);
  return SQLITE_OK;
}

/* Copies the counters into out[4]: runs, worst_ms, log, ckpt. reset_worst nonzero zeroes worst_ms as it reads it,
** so each caller window gets its own worst. */
void ckpt_stats(sqlite3_int64 *out, int reset_worst){
  for(int i=0; i<4; i++) out[i] = __atomic_load_n(&ckptCounters[i], __ATOMIC_ACQUIRE);
  if( reset_worst ) out[1] = __atomic_exchange_n(&ckptCounters[1], 0, __ATOMIC_ACQ_REL);
}

/* The counters array itself, for a JS view that reads runs without a call (ckptRuns in sqlite.ts). */
sqlite3_int64 *ckpt_counters(void){ return ckptCounters; }

/* The commit thread for server.ts's write connection. The request thread runs a batch's INSERTs on that
** connection, then wr_commit() hands it over: this thread runs COMMIT (the fsync, under synchronous=FULL) and a
** fresh BEGIN, bumps done, and writes wake_fd (an eventfd in the poller's epoll set) so px_wait returns. Until
** done moves the connection is this thread's; JS queues writes meanwhile and reads go to its own connection,
** so a 2-3 s disk stall delays writes only. Counters [done, worst_ms, last_rc]: a failed COMMIT is rolled back
** here and reported through last_rc. Pinned by ckpt_start, which runs first. */
static sqlite3_int64 wrCounters[3];
static pthread_mutex_t wrMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wrCv = PTHREAD_COND_INITIALIZER;
static int wrPending, wrWakeFd;
static sqlite3_stmt *wrCommit, *wrBegin, *wrRollback;

static void *wrLoop(void *arg){
  (void)arg;
  for(;;){
    pthread_mutex_lock(&wrMu);
    while( !wrPending ) pthread_cond_wait(&wrCv, &wrMu);
    wrPending = 0;
    pthread_mutex_unlock(&wrMu);
    sqlite3_int64 t = nowMs();
    int rc = sqlite3_step(wrCommit);
    sqlite3_reset(wrCommit);
    if( rc==SQLITE_DONE ){
      rc = SQLITE_OK;
    }else{
      sqlite3_step(wrRollback); /* no-op if SQLite already rolled it back */
      sqlite3_reset(wrRollback);
    }
    sqlite3_step(wrBegin);
    sqlite3_reset(wrBegin);
    t = nowMs()-t;
    sqlite3_int64 w = __atomic_load_n(&wrCounters[1], __ATOMIC_RELAXED);
    while( t>w && !__atomic_compare_exchange_n(&wrCounters[1], &w, t, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ){}
    __atomic_store_n(&wrCounters[2], rc, __ATOMIC_RELAXED);
    __atomic_store_n(&wrCounters[0], wrCounters[0]+1, __ATOMIC_RELEASE);
    sqlite3_uint64 one = 1;
    (void)!write(wrWakeFd, &one, 8); /* a missed wake only delays the release to px_wait's timeout */
  }
  return 0;
}

/* Prepares COMMIT/BEGIN/ROLLBACK on db (the caller's open write transaction stays its) and starts the thread.
** Once per process. 0, or an SQLite error code. */
int wr_start(sqlite3 *db, int wake_fd){
  pthread_t th;
  int rc;
  if( wrCommit ) return SQLITE_MISUSE;
  if( (rc = sqlite3_prepare_v3(db, "COMMIT", -1, SQLITE_PREPARE_PERSISTENT, &wrCommit, 0))!=SQLITE_OK
   || (rc = sqlite3_prepare_v3(db, "BEGIN", -1, SQLITE_PREPARE_PERSISTENT, &wrBegin, 0))!=SQLITE_OK
   || (rc = sqlite3_prepare_v3(db, "ROLLBACK", -1, SQLITE_PREPARE_PERSISTENT, &wrRollback, 0))!=SQLITE_OK ) return rc;
  wrWakeFd = wake_fd;
  if( pthread_create(&th, 0, wrLoop, 0) ) return SQLITE_ERROR;
  pthread_detach(th);
  return SQLITE_OK;
}

/* Hands the connection to the thread for COMMIT; BEGIN. The caller must not touch it until done moves. */
void wr_commit(void){
  pthread_mutex_lock(&wrMu);
  wrPending = 1;
  pthread_cond_signal(&wrCv);
  pthread_mutex_unlock(&wrMu);
}

/* out[3] = done, worst_ms, last_rc; reset_worst nonzero zeroes worst_ms as it reads it. */
void wr_stats(sqlite3_int64 *out, int reset_worst){
  for(int i=0; i<3; i++) out[i] = __atomic_load_n(&wrCounters[i], __ATOMIC_ACQUIRE);
  if( reset_worst ) out[1] = __atomic_exchange_n(&wrCounters[1], 0, __ATOMIC_ACQ_REL);
}

sqlite3_int64 *wr_counters(void){ return wrCounters; }
