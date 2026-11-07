// The SQLite side: the pinned, unmodified amalgamation compiled in this translation unit, plus
//
//   * a page cache plugged in through SQLITE_CONFIG_PCACHE2 (direct-indexed, huge-page backed),
//   * a VFS shim over unix-excl that writes a commit's WAL frames with one pwrite,
//   * a checkpoint thread with its own connection,
//   * the two read plans (feed, one post) on SQLite's own B-tree pages, through the pager, and
//   * the HTTP/JSON rendering of every database response, straight into the output buffer.
//
// Everything runs on the Go event-loop thread except ck_main. Nothing read here outlives the
// request that asked for it: pages are pinned for the duration of one request and released
// before the next one runs (rule 5). Anything unexpected falls back to the reference SQL.
#include "sqlite/sqlite3.c"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "shim.h"

#define FEED_N 20
#define FEED_SPAN 256 // the 20 newest ids are within this many of each other (one likes range walk)

static sqlite3 *db;
static Btree *bt;
static BtShared *bs;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_exists, *st_health, *st_feed, *st_one;
static Pgno root_posts, root_users, root_feed, root_likes;
static int in_tx, broken;
static int wal_frames;

// Counters (td_stats).
enum { S_FEED, S_POST, S_LIKE, S_INSERT, S_FALLBACK, S_SPILL, S_WAL_WRITES, S_WAL_FRAMES, S_TRUNC, S_TRUNC_SCAN, S_EVICT, S_FETCH, S_CK, S_HOLD_MS, S_HOLD_MAX, S_CK_INLINE, S_CK_FULL, S_CK_PARTIAL, S_RESTART, S_CK_RC, S_TAIL_MS, S_TAIL_MAX, S_BULK_MS, S_ROUNDS, N_STATS };
static int64_t stats[N_STATS];

// The first few failures of the request path are reported on stderr (they are never silent).
static void logerr(const char *what) {
  static int n;
  if (n++ < 20) fprintf(stderr, "shim: %s: %s\n", what, db ? sqlite3_errmsg(db) : "no database");
}

static int errf(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

static long env_long(const char *k, long d) {
  const char *v = getenv(k);
  return v && *v ? atol(v) : d;
}

// ---- page cache --------------------------------------------------------------------------------
//
// SQLITE_CONFIG_PCACHE2 (public plugin API). The pager asks for pages by number; this cache
// answers from an array indexed by page number: one load, no hashing, no LRU list. Page buffers
// come from one anonymous mapping backed by transparent huge pages (MADV_HUGEPAGE: 2 MB pages,
// so the TLB covers the working set), extras (SQLite's per-page headers) from a parallel array.
// Eviction, when the budget is full, is a CLOCK sweep over unpinned slots; the budget is set so
// it rarely runs. xTruncate (every commit) is a scan of the direct index between the limit and
// the largest page number ever stored, with the lock-byte page tracked apart: pcache1 would scan
// its whole hash table once a B-tree split parks a page there (the problem #245 and #343 work
// around with a wrapper).

#define DC_KEYS (1u << 20) // direct index for page numbers below this (4 GB of 4 KB pages)

typedef struct {
  sqlite3_pcache_page page; // handed to SQLite; stable for the slot's lifetime
  unsigned key;
  uint8_t pinned, ref, pad[2];
} dc_slot_t;

typedef struct {
  dc_slot_t **tab;
  dc_slot_t *slots;
  uint8_t *pages, *pages_raw;
  size_t pages_raw_len;
  uint8_t *extras;
  uint32_t *freelist;
  uint32_t nfree, cap, used, hand, count;
  unsigned maxkey, lockpg;
  int szPage, szExtra, big;
} dc_t;

static long dc_main_pages = 32768; // the main connection's budget (TD_CACHE_MB)
static int dc_main_taken;

static void *dc_map(size_t n) {
  void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}

static int dc_init(void *a) { (void)a; return SQLITE_OK; }
static void dc_shutdown(void *a) { (void)a; }

static sqlite3_pcache *dc_create(int szPage, int szExtra, int purgeable) {
  dc_t *c = sqlite3_malloc(sizeof *c);
  if (!c) return 0;
  memset(c, 0, sizeof *c);
  c->szPage = szPage;
  c->szExtra = (szExtra + 15) & ~15;
  c->lockpg = 0x40000000u / (unsigned)szPage + 1;
  if (purgeable && !dc_main_taken) {
    dc_main_taken = 1;
    c->big = 1;
    c->cap = (uint32_t)dc_main_pages;
  } else {
    c->cap = 4096;
  }
  size_t huge = 2u << 20;
  c->pages_raw_len = (size_t)c->cap * (size_t)szPage + huge + 64; // 64: cell_key may read 2 bytes past a page
  c->tab = dc_map(sizeof(dc_slot_t *) * DC_KEYS);
  c->slots = dc_map(sizeof(dc_slot_t) * c->cap);
  c->pages_raw = dc_map(c->pages_raw_len);
  c->extras = dc_map((size_t)c->cap * (size_t)c->szExtra);
  c->freelist = dc_map(sizeof(uint32_t) * c->cap);
  if (!c->tab || !c->slots || !c->pages_raw || !c->extras || !c->freelist) {
    sqlite3_free(c);
    return 0;
  }
  c->pages = (uint8_t *)(((uintptr_t)c->pages_raw + huge - 1) & ~(uintptr_t)(huge - 1));
  if (c->big) madvise(c->pages, (size_t)c->cap * (size_t)szPage, MADV_HUGEPAGE);
  return (sqlite3_pcache *)c;
}

static void dc_cachesize(sqlite3_pcache *p, int n) { (void)p; (void)n; }
static int dc_pagecount(sqlite3_pcache *p) { return (int)((dc_t *)p)->count; }
static void dc_shrink(sqlite3_pcache *p) { (void)p; }

static inline dc_slot_t *dc_slot(sqlite3_pcache_page *pg) { return (dc_slot_t *)pg; }

static inline void dc_drop(dc_t *c, dc_slot_t *s) {
  c->tab[s->key] = 0;
  s->page.pBuf = 0;
  c->freelist[c->nfree++] = (uint32_t)(s - c->slots);
  c->count--;
}

static int dc_evict(dc_t *c) {
  for (uint32_t tries = 2 * c->used; tries; tries--) {
    uint32_t h = c->hand;
    c->hand = h + 1 == c->used ? 0 : h + 1;
    dc_slot_t *s = &c->slots[h];
    if (!s->page.pBuf || s->pinned) continue;
    if (s->ref) {
      s->ref = 0;
      continue;
    }
    c->tab[s->key] = 0;
    c->count--;
    stats[S_EVICT]++;
    return (int)h;
  }
  return -1;
}

static sqlite3_pcache_page *dc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  dc_t *c = (dc_t *)p;
  if (key >= DC_KEYS) return 0;
  dc_slot_t *s = c->tab[key];
  if (s) {
    s->pinned = 1;
    return &s->page;
  }
  if (!create) return 0;
  int idx;
  if (c->nfree) idx = (int)c->freelist[--c->nfree];
  else if (c->used < c->cap) idx = (int)c->used++;
  else if ((idx = dc_evict(c)) < 0) return 0;
  s = &c->slots[idx];
  s->page.pBuf = c->pages + (size_t)idx * (size_t)c->szPage;
  s->page.pExtra = c->extras + (size_t)idx * (size_t)c->szExtra;
  memset(s->page.pExtra, 0, (size_t)c->szExtra);
  s->key = key;
  s->pinned = 1;
  s->ref = 1;
  c->tab[key] = s;
  c->count++;
  if (key != c->lockpg && key > c->maxkey) c->maxkey = key;
  return &s->page;
}

static void dc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) {
  dc_t *c = (dc_t *)p;
  dc_slot_t *s = dc_slot(pg);
  if (discard) {
    dc_drop(c, s);
    return;
  }
  s->pinned = 0;
  s->ref = 1;
}

static void dc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned from, unsigned to) {
  dc_t *c = (dc_t *)p;
  dc_slot_t *s = dc_slot(pg);
  if (to >= DC_KEYS) { // cannot be indexed: drop it (SQLite rekeys only pages it will rewrite)
    dc_drop(c, s);
    return;
  }
  if (c->tab[from] == s) c->tab[from] = 0;
  if (c->tab[to]) dc_drop(c, c->tab[to]);
  c->tab[to] = s;
  s->key = to;
  if (to != c->lockpg && to > c->maxkey) c->maxkey = to;
}

static void dc_truncate(sqlite3_pcache *p, unsigned limit) {
  dc_t *c = (dc_t *)p;
  stats[S_TRUNC]++;
  if (c->lockpg >= limit && c->lockpg < DC_KEYS && c->tab[c->lockpg]) dc_drop(c, c->tab[c->lockpg]);
  if (c->maxkey >= limit) {
    stats[S_TRUNC_SCAN]++;
    for (unsigned k = limit; k <= c->maxkey; k++)
      if (c->tab[k]) dc_drop(c, c->tab[k]);
    c->maxkey = limit ? limit - 1 : 0;
  }
}

