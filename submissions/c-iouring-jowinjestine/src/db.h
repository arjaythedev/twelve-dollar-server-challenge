/* db.h: SQLite. One connection on the event loop serves every request of a batch inside one
 * transaction (group commit); a second connection on a low-priority thread runs PASSIVE
 * checkpoints. The database is opened through a small VFS shim over SQLite's own `unix-excl`
 * (this process is the only one that opens the file, so the wal-index lives in heap memory and
 * no fcntl() lock is taken per transaction) that writes each commit's WAL frames with one
 * pwrite. */
#ifndef DB_H
#define DB_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "btree.h"

#define VFS_NAME "feed-wal"
static int ckpt_frames = 4096;   /* WAL frames (16 MB) after which the bulk thread copies the log back */
static int ckpt_tail = 1 << 30;  /* the tail takes whatever one bulk pass left (CKPT_TAIL: converge to this few first) */
static uint64_t hold_max_ns = 250000000ULL; /* a hold's limit, whatever the log's length */
static int ckpt_log = 0;         /* CKPT_LOG=1: one line per cycle on stderr */
static long wb_kick_ms = 250;    /* the bulk thread starts the log's writeback this often while idle (0: never) */
#define CKPT_PASSES_MAX 8        /* bulk passes before the tail is taken regardless */
#define CKPT_FORCE 1048576 /* last resort (4 GB): the loop checkpoints by itself only if the disk gave no restart for minutes */
static int ckpt_nice = 19; /* CKPT_NICE: the bulk checkpoint thread's nice value */
static size_t dbwarm_bytes = 0; /* DBWARM_MB: the database file's first bytes kept in the page cache (off by default) */
static uint64_t dbwarm_s = 5;
#define DBWARM_MIN_AVAIL_KB (512L << 10) /* no warming while MemAvailable is below this: it must never fight reclaim */

enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };

struct db {
  sqlite3 *c;
  Btree *btree;
  sqlite3_stmt *st_begin, *st_begin_imm, *st_commit, *st_rollback, *st_health;
  uint64_t n_begin_busy; /* batches that found the write lock taken (the tail thread restarting the log) */
  sqlite3_stmt *st_ins_post, *st_ins_like, *st_post_exists;
  sqlite3_stmt *st_feed_ref, *st_post_ref;
  int in_tx;  /* the batch's transaction is open */
  int broken; /* SQLite ended it by itself (SQLITE_FULL, an I/O error): the batch is lost */
  int wrote;
  struct bt bt;
  /* checkpoints */
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int ck_state; /* written under mu, read with an atomic load on the loop */
  int ck_left;  /* frames the last pass could not copy (written meanwhile) */
  int ck_nlog;  /* frames the log held when that pass ended */
  int ck_passes;
  int ck_tail_frames;
  int ck_min_frames; /* the next cycle starts at this many frames: CKPT_FRAMES, or more after a slow tail */
  const char *path;
  int wal_frames;  /* from the wal hook, on the loop thread */
  int writes_held; /* the tail is being copied: writes wait (reads go on) */
  uint64_t hold_t0;
  /* stats */
  uint64_t n_ckpt, n_cycles, n_force, n_fallback, n_tail_short, n_passes_max, n_hold_timeout, n_tail_restart;
  int tail_restarted; /* the tail thread restarted the log itself (its one-frame transaction) */
  uint64_t bulk_t0, bulk_max_ns, hold_max_ns, hold_total_ns, tail_frames_max;
};

static int db_open_conn(sqlite3 **out, const char *path, int cache_kib);

