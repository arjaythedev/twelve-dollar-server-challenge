/* db.h: SQLite. One connection on the event loop serves every request inside one transaction
 * per batch (group commit); a second connection on a thread runs PASSIVE checkpoints. The
 * `unix-excl` VFS keeps the wal-index in heap memory and drops every fcntl() lock, which is
 * allowed because this process is the only one that opens the file. */
#ifndef DB_H
#define DB_H

#include "json.h"
#include <pthread.h>
#include <stdatomic.h>

#define VFS_NAME "tdsc-wal" /* the WAL-write shim over unix-excl, defined below */
#define WAL_WAKE_FRAMES 1000   /* wake the checkpoint thread */
#define WAL_GATE_FRAMES 4000   /* hold writes until the log is fully copied, so the next write restarts it */
#define WAL_INLINE_FRAMES 16384 /* last resort: checkpoint on the loop */
#define WAL_GATE_MAX_NS 1000000000ULL

#define FEED_N 20
#define FEED_SPAN 256 /* the 20 newest ids must sit within this many ids for the range plan */

static inline void ob_post_obj(struct ob *o, int64_t id, const uint8_t *body, int body_n, const uint8_t *created,
                               int created_n, const uint8_t *author, int author_n, int64_t likes) {
  ob_lit(o, "{\"id\":");
  ob_u64(o, (uint64_t)id);
  ob_lit(o, ",\"body\":");
  ob_json_str(o, body, (size_t)body_n);
  ob_lit(o, ",\"created_at\":");
  ob_json_str(o, created, (size_t)created_n);
  ob_lit(o, ",\"author\":");
  ob_json_str(o, author, (size_t)author_n);
  ob_lit(o, ",\"like_count\":");
  ob_u64(o, (uint64_t)likes);
  ob_char(o, '}');
}

#include "btree.h"

struct db {
  sqlite3 *c;
  sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_health;
  sqlite3_stmt *st_feed_ids, *st_feed_rows, *st_feed_cnt, *st_feed_ref;
  sqlite3_stmt *st_post;
  sqlite3_stmt *st_ins_post, *st_created_at;
  sqlite3_stmt *st_ins_like, *st_post_exists;
  int in_txn;
  int wrote;
  struct bt bt;
  /* checkpointing */
  sqlite3 *c2;
  pthread_t thr;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int wanted, stop;
  _Atomic uint64_t commit_seq;     /* bumped by the wal hook after each commit */
  _Atomic uint64_t backfilled_seq; /* commit_seq at the start of the last pass that copied everything */
  _Atomic int wal_frames;
  int gate_held;
  uint64_t gate_since;
  /* stats */
  uint64_t n_ckpt, n_gate, n_inline, n_fallback;
};

static int db_wal_hook(void *ud, sqlite3 *c, const char *name, int frames) {
  (void)c; (void)name;
  struct db *d = ud;
  atomic_store_explicit(&d->wal_frames, frames, memory_order_relaxed);
  atomic_fetch_add_explicit(&d->commit_seq, 1, memory_order_release);
  if (frames >= WAL_WAKE_FRAMES) {
    if (frames >= WAL_GATE_FRAMES && !d->gate_held) {
      d->gate_held = 1;
      d->gate_since = now_ns();
      d->n_gate++;
    }
    pthread_mutex_lock(&d->mu);
    d->wanted = 1;
    pthread_cond_signal(&d->cv);
    pthread_mutex_unlock(&d->mu);
  }
  return SQLITE_OK;
}

static void *db_ckpt_thread(void *ud) {
  struct db *d = ud;
  prefault_stack(128 << 10);
  for (;;) {
    pthread_mutex_lock(&d->mu);
    while (!d->wanted && !d->stop) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 250000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&d->cv, &d->mu, &ts);
    }
    if (d->stop) { pthread_mutex_unlock(&d->mu); return NULL; }
    d->wanted = 0;
    pthread_mutex_unlock(&d->mu);
    uint64_t seq = atomic_load_explicit(&d->commit_seq, memory_order_acquire);
    int nlog = 0, nckpt = 0;
    int rc = sqlite3_wal_checkpoint_v2(d->c2, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &nckpt);
    d->n_ckpt++;
    if ((rc == SQLITE_OK || rc == SQLITE_BUSY) && nlog == nckpt)
      atomic_store_explicit(&d->backfilled_seq, seq, memory_order_release);
    /* while the gate is held, keep passing until the log is copied */
    if (d->gate_held && !(nlog == nckpt)) {
      pthread_mutex_lock(&d->mu);
      d->wanted = 1;
      pthread_mutex_unlock(&d->mu);
    }
  }
}

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

