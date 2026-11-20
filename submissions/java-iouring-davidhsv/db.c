/* SQLite: the unmodified amalgamation plus the calls Server.java makes into it.
 *
 * Reads (GET /feed, GET /posts/:id) move SQLite's own B-tree cursors inside the pass's read
 * transaction, so only the SQL bytecode engine is skipped: pages come through SQLite's pager, WAL
 * and page cache. This file moves a cursor and hands Java the address of the current row's record
 * bytes on SQLite's page; Java decodes the record in place and decides what to read next. Only
 * functions of SQLite's B-tree interface (btree.h) are called here, and no SQLite structure is
 * written. Writes, BEGIN/COMMIT and /health are ordinary prepared statements.
 *
 * Everything runs on the event-loop thread except WAL checkpoints, which a second thread runs
 * on its own connection when Server.java asks (design from rust-iouring-kensac #264), so
 * SQLite is built SQLITE_THREADSAFE=2: each connection is used by one thread. */
#include "sqlite3.c"
#include <pthread.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "feed.h"

/* The trees a pass keeps a cursor on (the same numbers as in Server.java). */
enum { T_POSTS, T_USERS, T_FEED, T_LIKES, NT };

static sqlite3 *db;
static Btree *bt;
static BtCursor *cur[NT];
static Pgno root[NT];
static KeyInfo *keys[NT];
static sqlite3_stmt *q_begin, *q_commit, *q_rollback, *q_post, *q_like, *q_ping, *q_feed;
static int in_pass;      /* the pass's transaction is open, with its cursors */
static int pass_lost;    /* SQLite rolled it back by itself: nothing more may run in this pass */
/* A record with overflow pages (none with this schema) is assembled here, one buffer per tree,
 * so that records of different cursors can be read side by side as they can on their pages. */
static u8 *spill[NT];
static u32 spill_cap[NT];
static int wal_size;     /* frames in the log after the last commit that wrote */
static feed_io_t io;
static char errbuf[512]; /* why db_open or db_ping failed */
static char created[64]; /* a new post's created_at */

API feed_io_t *db_io(void) { return &io; }
API const char *db_error(void) { return errbuf; }

/* ---- what FEED_STATS reports about the disk ---- */

/* How long the event-loop thread spends in the log's writes and syncs, and how long each
 * checkpoint takes on its thread: the places where a slow disk would hold up the loop or the
 * write queue. Counted always (a clock read per write call); reported only with FEED_STATS. */
enum { S_WRITE_N, S_WRITE_US, S_WRITE_MAX, S_SYNC_N, S_SYNC_US, S_SYNC_MAX, S_CK_N, S_CK_US, S_CK_MAX, NS };
static int64_t io_stats[NS];
static pthread_t loop_thread;

static int64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void account(int n, int us, int max, int64_t t) {
  __atomic_add_fetch(&io_stats[n], 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&io_stats[us], t, __ATOMIC_RELAXED);
  if (t > __atomic_load_n(&io_stats[max], __ATOMIC_RELAXED)) __atomic_store_n(&io_stats[max], t, __ATOMIC_RELAXED);
}

/* Copies the counters to out[0..8] and starts them over. */
API void db_stats_take(int64_t *out) {
  for (int i = 0; i < NS; i++) out[i] = __atomic_exchange_n(&io_stats[i], 0, __ATOMIC_RELAXED);
}

/* ---- the checkpoint thread ---- */

/* On one vCPU a checkpoint at the loop's priority would take the CPU from it for as long as it
 * runs; at nice 19 it runs in the loop's idle time. */
#define CK_NICE 19
enum { CK_IDLE, CK_ASKED, CK_DONE_ALL, CK_DONE_SOME };
static sqlite3 *ck_db;
static int ck_state;
/* The log's restart left its new header unsynced (below, "the loop never syncs"): the checkpoint
 * thread syncs it first thing. */
static int header_unsynced;
static int wal_fd = -1;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;

