/* The SQLite side of the server: the unmodified amalgamation plus a thin layer over its B-tree, after
 * rust-iouring-kensac (#264) and c-iouring-pavlyhalim (#245). GET /feed and GET /posts/:id walk SQLite's own B-tree
 * cursors inside the pass's transaction, skipping the bytecode engine; C# decodes the record bytes SQLite hands back.
 * Writes, BEGIN, COMMIT and /health are ordinary SQL. Every page comes through SQLite's pager, WAL and page cache. */
#include "sqlite3.c"

typedef long long i64;

static sqlite3 *db;
static Btree *bt;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;
static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;

/* Cursors: 0 posts, 1 users, 2 posts_created_at_id_idx, 3 likes_post_id_idx. They open after the pass's read
 * transaction starts and close before it commits. */
#define NCUR 4
static BtCursor *cur[NCUR];
static int cur_open;

static u8 *spill; /* a record that runs onto overflow pages is copied here */
static u32 spill_cap;

/* ---- One write per commit, through SQLite's public VFS API, after c-iouring-pavlyhalim (#245) ----
 * SQLite writes a transaction's WAL frames one after another, each as two calls (the 24-byte frame header, then the page),
 * and the last frame of a commit carries the database size in its header. This shim over unix-excl keeps writes that continue
 * exactly where the previous one on that file ended in a buffer, and writes the buffer with one pwrite when the commit frame's
 * page arrives, inside SQLite's own call. So the commit is in the kernel before SQLite updates the WAL index and before COMMIT
 * returns (rule 6). Any other write (the WAL header after a restart, the checksum rewrite) writes the buffer first and then goes
 * straight through, as does every other call on the file, so the bytes and their order are SQLite's. Readers find frames only
 * through the WAL index, so none can see a buffered one. */
#define WAL_FRAME_HDR 24
#define WAL_BUF_CAP 0x1ffff /* the unix VFS passes at most 17 bits of a write's length */

typedef struct WalFile {
  sqlite3_file base; /* must be first */
  sqlite3_file *real;
  int is_wal;
  int commit;            /* the buffer ends with a commit frame's header: write it after the page */
  int len;               /* bytes in buf */
  sqlite3_int64 off;     /* file offset of buf[0] */
  sqlite3_int64 last_end; /* where the previous write on this file ended */
  unsigned char *buf;    /* WAL_BUF_CAP bytes, allocated with the first buffered write */
} WalFile;

#define WF(p) ((WalFile *)(p))
#define WF_REAL(p) (WF(p)->real)
#define WF_REAL_OFFSET ((sizeof(WalFile) + 7) & ~(size_t)7)

static sqlite3_vfs *vfs_real;

static int wf_flush(WalFile *f) {
  if (!f->len) return SQLITE_OK;
  int n = f->len;
  f->len = 0;
  return f->real->pMethods->xWrite(f->real, f->buf, n, f->off);
}
#define WF_FLUSH(p) do { int rc_ = wf_flush(WF(p)); if (rc_ != SQLITE_OK) return rc_; } while (0)