static int db_open_conn(sqlite3 **out, const char *path, long long mmap_bytes, int cache_kib) {
  int rc = sqlite3_open_v2(path, out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE, VFS_NAME);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sqlite open %s: %s\n", path, sqlite3_errmsg(*out));
    return -1;
  }
  char sql[128];
  snprintf(sql, sizeof sql, "PRAGMA mmap_size=%lld", mmap_bytes);
  db_exec(*out, sql);
  snprintf(sql, sizeof sql, "PRAGMA cache_size=-%d", cache_kib);
  db_exec(*out, sql);
  db_exec(*out, "PRAGMA synchronous=NORMAL");
  db_exec(*out, "PRAGMA temp_store=MEMORY");
  db_exec(*out, "PRAGMA wal_autocheckpoint=0");
  sqlite3_busy_timeout(*out, 2000);
  return 0;
}

/* ---- page cache ----
 * SQLite's default page cache (pcache1) behind its documented plugin interface
 * (SQLITE_CONFIG_PCACHE2), with one decision changed: when a commit asks it to drop the pages
 * past the end of the file (xTruncate), and no such page exists, nothing is scanned.
 *
 * Why: pcache1 remembers the largest page number it has ever held and never lowers it. A
 * B-tree page split (balance_nonroot) briefly renames a page to the lock-byte page's number,
 * 1 GiB / page size + 1, and back. From then on every commit's truncate looks as if pages past
 * the end of a 250 MB file might exist, and pcache1 walks its whole hash table to find none.
 *
 * The wrapper sees every key that enters the cache (xFetch with create, xRekey) and keeps the
 * largest one, not counting pages parked at the lock-byte page's number, which it counts
 * separately. If nothing is parked and the largest key is below the limit, no page at or past
 * the limit can exist, so there is nothing to drop. Otherwise it calls pcache1's truncate as
 * usual. Pages that leave the cache only make the record an overestimate, which is safe. */
#define LOCK_BYTE_OFFSET 0x40000000u /* PENDING_BYTE in the file format */

struct wcache {
  sqlite3_pcache *base;
  unsigned max_key;   /* largest key ever entered, excluding the parked ones */
  unsigned lock_page; /* the lock-byte page's number for this page size */
  unsigned n_parked;  /* pages currently renamed to lock_page */
};
static sqlite3_pcache_methods2 pcache_base;
static uint64_t pcache_truncates, pcache_truncates_skipped;

static int wc_init(void *arg) { (void)arg; return pcache_base.xInit(pcache_base.pArg); }
static void wc_shutdown(void *arg) { (void)arg; pcache_base.xShutdown(pcache_base.pArg); }
static sqlite3_pcache *wc_create(int szPage, int szExtra, int bPurgeable) {
  struct wcache *w = sqlite3_malloc(sizeof *w);
  if (!w) return NULL;
  w->base = pcache_base.xCreate(szPage, szExtra, bPurgeable);
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
static void wc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) {
  pcache_base.xUnpin(((struct wcache *)p)->base, pg, discard);
}
static void wc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned old_key, unsigned new_key) {
  struct wcache *w = (struct wcache *)p;
  pcache_base.xRekey(w->base, pg, old_key, new_key);
  if (old_key == w->lock_page && w->n_parked) w->n_parked--;
  if (new_key == w->lock_page) w->n_parked++;
  else if (new_key > w->max_key) w->max_key = new_key;
}
static void wc_truncate(sqlite3_pcache *p, unsigned limit) {
  struct wcache *w = (struct wcache *)p;
  pcache_truncates++;
  if (w->n_parked == 0 && w->max_key < limit) { pcache_truncates_skipped++; return; }
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
 * frame header, then the page: walWriteOneFrame), and the last frame of a commit carries the
 * database size in its header. This VFS, a shim over unix-excl in the usual SQLite pattern, keeps
 * writes that continue exactly where the previous write on that file ended in a buffer, and
 * writes the buffer with one pwrite when the commit frame's page arrives, so a commit is one
 * system call instead of two per frame. Any other write (the WAL header after a restart, a page
 * rewritten in place, the checksum rewrite at commit) writes the buffer first and then goes
 * straight through, as does every other call on the file, so what reaches the kernel and its
 * order are unchanged, and the commit frame is in the kernel before SQLite updates the wal-index
 * and before COMMIT returns. Readers find frames only through the wal-index, so none can see a
 * buffered one; frames of a transaction that has not committed may wait in the buffer until the
 * commit or the next other call on the file. */
