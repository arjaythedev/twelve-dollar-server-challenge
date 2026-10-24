#include "rt.h"
/* SQLite access. This file is compiled in the same translation unit as the SQLite
 * amalgamation (see server.c), so generated read plans can drive SQLite's own B-tree layer
 * directly: the pager, WAL, locking and page cache are SQLite's; only the bytecode interpreter
 * (VDBE) is skipped for reads whose plans were derived ahead of time. Writes use SQL. */

#include <stdio.h>
#include <stdlib.h>

static sqlite3 *db;
static Btree *db_bt;
static int db_in_batch;      /* BEGIN executed, read transaction open */
static int db_wrote;         /* a write statement ran in this batch */
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;

static void db_die(const char *what) {
  fprintf(stderr, "fatal: %s: %s\n", what, db ? sqlite3_errmsg(db) : "no database");
  exit(1);
}
static sqlite3_stmt *db_prepare(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) db_die(sql);
  return s;
}
static void db_exec(const char *sql) {
  char *err = 0;
  if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "fatal: %s: %s\n", sql, err); exit(1); }
}
static int step_reset(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}
static Pgno db_rootpage(const char *name) {
  sqlite3_stmt *s = db_prepare("SELECT rootpage FROM sqlite_schema WHERE name = ?1");
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  Pgno r = sqlite3_step(s) == SQLITE_ROW ? (Pgno)sqlite3_column_int64(s, 0) : 0;
  sqlite3_finalize(s);
  if (!r) { fprintf(stderr, "fatal: no b-tree for %s\n", name); exit(1); }
  return r;
}
/* KeyInfo for an index whose leading key columns are compared with BINARY collation. */
static KeyInfo *db_keyinfo(int nkey, int nextra) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  if (!k) db_die("keyinfo");
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  return k;
}

static void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) db_die("open");
  /* Exclusive locking before the first read: the WAL index lives in heap memory, no shared
   * memory locks per transaction, and no other process can write while we run. That makes
   * this process the only writer, which the startup invariant check relies on. */
  db_exec("PRAGMA locking_mode = EXCLUSIVE");
  db_exec("PRAGMA journal_mode = WAL");
  db_exec("PRAGMA synchronous = NORMAL");
  db_exec("PRAGMA temp_store = MEMORY");
  db_exec("PRAGMA cache_size = -4096");   /* reads come from the mmap; the cache holds pages being written */
  db_exec("PRAGMA mmap_size = 1073741824");
  db_exec("PRAGMA wal_autocheckpoint = 4000");
  db_bt = db->aDb[0].pBt;
  st_begin = db_prepare("BEGIN");
  st_commit = db_prepare("COMMIT");
  st_rollback = db_prepare("ROLLBACK");
}

/* ---- batches --------------------------------------------------------------------------- */

/* A batch is one transaction: reads first (from the snapshot at batch start), then writes,
 * then COMMIT. Responses are released only after the batch ends. */
static void db_batch_begin(void) {
  if (step_reset(st_begin) != SQLITE_DONE) db_die("BEGIN");
  if (sqlite3BtreeBeginTrans(db_bt, 0, 0) != SQLITE_OK) db_die("read transaction");
  db_in_batch = 1;
  db_wrote = 0;
}
/* Returns 0 if the batch committed. */
static int db_batch_end(void) {
  db_in_batch = 0;
  if (step_reset(st_commit) == SQLITE_DONE) return 0;
  step_reset(st_rollback);
  return -1;
}

/* ---- cursors and records --------------------------------------------------------------- */

typedef struct { BtCursor *c; } cursor;

static cursor cursor_new(Pgno root, KeyInfo *ki) {
  cursor r;
  r.c = malloc((size_t)sqlite3BtreeCursorSize());
  if (!r.c) abort();
  sqlite3BtreeCursorZero(r.c);
  if (sqlite3BtreeCursor(db_bt, root, 0, ki, r.c) != SQLITE_OK) db_die("cursor");
  return r;
}
static void cursor_close(cursor *c) {
  sqlite3BtreeCloseCursor(c->c);
  free(c->c);
  c->c = 0;
}

/* Row payload. Records that spill onto overflow pages are copied into `spill`. */
typedef struct { const u8 *p; u32 n; } rec_t;
static u8 *rec_spill;
static u32 rec_spill_cap;

static int cur_record(cursor *c, rec_t *r) {
  u32 total = sqlite3BtreePayloadSize(c->c);
  u32 local;
  const u8 *p = sqlite3BtreePayloadFetch(c->c, &local);
  if (LIKELY(local == total)) { r->p = p; r->n = total; return 0; }
  if (total > rec_spill_cap) {
    free(rec_spill);
    rec_spill_cap = total + 4096;
    if (!(rec_spill = malloc(rec_spill_cap))) abort();
  }
  if (sqlite3BtreePayload(c->c, 0, total, rec_spill) != SQLITE_OK) return -1;
  r->p = rec_spill; r->n = total;
  return 0;
}
static inline i64 cur_rowid(cursor *c) { return sqlite3BtreeIntegerKey(c->c); }

