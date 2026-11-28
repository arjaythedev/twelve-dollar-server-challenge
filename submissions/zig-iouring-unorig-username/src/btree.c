// SQLite glue, and the only C in this entry besides SQLite itself. SQLite's B-tree functions are static inside the
// amalgamation, so this file includes it and exports thin wrappers for the ones src/main.zig calls: open the cursors,
// move them, hand back the record bytes under a cursor, and a view of the likes index page under its cursor.
// Everything else (record decoding, the feed and post plans, the SQL fallback, all of the server) is in Zig.
#include "sqlite3.c"

enum { C_POSTS, C_USERS, C_FEED, C_LIKES, NCUR };
static sqlite3 *db;
static Btree *bt;
static BtCursor *cur[NCUR];
static Pgno root[NCUR];
static KeyInfo *ki[NCUR];
static int cur_open;

static KeyInfo *key_info(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) return NULL;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}
// roots: posts, users, posts_created_at_id_idx, likes_post_id_idx. 1 when B-tree reads can be used.
int tds_setup(sqlite3 *d, const unsigned *roots) {
  db = d;
  if (ENC(db) != SQLITE_UTF8) return 0;
  for (int i = 0; i < NCUR; i++) root[i] = roots[i];
  ki[C_FEED] = key_info(2, 1, 1); ki[C_LIKES] = key_info(1, 1, 0);
  if (!ki[C_FEED] || !ki[C_LIKES]) return 0;
  bt = db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) if (!(cur[i] = malloc(sqlite3BtreeCursorSize()))) return 0;
  return 1;
}
// The pass's read transaction on the B-tree, after the SQL BEGIN. 1 on success.
int tds_begin(void) {
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return 0;
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, root[i], 0, ki[i], cur[i]) != SQLITE_OK) {
      while (i--) sqlite3BtreeCloseCursor(cur[i]);
      return 0;
    }
  }
  return cur_open = 1;
}
void tds_close(void) {
  if (!cur_open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}
int tds_first(int k, int *empty) { return sqlite3BtreeFirst(cur[k], empty); }
int tds_next(int k) { return sqlite3BtreeNext(cur[k], 0); }  // SQLITE_OK, SQLITE_DONE at the end, else an error
int tds_seek(int k, sqlite3_int64 id) {  // is rowid id there? -1 if the seek itself failed
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur[k], id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && cur[k]->eState == CURSOR_VALID;
}
// Record under the cursor, or NULL if it spills onto overflow pages.
const unsigned char *tds_record(int k, unsigned *n) {
  BtCursor *c = cur[k];
  getCellInfo(c);
  if (c->info.nLocal != c->info.nPayload) return NULL;
  *n = c->info.nPayload;
  return c->info.pPayload;
}
// Moves the likes cursor to the first entry for a post at or above lo. 1 on an entry, 0 past the end, -1 on error.
int tds_likes_from(sqlite3_int64 lo) {
  BtCursor *c = cur[C_LIKES];
  Mem key; UnpackedRecord want; int cmp = 0;
  memset(&key, 0, sizeof key); key.flags = MEM_Int; key.u.i = lo;
  memset(&want, 0, sizeof want); want.pKeyInfo = c->pKeyInfo; want.aMem = &key; want.nField = 1;
  want.default_rc = 1;  // an entry equal on post_id compares above the key, so the seek stops before the first of them
  if (sqlite3BtreeIndexMoveto(c, &want, &cmp) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (cmp >= 0) return 1;
  int rc = sqlite3BtreeNext(c, 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}
// The page under the likes cursor, for reading its cells in place (see tally in main.zig).
typedef struct {
  const unsigned char *data, *cell_idx, *end;
  unsigned short mask, ncell, max_local, ix;
  unsigned char child_ptr, leaf;
} tds_page;
void tds_likes_page(tds_page *v) {
  BtCursor *c = cur[C_LIKES]; MemPage *pg = c->pPage;
  v->data = pg->aData; v->cell_idx = pg->aCellIdx; v->end = pg->aDataEnd;
  v->mask = pg->maskPage; v->ncell = pg->nCell; v->max_local = pg->maxLocal; v->ix = c->ix;
  v->child_ptr = pg->childPtrSize; v->leaf = pg->leaf;
}
void tds_likes_set_ix(unsigned short ix) { cur[C_LIKES]->ix = ix; }
// One contiguous page cache (a variadic sqlite3_config call, kept on the C side).
int tds_pagecache(void *mem, int npg) {
  int hdr = 0; sqlite3_config(SQLITE_CONFIG_PCACHE_HDRSZ, &hdr);
  int sz = (4096 + hdr + 7) & ~7;
  if (!mem) return sz;
  return sqlite3_config(SQLITE_CONFIG_PAGECACHE, mem, sz, npg);
}