#define WAL_FRAME_HDR 24
#define WAL_BUF_CAP 0x1ffff /* the unix VFS passes at most 17 bits of a write's length (seekAndWriteFd) */

struct wfile {
  sqlite3_file base; /* must be first */
  sqlite3_file *real;
  int is_wal;
  int commit;               /* the buffer ends with a commit frame's header: write it after the page */
  int len;                  /* bytes in buf */
  sqlite3_int64 off;        /* file offset of buf[0] */
  sqlite3_int64 last_end;   /* where the previous write on this file ended */
  unsigned char *buf;       /* WAL_BUF_CAP bytes, allocated with the first buffered write */
};
static sqlite3_vfs *vfs_real;
static uint64_t wal_frames, wal_writes;

static int wf_flush(struct wfile *f) {
  if (!f->len) return SQLITE_OK;
  int n = f->len;
  f->len = 0;
  wal_writes++;
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
static int wf_read(sqlite3_file *p, void *b, int n, sqlite3_int64 off) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xRead(WF(p)->real, b, n, off);
}
static int wf_write(sqlite3_file *p, const void *b, int n, sqlite3_int64 off) {
  struct wfile *f = WF(p);
  if (!f->is_wal) return f->real->pMethods->xWrite(f->real, b, n, off);
  int append = off == f->last_end;
  f->last_end = off + n;
  if (f->len + n > WAL_BUF_CAP) WF_FLUSH(p); /* full: write what is held, keep going */
  if (!append || n > WAL_BUF_CAP || (!f->buf && !(f->buf = sqlite3_malloc(WAL_BUF_CAP)))) {
    WF_FLUSH(p);
    f->commit = 0;
    return f->real->pMethods->xWrite(f->real, b, n, off);
  }
  if (!f->len) f->off = off;
  memcpy(f->buf + f->len, b, (size_t)n);
  f->len += n;
  if (n == WAL_FRAME_HDR) { /* bytes 4-7: the database size after a commit, 0 on other frames */
    const unsigned char *h = b;
    f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    wal_frames++;
    return SQLITE_OK;
  }
  if (f->commit) {
    f->commit = 0;
    return wf_flush(f);
  }
  return SQLITE_OK;
}
static int wf_truncate(sqlite3_file *p, sqlite3_int64 size) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xTruncate(WF(p)->real, size);
}
static int wf_sync(sqlite3_file *p, int flags) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xSync(WF(p)->real, flags);
}
static int wf_filesize(sqlite3_file *p, sqlite3_int64 *size) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xFileSize(WF(p)->real, size);
}
static int wf_lock(sqlite3_file *p, int l) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xLock(WF(p)->real, l);
}
static int wf_unlock(sqlite3_file *p, int l) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xUnlock(WF(p)->real, l);
}
static int wf_check_reserved(sqlite3_file *p, int *out) {
  return WF(p)->real->pMethods->xCheckReservedLock(WF(p)->real, out);
}
static int wf_file_control(sqlite3_file *p, int op, void *arg) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xFileControl(WF(p)->real, op, arg);
}
static int wf_sector_size(sqlite3_file *p) { return WF(p)->real->pMethods->xSectorSize(WF(p)->real); }
static int wf_device_chars(sqlite3_file *p) { return WF(p)->real->pMethods->xDeviceCharacteristics(WF(p)->real); }
static int wf_shm_map(sqlite3_file *p, int pg, int sz, int extend, void volatile **out) {
  return WF(p)->real->pMethods->xShmMap(WF(p)->real, pg, sz, extend, out);
}
static int wf_shm_lock(sqlite3_file *p, int off, int n, int flags) {
  return WF(p)->real->pMethods->xShmLock(WF(p)->real, off, n, flags);
}
static void wf_shm_barrier(sqlite3_file *p) { WF(p)->real->pMethods->xShmBarrier(WF(p)->real); }
static int wf_shm_unmap(sqlite3_file *p, int del) { return WF(p)->real->pMethods->xShmUnmap(WF(p)->real, del); }
static int wf_fetch(sqlite3_file *p, sqlite3_int64 off, int n, void **pp) {
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xFetch(WF(p)->real, off, n, pp);
}
static int wf_unfetch(sqlite3_file *p, sqlite3_int64 off, void *pg) {
  return WF(p)->real->pMethods->xUnfetch(WF(p)->real, off, pg);
}