static int db_exec(sqlite3 *c, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(c, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sqlite: %s: %s\n", sql, err ? err : sqlite3_errstr(rc));
    sqlite3_free(err);
  }
  return rc;
}
static int db_prep(sqlite3 *c, sqlite3_stmt **st, const char *sql) {
  int rc = sqlite3_prepare_v3(c, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sqlite prepare: %s: %s\n", sql, sqlite3_errmsg(c));
  return rc;
}

/* ---- page cache wrapper ----
 * SQLite's default page cache (pcache1) behind its plugin interface (SQLITE_CONFIG_PCACHE2),
 * with one decision changed: at commit, when no page past the end of the file can be in the
 * cache, the truncate does not walk the whole hash table. pcache1 remembers the largest page
 * number it ever held and never lowers it; a B-tree page split briefly renames a page to the
 * lock-byte page's number (1 GiB / page size + 1), after which every commit's truncate scans
 * every slot to find nothing. The wrapper tracks the largest real key and the pages parked at
 * that number. Pages that leave the cache only make the record an overestimate (safe). The idea
 * is from c-iouring-pavlyhalim (#245). */
#define LOCK_BYTE_OFFSET 0x40000000u
struct wcache {
  sqlite3_pcache *base;
  unsigned max_key, lock_page, n_parked;
};
static sqlite3_pcache_methods2 pcache_base;
static int wc_init(void *arg) { (void)arg; return pcache_base.xInit(pcache_base.pArg); }
static void wc_shutdown(void *arg) { (void)arg; pcache_base.xShutdown(pcache_base.pArg); }
static sqlite3_pcache *wc_create(int szPage, int szExtra, int purgeable) {
  struct wcache *w = sqlite3_malloc(sizeof *w);
  if (!w) return NULL;
  w->base = pcache_base.xCreate(szPage, szExtra, purgeable);
  if (!w->base) { sqlite3_free(w); return NULL; }
  w->max_key = 0;
  w->lock_page = LOCK_BYTE_OFFSET / (unsigned)szPage + 1;
  w->n_parked = 0;
  return (sqlite3_pcache *)w;
}
static void wc_cachesize(sqlite3_pcache *p, int n) { pcache_base.xCachesize(((struct wcache *)p)->base, n); }
static int wc_pagecount(sqlite3_pcache *p) { return pcache_base.xPagecount(((struct wcache *)p)->base); }
static sqlite3_pcache_page *wc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  struct wcache *w = (struct wcache *)p;
  sqlite3_pcache_page *pg = pcache_base.xFetch(w->base, key, create);
  if (pg && create) {
    if (key == w->lock_page) w->n_parked++;
    else if (key > w->max_key) w->max_key = key;
  }
  return pg;
}
static void wc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) { pcache_base.xUnpin(((struct wcache *)p)->base, pg, discard); }
static void wc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned old_key, unsigned new_key) {
  struct wcache *w = (struct wcache *)p;
  pcache_base.xRekey(w->base, pg, old_key, new_key);
  if (old_key == w->lock_page && w->n_parked) w->n_parked--;
  if (new_key == w->lock_page) w->n_parked++;
  else if (new_key > w->max_key) w->max_key = new_key;
}
static void wc_truncate(sqlite3_pcache *p, unsigned limit) {
  struct wcache *w = (struct wcache *)p;
  if (w->n_parked == 0 && w->max_key < limit) return;
  pcache_base.xTruncate(w->base, limit);
  if (limit > 0 && w->max_key >= limit) w->max_key = limit - 1;
  if (w->lock_page >= limit) w->n_parked = 0;
}
static void wc_destroy(sqlite3_pcache *p) {
  struct wcache *w = (struct wcache *)p;
  pcache_base.xDestroy(w->base);
  sqlite3_free(w);
}
static void wc_shrink(sqlite3_pcache *p) { pcache_base.xShrink(((struct wcache *)p)->base); }
static int pcache_install(void) {
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pcache_base) != SQLITE_OK) return -1;
  static sqlite3_pcache_methods2 m;
  m = pcache_base;
  m.pArg = NULL;
  m.xInit = wc_init;
  m.xShutdown = wc_shutdown;
  m.xCreate = wc_create;
  m.xCachesize = wc_cachesize;
  m.xPagecount = wc_pagecount;
  m.xFetch = wc_fetch;
  m.xUnpin = wc_unpin;
  m.xRekey = wc_rekey;
  m.xTruncate = wc_truncate;
  m.xDestroy = wc_destroy;
  m.xShrink = wc_shrink;
  return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) == SQLITE_OK ? 0 : -1;
}

/* ---- WAL writes ----
 * SQLite writes a transaction's WAL frames one after another, each as two calls (the 24-byte
 * frame header, then the page), and the last frame of a commit carries the database size in its
 * header. This shim keeps writes that continue exactly where the previous write on the WAL
 * ended in a buffer and writes the buffer with one pwrite when the commit frame's page arrives,
 * inside SQLite's own xWrite call, so the commit is in the kernel before SQLite updates the
 * wal-index and before COMMIT returns. A write that does not continue the buffered run flushes
 * it and starts a new one (it never bypasses the buffer: a frame header that did would lose its
 * commit flag, and the page behind it would wait in user space after COMMIT returned); any other
 * call on the file flushes the buffer first and goes straight through, so the bytes and their
 * order are SQLite's. Readers find frames only through the wal-index, so none can see a
 * buffered one. (After c-iouring-pavlyhalim #245 and c-iouring-shivanshthapliyal #250.) */
#define WAL_FRAME_HDR 24
#define WAL_BUF_CAP 0x1ffff /* the unix VFS passes at most 17 bits of a write's length */
struct wfile {
  sqlite3_file base;
  sqlite3_file *real;
  int is_wal, commit, len;
  sqlite3_int64 off, last_end;
  unsigned char *buf;
};
static sqlite3_vfs *vfs_real;
static uint64_t wal_frames_written, wal_pwrites;