/* Position on the last row. Returns 0 on a row, 1 if the table is empty, -1 on error. */
static int cur_last(cursor *c) {
  int empty;
  if (sqlite3BtreeLast(c->c, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}
/* Step backwards/forwards. Returns 0 on a row, 1 past the end, -1 on error. */
static int cur_prev(cursor *c) {
  int rc = sqlite3BtreePrevious(c->c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static int cur_next(cursor *c) {
  int rc = sqlite3BtreeNext(c->c, 0);
  return rc == SQLITE_OK ? 0 : rc == SQLITE_DONE ? 1 : -1;
}
static int cur_first(cursor *c) {
  int empty;
  if (sqlite3BtreeFirst(c->c, &empty) != SQLITE_OK) return -1;
  return empty ? 1 : 0;
}
/* Seek a table row by rowid. Returns 0 if found, 1 if absent, -1 on error. */
static int cur_seek(cursor *c, i64 rowid) {
  int res;
  if (sqlite3BtreeTableMoveto(c->c, rowid, 0, &res) != SQLITE_OK) return -1;
  return res == 0 ? 0 : 1;
}
/* Position an index cursor on the first entry whose leading integer key is >= k.
 * Returns 0 on an entry, 1 if there is none, -1 on error. */
static int idx_seek_ge(cursor *c, KeyInfo *ki, i64 k) {
  Mem m;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  UnpackedRecord r;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = ki;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1;
  int res;
  if (sqlite3BtreeIndexMoveto(c->c, &r, &res) != SQLITE_OK) return -1;
  if (res < 0) return cur_next(c);
  return sqlite3BtreeEof(c->c) ? 1 : 0;
}

/* Record format (https://sqlite.org/fileformat2.html#record_format). */
typedef struct { u64 t; const u8 *p; } col_t;

static inline const u8 *rd_varint(const u8 *p, const u8 *end, u64 *v) {
  u64 x = 0;
  for (int i = 0; i < 8; i++) {
    if (p >= end) return 0;
    x = x << 7 | (p[0] & 0x7f);
    if (!(*p++ & 0x80)) { *v = x; return p; }
  }
  if (p >= end) return 0;
  *v = x << 8 | *p++;
  return p;
}
static inline u32 serial_len(u64 t) {
  static const u8 n[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return t < 12 ? n[t] : (u32)((t - 12) / 2);
}
/* Decode the first ncol columns. Returns 0, or -1 if the record is malformed or shorter. */
static inline int rec_cols(const rec_t *r, int ncol, col_t *c) {
  const u8 *end = r->p + r->n;
  u64 hsz;
  const u8 *h = rd_varint(r->p, end, &hsz);
  if (!h || hsz > r->n) return -1;
  const u8 *hend = r->p + hsz, *body = hend;
  for (int i = 0; i < ncol; i++) {
    if (h >= hend) return -1;
    h = rd_varint(h, hend, &c[i].t);
    if (!h) return -1;
    c[i].p = body;
    body += serial_len(c[i].t);
  }
  return body <= end ? 0 : -1;
}
static inline int col_is_int(const col_t *c) { return (c->t >= 1 && c->t <= 6) || c->t == 8 || c->t == 9; }
static inline int col_is_text(const col_t *c) { return c->t >= 13 && (c->t & 1); }
static inline u32 col_len(const col_t *c) { return serial_len(c->t); }
static inline i64 col_int(const col_t *c) {
  const u8 *p = c->p;
  switch (c->t) {
  case 1: return (int8_t)p[0];
  case 2: return (int16_t)((u16)p[0] << 8 | p[1]);
  case 3: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8) >> 8;
  case 4: return (int32_t)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
  case 5: return (i64)((u64)(int16_t)((u16)p[0] << 8 | p[1]) << 32 | (u64)((u32)p[2] << 24 | (u32)p[3] << 16 | (u32)p[4] << 8 | p[5]));
  case 6: { u64 v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return (i64)v; }
  case 9: return 1;
  default: return 0;
  }
}

/* Startup check that a table has the column layout a generated plan was derived for:
 * "name:TYPE:rowid,..." where rowid marks an INTEGER PRIMARY KEY (the rowid alias). */
static int table_layout(const char *table, const char *expect) {
  char sql[256], got[1024];
  snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", table);
  sqlite3_stmt *s = db_prepare(sql);
  int pks = 0;
  while (sqlite3_step(s) == SQLITE_ROW) if (sqlite3_column_int(s, 5)) pks++;
  sqlite3_reset(s);
  size_t n = 0;
  got[0] = 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(s, 1), *type = (const char *)sqlite3_column_text(s, 2);
    int alias = pks == 1 && sqlite3_column_int(s, 5) == 1 && !sqlite3_stricmp(type, "INTEGER");
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s:%s:%d", n ? "," : "", name, type, alias);
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  if (strcmp(got, expect)) { fprintf(stderr, "table %s: layout %s, expected %s\n", table, got, expect); return 0; }
  return 1;
}
static int index_layout(const char *index, const char *expect) {
  char sql[256], got[512];
  snprintf(sql, sizeof sql, "PRAGMA index_info(\"%s\")", index);
  sqlite3_stmt *s = db_prepare(sql);
  size_t n = 0;
  got[0] = 0;
  while (sqlite3_step(s) == SQLITE_ROW && n < sizeof got)
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", (const char *)sqlite3_column_text(s, 2));
  sqlite3_finalize(s);
  if (strcmp(got, expect)) { fprintf(stderr, "index %s: keys %s, expected %s\n", index, got, expect); return 0; }
  return 1;
}
