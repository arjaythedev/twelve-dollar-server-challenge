/*
** SQLite itself, plus a narrow read-only window onto its B-tree layer for
** the Ada code (Database.Fast_Read).
**
** SQLite's B-tree cursor functions are internal to the amalgamation, so this
** file compiles the amalgamation and exports a few small wrappers. They read
** the same pages through the same pager, WAL snapshot and page cache as SQL
** does, inside the transaction the Ada side has already begun; they never
** write. Ada decodes the records and decides what to do with them.
*/
#include "sqlite3.c"

#define ADA_CURSORS 4

static Btree *adaTree;
static BtCursor *adaCursor[ADA_CURSORS];
static KeyInfo *adaKeys[ADA_CURSORS];
static UnpackedRecord *adaProbe[ADA_CURSORS];
static Pgno adaRoot[ADA_CURSORS];
static int adaOpen;

/* Cursor slot i reads the b-tree rooted at page root: a table when
** fields is 0, else an index whose first fields columns are its key (all
** ascending or, when descending is set, all descending). */
int ada_bt_setup(sqlite3 *db, int i, unsigned root, int fields, int descending){
  int k;
  if( i<0 || i>=ADA_CURSORS || root<2 || db->aDb[0].pBt==0 ) return SQLITE_MISUSE;
  if( ENC(db)!=SQLITE_UTF8 ) return SQLITE_MISMATCH;
  adaTree = db->aDb[0].pBt;
  adaRoot[i] = root;
  if( adaCursor[i]==0 ){
    adaCursor[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if( adaCursor[i]==0 ) return SQLITE_NOMEM;
  }
  if( fields>0 && adaKeys[i]==0 ){
    adaKeys[i] = sqlite3KeyInfoAlloc(db, fields, 1);
    if( adaKeys[i]==0 ) return SQLITE_NOMEM;
    for(k=0; k<fields; k++){
      adaKeys[i]->aSortFlags[k] = descending ? KEYINFO_ORDER_DESC : 0;
    }
    adaProbe[i] = sqlite3VdbeAllocUnpackedRecord(adaKeys[i]);
    if( adaProbe[i]==0 ) return SQLITE_NOMEM;
  }
  return SQLITE_OK;
}

/* Opens every cursor. The caller has begun a transaction (BEGIN); this
** starts its read snapshot if no statement has yet. */
int ada_bt_begin(void){
  int i, rc;
  if( adaTree==0 || adaOpen ) return SQLITE_MISUSE;
  rc = sqlite3BtreeBeginTrans(adaTree, 0, 0);
  if( rc!=SQLITE_OK ) return rc;
  for(i=0; i<ADA_CURSORS; i++){
    if( adaCursor[i]==0 ) continue;
    sqlite3BtreeCursorZero(adaCursor[i]);
    rc = sqlite3BtreeCursor(adaTree, adaRoot[i], 0, adaKeys[i], adaCursor[i]);
    if( rc!=SQLITE_OK ){
      while( --i>=0 ){
        if( adaCursor[i] ) sqlite3BtreeCloseCursor(adaCursor[i]);
      }
      return rc;
    }
  }
  adaOpen = 1;
  return SQLITE_OK;
}

/* Closes every cursor (before the next SQL statement or the commit). */
void ada_bt_end(void){
  int i;
  if( !adaOpen ) return;
  for(i=0; i<ADA_CURSORS; i++){
    if( adaCursor[i] ) sqlite3BtreeCloseCursor(adaCursor[i]);
  }
  adaOpen = 0;
}

/* Table cursor: 1 if the row with this rowid exists, 0 if not, <0 error. */
int ada_bt_seek_row(int i, sqlite3_int64 rowid){
  int res = 0, rc;
  rc = sqlite3BtreeTableMoveto(adaCursor[i], rowid, 0, &res);
  if( rc!=SQLITE_OK ) return -rc;
  return res==0;
}

/* Index cursor: moves to the first entry whose first key column is at
** least key (an integer). 1 on an entry, 0 past the end, <0 error. */
int ada_bt_seek_int(int i, sqlite3_int64 key){
  UnpackedRecord *r = adaProbe[i];
  int res = 0, rc;
  if( r==0 ) return -SQLITE_MISUSE;
  memset(r->aMem, 0, sizeof(Mem));
  r->nField = 1;
  r->default_rc = 1;      /* equal prefix sorts after the probe: first match */
  r->eqSeen = 0;
  r->aMem[0].flags = MEM_Int;
  r->aMem[0].u.i = key;
  rc = sqlite3BtreeIndexMoveto(adaCursor[i], r, &res);
  if( rc!=SQLITE_OK ) return -rc;
  if( sqlite3BtreeEof(adaCursor[i]) ) return 0;
  if( res<0 ){
    rc = sqlite3BtreeNext(adaCursor[i], 0);
    if( rc==SQLITE_DONE ) return 0;
    if( rc!=SQLITE_OK ) return -rc;
  }
  return 1;
}

/* 1 on an entry, 0 if the b-tree is empty, <0 error. */
int ada_bt_first(int i){
  int empty = 1, rc;
  rc = sqlite3BtreeFirst(adaCursor[i], &empty);
  if( rc!=SQLITE_OK ) return -rc;
  return !empty;
}

/* Step forward (or back): 1 on an entry, 0 past the end, <0 error. */
int ada_bt_step(int i, int backward){
  int rc = backward ? sqlite3BtreePrevious(adaCursor[i], 0)
                    : sqlite3BtreeNext(adaCursor[i], 0);
  if( rc==SQLITE_DONE ) return 0;
  if( rc!=SQLITE_OK ) return -rc;
  return 1;
}

sqlite3_int64 ada_bt_rowid(int i){
  return sqlite3BtreeIntegerKey(adaCursor[i]);
}

/* The current entry's record, when it is stored whole on its page (always,
** for this schema's rows and keys); NULL when part of it overflows. */
const void *ada_bt_record(int i, unsigned *length){
  /* PayloadSize parses the current cell; PayloadFetch only reads that
  ** parse, so it must come second. */
  u32 size = sqlite3BtreePayloadSize(adaCursor[i]);
  u32 local = 0;
  const void *p = sqlite3BtreePayloadFetch(adaCursor[i], &local);
  if( local!=size ) return 0;
  *length = local;
  return p;
}

/* Frees what ada_bt_setup allocated (before the connection closes). */
void ada_bt_close(void){
  int i;
  ada_bt_end();
  for(i=0; i<ADA_CURSORS; i++){
    if( adaProbe[i] ){
      sqlite3DbFree(adaKeys[i]->db, adaProbe[i]);
      adaProbe[i] = 0;
    }
    if( adaKeys[i] ){
      sqlite3KeyInfoUnref(adaKeys[i]);
      adaKeys[i] = 0;
    }
    sqlite3_free(adaCursor[i]);
    adaCursor[i] = 0;
  }
  adaTree = 0;
}