static void *ck_main(void *arg) {
  (void)arg;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), CK_NICE);
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (ck_state != CK_ASKED) pthread_cond_wait(&ck_cv, &ck_mu);
    pthread_mutex_unlock(&ck_mu);
    if (__atomic_exchange_n(&header_unsynced, 0, __ATOMIC_ACQ_REL) && wal_fd >= 0) fdatasync(wal_fd);
    /* PASSIVE never waits on the loop's locks, and the loop never waits for this thread. */
    int in_log = -1, copied = -2;
    int64_t t0 = mono_us();
    int rc = sqlite3_wal_checkpoint_v2(ck_db, "main", SQLITE_CHECKPOINT_PASSIVE, &in_log, &copied);
    account(S_CK_N, S_CK_US, S_CK_MAX, mono_us() - t0);
    __atomic_store_n(&ck_state, rc == SQLITE_OK && in_log == copied ? CK_DONE_ALL : CK_DONE_SOME, __ATOMIC_RELEASE);
  }
  return 0;
}

/* Asks the thread for one PASSIVE checkpoint. */
API void db_ck_ask(void) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, CK_ASKED, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

/* 0 idle, 1 copying, 2 done with the whole log copied, 3 done with frames left. A finished
 * state is reported once. */
API int32_t db_ck_poll(void) {
  int s = __atomic_load_n(&ck_state, __ATOMIC_ACQUIRE);
  if (s == CK_DONE_ALL || s == CK_DONE_SOME) __atomic_store_n(&ck_state, CK_IDLE, __ATOMIC_RELEASE);
  return s;
}

API int64_t db_wal_frames(void) { return wal_size; }

/* The whole log is copied back: the next write starts it over, and the hook reports its size. */
API void db_wal_restarts(void) { wal_size = 0; }

static int on_commit_frames(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_size = frames;
  return SQLITE_OK;
}

/* ---- the log's writes: one per commit ---- */

/* SQLite appends each frame to the log with two writes (a 24-byte header, then the page), so a
 * commit of forty frames is eighty system calls. "feed" is a VFS over unix-excl, made with
 * SQLite's public VFS interface, that changes one thing: on the log file, the headers and pages
 * of frames appended at the log's end are gathered in memory, and all of it is written out,
 * inside SQLite's own write call, the moment the page of a commit frame arrives (the idea of
 * goose-iouring-ahmadsh2007 #291). That is before the frames enter the WAL index and before
 * COMMIT returns, the point by which SQLite itself would have written them, so a commit is
 * exactly as durable as without it. Gathered frames of a transaction that has not committed
 * are not on disk yet, and need not be.
 *
 * Only an append is ever held back. SQLite also rewrites frames in place (a page written twice
 * in a transaction that spilled, and then the checksums in the headers after it): those writes
 * are not at the log's end, so they go straight through, like every write that is not a frame.
 * And every read, sync, truncate, size query, file control and close first writes out what is
 * gathered. The database file is opened by unix-excl itself and never passes through here. */
#define GATHER_MAX (1 << 20)
/* unix-excl writes at most 0x1ffff bytes per call. */
#define GATHER_STEP 0x1f000

typedef struct {
  sqlite3_file base;
  sqlite3_file *real; /* unix-excl's file, in the memory after this struct */
  u8 *buf;
  i64 at;     /* where the gathered bytes start in the file */
  i64 tail;   /* where the log ends, as far as this file's writes show it; -1 not known yet */
  i64 lone;   /* the previous operation wrote a frame header through on its own: where, else -1 */
  int n;      /* bytes gathered */
  int header; /* the last gathered piece is a frame header: only its page may follow */
  int commit; /* ... of a commit frame */
  int fresh;  /* the last write was a new log header: the next sync is the restart's */
} log_file;

static sqlite3_vfs feed_vfs;
static sqlite3_vfs *excl_vfs;