static void dc_destroy(sqlite3_pcache *p) {
  dc_t *c = (dc_t *)p;
  munmap(c->tab, sizeof(dc_slot_t *) * DC_KEYS);
  munmap(c->slots, sizeof(dc_slot_t) * c->cap);
  munmap(c->pages_raw, c->pages_raw_len);
  munmap(c->extras, (size_t)c->cap * (size_t)c->szExtra);
  munmap(c->freelist, sizeof(uint32_t) * c->cap);
  sqlite3_free(c);
}

static int dc_install(void) {
  static sqlite3_pcache_methods2 m = {1, 0, dc_init, dc_shutdown, dc_create, dc_cachesize, dc_pagecount, dc_fetch,
                                      dc_unpin, dc_rekey, dc_truncate, dc_destroy, dc_shrink};
  return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m) == SQLITE_OK ? 0 : -1;
}

// ---- WAL writes --------------------------------------------------------------------------------
//
// A VFS shim over unix-excl (public VFS API; the pattern of #245). SQLite writes each WAL frame
// as two calls (24-byte header, then the page) and the commit frame last. Writes that continue
// exactly where the previous one ended are kept in a buffer and written with one pwrite when the
// commit frame's page arrives; any other call on the file writes the buffer first and goes
// through. What reaches the kernel, and its order, is unchanged: the commit frame is in the
// kernel before SQLite updates the wal-index and before COMMIT returns.

#define WAL_HDR 24
#define WAL_BUF 0x1ffff

typedef struct {
  sqlite3_file base;
  sqlite3_file *real;
  int is_wal, commit, len;
  sqlite3_int64 off, last_end;
  unsigned char *buf;
} wf_t;

static sqlite3_vfs *vfs_real;

static int wf_flush(wf_t *f) {
  if (!f->len) return SQLITE_OK;
  int n = f->len;
  f->len = 0;
  stats[S_WAL_WRITES]++;
  return f->real->pMethods->xWrite(f->real, f->buf, n, f->off);
}
#define WF(p) ((wf_t *)(p))
#define FLUSH(p)                            \
  do {                                      \
    int rc_ = wf_flush(WF(p));              \
    if (rc_ != SQLITE_OK) return rc_;       \
  } while (0)

static int wf_close(sqlite3_file *p) {
  wf_t *f = WF(p);
  int rc = wf_flush(f);
  int rc2 = f->real->pMethods ? f->real->pMethods->xClose(f->real) : SQLITE_OK;
  sqlite3_free(f->buf);
  f->buf = 0;
  return rc != SQLITE_OK ? rc : rc2;
}
static int wf_read(sqlite3_file *p, void *b, int n, sqlite3_int64 off) {
  FLUSH(p);
  return WF(p)->real->pMethods->xRead(WF(p)->real, b, n, off);
}
static int wf_write(sqlite3_file *p, const void *b, int n, sqlite3_int64 off) {
  wf_t *f = WF(p);
  if (!f->is_wal) return f->real->pMethods->xWrite(f->real, b, n, off);
  int append = off == f->last_end;
  f->last_end = off + n;
  if (f->len + n > WAL_BUF) FLUSH(p);
  if (!append || n > WAL_BUF || (!f->buf && !(f->buf = sqlite3_malloc(WAL_BUF)))) {
    FLUSH(p);
    f->commit = 0;
    stats[S_WAL_WRITES]++;
    return f->real->pMethods->xWrite(f->real, b, n, off);
  }
  if (!f->len) f->off = off;
  memcpy(f->buf + f->len, b, (size_t)n);
  f->len += n;
  if (n == WAL_HDR) { // bytes 4-7: the database size after a commit; 0 on other frames
    const unsigned char *h = b;
    f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    stats[S_WAL_FRAMES]++;
    return SQLITE_OK;
  }
  if (f->commit) {
    f->commit = 0;
    return wf_flush(f);
  }
  return SQLITE_OK;
}
static int wf_truncate(sqlite3_file *p, sqlite3_int64 size) {
  FLUSH(p);
  return WF(p)->real->pMethods->xTruncate(WF(p)->real, size);
}
static int wf_sync(sqlite3_file *p, int flags) {
  FLUSH(p);
  return WF(p)->real->pMethods->xSync(WF(p)->real, flags);
}
static int wf_filesize(sqlite3_file *p, sqlite3_int64 *size) {
  FLUSH(p);
  return WF(p)->real->pMethods->xFileSize(WF(p)->real, size);
}
static int wf_lock(sqlite3_file *p, int l) {
  FLUSH(p);
  return WF(p)->real->pMethods->xLock(WF(p)->real, l);
}
static int wf_unlock(sqlite3_file *p, int l) {
  FLUSH(p);
  return WF(p)->real->pMethods->xUnlock(WF(p)->real, l);
}
static int wf_check_reserved(sqlite3_file *p, int *out) { return WF(p)->real->pMethods->xCheckReservedLock(WF(p)->real, out); }
static int wf_file_control(sqlite3_file *p, int op, void *arg) {
  FLUSH(p);
  return WF(p)->real->pMethods->xFileControl(WF(p)->real, op, arg);
}
static int wf_sector_size(sqlite3_file *p) { return WF(p)->real->pMethods->xSectorSize(WF(p)->real); }
static int wf_device_chars(sqlite3_file *p) { return WF(p)->real->pMethods->xDeviceCharacteristics(WF(p)->real); }
static int wf_shm_map(sqlite3_file *p, int pg, int sz, int extend, void volatile **out) {
  return WF(p)->real->pMethods->xShmMap(WF(p)->real, pg, sz, extend, out);
}
static int wf_shm_lock(sqlite3_file *p, int off, int n, int flags) { return WF(p)->real->pMethods->xShmLock(WF(p)->real, off, n, flags); }
static void wf_shm_barrier(sqlite3_file *p) { WF(p)->real->pMethods->xShmBarrier(WF(p)->real); }
static int wf_shm_unmap(sqlite3_file *p, int del) { return WF(p)->real->pMethods->xShmUnmap(WF(p)->real, del); }
static int wf_fetch(sqlite3_file *p, sqlite3_int64 off, int n, void **pp) {
  FLUSH(p);
  return WF(p)->real->pMethods->xFetch(WF(p)->real, off, n, pp);
}
static int wf_unfetch(sqlite3_file *p, sqlite3_int64 off, void *pg) { return WF(p)->real->pMethods->xUnfetch(WF(p)->real, off, pg); }

static const sqlite3_io_methods wf_methods = {3,           wf_close,      wf_read,         wf_write,        wf_truncate,
                                              wf_sync,     wf_filesize,   wf_lock,         wf_unlock,       wf_check_reserved,
                                              wf_file_control, wf_sector_size, wf_device_chars, wf_shm_map,  wf_shm_lock,
                                              wf_shm_barrier, wf_shm_unmap, wf_fetch,       wf_unfetch};

#define WF_REAL_OFF ((sizeof(wf_t) + 15) & ~(size_t)15)

static int wv_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags, int *out_flags) {
  (void)vfs;
  wf_t *f = WF(p);
  memset(f, 0, sizeof *f);
  f->last_end = -1;
  f->real = (sqlite3_file *)((char *)p + WF_REAL_OFF);
  int rc = vfs_real->xOpen(vfs_real, name, f->real, flags, out_flags);
  if (f->real->pMethods == 0) return rc;
  if (f->real->pMethods->iVersion < 3) {
    f->real->pMethods->xClose(f->real);
    return SQLITE_CANTOPEN;
  }
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  p->pMethods = &wf_methods;
  return rc;
}
static int wv_delete(sqlite3_vfs *v, const char *z, int sync) { (void)v; return vfs_real->xDelete(vfs_real, z, sync); }
static int wv_access(sqlite3_vfs *v, const char *z, int flags, int *out) { (void)v; return vfs_real->xAccess(vfs_real, z, flags, out); }
static int wv_full_pathname(sqlite3_vfs *v, const char *z, int n, char *out) {
  (void)v;
  return vfs_real->xFullPathname(vfs_real, z, n, out);
}
static void *wv_dlopen(sqlite3_vfs *v, const char *z) { (void)v; return vfs_real->xDlOpen(vfs_real, z); }
static void wv_dlerror(sqlite3_vfs *v, int n, char *m) { (void)v; vfs_real->xDlError(vfs_real, n, m); }
static void (*wv_dlsym(sqlite3_vfs *v, void *h, const char *z))(void) { (void)v; return vfs_real->xDlSym(vfs_real, h, z); }
static void wv_dlclose(sqlite3_vfs *v, void *h) { (void)v; vfs_real->xDlClose(vfs_real, h); }
static int wv_randomness(sqlite3_vfs *v, int n, char *out) { (void)v; return vfs_real->xRandomness(vfs_real, n, out); }
static int wv_sleep(sqlite3_vfs *v, int us) { (void)v; return vfs_real->xSleep(vfs_real, us); }
static int wv_current_time(sqlite3_vfs *v, double *t) { (void)v; return vfs_real->xCurrentTime(vfs_real, t); }
static int wv_last_error(sqlite3_vfs *v, int n, char *m) { (void)v; return vfs_real->xGetLastError(vfs_real, n, m); }
static int wv_current_time64(sqlite3_vfs *v, sqlite3_int64 *t) { (void)v; return vfs_real->xCurrentTimeInt64(vfs_real, t); }

static sqlite3_vfs wv_vfs;