static int wf_flush(struct wfile *f) {
  if (!f->len) return SQLITE_OK;
  int n = f->len;
  f->len = 0;
  wal_pwrites++;
  return f->real->pMethods->xWrite(f->real, f->buf, n, f->off);
}
#define WF(p) ((struct wfile *)(p))
#define WF_FLUSH(p) do { int rc_ = wf_flush(WF(p)); if (rc_ != SQLITE_OK) return rc_; } while (0)
static int wf_close(sqlite3_file *p) {
  struct wfile *f = WF(p);
  int rc = wf_flush(f);
  int rc2 = f->real->pMethods ? f->real->pMethods->xClose(f->real) : SQLITE_OK;
  sqlite3_free(f->buf);
  f->buf = NULL;
  return rc != SQLITE_OK ? rc : rc2;
}
static int wf_read(sqlite3_file *p, void *b, int n, sqlite3_int64 off) { WF_FLUSH(p); return WF(p)->real->pMethods->xRead(WF(p)->real, b, n, off); }
static int wf_write(sqlite3_file *p, const void *b, int n, sqlite3_int64 off) {
  struct wfile *f = WF(p);
  if (!f->is_wal) return f->real->pMethods->xWrite(f->real, b, n, off);
  /* A write that does not continue the buffered run (the first after open, the first after
   * another connection restarted the log, a header rewrite) starts a new run: the old one goes
   * out first and no commit is pending. A write that continues the run but does not fit flushes
   * the run too, but keeps the commit flag: the flag belongs to the frame header that just went
   * out, and the page that follows it must still go out before COMMIT returns (a commit of 32
   * frames or more overflows the buffer; before this, such a commit left its last page in user
   * space, and a checkpoint reading that frame copied garbage into the database). It must not
   * bypass the buffer either, for the same reason. */
  if (off != f->last_end) {
    WF_FLUSH(p);
    f->commit = 0;
  } else if (f->len + n > WAL_BUF_CAP) {
    WF_FLUSH(p);
  }
  f->last_end = off + n;
  if (n > WAL_BUF_CAP || (!f->buf && !(f->buf = sqlite3_malloc(WAL_BUF_CAP)))) {
    WF_FLUSH(p); /* nothing of this transaction may stay behind a write that goes out directly */
    f->commit = 0;
    return f->real->pMethods->xWrite(f->real, b, n, off);
  }
  if (!f->len) f->off = off;
  memcpy(f->buf + f->len, b, (size_t)n);
  f->len += n;
  if (n == WAL_FRAME_HDR) { /* bytes 4-7: the database size after a commit, 0 on other frames */
    const unsigned char *h = b;
    f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    wal_frames_written++;
    return SQLITE_OK;
  }
  if (f->commit) {
    f->commit = 0;
    return wf_flush(f);
  }
  return SQLITE_OK;
}
static int wf_truncate(sqlite3_file *p, sqlite3_int64 size) { WF_FLUSH(p); return WF(p)->real->pMethods->xTruncate(WF(p)->real, size); }
static int wf_sync(sqlite3_file *p, int flags) { WF_FLUSH(p); return WF(p)->real->pMethods->xSync(WF(p)->real, flags); }
static int wf_filesize(sqlite3_file *p, sqlite3_int64 *size) { WF_FLUSH(p); return WF(p)->real->pMethods->xFileSize(WF(p)->real, size); }
static int wf_lock(sqlite3_file *p, int l) { WF_FLUSH(p); return WF(p)->real->pMethods->xLock(WF(p)->real, l); }
static int wf_unlock(sqlite3_file *p, int l) { WF_FLUSH(p); return WF(p)->real->pMethods->xUnlock(WF(p)->real, l); }
static int wf_check_reserved(sqlite3_file *p, int *out) { return WF(p)->real->pMethods->xCheckReservedLock(WF(p)->real, out); }
static int wf_file_control(sqlite3_file *p, int op, void *arg) { WF_FLUSH(p); return WF(p)->real->pMethods->xFileControl(WF(p)->real, op, arg); }
static int wf_sector_size(sqlite3_file *p) { return WF(p)->real->pMethods->xSectorSize(WF(p)->real); }
static int wf_device_chars(sqlite3_file *p) { return WF(p)->real->pMethods->xDeviceCharacteristics(WF(p)->real); }
static int wf_shm_map(sqlite3_file *p, int pg, int sz, int extend, void volatile **out) { return WF(p)->real->pMethods->xShmMap(WF(p)->real, pg, sz, extend, out); }
static int wf_shm_lock(sqlite3_file *p, int off, int n, int flags) { return WF(p)->real->pMethods->xShmLock(WF(p)->real, off, n, flags); }
static void wf_shm_barrier(sqlite3_file *p) { WF(p)->real->pMethods->xShmBarrier(WF(p)->real); }
static int wf_shm_unmap(sqlite3_file *p, int del) { return WF(p)->real->pMethods->xShmUnmap(WF(p)->real, del); }
static int wf_fetch(sqlite3_file *p, sqlite3_int64 off, int n, void **pp) { WF_FLUSH(p); return WF(p)->real->pMethods->xFetch(WF(p)->real, off, n, pp); }
static int wf_unfetch(sqlite3_file *p, sqlite3_int64 off, void *pg) { return WF(p)->real->pMethods->xUnfetch(WF(p)->real, off, pg); }
static const sqlite3_io_methods wf_methods = {3, wf_close, wf_read, wf_write, wf_truncate, wf_sync, wf_filesize, wf_lock, wf_unlock,
                                              wf_check_reserved, wf_file_control, wf_sector_size, wf_device_chars, wf_shm_map,
                                              wf_shm_lock, wf_shm_barrier, wf_shm_unmap, wf_fetch, wf_unfetch};
