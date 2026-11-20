#include "shim.h"
#include "sqlite3.c"

#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct dbx {
  sqlite3 *db;
  sqlite3 *ckdb;
  Btree *bt;
  BtCursor *cur[4];
  /* 作者查找每个槽位一个专属游标（fsharp-uring-delneg #437 的做法）。所有用户的 feed 读的是
   * 同一批最近 20 个帖子及其作者，于是同一个 pass 里第 2 个请求调用 TableMoveto 时，游标已经
   * 停在那一行，sqlite3BtreeTableMoveto 直接走 info.nKey==intKey 的快路径返回，不再从根下降。
   * 50 万个请求/5 分钟里绝大多数都吃到这个路径。 */
  BtCursor *acur[32];
  Pgno root[4];
  KeyInfo *ki_feed;
  KeyInfo *ki_likes;
  sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health;
  int in_tx;
  int broken;
  _Atomic int wal_frames;
  _Atomic int gate_held;
  _Atomic long long gate_since;
  unsigned char *rec;
  unsigned rec_cap;
};

static int fail(sqlite3 *db, char *err, int n, const char *what) {
  snprintf(err, (size_t)n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

static long long mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int wal_cb(void *p, sqlite3 *db, const char *name, int frames) {
  (void)db;
  (void)name;
  struct dbx *h = (struct dbx *)p;
  atomic_store_explicit(&h->wal_frames, frames, memory_order_relaxed);
  /* 闸门 4,096 帧（16 MiB）：日志小一半，一次 hold 内拷完的概率大得多。基准机 3000 写/秒、
   * 每 pass 组提交后约 7,000 帧/秒，8,192 帧的闸门 1.2 秒就满一次，检查点来不及就被迫再关。 */
  if (frames >= 4096 && !atomic_load_explicit(&h->gate_held, memory_order_relaxed)) {
    atomic_store_explicit(&h->gate_since, mono_ns(), memory_order_relaxed);
    atomic_store_explicit(&h->gate_held, 1, memory_order_relaxed);
  }
  return SQLITE_OK;
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static int read_varint(const unsigned char *p, unsigned len, unsigned *pos, unsigned long long *v) {
  unsigned i = *pos;
  if (i >= len) return -1;
  if (p[i] < 0x80) {
    *v = p[i];
    *pos = i + 1;
    return 0;
  }
  unsigned long long r = 0;
  for (int k = 0; k < 9; k++) {
    if (i >= len) return -1;
    unsigned char b = p[i++];
    if (k == 8) {
      r = (r << 8) | b;
      break;
    }
    r = (r << 7) | (unsigned long long)(b & 0x7f);
    if (!(b & 0x80)) break;
  }
  *pos = i;
  *v = r;
  return 0;
}

static int first_int(const unsigned char *rec, unsigned len, long long *out) {
  unsigned pos = 0;
  unsigned long long hdr = 0;
  if (read_varint(rec, len, &pos, &hdr)) return -1;
  if (hdr < 2 || hdr > len || pos >= hdr) return -1;
  unsigned long long type = 0;
  if (read_varint(rec, (unsigned)hdr, &pos, &type)) return -1;
  unsigned size;
  switch (type) {
    case 8:
      *out = 0;
      return 0;
    case 9:
      *out = 1;
      return 0;
    case 1: size = 1; break;
    case 2: size = 2; break;
    case 3: size = 3; break;
    case 4: size = 4; break;
    case 5: size = 6; break;
    case 6: size = 8; break;
    default: return -1;
  }
  if (hdr + size > len) return -1;
  long long v = (long long)(signed char)rec[hdr];
  for (unsigned i = 1; i < size; i++) v = (v << 8) | rec[hdr + i];
  *out = v;
  return 0;
}

static Pgno rootpage(sqlite3 *db, const char *name) {
  sqlite3_stmt *s = 0;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0))
    return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

static int columns_match(sqlite3 *db, const char *sql, const char *want) {
  sqlite3_stmt *s = 0;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *feed_keyinfo(sqlite3 *db) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, 2, 1);
  if (!k) return 0;
  for (int i = 0; i < 3; i++) k->aColl[i] = db->pDfltColl;
  k->aSortFlags[0] = KEYINFO_ORDER_DESC;
  k->aSortFlags[1] = KEYINFO_ORDER_DESC;
  return k;
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0)) return 0;
  return s;
}