static int wv_install(void) {
  vfs_real = sqlite3_vfs_find("unix-excl");
  if (!vfs_real || vfs_real->iVersion < 2) return -1;
  memset(&wv_vfs, 0, sizeof wv_vfs);
  wv_vfs.iVersion = 2;
  wv_vfs.szOsFile = (int)WF_REAL_OFF + vfs_real->szOsFile;
  wv_vfs.mxPathname = vfs_real->mxPathname;
  wv_vfs.zName = "td-wal";
  wv_vfs.xOpen = wv_open;
  wv_vfs.xDelete = wv_delete;
  wv_vfs.xAccess = wv_access;
  wv_vfs.xFullPathname = wv_full_pathname;
  wv_vfs.xDlOpen = wv_dlopen;
  wv_vfs.xDlError = wv_dlerror;
  wv_vfs.xDlSym = wv_dlsym;
  wv_vfs.xDlClose = wv_dlclose;
  wv_vfs.xRandomness = wv_randomness;
  wv_vfs.xSleep = wv_sleep;
  wv_vfs.xCurrentTime = wv_current_time;
  wv_vfs.xGetLastError = wv_last_error;
  wv_vfs.xCurrentTimeInt64 = wv_current_time64;
  return sqlite3_vfs_register(&wv_vfs, 0) == SQLITE_OK ? 0 : -1;
}

// ---- checkpoints -------------------------------------------------------------------------------
//
// Automatic checkpoints are off. Past CK_FRAMES frames a thread runs a PASSIVE checkpoint on its
// own connection while the loop goes on; since the log restarts only once fully copied, writes
// are then held (between passes, at most HOLD_MS) while the thread copies the tail. Past CK_FORCE
// frames the loop checkpoints inline. (The scheme of rust-iouring-kensac #264.)
//
// The thread runs at normal priority (TD_CK_NICE, default 0). A cycle is ~15 ms of copying, 1-2 %
// of the CPU at the benchmark's write rate, but at nice 19 it is starved exactly when the loop is
// saturated: the hold then expires at HOLD_MS without a restart, the log grows past CK_FORCE and
// the inline checkpoint stalls every request for hundreds of milliseconds (the first benchmark of
// this entry failed its p99 that way at 260,000 users while the CPU was at 78 %).
#define CK_FRAMES 4096
#define CK_FORCE 65536
#define HOLD_MS 250

static sqlite3 *ckdb;
enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static _Atomic int ck_state;
static int ck_full, held;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static struct timespec held_at;

static int wal_hook(void *a, sqlite3 *d, const char *name, int frames) {
  static int last;
  (void)a; (void)d; (void)name;
  if (frames < last) stats[S_RESTART]++; // the log restarted
  last = wal_frames = frames;
  return SQLITE_OK;
}

int td_wal_frames(void) { return wal_frames; }

static void ck_set(int s) {
  pthread_mutex_lock(&ck_mu);
  atomic_store_explicit(&ck_state, s, memory_order_release);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

// The bulk phase repeats (up to CK_ROUNDS times) while the frames it could not copy yet, those
// written meanwhile, are more than CK_TAIL_MAX: every round brings the backfill closer, so the
// tail copied under the hold is small. A passive checkpoint that does not complete the backfill
// writes its pages to the database file without syncing it (SQLite syncs only when the log is
// fully copied), so the tail's sync would flush the whole cycle's pages, ~14 MB and 120 ms on the
// benchmark box, while writes are held. The thread therefore syncs the database file itself
// before the hold and copies once more, leaving the held tail a few dozen frames and a few ms.
#define CK_ROUNDS 4
#define CK_TAIL_MAX 128
static int db_fd = -1;

static void presync(void) {
  if (db_fd >= 0) fdatasync(db_fd);
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

static void *ck_main(void *a) {
  (void)a;
  long nice = env_long("TD_CK_NICE", 0);
  if (nice) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), (int)nice);
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (ck_state != CK_BULK && ck_state != CK_TAIL) pthread_cond_wait(&ck_cv, &ck_mu);
    int st = ck_state;
    pthread_mutex_unlock(&ck_mu);
    int nlog = -1, ndone = -2, rc = SQLITE_OK;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int round = 0; round < (st == CK_BULK ? CK_ROUNDS : 1); round++) {
      rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
      stats[S_CK]++;
      stats[S_ROUNDS]++;
      if (rc != SQLITE_OK) {
        stats[S_CK_RC] = rc;
        break;
      }
      if (nlog - ndone <= CK_TAIL_MAX) break;
    }
    if (st == CK_BULK && rc == SQLITE_OK) {
      presync(); // the cycle's pages reach the disk before writes are held
      rc = sqlite3_wal_checkpoint_v2(ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
      stats[S_CK]++;
      presync();
    }
    long ms = ms_since(&t0);
    if (st == CK_TAIL) {
      stats[S_TAIL_MS] += ms;
      if (ms > stats[S_TAIL_MAX]) stats[S_TAIL_MAX] = ms;
    } else {
      stats[S_BULK_MS] += ms;
    }
    pthread_mutex_lock(&ck_mu);
    ck_full = rc == SQLITE_OK && nlog == ndone;
    if (st == CK_TAIL) stats[ck_full ? S_CK_FULL : S_CK_PARTIAL]++;
    atomic_store_explicit(&ck_state, st + 1, memory_order_release);
    pthread_mutex_unlock(&ck_mu);
  }
  return 0;
}

void td_tick(void) {
  switch (atomic_load_explicit(&ck_state, memory_order_acquire)) {
  case CK_IDLE:
    if (wal_frames >= CK_FORCE) {
      int nlog = 0, ndone = 0;
      stats[S_CK_INLINE]++;
      if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK && nlog == ndone)
        wal_frames = 0;
    } else if (wal_frames >= CK_FRAMES) {
      ck_set(CK_BULK);
    }
    break;
  case CK_BULK_DONE:
    held = 1;
    clock_gettime(CLOCK_MONOTONIC, &held_at);
    ck_set(CK_TAIL);
    break;
  case CK_TAIL_DONE: {
    long ms = held ? ms_since(&held_at) : HOLD_MS;
    stats[S_HOLD_MS] += ms;
    if (ms > stats[S_HOLD_MAX]) stats[S_HOLD_MAX] = ms;
    if (ck_full) wal_frames = 0;
    held = 0;
    ck_set(CK_IDLE);
    break;
  }
  default:
    if (held && ms_since(&held_at) > HOLD_MS) held = 0;
    break;
  }
}

int td_writes_held(void) { return held; }

// ---- open --------------------------------------------------------------------------------------

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

