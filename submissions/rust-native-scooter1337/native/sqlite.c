/* SQLite internals are private to the pinned amalgamation. Only opaque cursors
 * cross this ABI; query planning, record decoding and response assembly are Rust.
 * Inspired by hargup's MIT-licensed Lean-derived server (challenge PR #153). */
#include "../vendor/sqlite3.c"

typedef struct {
  Btree *bt;
  BtCursor *cur;
  KeyInfo *key;
  Pgno root;
  int active;
} NativeCursor;

int native_begin(sqlite3 *db) {
  return sqlite3BtreeBeginTrans(db->aDb[0].pBt, 0, 0);
}

NativeCursor *native_new(sqlite3 *db, unsigned root, int index_fields, int descending) {
  NativeCursor *c = calloc(1, sizeof(*c));
  if (!c) return 0;
  c->bt = db->aDb[0].pBt;
  c->root = root;
  c->cur = malloc((size_t)sqlite3BtreeCursorSize());
  if (!c->cur) { free(c); return 0; }
  if (index_fields) {
    c->key = sqlite3KeyInfoAlloc(db, index_fields, 1);
    if (!c->key) { free(c->cur); free(c); return 0; }
    for (int i = 0; i <= index_fields; ++i) {
      c->key->aColl[i] = db->pDfltColl;
      c->key->aSortFlags[i] = descending && i < index_fields ? KEYINFO_ORDER_DESC : 0;
    }
  }
  return c;
}

int native_open(NativeCursor *c) {
  if (c->active) return SQLITE_OK;
  sqlite3BtreeCursorZero(c->cur);
  int rc = sqlite3BtreeCursor(c->bt, c->root, 0, c->key, c->cur);
  c->active = rc == SQLITE_OK;
  return rc;
}

void native_close(NativeCursor *c) {
  if (c->active) sqlite3BtreeCloseCursor(c->cur);
  c->active = 0;
}

void native_free(NativeCursor *c) {
  if (!c) return;
  native_close(c);
  if (c->key) sqlite3KeyInfoUnref(c->key);
  free(c->cur);
  free(c);
}

int native_first(NativeCursor *c) {
  int empty, rc = sqlite3BtreeFirst(c->cur, &empty);
  return rc ? rc : empty ? SQLITE_DONE : SQLITE_OK;
}
int native_next(NativeCursor *c) { return sqlite3BtreeNext(c->cur, 0); }
int native_previous(NativeCursor *c) { return sqlite3BtreePrevious(c->cur, 0); }
int native_seek(NativeCursor *c, sqlite3_int64 id) {
  int found, rc = sqlite3BtreeTableMoveto(c->cur, id, 0, &found);
  return rc ? rc : found ? SQLITE_DONE : SQLITE_OK;
}
sqlite3_int64 native_rowid(NativeCursor *c) { return sqlite3BtreeIntegerKey(c->cur); }

int native_ge(NativeCursor *c, sqlite3_int64 id) {
  Mem m;
  memset(&m, 0, sizeof(m));
  m.flags = MEM_Int;
  m.u.i = id;
  UnpackedRecord key;
  memset(&key, 0, sizeof(key));
  key.pKeyInfo = c->key;
  key.aMem = &m;
  key.nField = 1;
  key.default_rc = 1;
  int found, rc = sqlite3BtreeIndexMoveto(c->cur, &key, &found);
  if (rc) return rc;
  if (found < 0) return native_next(c);
  return sqlite3BtreeEof(c->cur) ? SQLITE_DONE : SQLITE_OK;
}

const unsigned char *native_payload(NativeCursor *c, unsigned *total, unsigned *local) {
  *total = sqlite3BtreePayloadSize(c->cur);
  return sqlite3BtreePayloadFetch(c->cur, local);
}
int native_copy(NativeCursor *c, unsigned n, void *out) {
  return sqlite3BtreePayload(c->cur, 0, n, out);
}