#define WF_REAL_OFFSET ((sizeof(struct wfile) + 7) & ~(size_t)7)
static int wv_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags, int *out_flags) {
  (void)vfs;
  struct wfile *f = WF(p);
  memset(f, 0, sizeof *f);
  f->last_end = -1;
  f->real = (sqlite3_file *)((char *)p + WF_REAL_OFFSET);
  int rc = vfs_real->xOpen(vfs_real, name, f->real, flags, out_flags);
  if (f->real->pMethods == NULL) return rc;
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  if (f->real->pMethods->iVersion < 3) { f->real->pMethods->xClose(f->real); return SQLITE_CANTOPEN; }
  p->pMethods = &wf_methods;
  return rc;
}
static int wv_delete(sqlite3_vfs *v, const char *z, int sync) { (void)v; return vfs_real->xDelete(vfs_real, z, sync); }
static int wv_access(sqlite3_vfs *v, const char *z, int flags, int *out) { (void)v; return vfs_real->xAccess(vfs_real, z, flags, out); }
static int wv_full_pathname(sqlite3_vfs *v, const char *z, int n, char *out) { (void)v; return vfs_real->xFullPathname(vfs_real, z, n, out); }
static void *wv_dlopen(sqlite3_vfs *v, const char *z) { (void)v; return vfs_real->xDlOpen(vfs_real, z); }
static void wv_dlerror(sqlite3_vfs *v, int n, char *z) { (void)v; vfs_real->xDlError(vfs_real, n, z); }
static void (*wv_dlsym(sqlite3_vfs *v, void *h, const char *z))(void) { (void)v; return vfs_real->xDlSym(vfs_real, h, z); }
static void wv_dlclose(sqlite3_vfs *v, void *h) { (void)v; vfs_real->xDlClose(vfs_real, h); }
static int wv_randomness(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xRandomness(vfs_real, n, z); }
static int wv_sleep(sqlite3_vfs *v, int us) { (void)v; return vfs_real->xSleep(vfs_real, us); }
static int wv_current_time(sqlite3_vfs *v, double *t) { (void)v; return vfs_real->xCurrentTime(vfs_real, t); }
static int wv_last_error(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xGetLastError(vfs_real, n, z); }
static int wv_current_time64(sqlite3_vfs *v, sqlite3_int64 *t) { (void)v; return vfs_real->xCurrentTimeInt64(vfs_real, t); }
static int vfs_install(void) {
  vfs_real = sqlite3_vfs_find("unix-excl");
  if (!vfs_real || vfs_real->iVersion < 2) return -1;
  static sqlite3_vfs v;
  memset(&v, 0, sizeof v);
  v.iVersion = 2;
  v.szOsFile = (int)(WF_REAL_OFFSET + (size_t)vfs_real->szOsFile);
  v.mxPathname = vfs_real->mxPathname;
  v.zName = VFS_NAME;
  v.xOpen = wv_open;
  v.xDelete = wv_delete;
  v.xAccess = wv_access;
  v.xFullPathname = wv_full_pathname;
  v.xDlOpen = wv_dlopen;
  v.xDlError = wv_dlerror;
  v.xDlSym = wv_dlsym;
  v.xDlClose = wv_dlclose;
  v.xRandomness = wv_randomness;
  v.xSleep = wv_sleep;
  v.xCurrentTime = wv_current_time;
  v.xGetLastError = wv_last_error;
  v.xCurrentTimeInt64 = wv_current_time64;
  return sqlite3_vfs_register(&v, 0) == SQLITE_OK ? 0 : -1;
}

/* ---- checkpoints (after the design of rust-iouring-kensac #264) ----
 * Automatic checkpoints are off. Nothing on the loop ever fsyncs: every checkpoint, and so every
 * sync of the log and of the database, runs on a thread. Past CKPT_FRAMES frames a thread at
 * nice 19 runs PASSIVE checkpoints on its own connection in the loop's idle time (the bulk).
 * Under steady writes a pass cannot copy the frames written meanwhile, and the log only restarts
 * once it is fully copied; so that thread runs pass after pass, each copying what the previous
 * one missed, until fewer than CKPT_TAIL frames remain (this converges: it copies faster than
 * the loop writes). Then writes are held (reads go on; the next POST on each connection waits in
 * its buffer) while a second thread at the normal priority copies that remainder (the tail): a
 * few milliseconds of copying plus the syncs, on a thread. With no writer running meanwhile it
 * completes, and the next write restarts the log. A hold starts and ends only between batches
 * and lasts at most HOLD_MAX_NS: if the disk makes the tail's sync slow, writes resume and the
 * log restarts a cycle later instead. (Two things were tried and scored worse on the box: a tail
 * copied without the convergence, which on one vCPU had grown to tens of MB and competed with
 * the loop, 270,000 users; and the small tail copied inline on the loop, whose syncs stalled the
 * loop for seconds on the droplet's disk, 253,000.) Past CKPT_FORCE frames the loop checkpoints
 * by itself, as a last resort. */
static void ck_set(struct db *d, int s) {
  pthread_mutex_lock(&d->mu);
  atomic_store_explicit((_Atomic int *)&d->ck_state, s, memory_order_release);
  pthread_cond_broadcast(&d->cv);
  pthread_mutex_unlock(&d->mu);
}
struct ck_thread {
  struct db *d;
  sqlite3 *c;
  int state; /* the state this thread acts on: CK_BULK or CK_TAIL */
  int nice;
  int wal_fd;        /* the bulk thread's own descriptor on the log, for writeback kicks */
  int db_fd;         /* the bulk thread's descriptor on the database file, for warming */
  uint64_t warm_at;  /* when the database file is next read ahead */
  int64_t wb_kicked; /* log offset up to which writeback has been started */
  int uv;            /* the tail thread's user_version toggle: its restart transactions */
  int last_frames;   /* log size after the tail thread's last commit (1 = it restarted the log) */
};
static int tail_wal_hook(void *ud, sqlite3 *c, const char *name, int frames) {
  (void)c; (void)name;
  ((struct ck_thread *)ud)->last_frames = frames;
  return SQLITE_OK;
}
/* The log is fully copied and writes are held: restart it from the tail thread with a one-frame
 * transaction, so the header sync SQLite does on a restart (synchronous=NORMAL) lands here and
 * never on the loop. The loop's reads go on meanwhile; they read the database file (read mark 0),
 * which a restart does not disturb. A read that began earlier holds another mark and makes the
 * attempt BUSY, so it is retried; if the hold ends first, the loop's next write restarts the log
 * itself, as before. Returns 1 when the log restarted. */