// The record decoders assume this exact layout; refuse to start on anything else.
static int same(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[256];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

int td_open(const char *path, char *err, int n) {
  long mb = env_long("TD_CACHE_MB", 128);
  if (mb < 16) mb = 16;
  dc_main_pages = mb * 256; // 4 KB pages
  const char *pc = getenv("TD_PCACHE");
  int use_dc = !(pc && strcmp(pc, "default") == 0);
  const char *ws = getenv("TD_WALSHIM");
  int use_wv = !(ws && strcmp(ws, "0") == 0);
  if (use_dc && dc_install()) return errf(err, n, "page cache");
  if (sqlite3_initialize()) return errf(err, n, "init");
  if (use_wv && wv_install()) return errf(err, n, "vfs");
  const char *vfs = use_wv ? "td-wal" : "unix-excl";
  // unix-excl: this process owns the file; the WAL index lives in heap memory shared by its two
  // connections (no -shm file, no file locks per transaction).
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs)) return errf(err, n, "open");
  char pragmas[512];
  snprintf(pragmas, sizeof pragmas,
           "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;"
           "PRAGMA cache_size = -%ld; PRAGMA mmap_size = 0; PRAGMA wal_autocheckpoint = 0;"
           "SELECT count(*) FROM sqlite_schema",
           mb * 1024);
  if (sqlite3_exec(db, pragmas, 0, 0, 0)) return errf(err, n, "pragmas");
  sqlite3_wal_hook(db, wal_hook, 0);

  static const char *checks[][2] = {
      {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
      {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
      {"SELECT name FROM pragma_table_info('likes')", "user_id,post_id,created_at"},
      {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
      {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
      {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
      {"SELECT desc FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "0"},
      {"SELECT pk FROM pragma_table_info('posts') WHERE name = 'id'", "1"},
      {"SELECT pk FROM pragma_table_info('users') WHERE name = 'id'", "1"},
      {"SELECT journal_mode FROM pragma_journal_mode", "wal"},
      {"SELECT page_size FROM pragma_page_size", "4096"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++)
    if (!same(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  if (ENC(db) != SQLITE_UTF8) {
    snprintf(err, n, "database text encoding is not UTF-8");
    return -1;
  }
  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_feed = rootpage("posts_created_at_id_idx");
  root_likes = rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed || !root_likes) return errf(err, n, "root pages");

  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  st_post = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_exists = prep("SELECT 1 FROM posts WHERE id = ?1");
  st_health = prep("SELECT 1");
  // The reference queries: the answer whenever the data isn't in the form the plans expect.
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, "
                 "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                 "FROM posts p JOIN users u ON u.id = p.user_id "
                 "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  st_one = prep("SELECT p.id, p.body, p.created_at, u.username, "
                "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1");
  if (!st_begin || !st_commit || !st_rollback || !st_post || !st_like || !st_exists || !st_health || !st_feed || !st_one)
    return errf(err, n, "prepare");
  bt = db->aDb[0].pBt;
  bs = bt->pBt;

  if (sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs) ||
      sqlite3_exec(ckdb, "PRAGMA synchronous = NORMAL; PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 64", 0, 0, 0)) {
    snprintf(err, n, "checkpoint connection: %s", ckdb ? sqlite3_errmsg(ckdb) : "cannot open");
    return -1;
  }
  db_fd = open(path, O_RDONLY | O_CLOEXEC);
  // The database file grows in 64 MB steps (SQLITE_FCNTL_CHUNK_SIZE, a hint to SQLite's own VFS).
  // A checkpoint that completes the backfill truncates the file to its logical size and syncs it;
  // when that size changes by a few pages the fdatasync must wait for the file system's journal
  // (~170 ms on the benchmark box's disk against ~3 ms for the data alone), and it happens while
  // writes are held. With the file pre-extended the truncate is a no-op and the sync is data only.
  {
    int chunk = (int)(env_long("TD_CHUNK_MB", 64) << 20);
    if (chunk > 0) {
      sqlite3_file_control(ckdb, "main", SQLITE_FCNTL_CHUNK_SIZE, &chunk);
      sqlite3_file_control(db, "main", SQLITE_FCNTL_CHUNK_SIZE, &chunk);
    }
  }
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 256 << 10);
  if (pthread_create(&t, &a, ck_main, 0)) {
    snprintf(err, n, "checkpoint thread");
    return -1;
  }
  pthread_attr_destroy(&a);
  return 0;
}

// ---- transactions ------------------------------------------------------------------------------
//
// A pass's reads run inside one read transaction opened on the B-tree itself (no BEGIN/COMMIT
// statements: most passes only read). The first write of a pass runs BEGIN, which the statements
// need for group commit; COMMIT then ends both. Everything in between sees the same snapshot.
static int sql_tx; // BEGIN was run: COMMIT must end the pass

// Joins the pass's transaction, starting it on first use. 0 ok, -1 the pass is lost.
static int tx(void) {
  if (in_tx) {
    if (!sql_tx || !sqlite3_get_autocommit(db)) return 0;
    in_tx = sql_tx = 0; // SQLite ended it itself (I/O error, SQLITE_FULL): the pass is lost
    broken = 1;
    logerr("transaction ended by SQLite");
    return -1;
  }
  if (broken) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) {
    logerr("read transaction");
    return -1;
  }
  in_tx = 1;
  return 0;
}

// For a write: the pass's transaction, with BEGIN run so the statements join it.
static int tx_write(void) {
  if (tx()) return -1;
  if (sql_tx) return 0;
  if (step_reset(st_begin) != SQLITE_DONE) {
    logerr("BEGIN");
    return -1;
  }
  sql_tx = 1;
  return 0;
}

static int td_commit(void) {
  if (broken) {
    broken = 0;
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  if (!in_tx) return 0;
  in_tx = 0;
  if (!sql_tx) {
    if (sqlite3BtreeCommit(bt) != SQLITE_OK) {
      logerr("read commit");
      return -1;
    }
    return 0;
  }
  sql_tx = 0;
  if (step_reset(st_commit) != SQLITE_DONE) {
    logerr("COMMIT");
    if (!sqlite3_get_autocommit(db)) step_reset(st_rollback);
    return -1;
  }
  return 0;
}

// A statement failed: if SQLite ended the transaction itself, the pass is lost.
static void check_tx(void) {
  logerr("statement");
  if (in_tx && sql_tx && sqlite3_get_autocommit(db)) {
    in_tx = sql_tx = 0;
    broken = 1;
  }
}

// ---- records (https://sqlite.org/fileformat2.html#record_format) -------------------------------

typedef struct {
  u32 t;
  const u8 *p;
  u32 n;
} col_t;

static inline const u8 *varint(const u8 *p, const u8 *end, u64 *v) {
  u64 x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= end) return 0;
    x = x << 7 | (p[0] & 0x7f);
    if (!(*p++ & 0x80)) {
      *v = x;
      return p;
    }
  }
  if (p >= end) return 0;
  *v = x << 8 | *p++;
  return p;
}

static inline u32 serial_len(u64 t) {
  static const u8 fl[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? fl[t] : (u32)((t - 12) / 2);
}

// The first ncol columns of the record at rec (len bytes). 0 ok, -1 malformed.
static int rec_cols(const u8 *rec, u32 len, int ncol, col_t *c) {
  const u8 *end = rec + len;
  u64 hsz;
  const u8 *h = varint(rec, end, &hsz);
  if (!h || hsz > len) return -1;
  const u8 *hend = rec + hsz, *body = hend;
  for (int i = 0; i < ncol; i++) {
    u64 t;
    if (h >= hend || !(h = varint(h, hend, &t))) return -1;
    u32 sl = serial_len(t);
    if (body + sl > end) return -1;
    c[i].t = (u32)t;
    c[i].p = body;
    c[i].n = sl;
    body += sl;
  }
  return 0;
}

static inline int col_is_int(const col_t *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static inline int col_is_text(const col_t *c) { return c->t >= 13 && (c->t & 1); }
static inline i64 col_int(const col_t *c) {
  if (c->t == 8 || c->t == 9) return (i64)(c->t - 8);
  i64 x = (signed char)c->p[0];
  for (u32 j = 1; j < c->n; j++) x = x << 8 | c->p[j];
  return x;
}

// ---- pages -------------------------------------------------------------------------------------
//
// Pages are read with the pager's own getAndInitPage (through the WAL and the page cache,
// inside the pass's read transaction) and released with releasePageNotNull. No cursor state is
// touched; the structures read (MemPage, cells) are the ones SQLite's cursor code reads.

static inline MemPage *page_get(Pgno pgno) {
  MemPage *pg;
  if (getAndInitPage(bs, pgno, &pg, PAGER_GET_READONLY) != SQLITE_OK) return 0;
  stats[S_FETCH]++;
  return pg;
}

static inline Pgno child_of(MemPage *pg, int ix) {
  return ix < pg->nCell ? get4byte(findCell(pg, ix)) : get4byte(&pg->aData[pg->hdrOffset + 8]);
}

// A varint's value: rowids here are 1-3 bytes; longer ones take SQLite's decoder.
static inline i64 varint_value(const u8 *p) {
  if (p[0] < 0x80) return p[0];
  if (p[1] < 0x80) return (i64)(p[0] & 0x7f) << 7 | p[1];
  if (p[2] < 0x80) return (i64)(p[0] & 0x7f) << 14 | (i64)(p[1] & 0x7f) << 7 | p[2];
  return sqlite3VarintValue(p);
}

// The rowid of cell i on an intKey page, without a branch that depends on the data: the three
// candidate decodings are computed and selected (the search below runs many such reads in
// lockstep, and a mispredicted branch per probe would serialize them). Payload sizes on leaves
// are 1-2 bytes here (rows are far below 16 KB); anything longer takes the general decoder.
static inline i64 cell_key(MemPage *pg, int i) {
  const u8 *c = pg->aDataOfst + (pg->maskPage & get2byteAligned(&pg->aCellIdx[2 * i]));
  if (pg->intKeyLeaf) {
    if (__builtin_expect(c[0] >= 0x80 && c[1] >= 0x80, 0)) {
      while (0x80 <= *(c++))
        if (c >= pg->aDataEnd) return INT64_MAX;
    } else {
      c += 1 + (c[0] >= 0x80);
    }
  }
  u32 b0 = c[0], b1 = c[1], b2 = c[2];
  if (__builtin_expect((b0 & b1 & b2) >= 0x80, 0)) return sqlite3VarintValue(c);
  i64 v2 = (i64)(b0 & 0x7f) << 7 | b1, v3 = (i64)(b0 & 0x7f) << 14 | (i64)(b1 & 0x7f) << 7 | b2;
  i64 v = b1 < 0x80 ? v2 : v3;
  return b0 < 0x80 ? (i64)b0 : v;
}

static inline i64 tbl_key(MemPage *pg, int i) { return cell_key(pg, i); }

// The leading integer of the index entry in cell i (an index record's first column).
// INT64_MAX when it isn't an integer or the cell is malformed (the caller then declines).
static i64 idx_key_slow(MemPage *pg, int i);
static inline i64 idx_key(MemPage *pg, int i) {
  // The common shape: payload size, header size and the first serial type are one byte each
  // (every entry of likes_post_id_idx is); the body's first column is an integer of 1-6 bytes.
  const u8 *c = findCellPastPtr(pg, i);
  u32 b0 = c[0], b1 = c[1], t = c[2];
  if (__builtin_expect((b0 | b1 | t) >= 0x80 || t < 1 || t > 6, 0)) return idx_key_slow(pg, i);
  const u8 *p = c + 1 + b1;
  static const u8 fl[7] = {0, 1, 2, 3, 4, 6, 8};
  u32 len = fl[t];
  if (__builtin_expect(1 + b1 + len > b0 || c + 1 + b0 > pg->aDataEnd, 0)) return idx_key_slow(pg, i);
  i64 x = (signed char)p[0];
  for (u32 j = 1; j < len; j++) x = x << 8 | p[j];
  return x;
}
static i64 idx_key_slow(MemPage *pg, int i) {
  const u8 *c = findCellPastPtr(pg, i), *end = pg->aDataEnd;
  u64 npay, hsz, t;
  if (!(c = varint(c, end, &npay))) return INT64_MAX;
  if (npay > pg->maxLocal) return INT64_MAX;
  const u8 *rec = c;
  if (!(c = varint(c, end, &hsz)) || !(c = varint(c, end, &t))) return INT64_MAX;
  col_t col = {(u32)t, rec + hsz, serial_len(t)};
  if (!col_is_int(&col) || col.p + col.n > end) return INT64_MAX;
  return col_int(&col);
}

// The first cell whose key is >= k (nCell if none). Keys are dense and nearly linear in the
// cell index (post ids with a few likes each), so the search starts from an interpolated guess
// and probes next to it before halving: 2-3 cells read instead of 7-9, each on its own line.
#define LOWER_BOUND(NAME, KEY)                                                             \
  static int NAME(MemPage *pg, i64 k) {                                                    \
    int n = pg->nCell;                                                                     \
    i64 k0 = KEY(pg, 0), kn = KEY(pg, n - 1);                                              \
    if (k <= k0) return 0;                                                                 \
    if (k > kn) return n;                                                                  \
    int lo = 1, hi = n - 1; /* key(lo-1) < k <= key(hi) */                                 \
    int g = lo + (int)(((k - k0) * (i64)(n - 1) - (kn - k0) / 2) / (kn - k0));             \
    if (g < lo) g = lo;                                                                    \
    if (g > hi) g = hi;                                                                    \
    for (int probe = 0; lo < hi; probe++) {                                                \
      if (KEY(pg, g) >= k) hi = g;                                                         \
      else lo = g + 1;                                                                     \
      if (probe < 2 && lo < hi) g = hi == g ? g - 1 : g + 1;                               \
      else g = lo + ((hi - lo) >> 1);                                                      \
      if (g < lo) g = lo;                                                                  \
      if (g > hi) g = hi;                                                                  \
    }                                                                                      \
    return lo;                                                                             \
  }
LOWER_BOUND(idx_lower_bound, idx_key)

// ---- several rowid lookups in one table, one level at a time ------------------------------------
//
// The descent of sqlite3BtreeTableMoveto for up to FEED_N keys together (the idea of #245, made
// branch-free): every key's child is chosen on one level before any page of the next level is
// fetched, pages shared by several keys are fetched (and referenced) once, and on each page the
// binary searches of all keys advance in lockstep, one probe per key per round, with the compare
// folded into selects. The 20 dependent load chains then overlap instead of each paying its
// latency and a mispredicted branch per probe: ~3x fewer cycles than 20 cursor seeks.
typedef struct {
  MemPage *pg; // the leaf searched, referenced by the hit that owns it
  int ix;
  u8 found, own;
} hit_t;

static void hits_release(hit_t *h, int n) {
  for (int i = 0; i < n; i++)
    if (h[i].own) releasePageNotNull(h[i].pg);
}

// The first cell in [lo, hi) whose key is >= k (hi if none), starting from a guess g in [lo, hi):
// probe the guess, gallop outward until the answer is bracketed, then halve. Rowids are dense
// and interior keys nearly linear, so this is 2 probes where a binary search reads 7-9.
static inline int lb_from_guess(MemPage *pg, i64 k, int g, int lo, int hi) {
  if (cell_key(pg, g) < k) {
    lo = g + 1;
    for (int step = 1; lo < hi; step <<= 1) {
      int p = lo + step - 1;
      if (p >= hi) break;
      if (cell_key(pg, p) < k) lo = p + 1;
      else {
        hi = p;
        break;
      }
    }
  } else {
    hi = g;
    for (int step = 1; lo < hi; step <<= 1) {
      int p = hi - step;
      if (p < lo) break;
      if (cell_key(pg, p) < k) {
        lo = p + 1;
        break;
      }
      hi = p;
    }
  }
  while (lo < hi) {
    int m = lo + ((hi - lo) >> 1);
    if (cell_key(pg, m) < k) lo = m + 1;
    else hi = m;
  }
  return lo;
}

// An interpolated guess of the cell holding k on a page whose keys span [a, b].
static inline int guess_ix(i64 k, i64 a, i64 b, int nc) {
  if (k <= a || b <= a) return 0;
  if (k >= b) return nc - 1;
  int e = (int)((double)(k - a) * (double)(nc - 1) / (double)(b - a));
  return e < 0 ? 0 : e > nc - 1 ? nc - 1 : e;
}

// 0 with every hit set, -1 if the plan declines (no pages held).
//
// The keys are sorted, so on every level the keys that share a page form a run. A run on an
// interior page is resolved with two searches, for its first and last key: when both land on the
// same child the run follows it as a whole, otherwise the keys between are placed one by one,
// each bounded by the previous one's child. Each child's key range is known from its parent's
// separators (a child holds the keys in (sep[ix-1], sep[ix]]), so on a leaf with dense rowids
// the cell index is key - first key, verified by one probe. Pages shared by a run are fetched
// and referenced once.
static int seek_many(Pgno root, const i64 *key, int n, hit_t *hit) {
  MemPage *pg[FEED_N];
  Pgno next[FEED_N];
  int ix[FEED_N], ord[FEED_N];
  i64 sk[FEED_N], kmin[FEED_N], kmax[FEED_N];
  u8 own[FEED_N];
  if (n < 1 || n > FEED_N) return -1;
  // Sort (the feed's ids arrive newest first: reverse; authors arrive in any order).
  int desc = 1, asc = 1;
  for (int i = 1; i < n; i++) {
    desc &= key[i] < key[i - 1];
    asc &= key[i] > key[i - 1];
  }
  if (asc || desc) {
    for (int i = 0; i < n; i++) {
      int j = desc ? n - 1 - i : i;
      sk[j] = key[i];
      ord[j] = i;
    }
  } else {
    for (int i = 0; i < n; i++) {
      int j = i;
      while (j > 0 && sk[j - 1] > key[i]) {
        sk[j] = sk[j - 1];
        ord[j] = ord[j - 1];
        j--;
      }
      sk[j] = key[i];
      ord[j] = i;
    }
  }
  for (int i = 0; i < n; i++) {
    next[i] = root;
    kmin[i] = INT64_MIN;
    kmax[i] = INT64_MAX;
  }
  for (int depth = 0;; depth++) {
    // Fetch each run's page once.
    for (int i = 0; i < n;) {
      MemPage *p = page_get(next[i]);
      if (!p || !p->intKey || p->nCell == 0 || p->nOverflow || depth >= BTCURSOR_MAX_DEPTH || (i > 0 && p->leaf != pg[0]->leaf)) {
        if (p) releasePageNotNull(p);
        for (int k = 0; k < i; k++)
          if (own[k]) releasePageNotNull(pg[k]);
        return -1;
      }
      __builtin_prefetch(p->aCellIdx);
      __builtin_prefetch(p->aCellIdx + 2 * (p->nCell - 1));
      own[i] = 1;
      pg[i] = p;
      for (i++; i < n && next[i] == next[i - 1]; i++) {
        pg[i] = p;
        own[i] = 0;
      }
    }
    int leaf = pg[0]->leaf;
    for (int i = 0; i < n;) {
      MemPage *p = pg[i];
      int nc = p->nCell, j = i + 1;
      while (j < n && pg[j] == p) j++;
      i64 a = kmin[i] == INT64_MIN ? cell_key(p, 0) : kmin[i];     // the root's range: its own cells
      i64 b = kmax[i] == INT64_MAX ? cell_key(p, nc - 1) : kmax[i]; // (hot: every request reads them)
      if (leaf) {
        int dense = b - a + 1 == nc;
        int lo = 0;
        for (int k = i; k < j; k++) {
          i64 kk = sk[k];
          int e = dense ? (int)(kk - a) : guess_ix(kk, a, b, nc);
          if (e < lo) e = lo;
          if (e >= nc) e = nc - 1;
          if (!dense || cell_key(p, e) != kk) e = lo < nc ? lb_from_guess(p, kk, e, lo, nc) : nc;
          ix[k] = e;
          lo = e;
        }
      } else {
        int cl = lb_from_guess(p, sk[i], guess_ix(sk[i], a, b, nc), 0, nc);
        int ch = cl;
        if (j - i > 1 && cl < nc) {
          int e = guess_ix(sk[j - 1], a, b, nc);
          ch = lb_from_guess(p, sk[j - 1], e < cl ? cl : e, cl, nc);
        }
        ix[i] = cl;
        ix[j - 1] = ch;
        if (cl == ch) {
          for (int k = i + 1; k < j - 1; k++) ix[k] = cl;
        } else {
          int lo = cl;
          for (int k = i + 1; k < j - 1; k++) {
            int e = guess_ix(sk[k], a, b, nc);
            e = e < lo ? lo : e > ch ? ch : e;
            lo = ix[k] = lb_from_guess(p, sk[k], e, lo, ch < nc ? ch + 1 : nc);
          }
        }
        // the children and their key ranges, once per distinct child
        for (int k = i; k < j; k++) {
          int x = ix[k];
          if (k > i && x == ix[k - 1]) {
            next[k] = next[k - 1];
            kmin[k] = kmin[k - 1];
            kmax[k] = kmax[k - 1];
            continue;
          }
          next[k] = child_of(p, x);
          if (x > 0) kmin[k] = cell_key(p, x - 1) + 1;
          if (x < nc) kmax[k] = cell_key(p, x);
        }
      }
      i = j;
    }
    if (leaf) break;
    for (int i = 0; i < n; i++)
      if (own[i]) releasePageNotNull(pg[i]);
  }
  for (int i = 0; i < n; i++) {
    hit_t *h = &hit[ord[i]];
    h->pg = pg[i];
    h->ix = ix[i];
    h->own = own[i];
    h->found = ix[i] < pg[i]->nCell && cell_key(pg[i], ix[i]) == sk[i];
  }
  return 0;
}

// The record of a hit, when it is entirely on its page (posts and users rows always are).
static int hit_record(const hit_t *h, const u8 **rec, u32 *len) {
  CellInfo info;
  h->pg->xParseCell(h->pg, findCell(h->pg, h->ix), &info);
  if (info.nLocal != info.nPayload) return -1;
  *rec = info.pPayload;
  *len = info.nPayload;
  return 0;
}

// ---- walking an index in key order --------------------------------------------------------------
//
// The same walk as sqlite3BtreeNext over an index B-tree (whose interior cells are entries too):
// a stack of (page, cell) down to the current entry, pages pinned while on the stack. Used for
// the 20 first entries of posts_created_at_id_idx (the newest posts) and the likes_post_id_idx
// range that counts the likes of 20 posts, which a cursor would do in ~4x the instructions.
typedef struct {
  MemPage *pg;
  int ix;
} ipos_t;
typedef struct {
  ipos_t st[BTCURSOR_MAX_DEPTH];
  int n;
} iter_t;

static void iter_release(iter_t *it) {
  for (int i = 0; i < it->n; i++) releasePageNotNull(it->st[i].pg);
  it->n = 0;
}

static int iter_push(iter_t *it, Pgno pgno, int has_key, i64 key) {
  if (it->n >= BTCURSOR_MAX_DEPTH) return -1;
  MemPage *pg = page_get(pgno);
  if (!pg) return -1;
  if (pg->intKey || pg->nOverflow || (pg->nCell == 0 && !pg->leaf)) {
    releasePageNotNull(pg);
    return -1;
  }
  int ix = 0;
  if (has_key && pg->nCell) {
    ix = idx_lower_bound(pg, key);
    if (ix < pg->nCell && idx_key(pg, ix) == INT64_MAX) {
      releasePageNotNull(pg);
      return -1;
    }
  }
  it->st[it->n].pg = pg;
  it->st[it->n].ix = ix;
  it->n++;
  return 0;
}

// Positions on the first entry >= key (or the first entry at all). -1 if the plan declines.
static int iter_seek(iter_t *it, Pgno root, int has_key, i64 key) {
  it->n = 0;
  Pgno pgno = root;
  for (;;) {
    if (iter_push(it, pgno, has_key, key)) {
      iter_release(it);
      return -1;
    }
    ipos_t *t = &it->st[it->n - 1];
    if (t->pg->leaf) return 0;
    pgno = child_of(t->pg, t->ix);
  }
}

// The page and cell of the current entry; 0 past the end.
static inline ipos_t *iter_cur(iter_t *it) {
  for (;;) {
    if (it->n == 0) return 0;
    ipos_t *t = &it->st[it->n - 1];
    if (t->ix < t->pg->nCell) return t;
    releasePageNotNull(t->pg);
    it->n--;
  }
}

// Steps past the current entry. -1 if the plan declines.
static int iter_next(iter_t *it) {
  ipos_t *t = &it->st[it->n - 1];
  t->ix++;
  if (t->pg->leaf) return 0;
  Pgno pgno = child_of(t->pg, t->ix); // the leftmost entry below the next child
  for (;;) {
    if (iter_push(it, pgno, 0, 0)) {
      iter_release(it);
      return -1;
    }
    ipos_t *c = &it->st[it->n - 1];
    if (c->pg->leaf) return c->pg->nCell ? 0 : -1;
    pgno = child_of(c->pg, 0);
  }
}

// The current entry's record (local payload only).
static inline int iter_record(const ipos_t *t, const u8 **rec, u32 *len) {
  const u8 *c = findCellPastPtr(t->pg, t->ix);
  u64 npay;
  if (!(c = varint(c, t->pg->aDataEnd, &npay)) || npay > t->pg->maxLocal || c + npay > t->pg->aDataEnd) return -1;
  *rec = c;
  *len = (u32)npay;
  return 0;
}

// Likes for lo <= post_id <= hi: cnt[post_id - lo]++ per index entry. -1 if the plan declines.
// A leaf's entries are counted in one tight loop; the walk only steps between pages.
static int count_likes(i64 lo, i64 hi, u32 *cnt) {
  iter_t it;
  if (iter_seek(&it, root_likes, 1, lo)) return -1;
  ipos_t *t;
  while ((t = iter_cur(&it))) {
    MemPage *p = t->pg;
    if (p->leaf) {
      int nc = p->nCell, ix = t->ix;
      for (; ix < nc; ix++) {
        i64 v = idx_key(p, ix);
        if (v > hi) {
          if (v == INT64_MAX) {
            iter_release(&it);
            return -1;
          }
          iter_release(&it);
          return 0;
        }
        if (v >= lo) cnt[v - lo]++;
      }
      t->ix = nc;
      continue;
    }
    i64 v = idx_key(p, t->ix);
    if (v == INT64_MAX) {
      iter_release(&it);
      return -1;
    }
    if (v > hi) break;
    if (v >= lo) cnt[v - lo]++;
    if (iter_next(&it)) return -1;
  }
  iter_release(&it);
  return 0;
}

// The id (second column) of a posts_created_at_id_idx entry: (created_at TEXT, id INTEGER, rowid).
// INT64_MAX if the entry has another shape or is malformed.
static inline i64 feed_entry_id(const ipos_t *t) {
  const u8 *c = findCellPastPtr(t->pg, t->ix);
  // the usual shape: payload size, header size, the text's type and the id's type all one byte
  u32 b0 = c[0], hsz = c[1], tt = c[2], ti = c[3];
  if (__builtin_expect((b0 | hsz | tt | ti) < 0x80 && hsz >= 3 && tt >= 13 && (tt & 1) && ti >= 1 && ti <= 6, 1)) {
    static const u8 fl[7] = {0, 1, 2, 3, 4, 6, 8};
    u32 tl = (tt - 13) / 2, il = fl[ti];
    const u8 *p = c + 1 + hsz + tl;
    if (hsz + tl + il <= b0 && c + 1 + b0 <= t->pg->aDataEnd) {
      i64 x = (signed char)p[0];
      for (u32 j = 1; j < il; j++) x = x << 8 | p[j];
      return x;
    }
  }
  const u8 *rec;
  u32 len;
  col_t cols[2];
  if (iter_record(t, &rec, &len) || rec_cols(rec, len, 2, cols) || !col_is_text(&cols[0]) || !col_is_int(&cols[1])) return INT64_MAX;
  return col_int(&cols[1]);
}

// The ids of the first n entries of posts_created_at_id_idx (newest first). Returns the count.
static int newest_ids(i64 *ids, int n) {
  iter_t it;
  if (iter_seek(&it, root_feed, 0, 0)) return -1;
  int k = 0;
  ipos_t *t;
  while (k < n && (t = iter_cur(&it))) {
    i64 id = feed_entry_id(t);
    if (id == INT64_MAX) {
      iter_release(&it);
      return -1;
    }
    ids[k++] = id;
    if (iter_next(&it)) return -1;
  }
  iter_release(&it);
  return k;
}

// ---- the response writer -----------------------------------------------------------------------
//
// Status line, the two headers and the JSON body, written once into the caller's buffer. The
// Content-Length value is a 5-character field, right-aligned with leading spaces (optional
// whitespace before a field value, RFC 9110 5.6.3), so the body never moves. Text is checked
// for characters to escape 32 bytes at a time (AVX2) and copied in the same step.

typedef struct {
  u8 *d, *lim, *start, *clen;
  int ovf;
} wr_t;

#define ROOM 96 // every text write leaves this much for the rest of its row

static inline void wr_raw(wr_t *w, const void *s, size_t n) {
  if ((size_t)(w->lim - w->d) < n) {
    w->ovf = 1;
    return;
  }
  memcpy(w->d, s, n);
  w->d += n;
}
#define LIT(w, s) wr_raw(w, s, sizeof(s) - 1)

static const char digits2[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

// Decimal integer, two digits per step (ids and counts: 40 of them per feed).
static void wr_int(wr_t *w, i64 v) {
  if (w->lim - w->d < 21) {
    w->ovf = 1;
    return;
  }
  char t[20];
  int k = 20;
  u64 u = v < 0 ? (u64)0 - (u64)v : (u64)v;
  while (u >= 100) {
    u64 q = u / 100;
    memcpy(t + k - 2, digits2 + 2 * (u - q * 100), 2);
    k -= 2;
    u = q;
  }
  if (u >= 10) {
    memcpy(t + k - 2, digits2 + 2 * u, 2);
    k -= 2;
  } else {
    t[--k] = (char)('0' + u);
  }
  if (v < 0) *w->d++ = '-';
  memcpy(w->d, t + k, (size_t)(20 - k));
  w->d += 20 - k;
}

static const char hexd[] = "0123456789abcdef";

static void wr_esc_slow(wr_t *w, const u8 *s, u32 n) {
  u8 *d = w->d;
  for (u32 i = 0; i < n; i++) {
    u8 c = s[i];
    if (d + 6 + ROOM > w->lim) {
      w->ovf = 1;
      w->d = d;
      return;
    }
    if (c >= 0x20 && c != '"' && c != '\\') {
      *d++ = c;
      continue;
    }
    *d++ = '\\';
    switch (c) {
    case '"': *d++ = '"'; break;
    case '\\': *d++ = '\\'; break;
    case '\n': *d++ = 'n'; break;
    case '\r': *d++ = 'r'; break;
    case '\t': *d++ = 't'; break;
    case '\b': *d++ = 'b'; break;
    case '\f': *d++ = 'f'; break;
    default:
      *d++ = 'u'; *d++ = '0'; *d++ = '0';
      *d++ = hexd[c >> 4]; *d++ = hexd[c & 15];
    }
  }
  w->d = d;
}

// A JSON string's content (no quotes): escapes only ", \ and control characters.
static void wr_esc(wr_t *w, const u8 *s, u32 n) {
  if (w->ovf || (u64)(w->lim - w->d) < (u64)n + ROOM) {
    w->ovf = 1;
    return;
  }
  u8 *d = w->d;
  u32 i = 0;
#if defined(__AVX2__)
  const __m256i q = _mm256_set1_epi8('"'), b = _mm256_set1_epi8('\\'), c = _mm256_set1_epi8(0x1f);
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, b)),
                                _mm256_cmpeq_epi8(_mm256_max_epu8(v, c), c));
    _mm256_storeu_si256((__m256i *)(d + i), v);
    if (_mm256_movemask_epi8(m)) break;
  }
  if (i + 32 > n && i < n && n >= 32) { // the tail, overlapping the last full block
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + n - 32));
    __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, b)),
                                _mm256_cmpeq_epi8(_mm256_max_epu8(v, c), c));
    if (!_mm256_movemask_epi8(m)) {
      _mm256_storeu_si256((__m256i *)(d + n - 32), v);
      i = n;
    }
  }
