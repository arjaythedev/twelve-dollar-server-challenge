// The SQLite amalgamation (unmodified) plus read helpers that use SQLite's own B-tree cursor API for
// GET /feed and GET /posts/:id. Writes and every fallback stay on plain SQL. Any surprise returns -1 and
// the caller runs the SQL path instead.
#include "sqlite3.c"

#include "sqlite_bt.h"

static Btree *bt;
static Pgno root_posts, root_users, root_feed, root_likes;
static KeyInfo *ki_feed, *ki_likes;
static UnpackedRecord *key_likes;
static BtCursor *c_posts, *c_users, *c_feed, *c_likes;
static int open_now;
static i64 posts_at = -1;  // rowid under c_posts, or -1 if unknown

int bt_init(sqlite3 *db) {
  Table *tp = sqlite3FindTable(db, "posts", "main"), *tu = sqlite3FindTable(db, "users", "main");
  Index *ixf = sqlite3FindIndex(db, "posts_created_at_id_idx", "main"), *ixl = sqlite3FindIndex(db, "likes_post_id_idx", "main");
  if (!tp || !tu || !ixf || !ixl) return -1;
  root_posts = tp->tnum, root_users = tu->tnum, root_feed = ixf->tnum, root_likes = ixl->tnum;
  bt = db->aDb[0].pBt;
  ki_feed = sqlite3KeyInfoAlloc(db, 2, 1);
  ki_likes = sqlite3KeyInfoAlloc(db, 1, 1);
  if (!ki_feed || !ki_likes) return -1;
  ki_feed->aSortFlags[0] = ki_feed->aSortFlags[1] = KEYINFO_ORDER_DESC;
  key_likes = sqlite3VdbeAllocUnpackedRecord(ki_likes);
  if (!key_likes) return -1;
  // The allocator leaves aMem uninitialised; MemSetInt64 reads the flags, so they must start as MEM_Null.
  memset(key_likes->aMem, 0, sizeof(Mem) * key_likes->nField);
  for (int i = 0; i < key_likes->nField; i++) key_likes->aMem[i].flags = MEM_Null, key_likes->aMem[i].db = db;
  int sz = sqlite3BtreeCursorSize();
  c_posts = sqlite3MallocZero(sz), c_users = sqlite3MallocZero(sz), c_feed = sqlite3MallocZero(sz), c_likes = sqlite3MallocZero(sz);
  return key_likes && c_posts && c_users && c_feed && c_likes ? 0 : -1;
}

// Inside the caller's BEGIN ... COMMIT: opens the read transaction and the four cursors.
// COMMIT ends the read transaction (vdbeCommit commits read-only btree transactions too).
int bt_begin(void) {
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return -1;
  BtCursor *cs[4] = {c_posts, c_users, c_feed, c_likes};
  Pgno rs[4] = {root_posts, root_users, root_feed, root_likes};
  KeyInfo *ks[4] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(cs[i]);
    if (sqlite3BtreeCursor(bt, rs[i], 0, ks[i], cs[i]) != SQLITE_OK) {
      while (i--) sqlite3BtreeCloseCursor(cs[i]);
      return -1;
    }
  }
  open_now = 1;
  posts_at = -1;
  return 0;
}

void bt_end(void) {
  if (!open_now) return;
  sqlite3BtreeCloseCursor(c_posts), sqlite3BtreeCloseCursor(c_users);
  sqlite3BtreeCloseCursor(c_feed), sqlite3BtreeCloseCursor(c_likes);
  open_now = 0;
}

// ---------------------------------------------------------------- record format

typedef struct { u64 type; const u8 *p; u32 n; } col_t;