static int wf_close(sqlite3_file *p) {
  WalFile *f = WF(p);
  int rc = wf_flush(f);
  int rc2 = f->real->pMethods ? f->real->pMethods->xClose(f->real) : SQLITE_OK;
  sqlite3_free(f->buf);
  f->buf = 0;
  return rc != SQLITE_OK ? rc : rc2;
}
static int wf_read(sqlite3_file *p, void *b, int n, sqlite3_int64 off) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xRead(WF_REAL(p), b, n, off);
}
static int wf_write(sqlite3_file *p, const void *b, int n, sqlite3_int64 off) {
  WalFile *f = WF(p);
  if (!f->is_wal) return f->real->pMethods->xWrite(f->real, b, n, off);
  int append = off == f->last_end;
  f->last_end = off + n;
  if (f->len + n > WAL_BUF_CAP) WF_FLUSH(p);
  if (!append || n > WAL_BUF_CAP || (!f->buf && !(f->buf = sqlite3_malloc(WAL_BUF_CAP)))) {
    WF_FLUSH(p);
    /* A frame header can land here: the checkpoint connection starts the WAL again, so this connection's next frame
     * doesn't continue its own last write. It may be a commit frame's, so what it says still counts: without this its page
     * would stay in the buffer after COMMIT returned, and the WAL index would point at a frame that isn't in the file. */
    if (n == WAL_FRAME_HDR) {
      const unsigned char *h = b;
      f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    } else {
      f->commit = 0;
    }
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
  return WF_REAL(p)->pMethods->xTruncate(WF_REAL(p), size);
}
static int wf_sync(sqlite3_file *p, int flags) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xSync(WF_REAL(p), flags);
}
static int wf_filesize(sqlite3_file *p, sqlite3_int64 *size) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xFileSize(WF_REAL(p), size);
}
static int wf_lock(sqlite3_file *p, int l) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xLock(WF_REAL(p), l);
}
static int wf_unlock(sqlite3_file *p, int l) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xUnlock(WF_REAL(p), l);
}
static int wf_check_reserved(sqlite3_file *p, int *out) { return WF_REAL(p)->pMethods->xCheckReservedLock(WF_REAL(p), out); }
static int wf_file_control(sqlite3_file *p, int op, void *arg) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xFileControl(WF_REAL(p), op, arg);
}
static int wf_sector_size(sqlite3_file *p) { return WF_REAL(p)->pMethods->xSectorSize(WF_REAL(p)); }
static int wf_device_chars(sqlite3_file *p) { return WF_REAL(p)->pMethods->xDeviceCharacteristics(WF_REAL(p)); }
static int wf_shm_map(sqlite3_file *p, int pg, int sz, int extend, void volatile **out) {
  return WF_REAL(p)->pMethods->xShmMap(WF_REAL(p), pg, sz, extend, out);
}
static int wf_shm_lock(sqlite3_file *p, int off, int n, int flags) { return WF_REAL(p)->pMethods->xShmLock(WF_REAL(p), off, n, flags); }
static void wf_shm_barrier(sqlite3_file *p) { WF_REAL(p)->pMethods->xShmBarrier(WF_REAL(p)); }
static int wf_shm_unmap(sqlite3_file *p, int del) { return WF_REAL(p)->pMethods->xShmUnmap(WF_REAL(p), del); }
static int wf_fetch(sqlite3_file *p, sqlite3_int64 off, int n, void **pp) {
  WF_FLUSH(p);
  return WF_REAL(p)->pMethods->xFetch(WF_REAL(p), off, n, pp);
}
static int wf_unfetch(sqlite3_file *p, sqlite3_int64 off, void *pg) { return WF_REAL(p)->pMethods->xUnfetch(WF_REAL(p), off, pg); }

static const sqlite3_io_methods wf_methods = {
  3, wf_close, wf_read, wf_write, wf_truncate, wf_sync, wf_filesize, wf_lock, wf_unlock, wf_check_reserved, wf_file_control,
  wf_sector_size, wf_device_chars, wf_shm_map, wf_shm_lock, wf_shm_barrier, wf_shm_unmap, wf_fetch, wf_unfetch};

static int wv_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags, int *out_flags) {
  (void)vfs;
  WalFile *f = WF(p);
  memset(f, 0, sizeof *f);
  f->last_end = -1;
  f->real = (sqlite3_file *)((char *)p + WF_REAL_OFFSET);
  int rc = vfs_real->xOpen(vfs_real, name, f->real, flags, out_flags);
  if (f->real->pMethods == 0) return rc; /* the open failed: SQLite will not call xClose */
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  if (f->real->pMethods->iVersion < 3) { /* unix-excl is version 3; anything else is not expected */
    f->real->pMethods->xClose(f->real);
    return SQLITE_CANTOPEN;
  }
  p->pMethods = &wf_methods;
  return rc;
}
static int wv_delete(sqlite3_vfs *v, const char *z, int sync) { (void)v; return vfs_real->xDelete(vfs_real, z, sync); }
static int wv_access(sqlite3_vfs *v, const char *z, int flags, int *out) { (void)v; return vfs_real->xAccess(vfs_real, z, flags, out); }
static int wv_full_pathname(sqlite3_vfs *v, const char *z, int n, char *out) { (void)v; return vfs_real->xFullPathname(vfs_real, z, n, out); }
static int wv_randomness(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xRandomness(vfs_real, n, z); }
static int wv_sleep(sqlite3_vfs *v, int us) { (void)v; return vfs_real->xSleep(vfs_real, us); }
static int wv_current_time(sqlite3_vfs *v, double *t) { (void)v; return vfs_real->xCurrentTime(vfs_real, t); }
static int wv_last_error(sqlite3_vfs *v, int n, char *z) { (void)v; return vfs_real->xGetLastError(vfs_real, n, z); }
static int wv_current_time64(sqlite3_vfs *v, sqlite3_int64 *t) { (void)v; return vfs_real->xCurrentTimeInt64(vfs_real, t); }