/* unix-excl's write and sync, timed when the event-loop thread makes them. */
static int real_write(log_file *f, const void *data, int amt, i64 off) {
  if (!pthread_equal(pthread_self(), loop_thread)) return f->real->pMethods->xWrite(f->real, data, amt, off);
  int64_t t0 = mono_us();
  int rc = f->real->pMethods->xWrite(f->real, data, amt, off);
  account(S_WRITE_N, S_WRITE_US, S_WRITE_MAX, mono_us() - t0);
  return rc;
}

static int real_sync(log_file *f, int flags) {
  if (!pthread_equal(pthread_self(), loop_thread)) return f->real->pMethods->xSync(f->real, flags);
  int64_t t0 = mono_us();
  int rc = f->real->pMethods->xSync(f->real, flags);
  account(S_SYNC_N, S_SYNC_US, S_SYNC_MAX, mono_us() - t0);
  return rc;
}

/* Writes out what is gathered. Every operation but a write that is held back comes through
 * here first. */
static int log_flush(log_file *f) {
  int rc = SQLITE_OK;
  f->lone = -1;
  if (f->n > 0) f->fresh = 0;
  for (int done = 0; done < f->n && rc == SQLITE_OK;) {
    int step = f->n - done < GATHER_STEP ? f->n - done : GATHER_STEP;
    rc = real_write(f, f->buf + done, step, f->at + done);
    done += step;
  }
  f->n = 0;
  f->header = 0;
  f->commit = 0;
  return rc;
}

static int log_write(sqlite3_file *file, const void *data, int amt, sqlite3_int64 off) {
  log_file *f = (log_file *)file;
  if (f->header) {
    if (off == f->at + f->n && amt >= 512 && amt <= 65536) {
      /* the page of the header that is held: the frame is whole */
      memcpy(f->buf + f->n, data, (size_t)amt);
      f->n += amt;
      f->header = 0;
      f->lone = -1;
      f->tail = off + amt;
      return f->commit ? log_flush(f) : SQLITE_OK;
    }
    int rc = log_flush(f);
    if (rc != SQLITE_OK) return rc;
  }
  if (amt == WAL_FRAME_HDRSIZE && off == f->tail && (f->n == 0 || off == f->at + f->n)) {
    /* a new frame at the log's end: hold its header until its page comes */
    if (f->n + WAL_FRAME_HDRSIZE + 65536 > GATHER_MAX) {
      int rc = log_flush(f);
      if (rc != SQLITE_OK) return rc;
    }
    if (f->n == 0) f->at = off;
    memcpy(f->buf + f->n, data, WAL_FRAME_HDRSIZE);
    f->n += WAL_FRAME_HDRSIZE;
    f->header = 1;
    f->lone = -1;
    /* bytes 4-7: the database size after the commit, zero for a frame that does not commit */
    f->commit = sqlite3Get4byte((const u8 *)data + 4) != 0;
    return SQLITE_OK;
  }
  /* Anything else is written now, after what was gathered before it. */
  i64 lone = f->lone;
  int rc = log_flush(f);
  if (rc != SQLITE_OK) return rc;
  rc = real_write(f, data, amt, off);
  /* Where the log ends: after its own header when the log starts over, and after a frame that
   * was just written through whole, its header and then, as the very next operation, its page.
   * That is an append, or the last frame being rewritten; either way the log ends behind it. A
   * header on its own (a checksum rewrite) says nothing, so `tail` is never short of the end,
   * and a header held back above is always a new frame whose page comes next. */
  if (off == 0) f->tail = amt;
  else if (amt == WAL_FRAME_HDRSIZE) f->lone = off;
  else if (lone >= 0 && off == lone + WAL_FRAME_HDRSIZE) f->tail = off + amt;
  f->fresh = off == 0 && amt == WAL_HDRSIZE;
  return rc;
}

static int log_read(sqlite3_file *file, void *data, int amt, sqlite3_int64 off) {
  log_file *f = (log_file *)file;
  int rc = log_flush(f);
  return rc != SQLITE_OK ? rc : f->real->pMethods->xRead(f->real, data, amt, off);
}