static const sqlite3_io_methods wf_methods = {
    3,           wf_close,         wf_read,     wf_write,         wf_truncate,    wf_sync,
    wf_filesize, wf_lock,          wf_unlock,   wf_check_reserved, wf_file_control, wf_sector_size,
    wf_device_chars, wf_shm_map,   wf_shm_lock, wf_shm_barrier,   wf_shm_unmap,   wf_fetch,
    wf_unfetch};

#define WF_REAL_OFFSET ((sizeof(struct wfile) + 7) & ~(size_t)7)

static int wv_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags, int *out_flags) {
  (void)vfs;
  struct wfile *f = WF(p);
  memset(f, 0, sizeof *f);
  f->last_end = -1;
  f->real = (sqlite3_file *)((char *)p + WF_REAL_OFFSET);
  int rc = vfs_real->xOpen(vfs_real, name, f->real, flags, out_flags);
  if (f->real->pMethods == NULL) return rc; /* the open failed: SQLite will not call xClose */
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  if (f->real->pMethods->iVersion < 3) { /* unix-excl is version 3; anything else is not expected */
    f->real->pMethods->xClose(f->real);
    return SQLITE_CANTOPEN;
  }
  p->pMethods = &wf_methods;
  return rc;
}
static int wv_delete(sqlite3_vfs *v, const char *z, int sync) { (void)v; return vfs_real->xDelete(vfs_real, z, sync); }
static int wv_access(sqlite3_vfs *v, const char *z, int flags, int *out) {
  (void)v;
  return vfs_real->xAccess(vfs_real, z, flags, out);
}
static int wv_full_pathname(sqlite3_vfs *v, const char *z, int n, char *out) {
  (void)v;
  return vfs_real->xFullPathname(vfs_real, z, n, out);
}
static void *wv_dlopen(sqlite3_vfs *v, const char *z) { (void)v; return vfs_real->xDlOpen(vfs_real, z); }
static void wv_dlerror(sqlite3_vfs *v, int n, char *z) { (void)v; vfs_real->xDlError(vfs_real, n, z); }
static void (*wv_dlsym(sqlite3_vfs *v, void *h, const char *z))(void) { (void)v; return vfs_real->xDlSym(vfs_real, h, z); }
static void wv_dlclose(sqlite3_vfs *v, void *h) { (void)v; vfs_real->xDlClose(vfs_real, h); }
static int wv_randomness(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xRandomness(vfs_real, n, z); }
static int wv_sleep(sqlite3_vfs *v, int us) { (void)v; return vfs_real->xSleep(vfs_real, us); }
static int wv_current_time(sqlite3_vfs *v, double *t) { (void)v; return vfs_real->xCurrentTime(vfs_real, t); }
static int wv_last_error(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xGetLastError(vfs_real, n, z); }
static int wv_current_time64(sqlite3_vfs *v, sqlite3_int64 *t) {
  (void)v;
  return vfs_real->xCurrentTimeInt64(vfs_real, t);
}

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

