/*
** db.c — SQLite data layer.
**
** Included into the same translation unit immediately after sqlite/sqlite3.c,
** so SQLite's internal B-tree interface is used directly: Btree/BtCursor
** handles, KeyInfo-driven index seeks and record decoding all run without
** any per-request or per-batch allocation.
**
** Threading model: one event-loop thread owns the connection; one background
** thread (nice 19) checkpoints the WAL. The connection is opened with
** SQLITE_OPEN_NOMUTEX, so the two threads are serialized here by db_mu:
** a batch holds it from db_batch_begin() to db_batch_end(), and the
** checkpointer takes it around each checkpoint. Checkpoints therefore only
** ever run between batches — which is also what a single-vCPU event loop
** wants.
**
** Batching: all database work happens between db_batch_begin()/db_batch_end().
** Reads walk preallocated B-tree cursors; writes go through prepared SQL
** statements owned by the request layer.
*/
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>

/* Some helpers below are optional conveniences for the request layer;
** keep the build warning-clean even when they end up unused. */
#if defined(__GNUC__) || defined(__clang__)
#define DB_UNUSED __attribute__((unused))
#else
#define DB_UNUSED
#endif

/* ------------------------------------------------------------------ */
/* Connection state (visible to the request layer in the same TU)      */
/* ------------------------------------------------------------------ */

static sqlite3 *db;
static Btree *db_bt;
static int db_in_batch;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;

/* Data version tracking: detects writes made by other connections
** between batches. sqlite3BtreeGetMeta(BTREE_DATA_VERSION) is the pager's
** data-version counter adjusted so that commits made by this same
** connection do not change it. */
static u32 db_version;
static int db_version_known, db_foreign_write;

/* ------------------------------------------------------------------ */
/* Fatal errors: without the database there is nothing to serve        */
/* ------------------------------------------------------------------ */

static void db_die(const char *what, int rc){
  fprintf(stderr, "db: %s (%d): %s\n", what, rc,
          db ? sqlite3_errmsg(db) : "not open");
  fflush(stderr);
  exit(1);
}

static sqlite3_stmt *db_prepare(const char *sql){
  sqlite3_stmt *st = 0;
  if( sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, 0)!=SQLITE_OK ){
    db_die(sql, sqlite3_extended_errcode(db));
  }
  return st;
}

/* Step a no-row statement to completion and reset it for reuse. */
static void step_reset(sqlite3_stmt *st){
  int rc = sqlite3_step(st);
  if( rc!=SQLITE_DONE ) db_die("step", rc);
  sqlite3_reset(st);
}

/* Execute a statement that may return rows (e.g. pragmas), discard rows. */
static void db_exec(const char *sql){
  sqlite3_stmt *st = db_prepare(sql);
  for(;;){
    int rc = sqlite3_step(st);
    if( rc==SQLITE_DONE ) break;
    if( rc!=SQLITE_ROW ) db_die(sql, rc);
  }
  sqlite3_finalize(st);
}

/* ------------------------------------------------------------------ */
/* Record decoding                                                     */
/*                                                                     */
/* A record is [header-size varint][serial types...][values...]; the   */
/* header size includes itself.                                        */
/* ------------------------------------------------------------------ */

#define REC_MAX 4096   /* rec_t payload buffer size (payloads are capped
                          slightly below it to leave safe varint overread
                          slack inside the buffer) */

typedef struct col_t {
  u64 t;          /* serial type code */
  const u8 *p;    /* value bytes (text/blob/int as stored) */
} col_t;

typedef struct rec_t {
  const u8 *p;    /* zero-copy pointer into mmap/page-cache (or spill buffer) */
  u32 n;          /* payload size in bytes */
} rec_t;

static inline const u8 *rd_varint(const u8 *p, const u8 *end, u64 *v){
  u64 x = 0;
  for(int i=0; i<8; i++){
    if( UNLIKELY(p >= end) ) return 0;
    x = (x<<7) | (p[0] & 0x7f);
    if( !(*p++ & 0x80) ){ *v = x; return p; }
  }
  if( UNLIKELY(p >= end) ) return 0;
  *v = (x<<8) | *p++;
  return p;
}

static inline u32 serial_len(u64 t){
  static const u8 sl[12] = { 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0 };
  return t<12 ? sl[t] : (u32)((t-12)>>1);
}

static inline int col_is_int(const col_t *c){
  return (c->t>=1 && c->t<=6) || c->t==8 || c->t==9;
}

static inline int col_is_text(const col_t *c){
  return c->t>=13 && (c->t & 1);
}

static inline u32 col_len(const col_t *c){
  return serial_len(c->t);
}