static int log_truncate(sqlite3_file *file, sqlite3_int64 size) {
  log_file *f = (log_file *)file;
  int rc = log_flush(f);
  return rc != SQLITE_OK ? rc : f->real->pMethods->xTruncate(f->real, size);
}

/* The loop never syncs. With synchronous=NORMAL a commit does not sync, so the one sync SQLite
 * asks of the loop's connection is the log header's, when a write starts the log over after a
 * complete checkpoint. That sync orders the new header before any later write to the database
 * file (SQLite's ticket ff5be73dee: otherwise a power loss could leave the old header to replay
 * old frames over newer pages). The database file is only ever written by a checkpoint, on the
 * checkpoint thread, and that thread syncs the header first (ck_main), and SQLite's checkpoint
 * itself syncs the log before it copies a frame. So the header is synced in time, by the thread
 * that waits for the disk anyway, and a slow fsync never holds up the loop: on a slow disk it
 * took hundreds of milliseconds there. FEED_LOOP_SYNC (a test knob) syncs on the loop as SQLite
 * would. */
static int loop_sync;

static int log_sync(sqlite3_file *file, int flags) {
  log_file *f = (log_file *)file;
  int rc = log_flush(f);
  if (rc != SQLITE_OK) return rc;
  if (f->fresh && !loop_sync && pthread_equal(pthread_self(), loop_thread)) {
    f->fresh = 0;
    __atomic_store_n(&header_unsynced, 1, __ATOMIC_RELEASE);
    return SQLITE_OK;
  }
  return real_sync(f, flags);
}

static int log_size(sqlite3_file *file, sqlite3_int64 *size) {
  log_file *f = (log_file *)file;
  int rc = log_flush(f);
  return rc != SQLITE_OK ? rc : f->real->pMethods->xFileSize(f->real, size);
}

static int log_control(sqlite3_file *file, int op, void *arg) {
  log_file *f = (log_file *)file;
  int rc = log_flush(f);
  return rc != SQLITE_OK ? rc : f->real->pMethods->xFileControl(f->real, op, arg);
}

static int log_close(sqlite3_file *file) {
  log_file *f = (log_file *)file;
  int rc = log_flush(f);
  int rc2 = f->real->pMethods->xClose(f->real);
  sqlite3_free(f->buf);
  f->buf = 0;
  return rc != SQLITE_OK ? rc : rc2;
}

static int log_lock(sqlite3_file *file, int level) {
  log_file *f = (log_file *)file;
  return f->real->pMethods->xLock(f->real, level);
}

static int log_unlock(sqlite3_file *file, int level) {
  log_file *f = (log_file *)file;
  return f->real->pMethods->xUnlock(f->real, level);
}

static int log_reserved(sqlite3_file *file, int *out) {
  log_file *f = (log_file *)file;
  return f->real->pMethods->xCheckReservedLock(f->real, out);
}

static int log_sector(sqlite3_file *file) {
  log_file *f = (log_file *)file;
  return f->real->pMethods->xSectorSize(f->real);
}

static int log_traits(sqlite3_file *file) {
  log_file *f = (log_file *)file;
  return f->real->pMethods->xDeviceCharacteristics(f->real);
}

static const sqlite3_io_methods log_methods = {
  1, log_close, log_read, log_write, log_truncate, log_sync, log_size, log_lock, log_unlock, log_reserved,
  log_control, log_sector, log_traits,
};

/* FEED_WRITEBACK_KB (a test knob, off unless set): the database file is wrapped too, and after
 * every that many KB the checkpoint thread writes to it, it starts the kernel's writeback of what
 * it wrote (sync_file_range, which does not wait), so that the checkpoint's final fsync finds less
 * to write and the disk is used evenly. Everything else passes straight through. */