static u64 serial_len(u64 t) {
  static const u8 fixed[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? fixed[t] : (t - 12) / 2;
}

// Splits a record into its first `max` columns; returns the count or -1 if malformed.
static int rec_split(const u8 *p, u32 sz, col_t *c, int max) {
  if (sz == 0) return -1;
  u64 hdr;
  u32 h = sqlite3GetVarint(p, &hdr);
  if (hdr < h || hdr > sz) return -1;
  const u8 *hp = p + h, *he = p + hdr, *dp = p + hdr, *end = p + sz;
  int k = 0;
  while (hp < he && k < max) {
    if (he - hp < 9 && (hp[he - hp - 1] & 0x80)) return -1;  // a varint must end inside the header
    u64 t;
    hp += sqlite3GetVarint(hp, &t);
    if (hp > he) return -1;
    u64 len = serial_len(t);
    if (t == 10 || t == 11 || len > (u64)(end - dp)) return -1;
    c[k].type = t, c[k].p = dp, c[k].n = (u32)len;
    dp += len, k++;
  }
  return k;
}

static int col_int(const col_t *c, i64 *v) {
  if (c->type == 8) { *v = 0; return 0; }
  if (c->type == 9) { *v = 1; return 0; }
  if (c->type < 1 || c->type > 6) return -1;
  u64 x = 0;
  for (u32 i = 0; i < c->n; i++) x = (x << 8) | c->p[i];
  if (c->n < 8 && (c->p[0] & 0x80)) x |= ~(u64)0 << (8 * c->n);  // sign-extend
  *v = (i64)x;
  return 0;
}

static int col_text(const col_t *c) { return c->type >= 13 && (c->type & 1); }

// The current cell's full payload, local to the page. Returns NULL for overflow payloads (caller falls back).
static const u8 *cell(BtCursor *cur, u32 *sz) {
  u32 amt;
  *sz = sqlite3BtreePayloadSize(cur);  // first: it parses the cell info that PayloadFetch reads
  const u8 *p = sqlite3BtreePayloadFetch(cur, &amt);
  return amt >= *sz ? p : NULL;
}

// ---------------------------------------------------------------- reads

static char *put(char **w, char *end, const u8 *s, u32 n) {
  if (*w + n > end) return NULL;
  char *d = *w;
  memcpy(d, s, n);
  *w += n;
  return d;
}

// Loads post `id` and its author's username. 1 found, 0 no such post or no author row (the SPEC's inner
// join drops it), -1 fall back to SQL.
static int load_post(i64 id, bt_post *o, char **w, char *end) {
  int res;
  u32 sz;
  // Feed ids usually descend by one: a step back is cheaper than a seek from the root.
  if (posts_at == id + 1 && sqlite3BtreePrevious(c_posts, 0) == SQLITE_OK && sqlite3BtreeIntegerKey(c_posts) == id) {
    res = 0;
  } else {
    posts_at = -1;
    if (sqlite3BtreeTableMoveto(c_posts, id, 0, &res) != SQLITE_OK) return -1;
    if (res != 0 || sqlite3BtreeEof(c_posts)) return 0;
  }
  posts_at = id;
  const u8 *p = cell(c_posts, &sz);
  col_t c[4];
  // posts(id INTEGER PRIMARY KEY, user_id, body, created_at): id is the rowid, stored as NULL.
  if (!p || rec_split(p, sz, c, 4) != 4 || !col_text(&c[2]) || !col_text(&c[3])) return -1;
  i64 uid;
  if (col_int(&c[1], &uid)) return -1;
  o->id = id;
  if (!(o->body = put(w, end, c[2].p, c[2].n)) || !(o->ca = put(w, end, c[3].p, c[3].n))) return -1;
  o->bl = (int)c[2].n, o->cl = (int)c[3].n;
  if (sqlite3BtreeTableMoveto(c_users, uid, 0, &res) != SQLITE_OK) return -1;
  if (res != 0 || sqlite3BtreeEof(c_users)) return 0;
  const u8 *q = cell(c_users, &sz);
  col_t u[2];
  // users(id INTEGER PRIMARY KEY, username, created_at)
  if (!q || rec_split(q, sz, u, 2) != 2 || !col_text(&u[1])) return -1;
  if (!(o->au = put(w, end, u[1].p, u[1].n))) return -1;
  o->al = (int)u[1].n;
  return 1;
}

// Positions the likes index at the first entry with post_id >= v. 1 if positioned, 0 at end, -1 error.
static int likes_seek(i64 v) {
  sqlite3VdbeMemSetInt64(&key_likes->aMem[0], v);
  key_likes->nField = 1;
  key_likes->default_rc = 1;
  key_likes->eqSeen = 0;
  key_likes->errCode = 0;
  int res;
  if (sqlite3BtreeIndexMoveto(c_likes, key_likes, &res) != SQLITE_OK || key_likes->errCode) return -1;
  if (sqlite3BtreeEof(c_likes)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c_likes, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

static int likes_post_id(i64 *v) {
  u32 sz;
  const u8 *p = cell(c_likes, &sz);
  col_t c[1];
  return p && rec_split(p, sz, c, 1) == 1 ? col_int(&c[0], v) : -1;
}

// Adds the like count for every post in o[0..n) (ids within [lo, hi]), scanning [lo, hi] once.
static int count_range(bt_post *o, int n, i64 lo, i64 hi) {
  for (int k = 0; k < n; k++) o[k].likes = 0;
  int s = likes_seek(lo);
  if (s <= 0) return s;
  for (;;) {
    i64 pid;
    if (likes_post_id(&pid)) return -1;
    if (pid > hi) return 0;
    for (int k = 0; k < n; k++)
      if (o[k].id == pid) { o[k].likes++; break; }
    int rc = sqlite3BtreeNext(c_likes, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

static int count_one(bt_post *o) {
  return count_range(o, 1, o->id, o->id);
}

int bt_feed(bt_post *o, int *n, char *buf, size_t cap) {
  char *w = buf, *end = buf + cap;
  int res;
  *n = 0;
  if (sqlite3BtreeFirst(c_feed, &res) != SQLITE_OK) return -1;
  if (res) return 0;
  // The index is (created_at DESC, id DESC): its b-tree order is exactly the feed's order.
  while (*n < 20) {
    u32 sz;
    const u8 *p = cell(c_feed, &sz);
    col_t c[3];
    i64 id;
    int nc = p ? rec_split(p, sz, c, 3) : -1;
    // The entry ends with the rowid (posts.id), however many columns precede it.
    if (nc < 2 || col_int(&c[nc - 1], &id)) return -1;
    int k = load_post(id, &o[*n], &w, end);
    if (k < 0) return -1;
    *n += k;
    int rc = sqlite3BtreeNext(c_feed, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) return -1;
  }
  if (!*n) return 0;
  i64 lo = o[0].id, hi = o[0].id;
  for (int k = 1; k < *n; k++) lo = o[k].id < lo ? o[k].id : lo, hi = o[k].id > hi ? o[k].id : hi;
  if (hi - lo < 256) return count_range(o, *n, lo, hi);  // else the range may hold many other posts' likes
  for (int k = 0; k < *n; k++)
    if (count_one(&o[k])) return -1;
  return 0;
}

int bt_post_get(long long id, bt_post *o, char *buf, size_t cap) {
  char *w = buf;
  int k = load_post(id, o, &w, buf + cap);
  if (k <= 0) return k;
  return count_one(o) ? -1 : 1;
}

// ---------------------------------------------------------------- WAL write coalescing (custom VFS)
//
// "tds" wraps unix-excl. For WAL files only, consecutive appends are buffered and written with one pwrite
// when the commit frame's page arrives. That is still inside SQLite's commit, before the WAL index makes
// the frames visible to any reader, so no reader (including the checkpoint connection) can see a frame
// that is not in the file. Any other write, read, sync, truncate, size or close flushes first.

typedef struct {
  const sqlite3_io_methods *real;
  char *buf;
  sqlite3_int64 off;
  int len, cap, commit_hdr;
} walx;

#define WAL_WRITE_MAX (120 * 1024)

static sqlite3_vfs tds_vfs, *base_vfs;
static sqlite3_io_methods wal_io;

static walx *WX(sqlite3_file *f) { return (walx *)((char *)f + base_vfs->szOsFile); }

static int wflush(sqlite3_file *f) {
  walx *x = WX(f);
  if (!x->len) return SQLITE_OK;
  // unixWrite handles at most 128 KB per call (seekAndWriteFd masks nBuf with 0x1ffff), so write in pieces.
  int rc = SQLITE_OK;
  for (int done = 0; done < x->len && rc == SQLITE_OK;) {
    int n = x->len - done < WAL_WRITE_MAX ? x->len - done : WAL_WRITE_MAX;
    rc = x->real->xWrite(f, x->buf + done, n, x->off + done);
    done += n;
  }
  x->len = 0;
  return rc;
}

static int w_write(sqlite3_file *f, const void *z, int n, sqlite3_int64 off) {
  walx *x = WX(f);
  int append = x->len ? off == x->off + x->len : 1;
  if (!append || x->len + n > x->cap) {
    int rc = wflush(f);
    if (rc) return rc;
    if (!append || n > x->cap) return x->real->xWrite(f, z, n, off);  // not an append: write it directly
  }
  if (!x->len) x->off = off;
  memcpy(x->buf + x->len, z, n);
  x->len += n;
  if (n == WAL_FRAME_HDRSIZE) {
    const u8 *h = z;
    x->commit_hdr = (h[4] | h[5] | h[6] | h[7]) != 0;  // nTruncate != 0 marks the commit frame
  } else if (x->commit_hdr) {
    x->commit_hdr = 0;
    return wflush(f);
  }
  return SQLITE_OK;
}

static int w_read(sqlite3_file *f, void *z, int n, sqlite3_int64 off) {
  int rc = wflush(f);
  return rc ? rc : WX(f)->real->xRead(f, z, n, off);
}
static int w_sync(sqlite3_file *f, int fl) { int rc = wflush(f); return rc ? rc : WX(f)->real->xSync(f, fl); }
static int w_trunc(sqlite3_file *f, sqlite3_int64 s) { int rc = wflush(f); return rc ? rc : WX(f)->real->xTruncate(f, s); }
static int w_size(sqlite3_file *f, sqlite3_int64 *s) { int rc = wflush(f); return rc ? rc : WX(f)->real->xFileSize(f, s); }
static int w_close(sqlite3_file *f) {
  walx *x = WX(f);
  int rc = wflush(f);
  const sqlite3_io_methods *real = x->real;
  sqlite3_free(x->buf);
  x->buf = 0;
  f->pMethods = real;
  int rc2 = real->xClose(f);
  return rc ? rc : rc2;
}
static int w_lock(sqlite3_file *f, int l) { return WX(f)->real->xLock(f, l); }
static int w_unlock(sqlite3_file *f, int l) { return WX(f)->real->xUnlock(f, l); }
static int w_reserved(sqlite3_file *f, int *r) { return WX(f)->real->xCheckReservedLock(f, r); }
static int w_fcntl(sqlite3_file *f, int op, void *a) { return WX(f)->real->xFileControl(f, op, a); }
static int w_sector(sqlite3_file *f) { return WX(f)->real->xSectorSize(f); }
static int w_dev(sqlite3_file *f) { return WX(f)->real->xDeviceCharacteristics(f); }
static int w_shmmap(sqlite3_file *f, int i, int sz, int ext, void volatile **p) { return WX(f)->real->xShmMap(f, i, sz, ext, p); }
static int w_shmlock(sqlite3_file *f, int o, int n, int fl) { return WX(f)->real->xShmLock(f, o, n, fl); }
static void w_shmbarrier(sqlite3_file *f) { WX(f)->real->xShmBarrier(f); }
static int w_shmunmap(sqlite3_file *f, int del) { return WX(f)->real->xShmUnmap(f, del); }
static int w_fetch(sqlite3_file *f, sqlite3_int64 o, int n, void **p) { int rc = wflush(f); return rc ? rc : WX(f)->real->xFetch(f, o, n, p); }
static int w_unfetch(sqlite3_file *f, sqlite3_int64 o, void *p) { return WX(f)->real->xUnfetch(f, o, p); }

static int tds_open(sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *f, int flags, int *out) {
  (void)v;
  int rc = base_vfs->xOpen(base_vfs, name, f, flags, out);
  if (rc != SQLITE_OK || !(flags & SQLITE_OPEN_WAL) || !f->pMethods || f->pMethods->iVersion < 3) return rc;
  walx *x = WX(f);
  memset(x, 0, sizeof *x);
  x->cap = 4 << 20;
  if (!(x->buf = sqlite3_malloc(x->cap))) return SQLITE_OK;  // no buffer: leave the plain methods in place
  x->real = f->pMethods;
  f->pMethods = &wal_io;
  return SQLITE_OK;
}

int bt_vfs_register(void) {
  if (!(base_vfs = sqlite3_vfs_find("unix-excl"))) return -1;
  tds_vfs = *base_vfs;
  tds_vfs.zName = "tds";
  tds_vfs.szOsFile = base_vfs->szOsFile + (int)sizeof(walx);
  tds_vfs.xOpen = tds_open;
  tds_vfs.pNext = 0;
  wal_io = (sqlite3_io_methods){3, w_close, w_read, w_write, w_trunc, w_sync, w_size, w_lock, w_unlock, w_reserved, w_fcntl,
                                w_sector, w_dev, w_shmmap, w_shmlock, w_shmbarrier, w_shmunmap, w_fetch, w_unfetch};
  return sqlite3_vfs_register(&tds_vfs, 0);
}

// ---------------------------------------------------------------- page cache (SQLITE_CONFIG_PCACHE2 wrapper)
//
// balance_nonroot() briefly uses the pending-byte page number (about 262k with 4 KB pages) as a key. That
// raises pcache1's max key, and every later commit's truncate then walks the whole cache. This wrapper
// forwards everything to the default pcache1 but tracks the largest real key itself: a truncate that cannot
// drop anything is skipped, and the pending-byte page alone is dropped directly.

static sqlite3_pcache_methods2 pc_base;

typedef struct {
  sqlite3_pcache *inner;
  unsigned max_key, pend_key;
  int pend_in;
} pcw;

static int pc_init(void *a) { return pc_base.xInit ? pc_base.xInit(a) : SQLITE_OK; }
static void pc_shutdown(void *a) { if (pc_base.xShutdown) pc_base.xShutdown(a); }

static sqlite3_pcache *pc_create(int szPage, int szExtra, int purge) {
  pcw *w = sqlite3_malloc(sizeof *w);
  if (!w) return 0;
  if (!(w->inner = pc_base.xCreate(szPage, szExtra, purge))) { sqlite3_free(w); return 0; }
  w->max_key = 0, w->pend_in = 0;
  w->pend_key = (unsigned)(PENDING_BYTE / szPage) + 1;
  return (sqlite3_pcache *)w;
}
static void pc_cachesize(sqlite3_pcache *p, int n) { pc_base.xCachesize(((pcw *)p)->inner, n); }
static int pc_pagecount(sqlite3_pcache *p) { return pc_base.xPagecount(((pcw *)p)->inner); }
static void pc_note(pcw *w, unsigned key) {
  if (key == w->pend_key) w->pend_in = 1;
  else if (key > w->max_key) w->max_key = key;
}
static sqlite3_pcache_page *pc_fetch(sqlite3_pcache *p, unsigned key, int create) {
  pcw *w = (pcw *)p;
  sqlite3_pcache_page *pg = pc_base.xFetch(w->inner, key, create);
  if (pg) pc_note(w, key);
  return pg;
}
static void pc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard) { pc_base.xUnpin(((pcw *)p)->inner, pg, discard); }
static void pc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned o, unsigned n) {
  pcw *w = (pcw *)p;
  pc_base.xRekey(w->inner, pg, o, n);
  pc_note(w, n);
}
static void pc_truncate(sqlite3_pcache *p, unsigned limit) {
  pcw *w = (pcw *)p;
  if (w->pend_in && w->pend_key >= limit) {
    sqlite3_pcache_page *pg = pc_base.xFetch(w->inner, w->pend_key, 0);
    if (pg) pc_base.xUnpin(w->inner, pg, 1);
    w->pend_in = 0;
  }
  if (w->max_key >= limit) {
    pc_base.xTruncate(w->inner, limit);
    w->max_key = limit ? limit - 1 : 0;
  }
}
static void pc_destroy(sqlite3_pcache *p) { pc_base.xDestroy(((pcw *)p)->inner), sqlite3_free(p); }
static void pc_shrink(sqlite3_pcache *p) { pc_base.xShrink(((pcw *)p)->inner); }

int bt_pcache_install(void) {
  if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pc_base) != SQLITE_OK || !pc_base.xCreate) return -1;
  static sqlite3_pcache_methods2 m;
  m = (sqlite3_pcache_methods2){1, 0, pc_init, pc_shutdown, pc_create, pc_cachesize, pc_pagecount, pc_fetch,
                                pc_unpin, pc_rekey, pc_truncate, pc_destroy, pc_shrink};
  return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m);
}