static inline i64 col_int(const col_t *c){
  const u8 *p = c->p;
  switch( c->t ){
  case 1: return (int8_t)p[0];
  case 2: return (int16_t)((u16)p[0]<<8 | p[1]);
  case 3: return (int32_t)((u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8) >> 8;
  case 4: return (int32_t)((u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3]);
  case 5: return (i64)((u64)(int16_t)((u16)p[0]<<8 | p[1])<<32 |
                       (u64)((u32)p[2]<<24 | (u32)p[3]<<16 | (u32)p[4]<<8 | p[5]));
  case 6: { u64 v = 0; for(int i=0; i<8; i++) v = (v<<8) | p[i]; return (i64)v; }
  case 9: return 1;
  default: return 0;
  }
}

static inline int rec_cols(const rec_t *r, int ncol, col_t *c){
  const u8 *end = r->p + r->n;
  u64 hsz;
  const u8 *h = rd_varint(r->p, end, &hsz);
  if( UNLIKELY(!h || hsz > r->n) ) return -1;
  const u8 *hend = r->p + hsz, *body = hend;
  for(int i=0; i<ncol; i++){
    if( UNLIKELY(h >= hend) ) return -1;
    h = rd_varint(h, hend, &c[i].t);
    if( UNLIKELY(!h) ) return -1;
    c[i].p = body;
    body += serial_len(c[i].t);
  }
  return LIKELY(body <= end) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Preallocated persistent B-tree cursors                              */
/*                                                                     */
/* A small pool of BtCursor buffers is allocated once at startup.      */
/* cursor_new() zeroes and opens one in place; cursor_close() unlinks  */
/* it without freeing the buffer. Zero heap churn per batch.           */
/* ------------------------------------------------------------------ */

typedef struct {
  BtCursor *c;
  u8 *spill;      /* spill buffer for records not fetched whole (per cursor) */
  u32 spill_cap;  /* allocated size of spill */
} cursor;

#define CURSOR_POOL 3   /* maximum simultaneously open cursors */

static struct {
  BtCursor *c;
  int used;
} cur_pool[CURSOR_POOL];

/* Open a read-only cursor into caller-provided (or pooled) memory. */
static void cursor_open(Pgno root, KeyInfo *ki, BtCursor *mem){
  sqlite3BtreeCursorZero(mem);
  if( sqlite3BtreeCursor(db_bt, root, 0, ki, mem)!=SQLITE_OK ){
    db_die("cursor", 0);
  }
}

/* Take a cursor from the pool and open it on table/index b-tree `root`.
** `ki` must be 0 for table btrees, a KeyInfo for index btrees. */
static cursor cursor_new(Pgno root, KeyInfo *ki){
  int i;
  for(i=0; i<CURSOR_POOL; i++){
    cursor cu;
    if( cur_pool[i].used ) continue;
    cur_pool[i].used = 1;
    cursor_open(root, ki, cur_pool[i].c);
    cu.c = cur_pool[i].c;
    cu.spill = 0;
    cu.spill_cap = 0;
    return cu;
  }
  db_die("cursor pool exhausted", 0);
  {
    cursor cu;
    cu.c = 0;                       /* not reached */
    cu.spill = 0;
    cu.spill_cap = 0;
    return cu;
  }
}

/* Close a cursor; the BtCursor buffer returns to the pool. The cursor's
** spill buffer, if one was allocated, is freed here. */
static void cursor_close(cursor *c){
  sqlite3BtreeCloseCursor(c->c);
  free(c->spill);
  c->spill = 0;
  c->spill_cap = 0;
  {
    int i;
    for(i=0; i<CURSOR_POOL; i++){
      if( cur_pool[i].c==c->c ){
        cur_pool[i].used = 0;
        return;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* B-tree navigation                                                   */
/*                                                                     */
/* Navigation and seek functions return 0 when positioned on a valid   */
/* entry, 1 at end-of-scan (including empty btrees), -1 on error.      */
/* ------------------------------------------------------------------ */

static int DB_UNUSED cur_first(cursor *c){
  int res, rc = sqlite3BtreeFirst(c->c, &res);
  if( rc!=SQLITE_OK ) return -1;
  return res ? 1 : 0;
}

static int cur_last(cursor *c){
  int res, rc = sqlite3BtreeLast(c->c, &res);
  if( rc!=SQLITE_OK ) return -1;
  return res ? 1 : 0;
}

static int cur_next(cursor *c){
  int rc = sqlite3BtreeNext(c->c, 0);
  if( rc==SQLITE_DONE ) return 1;
  return rc==SQLITE_OK ? 0 : -1;
}

static int cur_prev(cursor *c){
  int rc = sqlite3BtreePrevious(c->c, 0);
  if( rc==SQLITE_DONE ) return 1;
  return rc==SQLITE_OK ? 0 : -1;
}

/* Table-cursor seek by rowid. When the row is absent the cursor is left
** on an unspecified nearby entry: do not read a record after a nonzero
** result. */
static int cur_seek(cursor *c, i64 rowid);

/* Rowid of the current entry. Works for table cursors (b-tree key) and
** for index cursors (trailing rowid field of the index record). */
static i64 cur_rowid(cursor *c){
  if( c->c->pKeyInfo==0 ){
    return sqlite3BtreeIntegerKey(c->c);
  }else{
    i64 rid = 0;
    if( sqlite3VdbeIdxRowid(db, c->c, &rid)!=SQLITE_OK ){
      db_die("idx rowid", 0);
    }
    return rid;
  }
}

/* Copy the current entry's payload into r->buf and decode its header.
** Returns the column count (>0) on success, <=0 on error. The col_t
** values produced by rec_cols() point into r->buf and stay valid until
** the next cur_record() on the same rec_t. */
static inline int cur_record(cursor *c, rec_t *r){
  u32 total = sqlite3BtreePayloadSize(c->c);
  u32 local;
  const u8 *p = sqlite3BtreePayloadFetch(c->c, &local);
  if( LIKELY(local == total) ){ r->p = p; r->n = total; return 0; }
  if( total > c->spill_cap ){
    free(c->spill);
    c->spill_cap = total + 4096;
    if( !(c->spill = malloc(c->spill_cap)) ) abort();
  }
  if( sqlite3BtreePayload(c->c, 0, total, c->spill)!=SQLITE_OK ) return -1;
  r->p = c->spill; r->n = total;
  return 0;
}

static inline int idx_seek_ge(cursor *c, KeyInfo *ki, i64 k){
  static Mem m;
  static UnpackedRecord r;
  m.flags = MEM_Int;
  m.u.i = k;
  r.pKeyInfo = ki;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1;
  r.errCode = 0;
  r.r1 = 0;
  r.r2 = 0;
  r.eqSeen = 0;
  int res;
  if( sqlite3BtreeIndexMoveto(c->c, &r, &res)!=SQLITE_OK ) return -1;
  if( res < 0 ) return cur_next(c);
  return sqlite3BtreeEof(c->c) ? 1 : 0;
}

/* Fast leaf-page scan of likes_post_id_idx over [lo..hi]: decodes cells directly on each leaf
** page before advancing the cursor once per leaf. Returns 0 on success, 1 to fall back to the
** record-by-record scan, or -1 on error. */
static inline int cur_count_likes_fast(cursor *c, KeyInfo *ki, i64 lo, i64 hi, i64 *counts){
  int rc = idx_seek_ge(c, ki, lo);
  if( rc > 0 ) return 0;
  if( rc < 0 ) return -1;
  for(;;){
    MemPage *pg = c->c->pPage;
    int i = c->c->ix;
    int last = pg->leaf ? pg->nCell : i + 1;
    const u8 *aData = pg->aData;
    const u8 *aEnd = pg->aDataEnd;
    u32 mask = pg->cellOffset;
    u8 cpsz = pg->childPtrSize;
    (void)aData; (void)mask;
    for(; i < last; i++){
      u8 *cell = findCell(pg, i) + cpsz;
      u32 len = cell[0];
      const u8 *p = cell + 1;
      if( UNLIKELY(len >= 0x80 || len < 3 || p + len > aEnd) ) return 1;
      u32 h = p[0], t = p[1];
      if( UNLIKELY(h >= len || h < 3) ) return 1;
      const u8 *q = p + h;
      i64 v;
      switch( t ){
      case 1:
        v = (int8_t)q[0];
        break;
      case 2:
        if( UNLIKELY(h + 2 > len) ) return 1;
        v = (int16_t)((u16)q[0]<<8 | q[1]);
        break;
      case 3:
        if( UNLIKELY(h + 3 > len) ) return 1;
        v = (int32_t)(((u32)q[0]<<24 | (u32)q[1]<<16 | (u32)q[2]<<8) >> 8);
        break;
      case 4:
        if( UNLIKELY(h + 4 > len) ) return 1;
        v = (int32_t)((u32)q[0]<<24 | (u32)q[1]<<16 | (u32)q[2]<<8 | q[3]);
        break;
      case 5:
        if( UNLIKELY(h + 6 > len) ) return 1;
        v = (i64)((u64)(int16_t)((u16)q[0]<<8 | q[1])<<32 |
                  (u64)((u32)q[2]<<24 | (u32)q[3]<<16 | (u32)q[4]<<8 | q[5]));
        break;
      case 6:
        if( UNLIKELY(h + 8 > len) ) return 1;
        { u64 w = 0; for(int j = 0; j < 8; j++) w = (w << 8) | q[j]; v = (i64)w; }
        break;
      case 8:
      case 9:
        v = (i64)(t - 8);
        break;
      default:
        return 1;
      }
      if( v > hi ) return 0;
      if( v >= lo ) counts[v - lo]++;
    }
    c->c->ix = (u16)(last - 1);
    rc = sqlite3BtreeNext(c->c, 0);
    if( rc == SQLITE_DONE ) return 0;
    if( rc != SQLITE_OK ) return -1;
  }
}

/* Multi-key level-by-level B-tree descent (credit: c-iouring-pavlyhalim #245, see README) */
typedef struct bt_hit { MemPage *pg; int ix; } bt_hit;

static inline i64 bt_cell_key(MemPage *pg, int i){
  const u8 *p = findCellPastPtr(pg, i);
  if( pg->intKeyLeaf ){
    while( *p++ >= 0x80 ){
      if( UNLIKELY(p >= pg->aDataEnd) ) return -1;
    }
  }
  return sqlite3VarintValue(p);
}

static inline int bt_lower_bound(MemPage *pg, i64 k){
  int n = pg->nCell;
  if( UNLIKELY(n <= 0) ) return 0;
  i64 k0 = bt_cell_key(pg, 0);
  if( k <= k0 ) return 0;
  int hi = n - 1;
  i64 kn = bt_cell_key(pg, hi);
  if( k > kn ) return n;
  if( k == kn || hi <= 1 ) return hi;
  int lo = 1;
  if( LIKELY(kn > k0) ){
    u64 span = (u64)kn - (u64)k0;
    int g = (int)((((u64)k - (u64)k0) * (u64)hi) / span);
    if( g < lo ) g = lo;
    if( g >= hi ) g = hi - 1;
    i64 kg = bt_cell_key(pg, g);
    if( kg == k ) return g;
    if( kg < k ){
      lo = g + 1;
      for(int s = 0; s < 2 && lo < hi; s++){
        i64 ks = bt_cell_key(pg, lo);
        if( ks >= k ) return lo;
        lo++;
      }
    }else{
      hi = g;
      for(int s = 0; s < 2 && lo < hi; s++){
        i64 ks = bt_cell_key(pg, hi - 1);
        if( ks < k ) return hi;
        hi--;
        if( ks == k ) return hi;
      }
    }
  }
  while( lo < hi ){
    int mid = (lo + hi) >> 1;
    if( bt_cell_key(pg, mid) < k ) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
static int cur_seek(cursor *c, i64 rowid){
  BtCursor *pCur = c->c;
  if( LIKELY(pCur->eState == CURSOR_VALID) ){
    if( (pCur->curFlags & BTCF_ValidNKey) != 0 && pCur->info.nKey == rowid ){
      return 0;
    }
    MemPage *pg = pCur->pPage;
    if( pg && pg->intKeyLeaf && pg->nCell > 0 ){
      i64 k0 = bt_cell_key(pg, 0);
      i64 kn = bt_cell_key(pg, pg->nCell - 1);
      if( rowid >= k0 && rowid <= kn ){
        int idx = bt_lower_bound(pg, rowid);
        if( LIKELY(idx < pg->nCell && bt_cell_key(pg, idx) == rowid) ){
          pCur->ix = (u16)idx;
          pCur->curFlags |= BTCF_ValidNKey;
          pCur->curFlags &= ~(BTCF_ValidOvfl | BTCF_AtLast);
          pCur->info.nKey = rowid;
          pCur->info.nSize = 0;
          return 0;
        }
        return 1;
      }
    }
  }
  int rc = moveToRoot(pCur);
  if( UNLIKELY(rc != SQLITE_OK) ) return rc == SQLITE_EMPTY ? 1 : -1;
  for(;;){
    MemPage *pg = pCur->pPage;
    if( UNLIKELY(!pg || !pg->intKey || pg->nCell <= 0) ) break;
    int idx = bt_lower_bound(pg, rowid);
    if( pg->leaf ){
      if( LIKELY(idx < pg->nCell && bt_cell_key(pg, idx) == rowid) ){
        pCur->ix = (u16)idx;
        pCur->curFlags |= BTCF_ValidNKey;
        pCur->curFlags &= ~(BTCF_ValidOvfl | BTCF_AtLast);
        pCur->info.nKey = rowid;
        pCur->info.nSize = 0;
        return 0;
      }
      pCur->ix = (u16)(idx < pg->nCell ? idx : pg->nCell - 1);
      pCur->info.nSize = 0;
      return 1;
    }
    Pgno chld = (idx < pg->nCell)
                ? get4byte(findCell(pg, idx))
                : get4byte(&pg->aData[pg->hdrOffset + 8]);
    pCur->ix = (u16)idx;
    if( UNLIKELY(moveToChild(pCur, chld) != SQLITE_OK) ) return -1;
  }
  int res;
  rc = sqlite3BtreeTableMoveto(pCur, rowid, 0, &res);
  if( rc!=SQLITE_OK ) return -1;
  return res==0 ? 0 : 1;
}

static inline void bt_release_hits(bt_hit *h, int n){
  for(int i = 0; i < n; i++){
    if( h[i].pg ){
      releasePageNotNull(h[i].pg);
      h[i].pg = 0;
    }
  }
}

static inline int bt_seek_many(Pgno root, const i64 *key, int n, bt_hit *hit){
  for(int i = 0; i < n; i++){
    hit[i].pg = 0;
    hit[i].ix = 0;
  }
  if( UNLIKELY(n <= 0) ) return 0;
  if( UNLIKELY(n > FEED_SIZE) ) return -1;
  BtShared *bs = db_bt->pBt;
  MemPage *root_pg = 0;
  if( UNLIKELY(getAndInitPage(bs, root, &root_pg, PAGER_GET_READONLY) != SQLITE_OK) ) return -1;
  if( UNLIKELY(!root_pg->intKey) ){
    releasePageNotNull(root_pg);
    return -1;
  }
  MemPage *cur[FEED_SIZE];
  cur[0] = root_pg;
  for(int i = 1; i < n; i++){
    sqlite3PagerRef(root_pg->pDbPage);
    cur[i] = root_pg;
  }
  for(int depth = 0; depth < BTCURSOR_MAX_DEPTH && !cur[0]->leaf; depth++){
    Pgno next[FEED_SIZE];
    for(int i = 0; i < n; i++){
      MemPage *pg = cur[i];
      if( UNLIKELY(pg->leaf || !pg->intKey || pg->nCell == 0) ){
        for(int j = i; j < n; j++) releasePageNotNull(cur[j]);
        return -1;
      }
      int idx = bt_lower_bound(pg, key[i]);
      next[i] = (idx < pg->nCell)
                ? get4byte(findCell(pg, idx))
                : get4byte(&pg->aData[pg->hdrOffset + 8]);
      releasePageNotNull(pg);
    }
    for(int i = 0; i < n; i++){
      Pgno pgno = next[i];
      MemPage *pg = 0;
      for(int j = 0; j < i; j++){
        if( next[j] == pgno ){
          pg = cur[j];
          sqlite3PagerRef(pg->pDbPage);
          break;
        }
      }
      if( !pg && UNLIKELY(getAndInitPage(bs, pgno, &pg, PAGER_GET_READONLY) != SQLITE_OK) ){
        for(int j = 0; j < i; j++) releasePageNotNull(cur[j]);
        return -1;
      }
      cur[i] = pg;
    }
  }
  if( UNLIKELY(!cur[0]->leaf) ){
    for(int i = 0; i < n; i++) releasePageNotNull(cur[i]);
    return -1;
  }
  for(int i = 0; i < n; i++){
    MemPage *pg = cur[i];
    if( UNLIKELY(!pg->intKeyLeaf) ){
      for(int j = i; j < n; j++) releasePageNotNull(cur[j]);
      bt_release_hits(hit, i);
      return -1;
    }
    int idx = bt_lower_bound(pg, key[i]);
    if( LIKELY(idx < pg->nCell && bt_cell_key(pg, idx) == key[i]) ){
      hit[i].pg = pg;
      hit[i].ix = idx;
    }else{
      releasePageNotNull(pg);
    }
  }
  return 0;
}

static inline int bt_hit_record(bt_hit *h, rec_t *r){
  MemPage *pg = h->pg;
  const u8 *end = pg->aDataEnd;
  u64 sz;
  const u8 *p = rd_varint(findCellPastPtr(pg, h->ix), end, &sz);
  if( UNLIKELY(!p || sz > pg->maxLocal || p >= end) ) return -1;
  while( *p++ >= 0x80 ){
    if( UNLIKELY(p >= end) ) return -1;
  }
  if( UNLIKELY(p + sz > end) ) return -1;
  r->p = p;
  r->n = (u32)sz;
  return 0;
}

static inline int db_user_names(Pgno root_users, int decode_cols, const i64 *uid, int n,
                                bt_hit *hit, const u8 **name, u32 *len){
  if( UNLIKELY(n <= 0) ) return 0;
  if( UNLIKELY(decode_cols < 2 || decode_cols > 8) ) return -1;
  if( UNLIKELY(bt_seek_many(root_users, uid, n, hit) < 0) ) return -1;
  for(int i = 0; i < n; i++){
    if( UNLIKELY(!hit[i].pg) ){
      bt_release_hits(hit, n);
      return 1;
    }
    rec_t r;
    col_t c[8];
    if( UNLIKELY(bt_hit_record(&hit[i], &r) || rec_cols(&r, decode_cols, c) || !col_is_text(&c[1])) ){
      bt_release_hits(hit, n);
      return -1;
    }
    name[i] = c[1].p;
    len[i] = col_len(&c[1]);
  }
  return 0;
}
static inline i64 bt_idx_cell_int0(MemPage *pg, int i, int *ok){
  const u8 *aEnd = pg->aDataEnd;
  const u8 *cell = findCellPastPtr(pg, i);
  if( UNLIKELY(cell + 4 > aEnd) ){ *ok = 0; return 0; }
  u32 len = cell[0];
  const u8 *p = cell + 1;
  if( UNLIKELY(len >= 0x80 || len < 3 || p + len > aEnd) ){ *ok = 0; return 0; }
  u32 h = p[0], t = p[1];
  if( UNLIKELY(h >= len || h < 3) ){ *ok = 0; return 0; }
  const u8 *q = p + h;
  *ok = 1;
  switch( t ){
  case 1: return (int8_t)q[0];
  case 2: if( UNLIKELY(h + 2 > len) ) break; return (int16_t)((u16)q[0]<<8 | q[1]);
  case 3: if( UNLIKELY(h + 3 > len) ) break; return (int32_t)(((u32)q[0]<<24 | (u32)q[1]<<16 | (u32)q[2]<<8) >> 8);
  case 4: if( UNLIKELY(h + 4 > len) ) break; return (int32_t)((u32)q[0]<<24 | (u32)q[1]<<16 | (u32)q[2]<<8 | q[3]);
  case 5: if( UNLIKELY(h + 6 > len) ) break;
          return (i64)((u64)(int16_t)((u16)q[0]<<8 | q[1])<<32 |
                       (u64)((u32)q[2]<<24 | (u32)q[3]<<16 | (u32)q[4]<<8 | q[5]));
  case 6: if( UNLIKELY(h + 8 > len) ) break;
          { u64 w = 0; for(int j = 0; j < 8; j++) w = (w << 8) | q[j]; return (i64)w; }
  case 8:
  case 9: return (i64)(t - 8);
  }
  *ok = 0;
  return 0;
}

static inline int bt_idx_lower_bound(MemPage *pg, i64 k, int *ok){
  int n = pg->nCell;
  if( UNLIKELY(n <= 0) ){ *ok = 1; return 0; }
  i64 k0 = bt_idx_cell_int0(pg, 0, ok);
  if( UNLIKELY(!*ok) ) return 0;
  if( k <= k0 ) return 0;
  int hi = n - 1;
  i64 kn = bt_idx_cell_int0(pg, hi, ok);
  if( UNLIKELY(!*ok) ) return 0;
  if( k > kn ) return n;
  int lo = 0;
  if( LIKELY(kn > k0 && hi > 4) ){
    u64 span = (u64)kn - (u64)k0;
    int g = (int)((((u64)k - (u64)k0) * (u64)hi) / span);
    if( g < 1 ) g = 1;
    if( g >= hi ) g = hi - 1;
    i64 kg = bt_idx_cell_int0(pg, g, ok);
    if( UNLIKELY(!*ok) ) return 0;
    if( kg < k ){
      lo = g + 1;
      for(int s = 0; s < 2 && lo < hi; s++){
        i64 ks = bt_idx_cell_int0(pg, lo, ok);
        if( UNLIKELY(!*ok) ) return 0;
        if( ks >= k ) break;
        lo++;
      }
    }else{
      hi = g;
      for(int s = 0; s < 2 && lo < hi; s++){
        i64 ks = bt_idx_cell_int0(pg, hi - 1, ok);
        if( UNLIKELY(!*ok) ) return 0;
        if( ks < k ) return hi;
        hi--;
      }
    }
  }
  while( lo < hi ){
    int mid = (lo + hi) >> 1;
    i64 km = bt_idx_cell_int0(pg, mid, ok);
    if( UNLIKELY(!*ok) ) return 0;
    if( km < k ) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/* Direct level-by-level descent of likes_post_id_idx for range [lo..hi].
** Returns 0 on success, or 1 to fall back to cur_count_likes_fast when the
** range spans a non-trivial leaf boundary. */
static inline int db_range_counts_direct(Pgno root_idx, i64 lo, i64 hi, i64 *counts){
  BtShared *bs = db_bt->pBt;
  MemPage *pg = 0;
  if( UNLIKELY(getAndInitPage(bs, root_idx, &pg, PAGER_GET_READONLY) != SQLITE_OK) ) return 1;
  int ok = 1;
  int parent_rightmost = 1, at_rightmost = 1, at_leftmost = 1;
  MemPage *parent = 0;
  int parent_idx = -1;
  for(int depth = 0; depth < BTCURSOR_MAX_DEPTH && !pg->leaf; depth++){
    if( UNLIKELY(pg->intKey || pg->nCell == 0 || pg->nOverflow) ){
      if( parent ) releasePageNotNull(parent);
      releasePageNotNull(pg);
      return 1;
    }
    int idx = bt_idx_lower_bound(pg, lo, &ok);
    if( UNLIKELY(!ok) ){
      if( parent ) releasePageNotNull(parent);
      releasePageNotNull(pg);
      return 1;
    }
    parent_rightmost = at_rightmost;
    if( idx < pg->nCell ) at_rightmost = 0;
    if( idx > 0 ) at_leftmost = 0;
    if( idx < pg->nCell ){
      i64 ki = bt_idx_cell_int0(pg, idx, &ok);
      if( !ok || ki == lo ){
        if( parent ) releasePageNotNull(parent);
        releasePageNotNull(pg);
        return 1;
      }
    }
    Pgno next = (idx < pg->nCell)
                ? get4byte(findCell(pg, idx))
                : get4byte(&pg->aData[pg->hdrOffset + 8]);
    if( parent ) releasePageNotNull(parent);
    parent = pg;
    parent_idx = idx;
    pg = 0;
    if( UNLIKELY(getAndInitPage(bs, next, &pg, PAGER_GET_READONLY) != SQLITE_OK) ){
      releasePageNotNull(parent);
      return 1;
    }
  }
  if( UNLIKELY(!pg->leaf || pg->intKey || pg->nOverflow) ){
    if( parent ) releasePageNotNull(parent);
    releasePageNotNull(pg);
    return 1;
  }
  int n = pg->nCell;
  if( n == 0 ){
    if( parent ) releasePageNotNull(parent);
    releasePageNotNull(pg);
    return at_rightmost ? 0 : 1;
  }
  int idx = bt_idx_lower_bound(pg, lo, &ok);
  if( UNLIKELY(!ok) ){
    if( parent ) releasePageNotNull(parent);
    releasePageNotNull(pg);
    return 1;
  }
  if( idx == n ){
    if( at_rightmost ){
      if( parent ) releasePageNotNull(parent);
      releasePageNotNull(pg);
      return 0;
    }
  }
  if( UNLIKELY(idx == 0 && !at_leftmost) ){
    if( parent ) releasePageNotNull(parent);
    releasePageNotNull(pg);
    return 1;
  }
  for(int step = 0; step < 4; step++){
    for(int i = idx; i < n; i++){
      i64 v = bt_idx_cell_int0(pg, i, &ok);
      if( UNLIKELY(!ok) ){
        if( parent ) releasePageNotNull(parent);
        releasePageNotNull(pg);
        return 1;
      }
      if( v > hi ){
        if( parent ) releasePageNotNull(parent);
        releasePageNotNull(pg);
        return 0;
      }
      if( v >= lo ) counts[v - lo]++;
    }
    if( at_rightmost ){
      if( parent ) releasePageNotNull(parent);
      releasePageNotNull(pg);
      return 0;
    }
    /* Advance to the sibling leaf via parent if available. */
    if( !parent || parent_idx >= parent->nCell ) break;
    i64 sep = bt_idx_cell_int0(parent, parent_idx, &ok);
    if( UNLIKELY(!ok) ) break;
    if( sep > hi ){
      releasePageNotNull(parent);
      releasePageNotNull(pg);
      return 0;
    }
    if( sep >= lo ) counts[sep - lo]++;
    parent_idx++;
    if( parent_idx == parent->nCell && parent_rightmost ) at_rightmost = 1;
    Pgno next = (parent_idx < parent->nCell)
                ? get4byte(findCell(parent, parent_idx))
                : get4byte(&parent->aData[parent->hdrOffset + 8]);
    releasePageNotNull(pg);
    pg = 0;
    if( UNLIKELY(getAndInitPage(bs, next, &pg, PAGER_GET_READONLY) != SQLITE_OK) ){
      releasePageNotNull(parent);
      return 1;
    }
    if( UNLIKELY(!pg->leaf || pg->intKey || pg->nOverflow) ) break;
    n = pg->nCell;
    idx = 0;
  }
  if( parent ) releasePageNotNull(parent);
  if( pg ) releasePageNotNull(pg);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Startup schema verification                                         */
/*                                                                     */
/* The schema is fixed, so it is verified against the in-memory Table  */
/* and Index structures after the schema is loaded.                    */
/* ------------------------------------------------------------------ */

/* Case-insensitive compare of n bytes; 0 when equal. */
static int ci_eq(const char *a, const char *b, u32 n){
  u32 i;
  for(i=0; i<n; i++){
    u8 x = (u8)a[i], y = (u8)b[i];
    if( x>='A' && x<='Z' ) x += 32;
    if( y>='A' && y<='Z' ) y += 32;
    if( x!=y ) return 1;
  }
  return 0;
}

/* Root page of a table or index by name. */
static Pgno db_rootpage(const char *name){
  Table *t = sqlite3FindTable(db, name, "main");
  if( t ) return t->tnum;
  {
    Index *ix = sqlite3FindIndex(db, name, "main");
    if( ix ) return ix->tnum;
  }
  db_die("rootpage", 0);
  return 0;                       /* not reached */
}

/* Allocate a KeyInfo with nkey key fields plus nextra trailing fields,
** BINARY collation and ascending order throughout (aColl[] = 0 and
** aSortFlags[] = 0 are exactly that; sqlite3KeyInfoAlloc zeroes them). */
static KeyInfo *db_keyinfo(int nkey, int nextra){
  KeyInfo *ki = sqlite3KeyInfoAlloc(db, nkey, nextra);
  int i;
  if( ki==0 ) db_die("keyinfo", 0);
  for(i=0; i<nkey+nextra; i++) ki->aColl[i] = db->pDfltColl;
  return ki;
}

/* Verify a table against an expectation string of comma-separated
** "name:TYPE:alias" segments in declared column order. TYPE is compared
** case-insensitively against the declared type; alias is '1' iff the
** column is the table's single INTEGER primary key (the rowid alias,
** Table.iPKey), else '0'. Returns nonzero when the table matches. */
static int table_layout(const char *table, const char *expect){
  Table *t = sqlite3FindTable(db, table, "main");
  const char *s = expect;
  int i;
  if( t==0 ) return 0;
  if( t->tabFlags & TF_WithoutRowid ) return 0;
  for(i=0; i<t->nCol; i++){
    const char *c1 = strchr(s, ':');
    const char *c2 = c1 ? strchr(c1+1, ':') : 0;
    const char *next;
    const char *dty;
    u32 nlen, tlen;
    if( c1==0 || c2==0 ) return 0;
    next = strchr(c2+1, ',');
    nlen = (u32)(c1 - s);
    tlen = (u32)(c2 - c1 - 1);
    if( strlen(t->aCol[i].zCnName)!=nlen ) return 0;
    if( memcmp(s, t->aCol[i].zCnName, nlen)!=0 ) return 0;
    dty = sqlite3ColumnType(&t->aCol[i], "");
    if( strlen(dty)!=tlen || ci_eq(dty, c1+1, tlen) ) return 0;
    if( c2[1] != ((i==t->iPKey) ? '1' : '0') ) return 0;
    if( next==0 ) return (i+1==t->nCol) ? 1 : 0;
    s = next + 1;
  }
  return 0;
}

/* Verify an index against an expectation string of comma-separated key
** column names in index order (the trailing implicit rowid is not part
** of the key). Every key column must also be ASC (aSortOrder 0) with
** BINARY collation: a DESC or re-collated rebuild passes a name-only
** check but orders rows differently. Returns nonzero when the index
** matches. */
static int index_layout(const char *index, const char *expect){
  Index *ix = sqlite3FindIndex(db, index, "main");
  const char *s = expect;
  int i;
  if( ix==0 ) return 0;
  for(i=0; i<ix->nKeyCol; i++){
    if( ix->aSortOrder && ix->aSortOrder[i]!=0 ) return 0;
    if( ix->azColl && sqlite3_stricmp(ix->azColl[i], "BINARY")!=0 ) return 0;
    const char *next = strchr(s, ',');
    u32 len = next ? (u32)(next - s) : (u32)strlen(s);
    i16 cn = ix->aiColumn[i];
    const char *name;
    if( cn==XN_ROWID ){
      name = "rowid";
    }else if( cn<0 ){
      return 0;                     /* expression columns are not expected */
    }else{
      name = ix->pTable->aCol[cn].zCnName;
    }
    if( strlen(name)!=len || memcmp(name, s, len)!=0 ) return 0;
    if( next==0 ) return (i+1==ix->nKeyCol) ? 1 : 0;
    s = next + 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Background WAL checkpointer                                         */
/*                                                                     */
/* The wal hook (runs on the event-loop thread after each committing   */
/* write) records the WAL frame count and wakes the checkpointer when  */
/* it crosses CKPT_FRAMES. The checkpointer runs passive checkpoints   */
/* from a nice-19 thread so they never preempt the event loop; WAL     */
/* growth is additionally bounded at WAL_HIGH by db_wal_bound().       */
/* ------------------------------------------------------------------ */

#define CKPT_FRAMES 2000
#define WAL_HIGH    16384

static pthread_mutex_t ckpt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_cv = PTHREAD_COND_INITIALIZER;
static int ckpt_wanted;
static int wal_frames;
static int last_ckpt_frames;
static int ckpt_can_restart;
static int ckpt_run = 1;
static sqlite3 *ckpt_db;
static int wal_hook(void *arg, sqlite3 *d, const char *z, int n){
  (void)arg; (void)d; (void)z;
  int last = __atomic_load_n(&last_ckpt_frames, __ATOMIC_RELAXED);
  if( n < last ) __atomic_store_n(&last_ckpt_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&wal_frames, n, __ATOMIC_RELAXED);
  if( n - __atomic_load_n(&last_ckpt_frames, __ATOMIC_RELAXED) >= CKPT_FRAMES
   && !__atomic_load_n(&ckpt_wanted, __ATOMIC_RELAXED) ){
    pthread_mutex_lock(&ckpt_mu);
    __atomic_store_n(&ckpt_wanted, 1, __ATOMIC_RELAXED);
    pthread_cond_signal(&ckpt_cv);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return 0;   /* must be SQLITE_OK: the code propagates into sqlite3_step */
}

static void *ckpt_main(void *arg){
  (void)arg;
  /* Best effort: run checkpoints at the lowest priority so they never
  ** preempt the single-vCPU event loop. PASSIVE checkpoints coordinate
  ** through SQLite's shared WAL index and never block readers or writers. */
  setpriority(PRIO_PROCESS, 0, 19);
  for(;;){
    int nlog = 0, ndone = 0;
    pthread_mutex_lock(&ckpt_mu);
    while( __atomic_load_n(&ckpt_run, __ATOMIC_RELAXED) && !__atomic_load_n(&ckpt_wanted, __ATOMIC_RELAXED) ){
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 1;
      pthread_cond_timedwait(&ckpt_cv, &ckpt_mu, &ts);
    }
    if( !__atomic_load_n(&ckpt_run, __ATOMIC_RELAXED) ){
      pthread_mutex_unlock(&ckpt_mu);
      break;
    }
    pthread_mutex_unlock(&ckpt_mu);

    int rc = sqlite3_wal_checkpoint_v2(ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE,
                                       &nlog, &ndone);
    if( rc==SQLITE_OK || rc==SQLITE_BUSY ){
      if( nlog > 0 && nlog==ndone ){
        __atomic_store_n(&wal_frames, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&last_ckpt_frames, 0, __ATOMIC_RELAXED);
      }else if( ndone > 0 ){
        __atomic_store_n(&last_ckpt_frames, ndone, __ATOMIC_RELAXED);
        if( nlog - ndone <= 128 ){
          __atomic_store_n(&ckpt_can_restart, 1, __ATOMIC_RELEASE);
        }
      }
    }
    pthread_mutex_lock(&ckpt_mu);
    __atomic_store_n(&ckpt_wanted, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return 0;
}

static void ckpt_start(const char *path);
/* ------------------------------------------------------------------ */
/* Batch transactions                                                  */
/* ------------------------------------------------------------------ */


/* Open a batch: SQL-level BEGIN, force the b-tree read transaction open,
** and record the data version so writes from other connections since the
** previous batch are visible in db_foreign_write. */
static void db_batch_begin(void){
  u32 dv = 0;
  step_reset(st_begin);
  if( sqlite3BtreeBeginTrans(db_bt, 0, 0)!=SQLITE_OK ){
    db_die("begin trans", 0);
  }
  sqlite3BtreeGetMeta(db_bt, BTREE_DATA_VERSION, &dv);
  db_foreign_write = (db_version_known && dv!=db_version);
  db_version = dv;
  db_version_known = 1;
  db_in_batch = 1;
}

/* Force a passive checkpoint when the WAL has grown past WAL_HIGH and
** the background thread is idle. */
static void db_wal_bound(void){
  int nlog = 0, ndone = 0;
  int want = __atomic_load_n(&ckpt_can_restart, __ATOMIC_ACQUIRE);
  if( !want && __atomic_load_n(&wal_frames, __ATOMIC_RELAXED) < WAL_HIGH ) return;
  __atomic_store_n(&ckpt_can_restart, 0, __ATOMIC_RELAXED);
  if( sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE,
                                &nlog, &ndone)==SQLITE_OK
   && nlog==ndone ){
    __atomic_store_n(&wal_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&last_ckpt_frames, 0, __ATOMIC_RELAXED);
  }else if( ndone > 0 ){
    __atomic_store_n(&last_ckpt_frames, ndone, __ATOMIC_RELAXED);
  }
}
/* Close a batch: COMMIT the transaction (closing any leftover pool cursors first).
** Returns 0 when committed, -1 when rolled back. */
static int db_batch_end(void){
  int rc;
  if( !db_in_batch ) return -1;
  {
    int i;
    for(i=0; i<CURSOR_POOL; i++){
      if( cur_pool[i].used ){
        sqlite3BtreeCloseCursor(cur_pool[i].c);
        cur_pool[i].used = 0;
      }
    }
  }
  db_in_batch = 0;
  rc = sqlite3_step(st_commit);
  sqlite3_reset(st_commit);
  if( rc==SQLITE_DONE ){
    db_wal_bound();
    return 0;
  }
  rc = sqlite3_step(st_rollback);
  sqlite3_reset(st_rollback);
  if( rc!=SQLITE_DONE ){
    fprintf(stderr, "db: rollback (%d): %s\n", rc,
            db ? sqlite3_errmsg(db) : "not open");
    fflush(stderr);
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Page-cache wrapper                                                  */
/*                                                                     */
/* SQLite's default page cache (pcache1) behind its documented plugin interface */
/* (SQLITE_CONFIG_PCACHE2; technique credit: c-iouring-pavlyhalim #245), with   */
/* one decision                                                                 */
/* changed: when a commit asks the cache to drop the pages past the    */
/* end of the file (xTruncate), and no such page exists, nothing is    */
/* scanned.                                                            */
/*                                                                     */
/* Why: pcache1 remembers the largest page number it has ever held and */
/* never lowers it. A B-tree page split (balance_nonroot) briefly      */
/* renames a page to the lock-byte page's number, 1 GiB / page size    */
/* + 1, and back. From then on every commit's truncate looks as if     */
/* pages past the end of the file might exist, and pcache1 walks its   */
/* whole hash table to find none.                                      */
/*                                                                     */
/* The wrapper sees every key that enters the cache (xFetch with       */
/* create, xRekey) and keeps the largest one, not counting pages       */
/* parked at the lock-byte page's number, which it counts separately.  */
/* If nothing is parked and the largest key is below the limit, no     */
/* page at or past the limit can exist, so there is nothing to drop.   */
/* Otherwise it calls pcache1's truncate as usual. Pages that leave    */
/* the cache only make the record an overestimate, which is safe.      */
/*                                                                     */
/* Conservative forwarding: where the base cache's contract is ambiguous, the   */
/* wrapper forwards the operation directly to pcache1 (notes at wc_fetch,       */
/* wc_rekey and wc_truncate below).                                             */
/* ------------------------------------------------------------------ */

#define LOCK_BYTE_OFFSET 0x40000000u /* PENDING_BYTE in the file format */

struct wcache {
  sqlite3_pcache *base;
  Pgno max_key;   /* largest key ever entered, excluding the parked ones */
  Pgno lock_page; /* the lock-byte page's number for this page size */
  int n_parked;   /* pages currently renamed to lock_page */
};
static sqlite3_pcache_methods2 pcache_base;

static int wc_init(void *arg){
  (void)arg;
  return pcache_base.xInit(pcache_base.pArg);
}

static void wc_shutdown(void *arg){
  (void)arg;
  pcache_base.xShutdown(pcache_base.pArg);
}

static sqlite3_pcache *wc_create(int szPage, int szExtra, int bPurgeable){
  struct wcache *w = sqlite3_malloc(sizeof *w);
  if( w==0 ) return 0;
  w->base = pcache_base.xCreate(szPage, szExtra, bPurgeable);
  if( w->base==0 ){ sqlite3_free(w); return 0; }
  w->max_key = 0;
  w->lock_page = LOCK_BYTE_OFFSET / (unsigned)szPage + 1;
  w->n_parked = 0;
  return (sqlite3_pcache *)w;
}

static void wc_cachesize(sqlite3_pcache *p, int n){
  pcache_base.xCachesize(((struct wcache *)p)->base, n);
}

static int wc_pagecount(sqlite3_pcache *p){
  return pcache_base.xPagecount(((struct wcache *)p)->base);
}

static sqlite3_pcache_page *wc_fetch(sqlite3_pcache *p, unsigned key, int create){
  struct wcache *w = (struct wcache *)p;
  sqlite3_pcache_page *pg = pcache_base.xFetch(w->base, key, create);
  if( pg && create ){
    /* Fetching an already-parked page with create counts it a second
    ** time: n_parked can only drift upward, and an inflated parked count
    ** merely disables the truncate skip below (forward to base — the
    ** conservative direction). */
    if( key==w->lock_page ) w->n_parked++;
    else if( key>w->max_key ) w->max_key = key;
  }
  return pg;
}

static void wc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *pg, int discard){
  pcache_base.xUnpin(((struct wcache *)p)->base, pg, discard);
}

static void wc_rekey(sqlite3_pcache *p, sqlite3_pcache_page *pg,
                      unsigned old_key, unsigned new_key){
  struct wcache *w = (struct wcache *)p;
  pcache_base.xRekey(w->base, pg, old_key, new_key);
  /* Guarded decrement: when a parked page could have left the cache
  ** without rekey (ambiguous bookkeeping), leaving n_parked at zero can
  ** only forward the truncate; decrementing without a matching increment
  ** could undercount it and wrongly enable the skip. */
  if( old_key==w->lock_page && w->n_parked ) w->n_parked--;
  if( new_key==w->lock_page ) w->n_parked++;
  else if( new_key>w->max_key ) w->max_key = new_key;
}

static void wc_truncate(sqlite3_pcache *p, unsigned limit){
  struct wcache *w = (struct wcache *)p;
  if( w->n_parked==0 && w->max_key<limit ){
    return;
  }
  pcache_base.xTruncate(w->base, limit);
  /* With limit==0 the clamp is skipped (limit-1 would wrap): the
  ** unclamped max_key only overcounts, so the next truncate forwards. */
  if( limit>0 && w->max_key>=limit ) w->max_key = limit-1;
  if( w->lock_page>=limit ) w->n_parked = 0;
}

static void wc_destroy(sqlite3_pcache *p){
  struct wcache *w = (struct wcache *)p;
  pcache_base.xDestroy(w->base);
  sqlite3_free(w);
}

static void wc_shrink(sqlite3_pcache *p){
  pcache_base.xShrink(((struct wcache *)p)->base);
}

/* Install the wrapper exactly once, as the first statement of db_open
** and before sqlite3_open_v2(): sqlite3_config() is process-global and
** must run before sqlite3_initialize(), which is otherwise reached
** implicitly by the first sqlite3_open_v2. If either config call fails,
** SQLite's default pcache1 stays in place unchanged. */
static void pcache_install(void){
  static int installed;
  static sqlite3_pcache_methods2 m;
  if( installed ) return;
  installed = 1;
  if( sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &pcache_base)!=SQLITE_OK ){
    fprintf(stderr, "db: SQLITE_CONFIG_GETPCACHE2 failed, keeping the default page cache\n");
    return;
  }
  m = pcache_base;
  m.pArg = 0;
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
  if( sqlite3_config(SQLITE_CONFIG_PCACHE2, &m)!=SQLITE_OK ){
    fprintf(stderr, "db: SQLITE_CONFIG_PCACHE2 failed, keeping the default page cache\n");
    return;
  }
  fprintf(stderr, "db: page-cache wrapper installed over pcache1\n");
}
/* ------------------------------------------------------------------ */
/* WAL-write coalescing VFS shim over unix-excl (technique credit:    */
/* c-iouring-pavlyhalim #245, see README): SQLite writes each WAL     */
/* frame as two xWrite calls (24-byte header + 4 KiB page), and only  */
/* the last frame of a commit carries a nonzero db-size in header     */
/* bytes 4..7. Buffering contiguous appends until that commit frame's */
/* page arrives turns a multi-frame COMMIT into one pwrite() syscall. */
/* ------------------------------------------------------------------ */

#define VFS_NAME      "mh-wal"
#define WAL_FRAME_HDR 24
#define WAL_BUF_CAP   0x1ffff

struct wfile {
  sqlite3_file base;
  sqlite3_file *real;
  int is_wal;
  int commit;
  int len;
  sqlite3_int64 off;
  sqlite3_int64 last_end;
  u8 *buf;
};
static sqlite3_vfs *vfs_real;

static int wf_flush(struct wfile *f){
  if( !f->len ) return SQLITE_OK;
  int n = f->len;
  f->len = 0;
  return f->real->pMethods->xWrite(f->real, f->buf, n, f->off);
}
#define WF(p) ((struct wfile *)(p))
#define WF_FLUSH(p) do { int rc_ = wf_flush(WF(p)); if( rc_ != SQLITE_OK ) return rc_; } while(0)

static int wf_close(sqlite3_file *p){
  struct wfile *f = WF(p);
  int rc = wf_flush(f);
  int rc2 = f->real->pMethods ? f->real->pMethods->xClose(f->real) : SQLITE_OK;
  sqlite3_free(f->buf);
  f->buf = 0;
  return rc != SQLITE_OK ? rc : rc2;
}
static int wf_read(sqlite3_file *p, void *b, int n, sqlite3_int64 off){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xRead(WF(p)->real, b, n, off);
}
static int wf_write(sqlite3_file *p, const void *b, int n, sqlite3_int64 off){
  struct wfile *f = WF(p);
  if( !f->is_wal ) return f->real->pMethods->xWrite(f->real, b, n, off);
  int append = (off == f->last_end);
  f->last_end = off + n;
  if( f->len + n > WAL_BUF_CAP ) WF_FLUSH(p);
  if( !append || n > WAL_BUF_CAP || (!f->buf && !(f->buf = sqlite3_malloc(WAL_BUF_CAP))) ){
    WF_FLUSH(p);
    f->commit = 0;
    if( n==WAL_FRAME_HDR ){
      /* A direct-written frame header may itself be a commit header: its
      ** commit page is buffered next, so publish f->commit now or the page
      ** could sit in f->buf past walIndexWriteHdr (crash loses the commit). */
      const u8 *h = (const u8 *)b;
      f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    }
    return f->real->pMethods->xWrite(f->real, b, n, off);
  }
  if( !f->len ) f->off = off;
  memcpy(f->buf + f->len, b, (size_t)n);
  f->len += n;
  if( n == WAL_FRAME_HDR ){
    const u8 *h = (const u8 *)b;
    f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
    return SQLITE_OK;
  }
  if( f->commit ){
    f->commit = 0;
    return wf_flush(f);
  }
  return SQLITE_OK;
}
static int wf_truncate(sqlite3_file *p, sqlite3_int64 size){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xTruncate(WF(p)->real, size);
}
static int wf_sync(sqlite3_file *p, int flags){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xSync(WF(p)->real, flags);
}
static int wf_filesize(sqlite3_file *p, sqlite3_int64 *size){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xFileSize(WF(p)->real, size);
}
static int wf_lock(sqlite3_file *p, int l){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xLock(WF(p)->real, l);
}
static int wf_unlock(sqlite3_file *p, int l){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xUnlock(WF(p)->real, l);
}
static int wf_check_reserved(sqlite3_file *p, int *out){
  return WF(p)->real->pMethods->xCheckReservedLock(WF(p)->real, out);
}
static int wf_file_control(sqlite3_file *p, int op, void *arg){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xFileControl(WF(p)->real, op, arg);
}
static int wf_sector_size(sqlite3_file *p){ return WF(p)->real->pMethods->xSectorSize(WF(p)->real); }
static int wf_device_chars(sqlite3_file *p){ return WF(p)->real->pMethods->xDeviceCharacteristics(WF(p)->real); }
static int wf_shm_map(sqlite3_file *p, int pg, int sz, int extend, void volatile **out){
  return WF(p)->real->pMethods->xShmMap(WF(p)->real, pg, sz, extend, out);
}
static int wf_shm_lock(sqlite3_file *p, int off, int n, int flags){
  return WF(p)->real->pMethods->xShmLock(WF(p)->real, off, n, flags);
}
static void wf_shm_barrier(sqlite3_file *p){ WF(p)->real->pMethods->xShmBarrier(WF(p)->real); }
static int wf_shm_unmap(sqlite3_file *p, int del){ return WF(p)->real->pMethods->xShmUnmap(WF(p)->real, del); }
static int wf_fetch(sqlite3_file *p, sqlite3_int64 off, int n, void **pp){
  WF_FLUSH(p);
  return WF(p)->real->pMethods->xFetch(WF(p)->real, off, n, pp);
}
static int wf_unfetch(sqlite3_file *p, sqlite3_int64 off, void *pg){
  return WF(p)->real->pMethods->xUnfetch(WF(p)->real, off, pg);
}

static const sqlite3_io_methods wf_methods = {
  3, wf_close, wf_read, wf_write, wf_truncate, wf_sync,
  wf_filesize, wf_lock, wf_unlock, wf_check_reserved, wf_file_control, wf_sector_size,
  wf_device_chars, wf_shm_map, wf_shm_lock, wf_shm_barrier, wf_shm_unmap, wf_fetch,
  wf_unfetch
};

#define WF_REAL_OFFSET ((sizeof(struct wfile) + 7) & ~(size_t)7)

static int wv_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags, int *out_flags){
  (void)vfs;
  struct wfile *f = WF(p);
  memset(f, 0, sizeof *f);
  f->last_end = -1;
  f->real = (sqlite3_file *)((char *)p + WF_REAL_OFFSET);
  int rc = vfs_real->xOpen(vfs_real, name, f->real, flags, out_flags);
  if( f->real->pMethods == 0 ) return rc;
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  if( f->real->pMethods->iVersion < 3 ){
    f->real->pMethods->xClose(f->real);
    return SQLITE_CANTOPEN;
  }
  p->pMethods = &wf_methods;
  return rc;
}
static int wv_delete(sqlite3_vfs *v, const char *z, int sync){ (void)v; return vfs_real->xDelete(vfs_real, z, sync); }
static int wv_access(sqlite3_vfs *v, const char *z, int flags, int *out){ (void)v; return vfs_real->xAccess(vfs_real, z, flags, out); }
static int wv_full_pathname(sqlite3_vfs *v, const char *z, int n, char *out){ (void)v; return vfs_real->xFullPathname(vfs_real, z, n, out); }
static void *wv_dlopen(sqlite3_vfs *v, const char *z){ (void)v; return vfs_real->xDlOpen(vfs_real, z); }
static void wv_dlerror(sqlite3_vfs *v, int n, char *z){ (void)v; vfs_real->xDlError(vfs_real, n, z); }
static void (*wv_dlsym(sqlite3_vfs *v, void *h, const char *z))(void){ (void)v; return vfs_real->xDlSym(vfs_real, h, z); }
static void wv_dlclose(sqlite3_vfs *v, void *h){ (void)v; vfs_real->xDlClose(vfs_real, h); }
static int wv_randomness(sqlite3_vfs *v, int n, char *z){ (void)v; return vfs_real->xRandomness(vfs_real, n, z); }
static int wv_sleep(sqlite3_vfs *v, int us){ (void)v; return vfs_real->xSleep(vfs_real, us); }
static int wv_current_time(sqlite3_vfs *v, double *t){ (void)v; return vfs_real->xCurrentTime(vfs_real, t); }
static int wv_last_error(sqlite3_vfs *v, int n, char *z){ (void)v; return vfs_real->xGetLastError(vfs_real, n, z); }
static int wv_current_time64(sqlite3_vfs *v, sqlite3_int64 *t){ (void)v; return vfs_real->xCurrentTimeInt64(vfs_real, t); }

static const char *vfs_install(void){
  static int done;
  static const char *chosen = "unix-excl";
  static sqlite3_vfs v;
  if( done ) return chosen;
  done = 1;
  if( sqlite3_initialize() != SQLITE_OK ) return chosen;
  vfs_real = sqlite3_vfs_find("unix-excl");
  if( !vfs_real || vfs_real->iVersion < 2 ) return chosen;
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
  if( sqlite3_vfs_register(&v, 0) == SQLITE_OK ) chosen = VFS_NAME;
  return chosen;
}

static void ckpt_start(const char *path){
  pthread_t th;
  if( sqlite3_open_v2(path, &ckpt_db,
                      SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX, vfs_install())!=SQLITE_OK ){
    db_die("open ckpt", ckpt_db ? sqlite3_extended_errcode(ckpt_db) : 0);
  }
  sqlite3_exec(ckpt_db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=16; SELECT 1 FROM sqlite_schema LIMIT 1", 0, 0, 0);
  if( pthread_create(&th, 0, ckpt_main, 0)!=0 ){
    db_die("ckpt thread", 0);
  }
  pthread_detach(th);
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

static void db_open(const char *path){
  int i;
  pcache_install();
  if( sqlite3_open_v2(path, &db,
                      SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX, vfs_install())!=SQLITE_OK ){
    db_die("open", db ? sqlite3_extended_errcode(db) : 0);
  }

  db_exec("PRAGMA journal_mode=WAL");
  db_exec("PRAGMA synchronous=NORMAL");
  db_exec("PRAGMA temp_store=MEMORY");
  db_exec("PRAGMA cache_size=-16384");         /* 16 MiB parsed-page cache */
  db_exec("PRAGMA mmap_size=0");               /* parsed pages stay in page cache, avoiding WAL-index lookups per fetch */
  /* WAL growth is managed here, not by inline autocheckpoint on the loop. */
  db_exec("PRAGMA wal_autocheckpoint=0");
  sqlite3_wal_hook(db, wal_hook, 0);

  db_bt = db->aDb[0].pBt;

  st_begin = db_prepare("BEGIN");
  st_commit = db_prepare("COMMIT");
  st_rollback = db_prepare("ROLLBACK");

  /* Touch a real table so the schema is fully loaded before the layout
  ** checks (and any later sqlite3FindTable) run. */
  db_exec("SELECT count(*) FROM users");

  for(i=0; i<CURSOR_POOL; i++){
    cur_pool[i].c = malloc((size_t)sqlite3BtreeCursorSize());
    if( cur_pool[i].c==0 ) db_die("cursor pool", 0);
    cur_pool[i].used = 0;
  }

  ckpt_start(path);
}