/* Registers the VFS named "unix-excl-1write". 0 ok. Called after sqlite3_initialize, before the first open. */
int td_vfs_install(void) {
  static sqlite3_vfs v;
  vfs_real = sqlite3_vfs_find("unix-excl");
  if (!vfs_real || vfs_real->iVersion < 2) return -1;
  memset(&v, 0, sizeof v);
  v.iVersion = 2;
  v.szOsFile = (int)(WF_REAL_OFFSET + (size_t)vfs_real->szOsFile);
  v.mxPathname = vfs_real->mxPathname;
  v.zName = "unix-excl-1write";
  v.xOpen = wv_open;
  v.xDelete = wv_delete;
  v.xAccess = wv_access;
  v.xFullPathname = wv_full_pathname;
  v.xRandomness = wv_randomness;
  v.xSleep = wv_sleep;
  v.xCurrentTime = wv_current_time;
  v.xGetLastError = wv_last_error;
  v.xCurrentTimeInt64 = wv_current_time64;
  return sqlite3_vfs_register(&v, 0) == SQLITE_OK ? 0 : -1;
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

/* The record decoders assume this exact column order; the server refuses to start on anything else. */
static int layout(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *keyinfo(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) return 0;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0)) return 0;
  return s;
}

static int fail(char *err, int n, const char *what) {
  snprintf(err, n, "%s: %s", what, db ? sqlite3_errmsg(db) : "no database");
  return -1;
}

/* Sets up the B-tree layer on the connection C# opened and configured. 0 ok, -1 with the reason in err. */
int td_attach(sqlite3 *conn, char *err, int n) {
  db = conn;
  static const char *checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
  };
  if (sqlite3_exec(db, "SELECT count(*) FROM sqlite_schema", 0, 0, 0)) return fail(err, n, "schema");
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!layout(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  }
  if (ENC(db) != SQLITE_UTF8) {
    snprintf(err, n, "database text encoding is not UTF-8");
    return -1;
  }
  root_posts = rootpage("posts");
  root_users = rootpage("users");
  root_feed_idx = rootpage("posts_created_at_id_idx");
  root_likes_idx = rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed_idx || !root_likes_idx) return fail(err, n, "root pages");
  ki_feed = keyinfo(2, 1, 1);
  ki_likes = keyinfo(1, 1, 0);
  if (!ki_feed || !ki_likes) return fail(err, n, "keyinfo");
  st_begin = prep("BEGIN");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
  if (!st_begin || !st_commit || !st_rollback) return fail(err, n, "prepare");
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) return fail(err, n, "cursor alloc");
  }
  return 0;
}

static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}

/* The pass's transaction: reads share its snapshot, writes are group-committed. */
int td_begin(void) {
  if (step_reset(st_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) { step_reset(st_rollback); return -1; }
  static const Pgno *roots[NCUR] = {&root_posts, &root_users, &root_feed_idx, &root_likes_idx};
  KeyInfo *kis[NCUR] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, *roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      step_reset(st_rollback);
      return -1;
    }
  }
  cur_open = 1;
  return 0;
}

/* 0 once the pass is committed, -1 if it was rolled back (also when SQLite rolled it back by itself mid-pass). */
int td_commit(void) {
  close_cursors();
  if (sqlite3_get_autocommit(db)) return -1;
  if (step_reset(st_commit) != SQLITE_DONE) {
    step_reset(st_rollback);
    return -1;
  }
  return 0;
}

void *td_cursor(int i) { return cur[i]; }