#endif
  for (; i + 8 <= n; i += 8) {
    u64 x;
    memcpy(&x, s + i, 8);
    const u64 lo = 0x0101010101010101ULL, hi = 0x8080808080808080ULL;
    u64 qq = x ^ (lo * '"'), bb = x ^ (lo * '\\');
    memcpy(d + i, &x, 8);
    if ((((x - lo * 0x20) & ~x) | ((qq - lo) & ~qq) | ((bb - lo) & ~bb)) & hi) break;
  }
  w->d = d + i;
  wr_esc_slow(w, s + i, n - i);
}

// Begins a response: everything up to the body.
static void resp_begin(wr_t *w, int status) {
  static const char *lines[] = {"HTTP/1.1 200 OK\r\n", "HTTP/1.1 201 Created\r\n", "HTTP/1.1 404 Not Found\r\n",
                                "HTTP/1.1 500 Internal Server Error\r\n", "HTTP/1.1 503 Service Unavailable\r\n"};
  int k = status == 200 ? 0 : status == 201 ? 1 : status == 404 ? 2 : status == 500 ? 3 : 4;
  w->ovf = 0;
  wr_raw(w, lines[k], strlen(lines[k]));
  LIT(w, "Content-Type: application/json\r\nContent-Length:     0\r\n\r\n");
  w->clen = w->d - 9; // the 5 characters before \r\n\r\n
  w->start = w->d;
}