typedef struct {
  sqlite3_file base;
  sqlite3_file *real; /* unix-excl's file, in the memory after this struct */
  i64 pending;        /* bytes written since writeback was last started */
} main_file;

static i64 writeback_bytes;

#define REAL(file) (((main_file *)(file))->real)
static int mf_close(sqlite3_file *f) { return REAL(f)->pMethods->xClose(REAL(f)); }
static int mf_read(sqlite3_file *f, void *p, int n, sqlite3_int64 off) { return REAL(f)->pMethods->xRead(REAL(f), p, n, off); }
static int mf_write(sqlite3_file *file, const void *p, int n, sqlite3_int64 off) {
  main_file *f = (main_file *)file;
  int rc = f->real->pMethods->xWrite(f->real, p, n, off);
  if (rc == SQLITE_OK && !pthread_equal(pthread_self(), loop_thread) && (f->pending += n) >= writeback_bytes) {
    sync_file_range(((unixFile *)f->real)->h, 0, 0, SYNC_FILE_RANGE_WRITE);
    f->pending = 0;
  }
  return rc;
}
static int mf_truncate(sqlite3_file *f, sqlite3_int64 size) { return REAL(f)->pMethods->xTruncate(REAL(f), size); }
static int mf_sync(sqlite3_file *f, int flags) { ((main_file *)f)->pending = 0; return REAL(f)->pMethods->xSync(REAL(f), flags); }
static int mf_size(sqlite3_file *f, sqlite3_int64 *size) { return REAL(f)->pMethods->xFileSize(REAL(f), size); }
static int mf_lock(sqlite3_file *f, int level) { return REAL(f)->pMethods->xLock(REAL(f), level); }
static int mf_unlock(sqlite3_file *f, int level) { return REAL(f)->pMethods->xUnlock(REAL(f), level); }
static int mf_reserved(sqlite3_file *f, int *out) { return REAL(f)->pMethods->xCheckReservedLock(REAL(f), out); }
static int mf_control(sqlite3_file *f, int op, void *arg) { return REAL(f)->pMethods->xFileControl(REAL(f), op, arg); }
static int mf_sector(sqlite3_file *f) { return REAL(f)->pMethods->xSectorSize(REAL(f)); }
static int mf_traits(sqlite3_file *f) { return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f)); }
static int mf_shm_map(sqlite3_file *f, int pg, int sz, int ext, void volatile **pp) { return REAL(f)->pMethods->xShmMap(REAL(f), pg, sz, ext, pp); }
static int mf_shm_lock(sqlite3_file *f, int off, int n, int flags) { return REAL(f)->pMethods->xShmLock(REAL(f), off, n, flags); }
static void mf_shm_barrier(sqlite3_file *f) { REAL(f)->pMethods->xShmBarrier(REAL(f)); }
static int mf_shm_unmap(sqlite3_file *f, int del) { return REAL(f)->pMethods->xShmUnmap(REAL(f), del); }
static int mf_fetch(sqlite3_file *f, sqlite3_int64 off, int n, void **pp) { return REAL(f)->pMethods->xFetch(REAL(f), off, n, pp); }
static int mf_unfetch(sqlite3_file *f, sqlite3_int64 off, void *p) { return REAL(f)->pMethods->xUnfetch(REAL(f), off, p); }
#undef REAL

static const sqlite3_io_methods main_methods = {
  3, mf_close, mf_read, mf_write, mf_truncate, mf_sync, mf_size, mf_lock, mf_unlock, mf_reserved, mf_control,
  mf_sector, mf_traits, mf_shm_map, mf_shm_lock, mf_shm_barrier, mf_shm_unmap, mf_fetch, mf_unfetch,
};

