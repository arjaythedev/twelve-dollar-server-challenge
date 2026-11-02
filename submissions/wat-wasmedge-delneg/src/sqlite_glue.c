// The only C in this entry besides stock SQLite and wasi-libc. It compiles the pinned, unmodified
// amalgamation into a WASI reactor module (one translation unit, so SQLite's internal B-tree calls are
// reachable) and exports the calls the hand-written WAT server uses: the public SQL API, and one-line
// wrappers over SQLite's B-tree cursor layer. No request logic lives here: which cursors to open, what
// to seek, how to decode records and when to fall back to SQL are all decided in src/app.wat.
#include "sqlite3.c"
#include <stdlib.h>

_Static_assert(SQLITE_VERSION_NUMBER == 3054001, "the B-tree wrappers below are checked against SQLite 3.54.1");

#define X(name) __attribute__((export_name(#name)))
// balance_nonroot swaps two pages through a temporary key, the lock-byte page's number (262,145 at 4 KiB).
// pcache1Rekey raises the cache's iMaxKey to it, so the commit's pcache1Truncate(dbSize + 1) then walks
// the whole hash table (~250 us at 8,192 slots) for pages past the end that are not there. The page
// moves on to a real number before balance_nonroot returns, so iMaxKey keeps its old value for that key.
static void (*pc_rekey)(sqlite3_pcache *, sqlite3_pcache_page *, unsigned, unsigned);
static void pc_rekey_tmp(sqlite3_pcache *p, sqlite3_pcache_page *pg, unsigned from, unsigned to) {
  PCache1 *c = (PCache1 *)p; unsigned m = c->iMaxKey;
  pc_rekey(p, pg, from, to);
  if (to == (unsigned)PENDING_BYTE / (unsigned)c->szPage + 1 && to > m) c->iMaxKey = m;
}
X(db_open) int db_open(const char *path, sqlite3 **db) {
  int rc = sqlite3_initialize();
  if (rc) return rc;
  if (!pc_rekey) { pc_rekey = sqlite3GlobalConfig.pcache2.xRekey; sqlite3GlobalConfig.pcache2.xRekey = pc_rekey_tmp; }
  return sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE, "unix-none");
}
X(db_exec) int db_exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0); }
X(db_prepare) int db_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **st) {
  return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, 0);
}
X(st_bind_int64) int st_bind_int64(sqlite3_stmt *s, int i, long long v) { return sqlite3_bind_int64(s, i, v); }
X(st_bind_text) int st_bind_text(sqlite3_stmt *s, int i, const char *p, int n) { return sqlite3_bind_text(s, i, p, n, SQLITE_STATIC); }
X(st_step) int st_step(sqlite3_stmt *s) { return sqlite3_step(s); }
X(st_reset) int st_reset(sqlite3_stmt *s) { return sqlite3_reset(s); }
X(st_int64) long long st_int64(sqlite3_stmt *s, int i) { return sqlite3_column_int64(s, i); }
X(st_text) const unsigned char *st_text(sqlite3_stmt *s, int i) { return sqlite3_column_text(s, i); }
X(st_bytes) int st_bytes(sqlite3_stmt *s, int i) { return sqlite3_column_bytes(s, i); }
X(db_changes) int db_changes(sqlite3 *db) { return sqlite3_changes(db); }
X(db_last_rowid) long long db_last_rowid(sqlite3 *db) { return sqlite3_last_insert_rowid(db); }
X(db_errmsg) const char *db_errmsg(sqlite3 *db) { return sqlite3_errmsg(db); }
X(db_checkpoint) int db_checkpoint(sqlite3 *db) { int log = 0, ck = 0; sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, &log, &ck); return log; }
X(db_autocommit) int db_autocommit(sqlite3 *db) { return sqlite3_get_autocommit(db); }
X(db_utf8) int db_utf8(sqlite3 *db) { return ENC(db) == SQLITE_UTF8; }
X(env_get) const char *env_get(const char *name) { return getenv(name); }
X(mem_alloc) void *mem_alloc(unsigned long n) { return malloc(n); }
X(mem_free) void mem_free(void *p) { free(p); }
X(mem_realloc) void *mem_realloc(void *p, unsigned long n) { return realloc(p, n); }