// Ends it: fills in Content-Length. 0 ok, -1 overflow.
static int resp_end(wr_t *w) {
  if (w->ovf) return -1;
  size_t n = (size_t)(w->d - w->start);
  if (n > 99999) return -1;
  for (int i = 4; i >= 0; i--) {
    if (n == 0 && i < 4) {
      w->clen[i] = ' ';
      continue;
    }
    w->clen[i] = (char)('0' + n % 10);
    n /= 10;
  }
  return 0;
}

static void wr_error(wr_t *w, int status, const char *msg) {
  resp_begin(w, status);
  LIT(w, "{\"error\":\"");
  wr_raw(w, msg, strlen(msg));
  LIT(w, "\"}");
  resp_end(w);
}

// A post object from raw columns.
static void wr_post(wr_t *w, i64 id, const u8 *body, u32 nbody, const u8 *created, u32 ncreated, const u8 *author,
                    u32 nauthor, i64 likes) {
  LIT(w, "{\"id\":");
  wr_int(w, id);
  LIT(w, ",\"body\":\"");
  wr_esc(w, body, nbody);
  LIT(w, "\",\"created_at\":\"");
  wr_esc(w, created, ncreated);
  LIT(w, "\",\"author\":\"");
  wr_esc(w, author, nauthor);
  LIT(w, "\",\"like_count\":");
  wr_int(w, likes);
  LIT(w, "}");
}