static int feed_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *file, int flags, int *out_flags) {
  (void)vfs;
  if ((flags & SQLITE_OPEN_MAIN_DB) && writeback_bytes > 0) {
    main_file *m = (main_file *)file;
    memset(m, 0, sizeof *m);
    m->real = (sqlite3_file *)(m + 1);
    int rc = excl_vfs->xOpen(excl_vfs, name, m->real, flags, out_flags);
    if (rc != SQLITE_OK) return rc;
    if (m->real->pMethods->iVersion < 3) { m->real->pMethods->xClose(m->real); return SQLITE_CANTOPEN; }
    m->base.pMethods = &main_methods;
    return SQLITE_OK;
  }
  if (!(flags & SQLITE_OPEN_WAL)) return excl_vfs->xOpen(excl_vfs, name, file, flags, out_flags);
  log_file *f = (log_file *)file;
  memset(f, 0, sizeof *f);
  f->tail = -1;
  f->lone = -1;
  f->real = (sqlite3_file *)(f + 1);
  f->buf = sqlite3_malloc(GATHER_MAX);
  if (!f->buf) return SQLITE_NOMEM;
  int rc = excl_vfs->xOpen(excl_vfs, name, f->real, flags, out_flags);
  if (rc != SQLITE_OK) {
    sqlite3_free(f->buf);
    f->buf = 0;
    return rc; /* base.pMethods stays null, so SQLite does not call xClose */
  }
  f->base.pMethods = &log_methods;
  if (pthread_equal(pthread_self(), loop_thread)) wal_fd = ((unixFile *)f->real)->h;
  return SQLITE_OK;
}

/* Registers "feed". With FEED_PLAIN_LOG set (a test knob), connections use unix-excl as it is. */
static const char *vfs_name(void) {
  if (getenv("FEED_PLAIN_LOG")) return "unix-excl";
  if (!excl_vfs) {
    excl_vfs = sqlite3_vfs_find("unix-excl");
    if (!excl_vfs) return "unix-excl";
    feed_vfs = *excl_vfs;
    feed_vfs.zName = "feed";
    feed_vfs.pNext = 0;
    feed_vfs.szOsFile = (int)(sizeof(log_file) > sizeof(main_file) ? sizeof(log_file) : sizeof(main_file)) + excl_vfs->szOsFile;
    loop_sync = getenv("FEED_LOOP_SYNC") != 0;
    if (getenv("FEED_WRITEBACK_KB")) writeback_bytes = (i64)atoi(getenv("FEED_WRITEBACK_KB")) * 1024;
    feed_vfs.xOpen = feed_open;
    if (sqlite3_vfs_register(&feed_vfs, 0) != SQLITE_OK) return "unix-excl";
  }
  return "feed";
}


/* ---- opening ---- */