static int tail_restart(struct ck_thread *t) {
  struct db *d = t->d;
  for (int tries = 0; tries < 400 && atomic_load_explicit((_Atomic int *)&d->writes_held, memory_order_relaxed); tries++) {
    t->last_frames = 0;
    int rc = sqlite3_exec(t->c, t->uv ? "PRAGMA user_version=1" : "PRAGMA user_version=0", NULL, NULL, NULL);
    if (rc == SQLITE_OK) { t->uv ^= 1; return t->last_frames == 1; }
    if ((rc & 0xff) != SQLITE_BUSY) return 0;
    usleep(250);
  }
  return 0;
}
/* Starts (does not wait for) the writeback of the log frames written since the last kick, so the
 * log's dirty pages reach the disk continuously instead of in one burst at the next fsync: the
 * tail's sync, which writes wait for, then finds little left. Runs on the bulk thread while idle. */
static int wal_fd_open(struct ck_thread *t) {
  if (t->wal_fd < 0) {
    char p[512];
    snprintf(p, sizeof p, "%s-wal", t->d->path);
    t->wal_fd = open(p, O_RDONLY | O_CLOEXEC);
  }
  return t->wal_fd;
}
static int64_t wal_end(struct db *d) {
  return 32 + (int64_t)atomic_load_explicit((_Atomic int *)&d->wal_frames, memory_order_relaxed) * (24 + 4096);
}
/* Before a pass: the pass reads the log's frames in page order, which is random in the file; if
 * the box's memory pressure has evicted them (they are clean once written back), that is one disk
 * seek per frame. A sequential read-ahead of the log first is what a disk does fastest, and it
 * costs nothing for pages still cached. On this thread; the loop never waits for it. */
static void wal_readahead(struct ck_thread *t) {
  int64_t end = wal_end(t->d);
  if (end > (int64_t)256 << 20) end = (int64_t)256 << 20; /* a log that long is a disk that cannot keep up: the pass reads the rest itself */
  if (wal_fd_open(t) >= 0) (void)readahead(t->wal_fd, 0, (size_t)end);
}
/* Keeps the database file's pages in the page cache, from the idle bulk thread: a page SQLite's
 * own cache misses is then a copy, not a disk read on the loop. readahead() of pages already
 * cached costs a lookup; it reads from disk only what memory pressure evicted, and only while
 * MemAvailable shows room, so it never competes with reclaim. */
static long mem_avail_kb(void) {
  char b[256];
  long kb = -1;
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return -1;
  while (fgets(b, sizeof b, f)) if (!strncmp(b, "MemAvailable:", 13)) { kb = atol(b + 13); break; }
  fclose(f);
  return kb;
}
static void db_warm(struct ck_thread *t) {
  uint64_t nw = now_ns();
  if (nw < t->warm_at) return;
  t->warm_at = nw + dbwarm_s * 1000000000ULL;
  long kb = mem_avail_kb();
  if (kb >= 0 && kb < DBWARM_MIN_AVAIL_KB) return;
  (void)readahead(t->db_fd, 0, dbwarm_bytes);
}
static void wb_kick(struct ck_thread *t) {
  struct db *d = t->d;
  if (wal_fd_open(t) < 0) return;
  int64_t end = wal_end(d);
  if (end < t->wb_kicked) t->wb_kicked = 0; /* the log restarted */
  if (end - t->wb_kicked < (int64_t)(64 * 4120)) return;
  if (sync_file_range(t->wal_fd, t->wb_kicked, end - t->wb_kicked, SYNC_FILE_RANGE_WRITE) == 0) t->wb_kicked = end;
}
static void *ck_main(void *arg) {
  struct ck_thread *t = arg;
  struct db *d = t->d;
  if (t->nice) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), t->nice);
  for (;;) {
    pthread_mutex_lock(&d->mu);
    while (d->ck_state != t->state) {
      if (t->state != CK_BULK || wb_kick_ms <= 0) { pthread_cond_wait(&d->cv, &d->mu); continue; }
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += wb_kick_ms * 1000000L;
      while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      if (pthread_cond_timedwait(&d->cv, &d->mu, &ts) == ETIMEDOUT) {
        pthread_mutex_unlock(&d->mu);
        wb_kick(t);
        if (t->db_fd >= 0) db_warm(t);
        pthread_mutex_lock(&d->mu);
      }
    }
    pthread_mutex_unlock(&d->mu);
    if (t->state == CK_BULK) wal_readahead(t);
    int nlog = -1, ndone = -2;
    int rc = sqlite3_wal_checkpoint_v2(t->c, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    int restarted = t->state == CK_TAIL && rc == SQLITE_OK && nlog > 0 && nlog == ndone && tail_restart(t);
    pthread_mutex_lock(&d->mu);
    d->tail_restarted = restarted;
    d->ck_left = rc == SQLITE_OK && nlog >= ndone ? nlog - ndone : CKPT_FORCE;
    d->ck_nlog = nlog;
    d->n_ckpt++;
    atomic_store_explicit((_Atomic int *)&d->ck_state, t->state + 1, memory_order_release);
    pthread_mutex_unlock(&d->mu);
  }
  return NULL;
}
static int ck_start(struct db *d, const char *path, int state, int nice) {
  struct ck_thread *t = calloc(1, sizeof *t);
  if (!t) return -1;
  t->d = d;
  t->state = state;
  t->nice = nice;
  t->wal_fd = -1;
  t->db_fd = -1;
  if (state == CK_BULK && dbwarm_bytes) t->db_fd = open(path, O_RDONLY | O_CLOEXEC);
  if (db_open_conn(&t->c, path, 64) < 0) return -1;
  if (state == CK_TAIL) sqlite3_wal_hook(t->c, tail_wal_hook, t);
  pthread_t th;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  int rc = pthread_create(&th, &a, ck_main, t);
  pthread_attr_destroy(&a);
  return rc ? -1 : 0;
}
static int db_wal_hook(void *ud, sqlite3 *c, const char *name, int frames) {
  (void)c; (void)name;
  struct db *d = ud;
  if (ckpt_log && frames < d->wal_frames) fprintf(stderr, "ckpt: log restarted (%d -> %d frames)\n", d->wal_frames, frames);
  d->wal_frames = frames;
  return SQLITE_OK;
}
/* Between batches, with no transaction open: drives the checkpoint cycle. The only place a hold
 * starts or ends. */
