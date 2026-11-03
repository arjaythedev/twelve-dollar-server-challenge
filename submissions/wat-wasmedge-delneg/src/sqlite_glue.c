// The only C in this entry besides stock SQLite and wasi-libc. It compiles the pinned, unmodified
// amalgamation into a WASI reactor module (one translation unit, so SQLite's internal B-tree calls are
// reachable). src/app.wat calls SQLite's public API directly (build.sh exports each function it imports);
// these one-line wrappers over SQLite's B-tree cursor layer are the rest. No request logic lives here:
// which cursors to open, what to seek, how to decode records and when to fall back to SQL are all decided
// in src/app.wat.
#include "sqlite3.c"

_Static_assert(SQLITE_VERSION_NUMBER == 3054001, "the B-tree wrappers below are checked against SQLite 3.54.1");

// ---- B-tree layer: pager, page cache, WAL and locks stay SQLite's; only the bytecode engine is skipped
Btree *bt_of(sqlite3 *db) { return db->aDb[0].pBt; }
// starts (or joins) the read transaction and writes the schema cookie to *cookie
int bt_read(Btree *bt, u32 *cookie) {
  int rc = sqlite3BtreeBeginTrans(bt, 0, 0);
  if (rc == SQLITE_OK) sqlite3BtreeGetMeta(bt, BTREE_SCHEMA_VERSION, cookie);
  return rc;
}
KeyInfo *key_info(sqlite3 *db, int nKey, int nExtra) { return sqlite3KeyInfoAlloc(db, nKey, nExtra); }
int cur_size(void) { return sqlite3BtreeCursorSize(); }
int cur_open(Btree *bt, int root, KeyInfo *k, BtCursor *c) {
  memset(c, 0, (size_t)sqlite3BtreeCursorSize());
  sqlite3BtreeCursorZero(c);
  return sqlite3BtreeCursor(bt, (Pgno)root, 0, k, c);
}
int cur_close(BtCursor *c) { return sqlite3BtreeCloseCursor(c); }
int cur_first(BtCursor *c, int *res) { return sqlite3BtreeFirst(c, res); }
int cur_next(BtCursor *c) { return sqlite3BtreeNext(c, 0); }
int cur_prev(BtCursor *c) { return sqlite3BtreePrevious(c, 0); }
int cur_eof(BtCursor *c) { return sqlite3BtreeEof(c); }
long long cur_rowid(BtCursor *c) { return sqlite3BtreeIntegerKey(c); }
int cur_seek_rowid(BtCursor *c, long long key, int *res) { return sqlite3BtreeTableMoveto(c, key, 0, res); }
// seeks an index on its first column, an integer; default_rc says where entries equal to the key sort
int cur_seek_int(BtCursor *c, KeyInfo *k, long long key, int default_rc, int *res) {
  Mem m; memset(&m, 0, sizeof m); m.flags = MEM_Int; m.u.i = key;
  UnpackedRecord r; memset(&r, 0, sizeof r);
  r.pKeyInfo = k; r.aMem = &m; r.nField = 1; r.default_rc = (i8)default_rc;
  return sqlite3BtreeIndexMoveto(c, &r, res);
}
unsigned cur_payload_size(BtCursor *c) { return sqlite3BtreePayloadSize(c); }
// the record's bytes on the cursor's page; *amt = how many are there (fewer than the size: overflow)
const u8 *cur_payload(BtCursor *c, u32 *amt) { return sqlite3BtreePayloadFetch(c, amt); }