// ---- the feed ----------------------------------------------------------------------------------

typedef struct {
  int n;
  i64 ids[FEED_N], uids[FEED_N], likes[FEED_N];
  hit_t posts[FEED_N], users[FEED_N];
  col_t pc[FEED_N][4], uc[FEED_N][2];
} feed_t;

static void feed_release(feed_t *f) {
  hits_release(f->posts, f->n);
  hits_release(f->users, f->n);
}

// Gathers the feed's rows: pages pinned in f until feed_release. 0 ok, -1 the plan declines.
static int feed_plan(feed_t *f) {
  f->n = 0;
  int n = newest_ids(f->ids, FEED_N);
  if (n < 0) return -1;
  if (n == 0) return 0;
  i64 lo = f->ids[0], hi = f->ids[0];
  for (int i = 1; i < n; i++) {
    if (f->ids[i] < lo) lo = f->ids[i];
    if (f->ids[i] > hi) hi = f->ids[i];
  }
  if (hi - lo >= FEED_SPAN) return -1;
  u32 cnt[FEED_SPAN];
  memset(cnt, 0, (size_t)(hi - lo + 1) * sizeof cnt[0]);
  if (count_likes(lo, hi, cnt)) return -1;
  if (seek_many(root_posts, f->ids, n, f->posts)) return -1;
  f->n = n;
  for (int i = 0; i < n; i++) {
    const u8 *rec;
    u32 len;
    col_t *c = f->pc[i];
    if (!f->posts[i].found || hit_record(&f->posts[i], &rec, &len) || rec_cols(rec, len, 4, c) || !col_is_int(&c[1]) ||
        !col_is_text(&c[2]) || !col_is_text(&c[3])) {
      hits_release(f->posts, n);
      f->n = 0;
      return -1; // the index names a row the table lacks, or an odd row: the SQL path decides
    }
    f->uids[i] = col_int(&c[1]);
    f->likes[i] = cnt[f->ids[i] - lo];
  }
  if (seek_many(root_users, f->uids, n, f->users)) {
    hits_release(f->posts, n);
    f->n = 0;
    return -1;
  }
  for (int i = 0; i < n; i++) {
    const u8 *rec;
    u32 len;
    col_t *c = f->uc[i];
    if (!f->users[i].found || hit_record(&f->users[i], &rec, &len) || rec_cols(rec, len, 2, c) || !col_is_text(&c[1])) {
      feed_release(f);
      f->n = 0;
      return -1; // a post without a user: the JOIN decides
    }
  }
  return 0;
}

static void feed_render(wr_t *w, const feed_t *f) {
  resp_begin(w, 200);
  LIT(w, "{\"posts\":[");
  for (int i = 0; i < f->n; i++) {
    if (i) LIT(w, ",");
    const col_t *p = f->pc[i], *u = f->uc[i];
    wr_post(w, f->ids[i], p[2].p, p[2].n, p[3].p, p[3].n, u[1].p, u[1].n, f->likes[i]);
  }
  LIT(w, "]}");
  resp_end(w);
}

// The SQL fallbacks render as they step; a second rendering (spill) re-runs the statement.
static void sql_post(wr_t *w, sqlite3_stmt *s) {
  wr_post(w, sqlite3_column_int64(s, 0), sqlite3_column_text(s, 1), (u32)sqlite3_column_bytes(s, 1),
          sqlite3_column_text(s, 2), (u32)sqlite3_column_bytes(s, 2), sqlite3_column_text(s, 3),
          (u32)sqlite3_column_bytes(s, 3), sqlite3_column_int64(s, 4));
}

static int feed_sql(wr_t *w) {
  resp_begin(w, 200);
  LIT(w, "{\"posts\":[");
  int rc, i = 0;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
    if (i++) LIT(w, ",");
    sql_post(w, st_feed);
  }
  sqlite3_reset(st_feed);
  if (rc != SQLITE_DONE) {
    check_tx();
    return -1;
  }
  LIT(w, "]}");
  return resp_end(w);
}

// 1 found, 0 missing, -1 error.
static int post_sql(wr_t *w, i64 id) {
  sqlite3_bind_int64(st_one, 1, id);
  int rc = sqlite3_step(st_one);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st_one);
    if (rc != SQLITE_DONE) check_tx();
    return rc == SQLITE_DONE ? 0 : -1;
  }
  resp_begin(w, 200);
  LIT(w, "{\"post\":");
  sql_post(w, st_one);
  LIT(w, "}");
  sqlite3_reset(st_one);
  return resp_end(w) ? -1 : 1;
}