static void db_tick(struct db *d) {
  switch (atomic_load_explicit((_Atomic int *)&d->ck_state, memory_order_acquire)) {
  case CK_IDLE:
    if (d->wal_frames >= CKPT_FORCE) {
      int nlog = 0, ndone = 0;
      d->n_force++;
      if (sqlite3_wal_checkpoint_v2(d->c, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK && nlog == ndone)
        d->wal_frames = 0;
    } else if (d->wal_frames >= d->ck_min_frames) {
      d->bulk_t0 = now_ns();
      d->ck_passes = 0;
      d->n_cycles++;
      ck_set(d, CK_BULK);
    }
    break;
  case CK_BULK_DONE: {
    d->ck_passes++;
    int behind = d->ck_left + (d->wal_frames - d->ck_nlog); /* skipped by the pass, plus appended since it began */
    if (behind > ckpt_tail && d->ck_passes < CKPT_PASSES_MAX) { ck_set(d, CK_BULK); break; } /* another pass */
    if (d->ck_passes >= CKPT_PASSES_MAX) d->n_passes_max++;
    uint64_t t = now_ns(), dt = t - d->bulk_t0;
    if (dt > d->bulk_max_ns) d->bulk_max_ns = dt;
    if (behind == 0) { /* nothing written since: the next write restarts the log by itself */
      if (ckpt_log) fprintf(stderr, "ckpt: cycle %llu: %d passes, nothing behind, no hold\n", (unsigned long long)d->n_cycles, d->ck_passes);
      d->wal_frames = 0;
      ck_set(d, CK_IDLE);
      break;
    }
    if ((uint64_t)behind > d->tail_frames_max) d->tail_frames_max = (uint64_t)behind;
    d->ck_tail_frames = behind;
    d->writes_held = 1; /* the tail: writes wait so that it completes and the log restarts */
    d->hold_t0 = t;
    ck_set(d, CK_TAIL);
    break;
  }
  case CK_TAIL_DONE: {
    int clean = d->ck_left == 0 && d->writes_held; /* copied everything, and no write slipped in */
    if (d->ck_left != 0) d->n_tail_short++;
    if (d->writes_held) { /* the hold lasted the whole tail */
      uint64_t dt = now_ns() - d->hold_t0;
      d->hold_total_ns += dt;
      if (dt > d->hold_max_ns) d->hold_max_ns = dt;
      d->writes_held = 0;
    }
    if (d->tail_restarted) d->n_tail_restart++;
    if (ckpt_log)
      fprintf(stderr, "ckpt: cycle %llu: %d passes, tail %d frames, hold %.1f ms%s%s%s\n", (unsigned long long)d->n_cycles, d->ck_passes,
              d->ck_tail_frames, (double)(now_ns() - d->hold_t0) / 1e6, clean ? "" : " TIMED OUT", d->ck_left ? " SHORT" : "",
              d->tail_restarted ? ", restarted by the tail thread" : "");
    if (clean) {
      d->wal_frames = d->tail_restarted ? 1 : 0;
      d->ck_min_frames = ckpt_frames;
    } else { /* the log may not restart: the next cycle waits for half a log more, so a slow disk never gives back-to-back holds */
      d->ck_min_frames = d->wal_frames + ckpt_frames / 2;
    }
    ck_set(d, CK_IDLE);
    break;
  }
  default: { /* a thread is working */
    uint64_t limit = hold_max_ns; /* flat, as the leader's: a missed restart costs one short cycle */
    if (d->writes_held && now_ns() - d->hold_t0 > limit) { /* a slow sync: writes resume, the log restarts next cycle */
      d->writes_held = 0;
      d->n_hold_timeout++;
      d->hold_total_ns += limit;
      if (limit > d->hold_max_ns) d->hold_max_ns = limit;
    }
    break;
  }
  }
}

static int db_open_conn(sqlite3 **out, const char *path, int cache_kib) {
  int rc = sqlite3_open_v2(path, out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE, VFS_NAME);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sqlite open %s: %s\n", path, *out ? sqlite3_errmsg(*out) : sqlite3_errstr(rc));
    return -1;
  }
  char sql[96];
  snprintf(sql, sizeof sql, "PRAGMA cache_size=-%d", cache_kib);
  db_exec(*out, sql);
  db_exec(*out, "PRAGMA mmap_size=0"); /* the B-tree reader pins pages through the cache; mapped pages have none */
  db_exec(*out, "PRAGMA synchronous=NORMAL");
  db_exec(*out, "PRAGMA temp_store=MEMORY");
  db_exec(*out, "PRAGMA wal_autocheckpoint=0");
  return 0;
}

/* Hands SQLite a page-cache arena backed (where available) by transparent huge pages: the
 * pages the requests touch then cost few TLB misses. SQLite's own allocator still decides
 * what goes in it. */