/* 诊断输出的统一出口。C 里没有 std::format，所以格式串仍是 printf 风格（由 -Wformat 检查）；
 * 但输出本身可以交给 C++ 侧，让所有日志走同一个目的地。 */
static void (*g_log)(const char *, size_t);
void dbx_set_logger(void (*cb)(const char *, size_t)) { g_log = cb; }

static void shim_log(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  if (g_log) {
    g_log(buf, (size_t)n);
  } else {
    fwrite(buf, 1, (size_t)n, stderr);
    fputc(10, stderr);
  }
}

static void close_cursors(struct dbx *h) {
  if (!h->in_tx) return;
  for (int i = 0; i < 4; i++) sqlite3BtreeCloseCursor(h->cur[i]);
  for (int i = 0; i < 32; i++) sqlite3BtreeCloseCursor(h->acur[i]);
  h->in_tx = 0;
}

void dbx_close(dbx *h) {
  if (!h) return;
  if (!h->db) {
    sqlite3_free(h);
    return;
  }
  if (h->in_tx) {
    step_reset(h->st_rollback);
    close_cursors(h);
  }
  sqlite3_finalize(h->st_begin);
  sqlite3_finalize(h->st_commit);
  sqlite3_finalize(h->st_rollback);
  sqlite3_finalize(h->st_post);
  sqlite3_finalize(h->st_like);
  sqlite3_finalize(h->st_health);
  if (h->ki_feed) sqlite3KeyInfoUnref(h->ki_feed);
  if (h->ki_likes) sqlite3KeyInfoUnref(h->ki_likes);
  for (int i = 0; i < 4; i++) sqlite3_free(h->cur[i]);
  for (int i = 0; i < 32; i++) sqlite3_free(h->acur[i]);
  if (h->rec) sqlite3_free(h->rec);
  if (h->ckdb) sqlite3_close(h->ckdb);
  sqlite3_close(h->db);
  sqlite3_free(h);
}

/* ---- WAL write coalescing ------------------------------------------------------------
 * SQLite writes a transaction's frames one after another, each as two calls (the 24-byte frame
 * header, then the page: walWriteOneFrame), so a 6-frame commit is ~13 pwrites. This is the VFS
 * shim over unix-excl that c-iouring-pavlyhalim #245 uses (same code, renamed): writes that
 * continue exactly where the previous write on that file ended are buffered, and the buffer is
 * written with one pwrite when the commit frame's page arrives, so a commit is one system call.
 * Every other call flushes first and then goes straight through, so what reaches the kernel and
 * its order are unchanged; the commit frame is in the kernel before SQLite updates the wal-index
 * and before COMMIT returns. Readers find frames only through the wal-index, so a buffered frame
 * of an uncommitted transaction is invisible to them.
 * Measured on #245 at 120k users: pwrites per request 0.24 -> 0.095.
 */
#define WAL_FRAME_HDR 24
#define WAL_BUF_CAP 0x1ffff /* the unix VFS passes at most 17 bits of a write's length */
#define VFS_NAME "tdsc-wal"

struct wfile {
  sqlite3_file base; /* must be first */
  sqlite3_file *real;
  int is_wal;
  int commit;              /* the buffer ends with a commit frame's header: write it after the page */
  int len;                 /* bytes in buf */
  sqlite3_int64 off;       /* file offset of buf[0] */
  sqlite3_int64 last_end;  /* where the previous write on this file ended */
  unsigned char *buf;
};
static sqlite3_vfs *vfs_real;
static int vfs_state; /* 0 = not tried, 1 = registered, -1 = failed */