// ---- B-tree layer: pager, page cache, WAL and locks stay SQLite's; only the bytecode engine is skipped
X(bt_of) Btree *bt_of(sqlite3 *db) { return db->aDb[0].pBt; }
// starts (or joins) the read transaction and writes the schema cookie to *cookie
X(bt_read) int bt_read(Btree *bt, u32 *cookie) {
  int rc = sqlite3BtreeBeginTrans(bt, 0, 0);
  if (rc == SQLITE_OK) sqlite3BtreeGetMeta(bt, BTREE_SCHEMA_VERSION, cookie);
  return rc;
}
X(key_info) KeyInfo *key_info(sqlite3 *db, int nKey, int nExtra) { return sqlite3KeyInfoAlloc(db, nKey, nExtra); }
X(cur_size) int cur_size(void) { return sqlite3BtreeCursorSize(); }
X(cur_open) int cur_open(Btree *bt, int root, KeyInfo *k, BtCursor *c) {
  memset(c, 0, (size_t)sqlite3BtreeCursorSize());
  sqlite3BtreeCursorZero(c);
  return sqlite3BtreeCursor(bt, (Pgno)root, 0, k, c);
}
X(cur_close) int cur_close(BtCursor *c) { return sqlite3BtreeCloseCursor(c); }
X(cur_first) int cur_first(BtCursor *c, int *res) { return sqlite3BtreeFirst(c, res); }
X(cur_next) int cur_next(BtCursor *c) { return sqlite3BtreeNext(c, 0); }
X(cur_prev) int cur_prev(BtCursor *c) { return sqlite3BtreePrevious(c, 0); }
X(cur_eof) int cur_eof(BtCursor *c) { return sqlite3BtreeEof(c); }
X(cur_rowid) long long cur_rowid(BtCursor *c) { return sqlite3BtreeIntegerKey(c); }
X(cur_seek_rowid) int cur_seek_rowid(BtCursor *c, long long key, int *res) { return sqlite3BtreeTableMoveto(c, key, 0, res); }
// seeks an index on its first column, an integer; default_rc says where entries equal to the key sort
X(cur_seek_int) int cur_seek_int(BtCursor *c, KeyInfo *k, long long key, int default_rc, int *res) {
  Mem m; memset(&m, 0, sizeof m); m.flags = MEM_Int; m.u.i = key;
  UnpackedRecord r; memset(&r, 0, sizeof r);
  r.pKeyInfo = k; r.aMem = &m; r.nField = 1; r.default_rc = (i8)default_rc;
  return sqlite3BtreeIndexMoveto(c, &r, res);
}
X(cur_payload_size) unsigned cur_payload_size(BtCursor *c) { return sqlite3BtreePayloadSize(c); }
// the record's bytes on the cursor's page; *amt = how many are there (fewer than the size: overflow)
X(cur_payload) const u8 *cur_payload(BtCursor *c, u32 *amt) { return sqlite3BtreePayloadFetch(c, amt); }

// ---- checkpoints with the database file's sync done through io_uring (decided in the WAT, see $ckpt).
// db_ckpt runs a PASSIVE checkpoint; with skip set, the sync of the database file at its end returns at
// once without syncing (the WAL's sync before the copy is still made). The WAT then puts nBackfill back
// to what it was, so the WAL cannot restart over frames whose pages are not yet durable in the database
// file, submits an fdatasync of that file, and sets nBackfill once it has completed.
static sqlite3_io_methods db_io;
static int (*db_xsync)(sqlite3_file *, int);
static int db_skip;
static sqlite3_file *db_file;
static int db_sync(sqlite3_file *f, int flags) {
  if (db_skip) { db_skip = 2; return SQLITE_OK; }
  return db_xsync(f, flags);
}
// once, after opening: route the main database file's xSync through db_sync
X(db_sync_hook) int db_sync_hook(sqlite3 *db) {
  sqlite3_file *f = 0;
  if (sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &f) != SQLITE_OK || !f || !f->pMethods) return 1;
  db_io = *f->pMethods; db_xsync = db_io.xSync; db_io.xSync = db_sync; f->pMethods = &db_io; db_file = f;
  return 0;
}
// -1: error; else 1 if the database file's sync was skipped, 0 if none was needed
X(db_ckpt) int db_ckpt(sqlite3 *db, int skip) {
  int log = 0, ck = 0;
  db_skip = skip ? 1 : 0;
  int rc = sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, &log, &ck);
  int s = db_skip; db_skip = 0;
  return rc != SQLITE_OK ? -1 : s == 2;
}
// the database file's sync, now (when nBackfill could not be put back)
X(db_fsync) int db_fsync(sqlite3 *db) { (void)db; return db_file ? db_xsync(db_file, SQLITE_SYNC_NORMAL) : 0; }
static Wal *wal_of(sqlite3 *db) { return sqlite3BtreePager(db->aDb[0].pBt)->pWal; }
// frames in the WAL
X(wal_mx) int wal_mx(sqlite3 *db) { Wal *w = wal_of(db); return w && w->apWiData && w->apWiData[0] ? (int)walIndexHdr(w)->mxFrame : -1; }
// nBackfill: how many of the WAL's frames are copied into the database file. n >= 0 sets it, only between
// transactions and never past the last frame (-2 if refused). Returns the value before.
X(wal_backfill) int wal_backfill(sqlite3 *db, int n) {
  Wal *w = wal_of(db);
  if (!w || !w->apWiData || !w->apWiData[0]) return -1;
  volatile WalCkptInfo *ci = walCkptInfo(w);
  int old = (int)ci->nBackfill;
  if (n >= 0) {
    if (w->readLock >= 0 || w->writeLock || (u32)n > walIndexHdr(w)->mxFrame || (u32)n > ci->nBackfillAttempted) return -2;
    ci->nBackfill = (u32)n;
  }
  return old;
}