static void pagecache_arena(int cache_kib) {
  int hdr = 0;
  if (sqlite3_config(SQLITE_CONFIG_PCACHE_HDRSZ, &hdr) != SQLITE_OK) return;
  int sz = 4096 + hdr;
  sz = (sz + 63) & ~63;
  int n = cache_kib / 4 + 64;
  size_t bytes = ((size_t)sz * (size_t)n + (2u << 20) - 1) & ~((size_t)(2u << 20) - 1);
  void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return;
#ifdef MADV_HUGEPAGE
  madvise(m, bytes, MADV_HUGEPAGE);
#endif
  prefault(m, bytes); /* faulted in now, while memory is plentiful */
  if (sqlite3_config(SQLITE_CONFIG_PAGECACHE, m, sz, n) != SQLITE_OK) munmap(m, bytes);
}

static int db_open(struct db *d, const char *path, int cache_kib) {
  memset(d, 0, sizeof *d);
  d->path = path;
  d->ck_min_frames = ckpt_frames;
  if (pcache_install() < 0) { fprintf(stderr, "sqlite: could not install the page cache\n"); return -1; }
  pagecache_arena(cache_kib);
  if (sqlite3_initialize() != SQLITE_OK) { fprintf(stderr, "sqlite3_initialize failed\n"); return -1; }
  if (vfs_install() < 0) { fprintf(stderr, "sqlite: could not register the VFS\n"); return -1; }
  if (db_open_conn(&d->c, path, cache_kib) < 0) return -1;
  sqlite3_stmt *jm;
  if (db_prep(d->c, &jm, "PRAGMA journal_mode=WAL") != SQLITE_OK) return -1;
  int rc = sqlite3_step(jm);
  if (rc != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(jm, 0), "wal") != 0) {
    fprintf(stderr, "sqlite: could not enable WAL\n");
    return -1;
  }
  sqlite3_finalize(jm);
  if (db_prep(d->c, &d->st_begin, "BEGIN") || db_prep(d->c, &d->st_begin_imm, "BEGIN IMMEDIATE") || db_prep(d->c, &d->st_commit, "COMMIT") ||
      db_prep(d->c, &d->st_rollback, "ROLLBACK") ||
      db_prep(d->c, &d->st_health, "SELECT 1") ||
      db_prep(d->c, &d->st_ins_post, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at") ||
      db_prep(d->c, &d->st_ins_like, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING") ||
      db_prep(d->c, &d->st_post_exists, "SELECT 1 FROM posts WHERE id = ?1") ||
      db_prep(d->c, &d->st_feed_ref,
              "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
              "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20") ||
      db_prep(d->c, &d->st_post_ref,
              "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
              "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1"))
    return -1;
  /* warm the pages the feed touches, then the index every like lands in at random (one leaf per
   * like, ~24 MB): under load those leaves are then found in SQLite's cache instead of being read
   * from the file on the first like that hits them */
  while (sqlite3_step(d->st_feed_ref) == SQLITE_ROW) {}
  sqlite3_reset(d->st_feed_ref);
  db_exec(d->c, "SELECT count(*) FROM likes WHERE user_id > 0"); /* a covering scan of that index */
  d->btree = d->c->aDb[0].pBt;
  if (!bt_init(&d->bt, d->c)) fprintf(stderr, "btree read plans: off (schema differs); reads use SQL\n");
  sqlite3_wal_hook(d->c, db_wal_hook, d);
  pthread_mutex_init(&d->mu, NULL);
  pthread_cond_init(&d->cv, NULL);
  if (ck_start(d, path, CK_BULK, ckpt_nice) || ck_start(d, path, CK_TAIL, 0)) {
    fprintf(stderr, "checkpoint threads: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

/* ---- transactions: one per batch ---- */

static inline int db_begin(struct db *d) {
  if (d->broken) return -1;
  if (d->in_tx) {
    if (!sqlite3_get_autocommit(d->c)) return 0;
    d->in_tx = 0; /* SQLite ended it by itself */
    d->broken = 1;
    return -1;
  }
  int rc = sqlite3_step(d->st_begin);
  sqlite3_reset(d->st_begin);
  if (rc != SQLITE_DONE) return -1;
  d->in_tx = 1;
  d->wrote = 0;
  if (d->bt.ok && bt_open(&d->bt) < 0) {
    fprintf(stderr, "btree: could not open cursors; reads use SQL\n");
    d->bt.ok = 0;
  }
  return 0;
}
/* Opens a batch's transaction as a writer (BEGIN IMMEDIATE) when the batch may write. The lock is
 * free unless the tail thread is just then restarting the log; then this returns -1 at once, the
 * batch reads from a deferred transaction and its writes wait a batch. Without this, a restart
 * committed between a batch's reads and its first write would make that write BUSY_SNAPSHOT. */
static inline int db_begin_write(struct db *d) {
  if (d->in_tx) return 0;
  int rc = sqlite3_step(d->st_begin_imm);
  sqlite3_reset(d->st_begin_imm);
  if (rc != SQLITE_DONE) { d->n_begin_busy++; return -1; }
  d->in_tx = 1;
  d->wrote = 0;
  if (d->bt.ok && bt_open(&d->bt) < 0) {
    fprintf(stderr, "btree: could not open cursors; reads use SQL\n");
    d->bt.ok = 0;
  }
  return 0;
}
/* 0 once the batch is committed, -1 if it was rolled back (its writes did not land). */
static int db_commit(struct db *d) {
  if (d->broken) {
    d->broken = 0;
    bt_close(&d->bt);
    if (!sqlite3_get_autocommit(d->c)) { sqlite3_step(d->st_rollback); sqlite3_reset(d->st_rollback); }
    d->in_tx = 0;
    return -1;
  }
  if (!d->in_tx) return 0;
  d->in_tx = 0;
  bt_close(&d->bt);
  int rc = sqlite3_step(d->st_commit);
  sqlite3_reset(d->st_commit);
  if (rc == SQLITE_DONE) return 0;
  fprintf(stderr, "sqlite commit: %s\n", sqlite3_errmsg(d->c));
  sqlite3_step(d->st_rollback);
  sqlite3_reset(d->st_rollback);
  return -1;
}
static inline void db_check_tx(struct db *d) {
  if (d->in_tx && sqlite3_get_autocommit(d->c)) { d->in_tx = 0; d->broken = 1; }
}

/* ---- the queries ---- */

static inline int db_health(struct db *d) {
  if (db_begin(d) < 0) return -1;
  int rc = sqlite3_step(d->st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(d->st_health, 0) == 1;
  sqlite3_reset(d->st_health);
  return ok ? 0 : -1;
}

static inline void ob_post_row(struct ob *o, sqlite3_stmt *st) {
  ob_post_obj(o, sqlite3_column_int64(st, 0), sqlite3_column_text(st, 1), (u32)sqlite3_column_bytes(st, 1), sqlite3_column_text(st, 2),
              (u32)sqlite3_column_bytes(st, 2), sqlite3_column_text(st, 3), (u32)sqlite3_column_bytes(st, 3), sqlite3_column_int64(st, 4));
}

static int db_feed(struct db *d, struct ob *o) {
  if (db_begin(d) < 0) return -1;
  if (d->bt.open) {
    size_t mark = o->n;
    if (bt_feed(&d->bt, o) == 0 && !o->err) return 0;
    o->n = mark;
    o->err = 0;
    d->n_fallback++;
  }
  ob_lit(o, "{\"posts\":[");
  int n = 0, rc;
  while ((rc = sqlite3_step(d->st_feed_ref)) == SQLITE_ROW) {
    if (n++) ob_char(o, ',');
    ob_post_row(o, d->st_feed_ref);
  }
  sqlite3_reset(d->st_feed_ref);
  if (rc != SQLITE_DONE) {
    static int printed;
    if (printed < 8) { printed++; fprintf(stderr, "feed: %s\n", sqlite3_errmsg(d->c)); }
    return -1;
  }
  ob_lit(o, "]}");
  return 0;
}

/* 1 written, 0 no such post, -1 error */
static int db_post(struct db *d, struct ob *o, i64 id) {
  if (db_begin(d) < 0) return -1;
  if (d->bt.open) {
    size_t mark = o->n;
    int rc = bt_post(&d->bt, o, id);
    if (rc >= 0 && !o->err) return rc;
    o->n = mark;
    o->err = 0;
    d->n_fallback++;
  }
  sqlite3_bind_int64(d->st_post_ref, 1, id);
  int rc = sqlite3_step(d->st_post_ref), ret;
  if (rc == SQLITE_ROW) {
    ob_lit(o, "{\"post\":");
    ob_post_row(o, d->st_post_ref);
    ob_char(o, '}');
    ret = 1;
  } else {
    ret = rc == SQLITE_DONE ? 0 : -1;
  }
  sqlite3_reset(d->st_post_ref);
  return ret;
}

/* 1 exists, 0 absent, -1 error */
static int db_post_exists(struct db *d, i64 id) {
  if (db_begin(d) < 0) return -1;
  if (d->bt.open) {
    int rc = bt_post_exists(&d->bt, id);
    if (rc >= 0) return rc;
  }
  sqlite3_bind_int64(d->st_post_exists, 1, id);
  int rc = sqlite3_step(d->st_post_exists);
  sqlite3_reset(d->st_post_exists);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* Inserts the post; fills id and created_at (as stored). 0 ok, -1 error. */
static int db_insert_post(struct db *d, i64 user_id, const uint8_t *body, uint32_t body_n, i64 *id, uint8_t *created, uint32_t *created_n) {
  if (db_begin(d) < 0) return -1;
  sqlite3_bind_int64(d->st_ins_post, 1, user_id);
  sqlite3_bind_text(d->st_ins_post, 2, (const char *)body, (int)body_n, SQLITE_STATIC);
  int rc = sqlite3_step(d->st_ins_post);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(d->st_ins_post);
    sqlite3_clear_bindings(d->st_ins_post);
    db_check_tx(d);
    return -1;
  }
  *id = sqlite3_column_int64(d->st_ins_post, 0);
  int n = sqlite3_column_bytes(d->st_ins_post, 1);
  const unsigned char *t = sqlite3_column_text(d->st_ins_post, 1);
  if (!t || n > 63) { sqlite3_reset(d->st_ins_post); sqlite3_clear_bindings(d->st_ins_post); return -1; }
  memcpy(created, t, (size_t)n);
  *created_n = (uint32_t)n;
  rc = sqlite3_step(d->st_ins_post);
  sqlite3_reset(d->st_ins_post);
  sqlite3_clear_bindings(d->st_ins_post);
  if (rc != SQLITE_DONE) { db_check_tx(d); return -1; }
  d->wrote = 1;
  return 0;
}

/* 1 liked now, 0 already liked, -1 error. The caller has checked that the post exists. */
static int db_insert_like(struct db *d, i64 user_id, i64 post_id) {
  if (db_begin(d) < 0) return -1;
  sqlite3_bind_int64(d->st_ins_like, 1, user_id);
  sqlite3_bind_int64(d->st_ins_like, 2, post_id);
  int rc = sqlite3_step(d->st_ins_like);
  sqlite3_reset(d->st_ins_like);
  if (rc != SQLITE_DONE) { db_check_tx(d); return -1; }
  if (sqlite3_changes(d->c) > 0) { d->wrote = 1; return 1; }
  return 0;
}

#endif