static void explain(const char *what, sqlite3 *d) {
  snprintf(errbuf, sizeof errbuf, "%s: %s", what, d ? sqlite3_errmsg(d) : "no connection");
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

/* 0, or -1 with the reason in db_error(). */
API int32_t db_open(const uint8_t *path_p, int64_t path_len) {
  char path[4096];
  if (path_len <= 0 || path_len >= (int64_t)sizeof path) {
    snprintf(errbuf, sizeof errbuf, "bad SQLITE_PATH");
    return -1;
  }
  memcpy(path, path_p, (size_t)path_len);
  path[path_len] = 0;
  loop_thread = pthread_self();
  if (sqlite3_initialize() != SQLITE_OK) { snprintf(errbuf, sizeof errbuf, "sqlite3_initialize"); return -1; }
  /* unix-excl (under "feed", above): this process locks the file once and keeps it, so no other
   * process can open it, and the WAL index is heap memory shared by the process's two
   * connections (no -shm file, no file locks per transaction). */
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs_name()) != SQLITE_OK) {
    explain("open", db);
    return -1;
  }
  /* WAL with synchronous=NORMAL: a commit survives a process crash once COMMIT returns (rule 6).
   * No mmap: a page in SQLite's own cache keeps its parsed header and needs no WAL lookup. */
  if (sqlite3_exec(db,
                   "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;"
                   "PRAGMA cache_size = -65536; PRAGMA mmap_size = 0; PRAGMA wal_autocheckpoint = 0;"
                   "SELECT count(*) FROM sqlite_schema",
                   0, 0, 0) != SQLITE_OK) {
    explain("pragmas", db);
    return -1;
  }
  /* FEED_CACHE_PAGES (a test knob): a small page cache, so that a large transaction spills
   * frames to the log before it commits. */
  if (getenv("FEED_CACHE_PAGES")) {
    char sql[64];
    snprintf(sql, sizeof sql, "PRAGMA cache_size = %d", atoi(getenv("FEED_CACHE_PAGES")));
    sqlite3_exec(db, sql, 0, 0, 0);
  }
  sqlite3_wal_hook(db, on_commit_frames, 0);
  /* Server.java decodes records by position: refuse any other column order. */
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
      snprintf(errbuf, sizeof errbuf, "unexpected schema: %s", checks[i][0]);
      return -1;
    }
  }
  if (ENC(db) != SQLITE_UTF8) { snprintf(errbuf, sizeof errbuf, "the database is not UTF-8"); return -1; }
  root[T_POSTS] = root_of("posts");
  root[T_USERS] = root_of("users");
  root[T_FEED] = root_of("posts_created_at_id_idx");
  root[T_LIKES] = root_of("likes_post_id_idx");
  keys[T_FEED] = key_info(2, 1);
  keys[T_LIKES] = key_info(1, 0);
  for (int t = 0; t < NT; t++) {
    cur[t] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!root[t] || !cur[t] || (t >= T_FEED && !keys[t])) { explain("tables", db); return -1; }
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
    explain("prepare", db);
    return -1;
  }
  /* The checkpoint connection copies frames from the log; it reads no tables. */
  if (sqlite3_open_v2(path, &ck_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs_name()) != SQLITE_OK ||
      sqlite3_exec(ck_db, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16", 0, 0, 0) !=
        SQLITE_OK) {
    explain("checkpoint connection", ck_db);
    return -1;
  }
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 512 << 10);
  if (pthread_create(&t, &a, ck_main, 0) != 0) { snprintf(errbuf, sizeof errbuf, "checkpoint thread"); return -1; }
  pthread_attr_destroy(&a);
  return 0;
}

/* ---- the pass's transaction ---- */

static void close_cursors(void) {
  for (int t = 0; t < NT; t++) sqlite3BtreeCloseCursor(cur[t]);
}

/* Joins the pass's transaction, opening it (and a cursor on each tree) for the first request
 * that needs the database: its reads share one snapshot and its writes one COMMIT. 0, or -1. */
API int32_t db_begin(void) {
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

/* Ends the pass's transaction: 1 once it is committed (or there was none), 0 if it was rolled
 * back. No response of the pass is sent before this returns. */
API int32_t db_commit(void) {
  if (pass_lost) {
    pass_lost = 0;
    if (!sqlite3_get_autocommit(db)) run(q_rollback);
    return 0;
  }
  if (!in_pass) return 1;
  in_pass = 0;
  close_cursors();
  if (run(q_commit) != SQLITE_DONE) {
    fprintf(stderr, "commit failed: %s\n", sqlite3_errmsg(db));
    if (!sqlite3_get_autocommit(db)) run(q_rollback);
    return 0;
  }
  return 1;
}

/* ---- cursors ---- */

/* Moves a table's cursor to rowid `key`: 1 there, 0 no such row, -1 error. */
API int32_t db_seek(int32_t t, int64_t key) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur[t], key, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && !sqlite3BtreeEof(cur[t]);
}

/* Moves an index's cursor to its first entry whose leading integer column is >= key: 1 on an
 * entry, 0 none, -1 error. */