static int db_open(struct db *d, const char *path, long long mmap_bytes, int cache_kib) {
  memset(d, 0, sizeof *d);
  if (!getenv("DEFAULT_PCACHE") && pcache_install() < 0) {
    fprintf(stderr, "sqlite: could not install the page cache\n");
    return -1;
  }
  if (sqlite3_initialize() != SQLITE_OK) { /* SQLITE_OMIT_AUTOINIT: nothing else does this */
    fprintf(stderr, "sqlite3_initialize failed\n");
    return -1;
  }
  if (vfs_install() < 0) {
    fprintf(stderr, "sqlite: could not register the %s VFS\n", VFS_NAME);
    return -1;
  }
  if (db_open_conn(&d->c, path, mmap_bytes, cache_kib) < 0) return -1;
  /* the seed is already in WAL mode; make sure, and refuse anything else */
  sqlite3_stmt *jm;
  if (db_prep(d->c, &jm, "PRAGMA journal_mode=WAL") != SQLITE_OK) return -1;
  int rc = sqlite3_step(jm);
  if (rc != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(jm, 0), "wal") != 0) {
    fprintf(stderr, "sqlite: could not enable WAL\n");
    return -1;
  }
  sqlite3_finalize(jm);

  if (db_prep(d->c, &d->st_begin, "BEGIN") || db_prep(d->c, &d->st_commit, "COMMIT") ||
      db_prep(d->c, &d->st_rollback, "ROLLBACK") || db_prep(d->c, &d->st_health, "SELECT 1") ||
      db_prep(d->c, &d->st_feed_ids, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20") ||
      db_prep(d->c, &d->st_feed_rows,
              "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id "
              "WHERE p.id BETWEEN ?1 AND ?2") ||
      db_prep(d->c, &d->st_feed_cnt, "SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id") ||
      db_prep(d->c, &d->st_feed_ref,
              "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
              "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20") ||
      db_prep(d->c, &d->st_post,
              "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
              "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1") ||
      db_prep(d->c, &d->st_ins_post, "INSERT INTO posts (user_id, body) VALUES (?1, ?2)") ||
      db_prep(d->c, &d->st_created_at, "SELECT created_at FROM posts WHERE id = ?1") ||
      db_prep(d->c, &d->st_ins_like,
              "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
              "ON CONFLICT (user_id, post_id) DO NOTHING") ||
      db_prep(d->c, &d->st_post_exists, "SELECT 1 FROM posts WHERE id = ?1"))
    return -1;

  /* warm the pages the feed touches */
  while (sqlite3_step(d->st_feed_ref) == SQLITE_ROW) {}
  sqlite3_reset(d->st_feed_ref);

#ifndef NO_BTREE
  if (bt_init(&d->bt, d->c)) fprintf(stderr, "btree read plans: on\n");
#endif

  sqlite3_wal_hook(d->c, db_wal_hook, d);

  /* checkpoint thread on its own connection */
  pthread_mutex_init(&d->mu, NULL);
  pthread_cond_init(&d->cv, NULL);
  if (db_open_conn(&d->c2, path, 0, 2048) < 0) return -1;
  if (pthread_create(&d->thr, NULL, db_ckpt_thread, d) != 0) {
    fprintf(stderr, "pthread_create failed\n");
    return -1;
  }
  return 0;
}

/* ---- transactions ---- */

static inline int db_begin(struct db *d) {
  if (d->in_txn) return 0;
  int rc = sqlite3_step(d->st_begin);
  sqlite3_reset(d->st_begin);
  if (rc != SQLITE_DONE) return -1;
  d->in_txn = 1;
  d->wrote = 0;
  if (d->bt.ok && bt_open(&d->bt) < 0) { /* keep serving, through SQL */
    fprintf(stderr, "btree: could not open cursors; reads use SQL\n");
    d->bt.ok = 0;
  }
  return 0;
}

/* Commits the batch. Returns 0 when every write landed, -1 if the batch rolled back. */
static int db_commit(struct db *d) {
  if (!d->in_txn) return 0;
  d->in_txn = 0;
  bt_close(&d->bt);
  int rc = sqlite3_step(d->st_commit);
  sqlite3_reset(d->st_commit);
  if (rc == SQLITE_DONE) return 0;
  fprintf(stderr, "sqlite commit: %s\n", sqlite3_errmsg(d->c));
  sqlite3_step(d->st_rollback);
  sqlite3_reset(d->st_rollback);
  return -1;
}

/* Called between batches. Opens the write gate when the log is copied, and runs the
 * last-resort inline checkpoint if the thread fell too far behind. */
static inline int db_writes_allowed(struct db *d) {
  if (d->gate_held) {
    uint64_t seq = atomic_load_explicit(&d->commit_seq, memory_order_acquire);
    if (atomic_load_explicit(&d->backfilled_seq, memory_order_acquire) == seq || now_ns() - d->gate_since > WAL_GATE_MAX_NS)
      d->gate_held = 0;
  }
  if (atomic_load_explicit(&d->wal_frames, memory_order_relaxed) >= WAL_INLINE_FRAMES && !d->in_txn) {
    int nlog = 0, nckpt = 0;
    sqlite3_wal_checkpoint_v2(d->c, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &nckpt);
    d->n_inline++;
    if (nlog == nckpt) {
      atomic_store_explicit(&d->wal_frames, 0, memory_order_relaxed);
      d->gate_held = 0;
    }
  }
  return !d->gate_held;
}