// ---- one post ----------------------------------------------------------------------------------

typedef struct {
  i64 id, uid, likes;
  hit_t post, user;
  col_t pc[4], uc[2];
} post_t;

static void post_release(post_t *p) {
  hits_release(&p->post, 1);
  hits_release(&p->user, 1);
}

// 1 found (pages pinned until post_release), 0 missing, -1 the plan declines.
static int post_plan(post_t *p, i64 id) {
  p->id = id;
  p->post.own = p->user.own = 0;
  u32 cnt = 0;
  if (count_likes(id, id, &cnt)) return -1;
  p->likes = cnt;
  if (seek_many(root_posts, &id, 1, &p->post)) return -1;
  if (!p->post.found) {
    post_release(p);
    return 0;
  }
  const u8 *rec;
  u32 len;
  if (hit_record(&p->post, &rec, &len) || rec_cols(rec, len, 4, p->pc) || !col_is_int(&p->pc[1]) ||
      !col_is_text(&p->pc[2]) || !col_is_text(&p->pc[3])) {
    post_release(p);
    return -1;
  }
  p->uid = col_int(&p->pc[1]);
  if (seek_many(root_users, &p->uid, 1, &p->user)) {
    post_release(p);
    return -1;
  }
  if (!p->user.found || hit_record(&p->user, &rec, &len) || rec_cols(rec, len, 2, p->uc) || !col_is_text(&p->uc[1])) {
    post_release(p);
    return -1; // the JOIN decides
  }
  return 1;
}

static void post_render(wr_t *w, const post_t *p) {
  resp_begin(w, 200);
  LIT(w, "{\"post\":");
  wr_post(w, p->id, p->pc[2].p, p->pc[2].n, p->pc[3].p, p->pc[3].n, p->uc[1].p, p->uc[1].n, p->likes);
  LIT(w, "}");
  resp_end(w);
}

// ---- requests ----------------------------------------------------------------------------------

// Responses that don't fit the reserved buffer are rendered again into a buffer the shim keeps
// until the next batch. In practice a feed is ~5 KB and the reserve 16 KB; this is for bodies
// full of characters that escape to \u00XX.
#define SPILL_CAP (256 << 10)
static u8 *spills[64];
static int nspills;

static int spill(td_req_t *r, wr_t *w) {
  if (nspills >= (int)(sizeof spills / sizeof spills[0])) return -1;
  u8 *b = sqlite3_malloc(SPILL_CAP);
  if (!b) return -1;
  spills[nspills++] = b;
  stats[S_SPILL]++;
  r->out = b;
  r->cap = SPILL_CAP;
  r->status |= TD_SPILL;
  w->d = b;
  w->lim = b + SPILL_CAP;
  w->ovf = 0;
  return 0;
}

static void spills_free(void) {
  for (int i = 0; i < nspills; i++) sqlite3_free(spills[i]);
  nspills = 0;
}

static void finish(td_req_t *r, wr_t *w, int status) {
  r->status = (uint8_t)(status | (r->status & TD_SPILL));
  r->n = (int32_t)(w->d - r->out);
}

static void fail(td_req_t *r, wr_t *w) {
  logerr("request failed");
  w->d = r->out;
  w->lim = r->out + r->cap;
  wr_error(w, 500, "internal server error");
  finish(r, w, TD_ERR);
}

static void run_feed(td_req_t *r, wr_t *w) {
  stats[S_FEED]++;
  if (tx()) return fail(r, w);
  feed_t f;
  if (feed_plan(&f) == 0) {
    feed_render(w, &f);
    if (w->ovf && spill(r, w) == 0) feed_render(w, &f);
    feed_release(&f);
    if (!w->ovf) return finish(r, w, TD_OK);
    return fail(r, w);
  }
  stats[S_FALLBACK]++;
  int rc = feed_sql(w);
  if (rc && w->ovf && spill(r, w) == 0) rc = feed_sql(w);
  if (rc) return fail(r, w);
  finish(r, w, TD_OK);
}

static void run_post(td_req_t *r, wr_t *w) {
  stats[S_POST]++;
  if (tx()) return fail(r, w);
  post_t p;
  int rc = post_plan(&p, r->id);
  if (rc == 1) {
    post_render(w, &p);
    if (w->ovf && spill(r, w) == 0) post_render(w, &p);
    post_release(&p);
    if (!w->ovf) return finish(r, w, TD_OK);
    return fail(r, w);
  }
  if (rc < 0) {
    stats[S_FALLBACK]++;
    rc = post_sql(w, r->id);
    if (rc < 0 && w->ovf && spill(r, w) == 0) rc = post_sql(w, r->id);
    if (rc < 0) return fail(r, w);
    if (rc == 1) return finish(r, w, TD_OK);
  }
  wr_error(w, 404, "post not found");
  finish(r, w, TD_MISSING);
}

// Does the post exist? 1, 0, or -1 on error.
static int post_exists(i64 id) {
  hit_t h;
  if (seek_many(root_posts, &id, 1, &h) == 0) {
    int found = h.found;
    hits_release(&h, 1);
    return found;
  }
  stats[S_FALLBACK]++;
  sqlite3_bind_int64(st_exists, 1, id);
  int rc = step_reset(st_exists);
  if (rc == SQLITE_ROW) return 1;
  if (rc == SQLITE_DONE) return 0;
  check_tx();
  return -1;
}

static void run_like(td_req_t *r, wr_t *w) {
  stats[S_LIKE]++;
  if (tx_write()) return fail(r, w);
  int ex = post_exists(r->id);
  if (ex < 0) return fail(r, w);
  if (ex == 0) {
    wr_error(w, 404, "post not found");
    return finish(r, w, TD_MISSING);
  }
  sqlite3_bind_int64(st_like, 1, r->uid);
  sqlite3_bind_int64(st_like, 2, r->id);
  if (step_reset(st_like) != SQLITE_DONE) {
    check_tx();
    return fail(r, w);
  }
  int fresh = sqlite3_changes(db) > 0;
  resp_begin(w, fresh ? 201 : 200);
  wr_raw(w, fresh ? "{\"liked\":true,\"already_liked\":false,\"post_id\":" : "{\"liked\":true,\"already_liked\":true,\"post_id\":",
         fresh ? 46 : 45);
  wr_int(w, r->id);
  LIT(w, "}");
  if (resp_end(w)) return fail(r, w);
  finish(r, w, fresh ? TD_OK : TD_REPEAT);
}

static void run_insert(td_req_t *r, wr_t *w) {
  stats[S_INSERT]++;
  if (tx_write()) return fail(r, w);
  sqlite3_bind_int64(st_post, 1, r->uid);
  sqlite3_bind_text(st_post, 2, (const char *)r->body, r->nbody, SQLITE_STATIC);
  int rc = sqlite3_step(st_post);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st_post);
    sqlite3_clear_bindings(st_post);
    check_tx();
    return fail(r, w);
  }
  i64 id = sqlite3_column_int64(st_post, 0);
  const u8 *created = sqlite3_column_text(st_post, 1);
  u32 ncreated = (u32)sqlite3_column_bytes(st_post, 1);
  for (int pass = 0; pass < 2; pass++) {
    resp_begin(w, 201);
    LIT(w, "{\"post\":");
    wr_post(w, id, r->body, (u32)r->nbody, created, ncreated, r->name, (u32)r->nname, 0);
    LIT(w, "}");
    if (!resp_end(w) || pass || spill(r, w)) break;
  }
  int ok = !w->ovf;
  rc = sqlite3_step(st_post);
  sqlite3_reset(st_post);
  sqlite3_clear_bindings(st_post);
  if (rc != SQLITE_DONE) {
    check_tx();
    return fail(r, w);
  }
  if (!ok) return fail(r, w);
  r->id = id;
  finish(r, w, TD_OK);
}

static void run_health(td_req_t *r, wr_t *w) {
  int rc = sqlite3_step(st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(st_health, 0) == 1;
  const char *msg = ok ? "" : sqlite3_errmsg(db);
  sqlite3_reset(st_health);
  if (!ok) {
    resp_begin(w, 503);
    LIT(w, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    wr_esc(w, (const u8 *)msg, (u32)strlen(msg));
    LIT(w, "\"}");
    if (resp_end(w)) return fail(r, w);
    return finish(r, w, TD_ERR);
  }
  resp_begin(w, 200);
  LIT(w, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  wr_int(w, r->id);
  LIT(w, "}");
  if (resp_end(w)) return fail(r, w);
  finish(r, w, TD_OK);
}

int td_batch(td_req_t *reqs, int n) {
  spills_free();
  wr_t w;
  for (int i = 0; i < n; i++) {
    td_req_t *r = &reqs[i];
    r->status = 0;
    w.d = r->out;
    w.lim = r->out + r->cap;
    w.ovf = 0;
    switch (r->kind) {
    case TD_FEED: run_feed(r, &w); break;
    case TD_POST: run_post(r, &w); break;
    case TD_LIKE: run_like(r, &w); break;
    case TD_INSERT: run_insert(r, &w); break;
    case TD_HEALTH: run_health(r, &w); break;
    default: fail(r, &w);
    }
  }
  return td_commit();
}

void td_stats(int64_t *out, int n) {
  for (int i = 0; i < n && i < N_STATS; i++) out[i] = stats[i];
}