API int32_t db_seek_ge(int32_t t, int64_t key) {
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
API int32_t db_first(int32_t t) {
  int empty = 1;
  if (sqlite3BtreeFirst(cur[t], &empty) != SQLITE_OK) return -1;
  return !empty;
}

/* 1 on the next entry, 0 at the end, -1 error. */
API int32_t db_next(int32_t t) {
  int rc = sqlite3BtreeNext(cur[t], 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* The current entry's record: its address (on SQLite's page, or assembled in the tree's spill
 * buffer when it has overflow pages), its length in io.len; 0 on an error. It stays valid until
 * this tree's cursor moves or the pass's transaction ends. */
API uint64_t db_rec(int32_t t) {
  BtCursor *c = cur[t];
  u32 total = sqlite3BtreePayloadSize(c);
  u32 local = 0;
  const void *p = sqlite3BtreePayloadFetch(c, &local);
  io.len = total;
  if (local >= total) return (uint64_t)(uintptr_t)p;
  if (total > spill_cap[t]) {
    sqlite3_free(spill[t]);
    spill_cap[t] = total + 4096;
    spill[t] = sqlite3_malloc(spill_cap[t]);
    if (!spill[t]) { spill_cap[t] = 0; return 0; }
  }
  if (sqlite3BtreePayload(c, 0, total, spill[t]) != SQLITE_OK) return 0;
  return (uint64_t)(uintptr_t)spill[t];
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

/* INSERT ... RETURNING id, created_at: 0 (io.id, and created_at in io.text[0], valid until the
 * next post), -1 error. */
API int32_t db_add_post(int64_t user_id, const uint8_t *body, int64_t body_len) {
  sqlite3_bind_int64(q_post, 1, user_id);
  sqlite3_bind_text(q_post, 2, (const char *)body, (int)body_len, SQLITE_STATIC);
  int ok = sqlite3_step(q_post) == SQLITE_ROW;
  if (ok) {
    io.id = sqlite3_column_int64(q_post, 0);
    const unsigned char *text = sqlite3_column_text(q_post, 1);
    int n = sqlite3_column_bytes(q_post, 1);
    ok = text && n > 0 && n < (int)sizeof created;
    if (ok) {
      memcpy(created, text, (size_t)n);
      io.text[0] = (uint64_t)(uintptr_t)created;
      io.text_len[0] = n;
    }
    ok = ok && sqlite3_step(q_post) == SQLITE_DONE;
  }
  sqlite3_reset(q_post);
  sqlite3_clear_bindings(q_post);
  if (!ok) after_failed_write();
  return ok ? 0 : -1;
}

/* 1 a new like, 0 already liked, -1 error. */
API int32_t db_add_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(q_like, 1, user_id);
  sqlite3_bind_int64(q_like, 2, post_id);
  if (run(q_like) != SQLITE_DONE) { after_failed_write(); return -1; }
  return sqlite3_changes(db) > 0;
}

/* SELECT 1: 0 ok, -1 with SQLite's message in db_error(). */
API int32_t db_ping(void) {
  int ok = sqlite3_step(q_ping) == SQLITE_ROW && sqlite3_column_int(q_ping, 0) == 1;
  sqlite3_reset(q_ping);
  if (ok) return 0;
  snprintf(errbuf, sizeof errbuf, "%s", sqlite3_errmsg(db));
  return -1;
}

/* One row of the reference feed query: 1 (io.id, io.likes, and io.text[] the body, created_at
 * and author, valid until the next step), 0 done, -1 error. */
API int32_t db_feed_step(void) {
  int rc = sqlite3_step(q_feed);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(q_feed);
    return rc == SQLITE_DONE ? 0 : -1;
  }
  io.id = sqlite3_column_int64(q_feed, 0);
  io.likes = sqlite3_column_int64(q_feed, 4);
  for (int k = 0; k < 3; k++) {
    const unsigned char *s = sqlite3_column_text(q_feed, k + 1);
    io.text[k] = (uint64_t)(uintptr_t)s;
    io.text_len[k] = s ? sqlite3_column_bytes(q_feed, k + 1) : 0;
  }
  return 1;
}

/* Abandons the reference query before its last row. */
API void db_feed_reset(void) { sqlite3_reset(q_feed); }