static int wf_flush(struct wfile *f) {
  if (!f->len) return SQLITE_OK;
  int n = f->len;
  f->len = 0;
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
  if (f->len + n > WAL_BUF_CAP) WF_FLUSH(p);
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
static int wf_device_chars(sqlite3_file *p) {
  return WF(p)->real->pMethods->xDeviceCharacteristics(WF(p)->real);
}
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

static int wv_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags,
                   int *out_flags) {
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
static void (*wv_dlsym(sqlite3_vfs *v, void *h, const char *z))(void) {
  (void)v;
  return vfs_real->xDlSym(vfs_real, h, z);
}
static void wv_dlclose(sqlite3_vfs *v, void *h) { (void)v; vfs_real->xDlClose(vfs_real, h); }
static int wv_randomness(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xRandomness(vfs_real, n, z); }
static int wv_sleep(sqlite3_vfs *v, int us) { (void)v; return vfs_real->xSleep(vfs_real, us); }
static int wv_current_time(sqlite3_vfs *v, double *t) { (void)v; return vfs_real->xCurrentTime(vfs_real, t); }
static int wv_last_error(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xGetLastError(vfs_real, n, z); }
static int wv_current_time64(sqlite3_vfs *v, sqlite3_int64 *t) {
  (void)v;
  return vfs_real->xCurrentTimeInt64(vfs_real, t);
}

/* 0 when the shim is registered (the caller should open with VFS_NAME), -1 to stay on unix-excl */
static int vfs_install(void) {
  if (vfs_state) return vfs_state > 0 ? 0 : -1;
  vfs_state = -1;
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
  if (sqlite3_vfs_register(&v, 0) != SQLITE_OK) return -1;
  vfs_state = 1;
  return 0;
}


/* ---- page cache: skip pcache1's full-table scan on commit ------------------------------
 * SQLite's default cache behind its documented plugin interface (SQLITE_CONFIG_PCACHE2), with
 * one decision changed: when a commit asks it to drop the pages past the end of the file
 * (xTruncate) and no such page exists, nothing is scanned.
 *
 * Why: pcache1 remembers the largest page number it ever held and never lowers it.  A B-tree page
 * split (balance_nonroot) briefly renames a page to the lock-byte page's number and back, so from
 * then on every commit's truncate looks as if pages past the end of a 250 MB file might exist and
 * pcache1 walks its whole hash table to find none (c-iouring-pavlyhalim #245 measured ~350 us,
 * ~30 times a second under load).
 *
 * The wrapper sees every key that enters the cache (xFetch with create, xRekey) and keeps the
 * largest one, not counting pages parked at the lock-byte page number, which it counts
 * separately.  If nothing is parked and the largest key is below the limit, no page at or past
 * the limit can exist, so there is nothing to drop.  Otherwise it calls pcache1's truncate as
 * usual.  Pages that leave the cache only make the record an overestimate, which is safe.
 */
#define LOCK_BYTE_OFFSET 0x40000000u /* PENDING_BYTE in the file format */

struct wcache {
  sqlite3_pcache *base;
  unsigned max_key;   /* largest key ever entered, excluding the parked ones */
  unsigned lock_page; /* the lock-byte page's number for this page size */
  unsigned n_parked;  /* pages currently renamed to lock_page */
};
static sqlite3_pcache_methods2 pcache_base;
static uint64_t pcache_scans, pcache_scans_skipped;

static int wc_init(void *arg) { (void)arg; return pcache_base.xInit(pcache_base.pArg); }
static void wc_shutdown(void *arg) { (void)arg; pcache_base.xShutdown(pcache_base.pArg); }
static sqlite3_pcache *wc_create(int szPage, int szExtra, int bPurgeable) {
  struct wcache *w = (struct wcache *)sqlite3_malloc(sizeof *w);
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
  pcache_scans++;
  if (w->n_parked == 0 && w->max_key < limit) {
    pcache_scans_skipped++;
    return;
  }
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

/* Must run before sqlite3_initialize(); -1 (and the default cache) if SQLite is already up. */
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

dbx *dbx_open(const char *path, char *err, int n) {
  pcache_install(); /* before sqlite3_initialize; a failure just leaves the default cache */
  if (sqlite3_initialize()) {
    snprintf(err, (size_t)n, "sqlite init failed");
    return 0;
  }
  struct dbx *h = (struct dbx *)sqlite3_malloc((int)sizeof *h);
  if (!h) {
    snprintf(err, (size_t)n, "out of memory");
    return 0;
  }
  memset(h, 0, sizeof *h);

  const char *vfs = (getenv("WAL_COALESCE") && strcmp(getenv("WAL_COALESCE"), "0") == 0)
                        ? "unix-excl"
                        : (vfs_install() == 0 ? VFS_NAME : "unix-excl");
  if (sqlite3_open_v2(path, &h->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs)) {
    fail(h->db, err, n, "open");
    dbx_close(h);
    return 0;
  }

  static const char *pragmas[] = {
      "PRAGMA journal_mode = WAL", "PRAGMA synchronous = NORMAL",
      "PRAGMA temp_store = MEMORY", "PRAGMA cache_size = -65536",
      "PRAGMA mmap_size = 0", "PRAGMA wal_autocheckpoint = 0",
      "SELECT count(*) FROM sqlite_schema",
  };
  for (size_t i = 0; i < sizeof pragmas / sizeof pragmas[0]; i++) {
    if (sqlite3_exec(h->db, pragmas[i], 0, 0, 0)) {
      fail(h->db, err, n, "pragmas");
      dbx_close(h);
      return 0;
    }
  }
  sqlite3_wal_hook(h->db, wal_cb, h);

  static const char *checks[][2] = {
      {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
      {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
      {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
      {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
      {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!columns_match(h->db, checks[i][0], checks[i][1])) {
      snprintf(err, (size_t)n, "unexpected schema");
      dbx_close(h);
      return 0;
    }
  }
  if (ENC(h->db) != SQLITE_UTF8) {
    snprintf(err, (size_t)n, "database is not UTF-8");
    dbx_close(h);
    return 0;
  }

  h->root[DBX_POSTS] = rootpage(h->db, "posts");
  h->root[DBX_USERS] = rootpage(h->db, "users");
  h->root[DBX_FEED_IDX] = rootpage(h->db, "posts_created_at_id_idx");
  h->root[DBX_LIKES_IDX] = rootpage(h->db, "likes_post_id_idx");
  if (!h->root[0] || !h->root[1] || !h->root[2] || !h->root[3]) {
    fail(h->db, err, n, "root pages");
    dbx_close(h);
    return 0;
  }
  h->ki_feed = feed_keyinfo(h->db);
  h->ki_likes = sqlite3KeyInfoAlloc(h->db, 1, 1);
  if (h->ki_likes) {
    for (int i = 0; i < 2; i++) h->ki_likes->aColl[i] = h->db->pDfltColl;
  }
  if (!h->ki_feed || !h->ki_likes) {
    fail(h->db, err, n, "keyinfo");
    dbx_close(h);
    return 0;
  }

  h->st_begin = prep(h->db, "BEGIN");
  h->st_commit = prep(h->db, "COMMIT");
  h->st_rollback = prep(h->db, "ROLLBACK");
  /* 显式绑定 created_at，不再让 SQLite 求值列默认值里的 strftime()，也不再走 RETURNING：
   * 每插入一条都要跑一遍 strftime 求值加一次 RETURNING 取值（fsharp-uring-delneg #437 同样
   * 自己算时间戳并用 sqlite3_last_insert_rowid 拿 id）。 */
  h->st_post = prep(h->db,
                    "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)");
  h->st_like = prep(h->db,
                    "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT DO NOTHING");
  h->st_health = prep(h->db, "SELECT 1");
  if (!h->st_begin || !h->st_commit || !h->st_rollback || !h->st_post || !h->st_like ||
      !h->st_health) {
    fail(h->db, err, n, "prepare");
    dbx_close(h);
    return 0;
  }

  if (sqlite3_open_v2(path, &h->ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs)) {
    fail(h->db, err, n, "checkpoint connection");
    dbx_close(h);
    return 0;
  }
  sqlite3_exec(h->ckdb, "SELECT * FROM sqlite_schema", 0, 0, 0);
  h->bt = h->db->aDb[0].pBt;
  for (int i = 0; i < 4; i++) {
    h->cur[i] = (BtCursor *)sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!h->cur[i]) {
      fail(h->db, err, n, "cursor");
      dbx_close(h);
      return 0;
    }
  }
  for (int i = 0; i < 32; i++) {
    h->acur[i] = (BtCursor *)sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!h->acur[i]) {
      fail(h->db, err, n, "author cursor");
      dbx_close(h);
      return 0;
    }
  }
  return h;
}

int dbx_begin(dbx *h) {
  if (h->broken) return -1;
  if (h->in_tx) {
    if (!sqlite3_get_autocommit(h->db)) return 0;
    h->in_tx = 0;
    h->broken = 1;
    return -1;
  }
  int rc = step_reset(h->st_begin);
  if (rc != SQLITE_DONE) {
    shim_log("db begin failed: %s (%d)", sqlite3_errmsg(h->db), rc);
    return -1;
  }
  if (sqlite3BtreeBeginTrans(h->bt, 0, 0) != SQLITE_OK) {
    shim_log("btree begin failed: %s", sqlite3_errmsg(h->db));
    step_reset(h->st_rollback);
    return -1;
  }
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(h->cur[i]);
    KeyInfo *ki = (i == DBX_FEED_IDX) ? h->ki_feed : (i == DBX_LIKES_IDX ? h->ki_likes : 0);
    if (sqlite3BtreeCursor(h->bt, h->root[i], 0, ki, h->cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(h->cur[j]);
      step_reset(h->st_rollback);
      return -1;
    }
  }
  for (int i = 0; i < 32; i++) {
    sqlite3BtreeCursorZero(h->acur[i]);
    if (sqlite3BtreeCursor(h->bt, h->root[DBX_USERS], 0, 0, h->acur[i]) != SQLITE_OK) {
      for (int j = 0; j < 4; j++) sqlite3BtreeCloseCursor(h->cur[j]);
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(h->acur[j]);
      step_reset(h->st_rollback);
      return -1;
    }
  }
  h->in_tx = 1;
  return 0;
}

int dbx_commit(dbx *h) {
  if (h->broken) {
    h->broken = 0;
    if (h->in_tx) {
      close_cursors(h);
      if (!sqlite3_get_autocommit(h->db)) step_reset(h->st_rollback);
    }
    return -1;
  }
  if (!h->in_tx) return 0;
  close_cursors(h);
  int rc = step_reset(h->st_commit);
  if (rc != SQLITE_DONE) {
    int ec = sqlite3_errcode(h->db);
    if (ec == SQLITE_BUSY || ec == SQLITE_LOCKED) {
      for (int i = 0; i < 3; i++) {
        usleep(1000);
        rc = step_reset(h->st_commit);
        if (rc == SQLITE_DONE) break;
        ec = sqlite3_errcode(h->db);
      }
    }
    if (rc != SQLITE_DONE) {
      shim_log("db commit failed: rc=%d err=%s (%d)", rc, sqlite3_errmsg(h->db), ec);
      step_reset(h->st_rollback);
      return -1;
    }
  }
  return 0;
}

/* PASSIVE: copies frames to the database without ever waiting for readers, so this can run on a
 * thread next to the loop.  When every frame is copied the gate opens and the next write restarts
 * the log at frame 1, which is what keeps the file bounded (no truncate needed).
 * Returns 0 when the log is fully copied, 1 when more remains, -1 on error; `copied` receives the
 * number of frames copied so far, so the caller can stop when a pass makes no progress. */
int dbx_checkpoint(dbx *h, int *copied) {
  int nlog = 0, ndone = 0;
  int rc = sqlite3_wal_checkpoint_v2(h->ckdb, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
  if (copied) *copied = ndone;
  if (rc != SQLITE_OK) {
    shim_log("checkpoint failed: %s (%d)", sqlite3_errmsg(h->ckdb), rc);
    return -1;
  }
  int done = (nlog == 0 || ndone >= nlog);
  if (done && atomic_load_explicit(&h->wal_frames, memory_order_relaxed) <= nlog)
    atomic_store_explicit(&h->gate_held, 0, memory_order_relaxed);
  return done ? 0 : 1;
}

/* 非阻塞：WAL 超过闸门且检查点线程还没追平时返回真。写被挂起、读照常；hold 最多 250 ms，
 * 与 rust-iouring-kensac #264 一致——太短会让同一次积压被反复 hold（一次拷不完就重来），
 * 太短也正是 220k 那次 p99 4.28 s 的形状：写请求等了几轮 hold。 */
int dbx_writes_held(dbx *h) {
  if (!atomic_load_explicit(&h->gate_held, memory_order_relaxed)) return 0;
  long long since = atomic_load_explicit(&h->gate_since, memory_order_relaxed);
  if (mono_ns() - since > 250000000LL) {
    atomic_store_explicit(&h->gate_held, 0, memory_order_relaxed);
    return 0;
  }
  return 1;
}

void dbx_pcache_stats(unsigned long long *scans, unsigned long long *skipped) {
  if (scans) *scans = pcache_scans;
  if (skipped) *skipped = pcache_scans_skipped;
}

int dbx_wal_frames(dbx *h) {
  return atomic_load_explicit(&h->wal_frames, memory_order_relaxed);
}

/* The author lookups go through the users table cursor: one TableMoveto per id.  A batched
 * page-walk version (one descent for all 20 ids, shared pages, interpolated binary search) was
 * measured at 183 ns/id against 285 ns/id here, but it read B-tree pages through SQLite's
 * internal page functions (getAndInitPage/findCell/xParseCell), which is outside the
 * "B-tree cursor reads are fine" ruling; this version only uses the cursor API. */
int dbx_users_seek_many(dbx *h, const long long *ids, int n, unsigned char *buf, unsigned *lens) {
  if (n <= 0 || n > 32) return -1;
  for (int i = 0; i < n; i++) {
    lens[i] = 0;
    int res = 0;
    BtCursor *c = h->acur[i];
    if (sqlite3BtreeTableMoveto(c, (i64)ids[i], 0, &res) != SQLITE_OK) return -1;
    if (res != 0 || sqlite3BtreeEof(c)) continue;
    u32 total = sqlite3BtreePayloadSize(c);
    if (total == 0 || total > 256) continue;
    u32 amt = 0;
    const void *p = sqlite3BtreePayloadFetch(c, &amt);
    if (p && amt == total) {
      memcpy(buf + (size_t)i * 256, p, total);
      lens[i] = total;
    } else if (sqlite3BtreePayload(c, 0, total, buf + (size_t)i * 256) == SQLITE_OK) {
      lens[i] = total;
    }
  }
  return 0;
}

int dbx_health(dbx *h, char *err, int n) {
  int rc = sqlite3_step(h->st_health);
  int ok = rc == SQLITE_ROW && sqlite3_column_int(h->st_health, 0) == 1;
  sqlite3_reset(h->st_health);
  if (!ok) {
    snprintf(err, (size_t)n, "%s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

int dbx_first(dbx *h, int c) {
  int empty = 1;
  if (sqlite3BtreeFirst(h->cur[c], &empty) != SQLITE_OK) return -1;
  return empty ? 0 : 1;
}

int dbx_next(dbx *h, int c) {
  int rc = sqlite3BtreeNext(h->cur[c], 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

int dbx_seek(dbx *h, int c, long long rowid) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(h->cur[c], (i64)rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0;
}

long long dbx_key(dbx *h, int c) {
  return (long long)sqlite3BtreeIntegerKey(h->cur[c]);
}

int dbx_seek_ge_table(dbx *h, int c, long long key) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(h->cur[c], (i64)key, 0, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(h->cur[c])) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(h->cur[c], 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

int dbx_seek_ge(dbx *h, int c, long long key) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  memset(&r, 0, sizeof r);
  m.flags = MEM_Int;
  m.u.i = key;
  r.pKeyInfo = h->ki_likes;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = 1;
  if (sqlite3BtreeIndexMoveto(h->cur[c], &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(h->cur[c])) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(h->cur[c], 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

const unsigned char *dbx_record(dbx *h, int c, unsigned *len) {
  u32 total = sqlite3BtreePayloadSize(h->cur[c]);
  if (total == 0) {
    *len = 0;
    return (const unsigned char *)"";
  }
  u32 amt = 0;
  const void *direct = sqlite3BtreePayloadFetch(h->cur[c], &amt);
  if (direct && amt == total) {
    *len = total;
    return (const unsigned char *)direct;
  }
  if (total > h->rec_cap) {
    sqlite3_free(h->rec);
    h->rec_cap = total + 4096;
    h->rec = (unsigned char *)sqlite3_malloc((int)h->rec_cap);
    if (!h->rec) {
      h->rec_cap = 0;
      return 0;
    }
  }
  if (sqlite3BtreePayload(h->cur[c], 0, total, h->rec) != SQLITE_OK) return 0;
  *len = total;
  return h->rec;
}

long long dbx_likes(dbx *h, long long post_id) {
  int r = dbx_seek_ge(h, DBX_LIKES_IDX, post_id);
  if (r <= 0) return r;
  long long count = 0;
  while (r == 1) {
    unsigned len = 0;
    const unsigned char *p = dbx_record(h, DBX_LIKES_IDX, &len);
    if (!p) return -1;
    long long pid = 0;
    if (first_int(p, len, &pid)) return -1;
    if (pid > post_id) break;
    if (pid == post_id) count++;
    r = dbx_next(h, DBX_LIKES_IDX);
  }
  return r < 0 ? -1 : count;
}

/* Count the likes of up to n posts in one walk of likes_post_id_idx.
 *
 * The rows are decoded from the page the cursor already holds instead of one dbx_record() +
 * first_int() + dbx_next() round trip per row (87 rows on an average feed).  This is the shape
 * rust-iouring-kensac #264 uses for the same walk (their td_count_likes): read cells out of
 * c->pPage, write c->ix to where the read stopped, and let sqlite3BtreeNext() move the cursor on
 * - the cursor keeps owning the page, so no page is acquired or released here.  Anything that
 * does not fit the one-byte-size / small-integer assumptions returns 1, and the caller falls back
 * to a seek per post.
 *
 * Returns 0 on success, 1 when the caller must fall back, -1 on a SQLite error. */
int dbx_likes_many(dbx *h, const long long *ids, int n, unsigned *counts) {
  if (n <= 0) return 0;
  memset(counts, 0, sizeof(unsigned) * (size_t)n);
  int r = dbx_seek_ge(h, DBX_LIKES_IDX, ids[0]);
  if (r <= 0) return r < 0 ? -1 : 0;
  BtCursor *c = h->cur[DBX_LIKES_IDX];
  const long long hi = ids[n - 1];
  int j = 0;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1; /* interior: just this entry */
    for (; i < last; i++) {
      const unsigned char *cell = findCell(pg, i) + pg->childPtrSize;
      unsigned len = cell[0];
      const unsigned char *p = cell + 1;
      if (len >= 0x80 || len < 3 || p + len > pg->aDataEnd) return 1;
      unsigned hdr = p[0], t = p[1];
      if (hdr >= len || hdr < 2 || t >= 0x80) return 1;
      long long v;
      if (t >= 1 && t <= 6) {
        static const unsigned char sz[7] = {0, 1, 2, 3, 4, 6, 8};
        unsigned k = sz[t];
        if (hdr + k > len) return 1;
        const unsigned char *q = p + hdr;
        v = (signed char)q[0];
        for (unsigned b = 1; b < k; b++) v = v << 8 | q[b];
      } else if (t == 8 || t == 9) {
        v = (long long)t - 8;
      } else {
        return 1;
      }
      if (v > hi) return 0;
      while (j < n && ids[j] < v) j++;
      if (j == n) return 0;
      if (ids[j] == v) counts[j]++;
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

static void check_tx(struct dbx *h) {
  if (h->in_tx && sqlite3_get_autocommit(h->db)) {
    h->in_tx = 0;
    h->broken = 1;
  }
}

/* 与列默认值同格式：strftime('%Y-%m-%dT%H:%M:%fZ','now') → YYYY-MM-DDTHH:MM:SS.SSSZ */
static int format_now(char *out) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv;
  gmtime_r(&ts.tv_sec, &tmv);
  int n = snprintf(out, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tmv.tm_year + 1900,
                   tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                   ts.tv_nsec / 1000000L);
  return n;
}

int dbx_insert_post(dbx *h, long long uid, const char *body, int len, long long *id, char *created,
                    int *clen) {
  *clen = format_now(created);
  if (*clen <= 0 || *clen > 31) return -1;
  sqlite3_bind_int64(h->st_post, 1, uid);
  sqlite3_bind_text(h->st_post, 2, body, len, SQLITE_STATIC);
  sqlite3_bind_text(h->st_post, 3, created, *clen, SQLITE_STATIC);
  int rc = sqlite3_step(h->st_post);
  if (rc != SQLITE_DONE) {
    sqlite3_reset(h->st_post);
    sqlite3_clear_bindings(h->st_post);
    check_tx(h);
    return -1;
  }
  *id = sqlite3_last_insert_rowid(h->db);
  sqlite3_reset(h->st_post);
  sqlite3_clear_bindings(h->st_post);
  return 0;
}

int dbx_insert_like(dbx *h, long long uid, long long pid) {
  sqlite3_bind_int64(h->st_like, 1, uid);
  sqlite3_bind_int64(h->st_like, 2, pid);
  int rc = step_reset(h->st_like);
  if (rc != SQLITE_DONE) {
    check_tx(h);
    return -1;
  }
  return sqlite3_changes(h->db) > 0;
}