/* ---- the queries ---- */

static inline int db_health(struct db *d) {
  int rc = sqlite3_step(d->st_health);
  sqlite3_reset(d->st_health);
  return rc == SQLITE_ROW ? 0 : -1;
}

static inline void ob_post_row(struct ob *o, sqlite3_stmt *st, int64_t likes) {
  ob_post_obj(o, sqlite3_column_int64(st, 0), sqlite3_column_text(st, 1), sqlite3_column_bytes(st, 1),
              sqlite3_column_text(st, 2), sqlite3_column_bytes(st, 2), sqlite3_column_text(st, 3),
              sqlite3_column_bytes(st, 3), likes);
}

static int db_feed_ref(struct db *d, struct ob *o) {
  ob_lit(o, "{\"posts\":[");
  int n = 0, rc;
  while ((rc = sqlite3_step(d->st_feed_ref)) == SQLITE_ROW) {
    if (n++) ob_char(o, ',');
    ob_post_row(o, d->st_feed_ref, sqlite3_column_int64(d->st_feed_ref, 4));
  }
  sqlite3_reset(d->st_feed_ref);
  if (rc != SQLITE_DONE) return -1;
  ob_lit(o, "]}");
  return 0;
}

/* The feed: the 20 newest ids off the (created_at, id) index, then, when they lie within
 * FEED_SPAN ids of each other (they do: ids and timestamps both grow), one rowid range scan of
 * posts joined to users and one range scan of likes_post_id_idx for all 20 counts. Rows come
 * back in id order and are put back into feed order here. Anything unusual takes the
 * reference query. Everything is read inside the batch's transaction. */
static int db_feed(struct db *d, struct ob *o) {
  if (db_begin(d) < 0) return -1;
  if (d->bt.open) {
    size_t mark = o->n;
    if (bt_feed(&d->bt, o) == 0 && !o->err) return 0;
    o->n = mark;
    o->err = 0;
    d->n_fallback++;
  }
  int64_t ids[FEED_N];
  int n = 0, rc;
  while ((rc = sqlite3_step(d->st_feed_ids)) == SQLITE_ROW && n < FEED_N) ids[n++] = sqlite3_column_int64(d->st_feed_ids, 0);
  sqlite3_reset(d->st_feed_ids);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
  if (n == 0) { ob_lit(o, "{\"posts\":[]}"); return 0; }
  int64_t lo = ids[0], hi = ids[0];
  for (int i = 1; i < n; i++) {
    if (ids[i] < lo) lo = ids[i];
    if (ids[i] > hi) hi = ids[i];
  }
  if (hi - lo >= FEED_SPAN) { d->n_fallback++; return db_feed_ref(d, o); }

  int64_t cnt[FEED_SPAN];
  memset(cnt, 0, sizeof cnt);
  sqlite3_bind_int64(d->st_feed_cnt, 1, lo);
  sqlite3_bind_int64(d->st_feed_cnt, 2, hi);
  while ((rc = sqlite3_step(d->st_feed_cnt)) == SQLITE_ROW)
    cnt[sqlite3_column_int64(d->st_feed_cnt, 0) - lo] = sqlite3_column_int64(d->st_feed_cnt, 1);
  sqlite3_reset(d->st_feed_cnt);
  if (rc != SQLITE_DONE) return -1;

  /* position of each wanted id, and each post's JSON built once into scratch */
  int8_t pos[FEED_SPAN];
  memset(pos, -1, sizeof pos);
  for (int i = 0; i < n; i++) pos[ids[i] - lo] = (int8_t)i;
  static uint8_t scratch[FEED_N][4096];
  unsigned frag_len[FEED_N];
  memset(frag_len, 0, sizeof frag_len);
  int found = 0;
  sqlite3_bind_int64(d->st_feed_rows, 1, lo);
  sqlite3_bind_int64(d->st_feed_rows, 2, hi);
  while ((rc = sqlite3_step(d->st_feed_rows)) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(d->st_feed_rows, 0);
    int p = pos[id - lo];
    if (p < 0) continue;
    struct ob f;
    ob_init(&f, scratch[p], sizeof scratch[p]);
    ob_post_row(&f, d->st_feed_rows, cnt[id - lo]);
    if (f.err) { sqlite3_reset(d->st_feed_rows); return -1; }
    frag_len[p] = (unsigned)f.n;
    found++;
  }
  sqlite3_reset(d->st_feed_rows);
  if (rc != SQLITE_DONE) return -1;
  if (found != n) { d->n_fallback++; return db_feed_ref(d, o); }

  ob_lit(o, "{\"posts\":[");
  for (int i = 0; i < n; i++) {
    if (i) ob_char(o, ',');
    ob_bytes(o, scratch[i], frag_len[i]);
  }
  ob_lit(o, "]}");
  return 0;
}