/* Table cursor to rowid id: 1 found, 0 absent, -1 error. */
int td_seek(BtCursor *c, i64 id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

/* Index cursor to the first entry whose leading integer column is >= k: 1 on an entry, 0 none, -1 error. */
static int seek_ge(BtCursor *c, i64 k) {
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
  r.default_rc = +1; /* entries equal on the key sort after it: land on the first of them */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

/* Decodes the leading integer of an index record, generally. 0 ok, -1 not an integer. */
static int leading_int(BtCursor *c, i64 *v) {
  u32 total = sqlite3BtreePayloadSize(c), got = 0;
  const u8 *p = sqlite3BtreePayloadFetch(c, &got);
  u32 h = 0, t = 0, hn;
  if (got < 2 || total < 2) return -1;
  hn = getVarint32(p, h);
  hn += getVarint32(p + hn, t);
  if (h > got || hn > h) return -1;
  if (t == 8 || t == 9) { *v = t - 8; return 0; }
  if (t < 1 || t > 6) return -1;
  static const u8 sz[7] = {0, 1, 2, 3, 4, 6, 8};
  u32 k = sz[t];
  if (h + k > got) return -1;
  const u8 *q = p + h;
  i64 x = (i8)q[0];
  for (u32 j = 1; j < k; j++) x = x << 8 | q[j];
  *v = x;
  return 0;
}

/* Likes per post for lo <= post_id <= hi off likes_post_id_idx in one walk: n[post_id - lo] counts the entries (n has
 * hi - lo + 1 zeroed slots). The cells of a leaf are decoded here at once, not by one call per entry. 0 ok, -1 error. */
int td_count_likes(BtCursor *c, i64 lo, i64 hi, unsigned *n) {
  int r = seek_ge(c, lo);
  if (r <= 0) return r;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1; /* an interior entry: just this one */
    for (; i < last; i++) {
      u8 *cell = findCell(pg, i) + pg->childPtrSize;
      u32 len = cell[0];
      const u8 *p = cell + 1;
      i64 v;
      int fast = 0;
      /* One-byte sizes: the whole record is on this page (maxLocal is far larger). */
      if (len < 0x80 && len >= 3 && p + len <= pg->aDataEnd) {
        u32 h = p[0], t = p[1];
        if (h < len && h >= 3 && t < 0x80) {
          if (t >= 1 && t <= 6) {
            static const u8 sz[7] = {0, 1, 2, 3, 4, 6, 8};
            u32 k = sz[t];
            if (h + k <= len) {
              const u8 *q = p + h;
              v = (i8)q[0];
              for (u32 j = 1; j < k; j++) v = v << 8 | q[j];
              fast = 1;
            }
          } else if (t == 8 || t == 9) {
            v = t - 8;
            fast = 1;
          }
        }
      }
      if (!fast) {
        /* An unusual cell: position on it and decode it in full. */
        c->ix = (u16)i;
        c->info.nSize = 0;
        c->curFlags &= ~BTCF_ValidNKey;
        if (leading_int(c, &v)) return -1;
      }
      if (v > hi) return 0;
      if (v >= lo) n[v - lo]++;
    }
    c->ix = (u16)(last - 1);
    c->info.nSize = 0;
    c->curFlags &= ~BTCF_ValidNKey;
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

/* 1 on a row, 0 empty, -1 error. */
int td_first(BtCursor *c) {
  int empty = 1;
  if (sqlite3BtreeFirst(c, &empty) != SQLITE_OK) return -1;
  return !empty;
}

/* 1 on the next row, 0 at the end, -1 error. */
int td_next(BtCursor *c) {
  int rc = sqlite3BtreeNext(c, 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

/* 1 on the previous row, 0 at the start, -1 error. */
int td_prev(BtCursor *c) {
  int rc = sqlite3BtreePrevious(c, 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

/* The current row's record bytes, valid until the cursor moves: a cursor holds a reference to its page. */
const u8 *td_payload(BtCursor *c, u32 *len) {
  getCellInfo(c);
  u32 total = c->info.nPayload;
  if (c->info.nLocal == total) { *len = total; return c->info.pPayload; }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) { spill_cap = 0; return 0; }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return spill;
}

i64 td_rowid(BtCursor *c) { return sqlite3BtreeIntegerKey(c); }