/* Returns 1 and writes {"post":…}, 0 when the post does not exist, -1 on error. */
static int db_post(struct db *d, struct ob *o, int64_t id) {
  if (db_begin(d) < 0) return -1;
  if (d->bt.open) {
    size_t mark = o->n;
    int rc = bt_post(&d->bt, o, id);
    if (rc >= 0 && !o->err) return rc;
    o->n = mark;
    o->err = 0;
    d->n_fallback++;
  }
  sqlite3_bind_int64(d->st_post, 1, id);
  int rc = sqlite3_step(d->st_post);
  int ret;
  if (rc == SQLITE_ROW) {
    ob_lit(o, "{\"post\":");
    ob_post_row(o, d->st_post, sqlite3_column_int64(d->st_post, 4));
    ob_char(o, '}');
    ret = 1;
  } else {
    ret = rc == SQLITE_DONE ? 0 : -1;
  }
  sqlite3_reset(d->st_post);
  return ret;
}

/* Inserts the post and writes {"post":…}. The id and created_at come from the row as stored. */
static int db_create(struct db *d, struct ob *o, int64_t user_id, const uint8_t *body, unsigned body_n,
                     const uint8_t *author, unsigned author_n) {
  if (db_begin(d) < 0) return -1;
  sqlite3_bind_int64(d->st_ins_post, 1, user_id);
  sqlite3_bind_text(d->st_ins_post, 2, (const char *)body, (int)body_n, SQLITE_STATIC);
  int rc = sqlite3_step(d->st_ins_post);
  sqlite3_reset(d->st_ins_post);
  if (rc != SQLITE_DONE) return -1;
  d->wrote = 1;
  int64_t id = sqlite3_last_insert_rowid(d->c);
  sqlite3_bind_int64(d->st_created_at, 1, id);
  rc = sqlite3_step(d->st_created_at);
  if (rc != SQLITE_ROW) { sqlite3_reset(d->st_created_at); return -1; }
  ob_lit(o, "{\"post\":");
  ob_post_obj(o, id, body, (int)body_n, sqlite3_column_text(d->st_created_at, 0), sqlite3_column_bytes(d->st_created_at, 0),
              author, (int)author_n, 0);
  ob_char(o, '}');
  sqlite3_reset(d->st_created_at);
  return 0;
}

/* Returns 1 = liked now (201), 2 = already liked (200), 0 = no such post (404), -1 = error. */
static int db_like(struct db *d, int64_t user_id, int64_t post_id) {
  if (db_begin(d) < 0) return -1;
  sqlite3_bind_int64(d->st_ins_like, 1, user_id);
  sqlite3_bind_int64(d->st_ins_like, 2, post_id);
  int rc = sqlite3_step(d->st_ins_like);
  sqlite3_reset(d->st_ins_like);
  if (rc != SQLITE_DONE) return -1;
  if (sqlite3_changes(d->c) == 1) { d->wrote = 1; return 1; }
  if (d->bt.open) {
    rc = bt_post_exists(&d->bt, post_id);
    if (rc >= 0) return rc == 0 ? 2 : 0;
  }
  sqlite3_bind_int64(d->st_post_exists, 1, post_id);
  rc = sqlite3_step(d->st_post_exists);
  sqlite3_reset(d->st_post_exists);
  if (rc == SQLITE_ROW) return 2;
  return rc == SQLITE_DONE ? 0 : -1;
}

#endif
