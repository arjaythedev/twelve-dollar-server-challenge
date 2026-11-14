#include "rt.h"
/* SQLite access. This file is compiled in the same translation unit as the SQLite
 * amalgamation (see server.c), so generated read plans can drive SQLite's own B-tree layer
 * directly: the pager, WAL, locking and page cache are SQLite's; only the bytecode interpreter
 * (VDBE) is skipped for reads whose plans were derived ahead of time. Writes use SQL. */

#include <stdio.h>
#include <stdlib.h>

#ifndef DB_CACHE_KIB
#define DB_CACHE_KIB 1024
#endif
#ifndef DB_MMAP_BYTES
#define DB_MMAP_BYTES 1073741824
#endif
#define STR_(x) #x
#define XSTR(x) STR_(x)

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

/* ---- checkpoints ------------------------------------------------------------------------
 * WAL checkpoints copy committed pages into the database file and fsync it. Run inside the
 * event loop they stall every connection for as long as the disk takes, so they run on a
 * thread with its own connection (SQLite is built multi-thread: each connection is used by one
 * thread). A commit's durability does not depend on them (WAL, synchronous=NORMAL); the WAL
 * hook wakes the thread once the log passes CKPT_FRAMES. */
#include <pthread.h>
#ifndef CKPT_FRAMES
#define CKPT_FRAMES 1000
#endif
#define WAL_HIGH (CKPT_FRAMES > 4096 ? 4 * CKPT_FRAMES : 16384)   /* see db_wal_bound */

static sqlite3 *ckpt_db;
static pthread_mutex_t ckpt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_cv = PTHREAD_COND_INITIALIZER;
static int ckpt_wanted;
static int wal_frames;      /* frames in the log after the last commit */

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  if (frames >= CKPT_FRAMES && !__atomic_load_n(&ckpt_wanted, __ATOMIC_RELAXED)) {
    pthread_mutex_lock(&ckpt_mu);
    ckpt_wanted = 1;
    pthread_cond_signal(&ckpt_cv);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return SQLITE_OK;
}
static void *ckpt_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&ckpt_mu);
    while (!ckpt_wanted) pthread_cond_wait(&ckpt_cv, &ckpt_mu);
    pthread_mutex_unlock(&ckpt_mu);
    int nlog = 0, ndone = 0;
    int rc = sqlite3_wal_checkpoint_v2(ckpt_db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    if (rc != SQLITE_OK && rc != SQLITE_BUSY) fprintf(stderr, "checkpoint: %s\n", sqlite3_errmsg(ckpt_db));
    __atomic_store_n(&ckpt_wanted, 0, __ATOMIC_RELEASE);
  }
  return 0;
}
static void ckpt_start(const char *path) {
  if (sqlite3_open_v2(path, &ckpt_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) db_die("open (checkpointer)");
  sqlite3_exec(ckpt_db, "PRAGMA synchronous = NORMAL", 0, 0, 0);
  pthread_t t;
  if (pthread_create(&t, 0, ckpt_main, 0)) { perror("pthread_create"); exit(1); }
  pthread_detach(t);
}

/* ---- WAL write coalescing ---------------------------------------------------------------
 * SQLite writes each WAL frame with two writes (its 24-byte header, then the page), so a
 * commit of n pages costs 2n pwrite calls. The request connection opens its WAL through this
 * VFS, which keeps a run of contiguous writes in a buffer and writes it (in pieces of up to
 * 124 KiB) when the commit frame is complete (a frame header's "database size after commit" field is non-zero
 * only for the commit frame). That flush is inside the commit page's xWrite: before SQLite
 * publishes the frames in the WAL index, so the checkpoint thread never sees an unwritten
 * frame; a failed flush fails the commit; and every byte is in the file when COMMIT returns,
 * as without the buffer (synchronous=NORMAL issues no fsync at commit either). Any other
 * access to the WAL flushes first, and a write that does not continue the previous one (a
 * frame rewritten in place, walRewriteChecksums() fixing headers after the commit frame) is
 * written through, so the file always receives the writes in SQLite's order. Database files
 * are opened with the default VFS's own methods; only the WAL is wrapped. */
#ifndef DB_WALBUF
#define DB_WALBUF 1
#endif
#define WALBUF_CAP (256u << 10)

typedef struct {
  sqlite3_file base;
  sqlite3_file *real;      /* the default VFS's file, stored after this struct */
  u8 *buf;
  u32 n;                   /* buffered bytes, for file offset off.. */
  i64 off, next;           /* next: the end of the previous write */
  int commit;              /* the last frame header written was a commit frame's */
} walbuf_file;

static sqlite3_vfs *walbuf_base, walbuf_vfs;
#ifdef WALBUF_TRACE
static int walbuf_trace_fd = -1;
#define WTRACE(...) do { if (walbuf_trace_fd >= 0) dprintf(walbuf_trace_fd, __VA_ARGS__); } while (0)
#else
#define WTRACE(...) do { } while (0)
#endif

/* The unix VFS writes less than 128 KiB per call (seekAndWriteFd masks the length with
 * 0x1ffff and reports a longer write as SQLITE_FULL), so a long run goes out in pieces. A
 * failed flush keeps the whole run: SQLite may treat the error as failing only one statement
 * (a read) and go on to commit, so the bytes must still reach the file, in order, first. */
#define WALBUF_CHUNK 0x1f000u
static int walbuf_flush(walbuf_file *f) {
  for (u32 done = 0; done < f->n; ) {
    u32 k = f->n - done < WALBUF_CHUNK ? f->n - done : WALBUF_CHUNK;
    int rc = f->real->pMethods->xWrite(f->real, f->buf + done, (int)k, f->off + done);
    WTRACE("flush %lld %u rc=%d\n", (long long)(f->off + done), k, rc);
    if (rc) return rc;
    done += k;
  }
  f->n = 0;
  return SQLITE_OK;
}
#define WALBUF_FLUSH(file) do { int rc_ = walbuf_flush((walbuf_file *)(file)); if (rc_) return rc_; } while (0)
#define WALBUF_REAL(file) (((walbuf_file *)(file))->real)

static int walbuf_write(sqlite3_file *file, const void *p, int n, sqlite3_int64 o) {
  walbuf_file *f = (walbuf_file *)file;
  int header = n == 24, completes_commit = !header && f->commit;
  int contiguous = o == f->next && (u32)n <= WALBUF_CAP;
  f->commit = header && (((const u8 *)p)[4] | ((const u8 *)p)[5] | ((const u8 *)p)[6] | ((const u8 *)p)[7]);
  f->next = -1;                     /* set again once this write has succeeded */
  WTRACE("write %lld %d %s%s\n", (long long)o, n, contiguous ? "buf" : "through", f->commit ? " commit-hdr" : completes_commit ? " commit-page" : "");
  int rc;
  if (!contiguous) {
    rc = walbuf_flush(f);
    if (rc == SQLITE_OK) rc = f->real->pMethods->xWrite(f->real, p, n, o);
  } else if (f->n + (u32)n > WALBUF_CAP && (rc = walbuf_flush(f)) != SQLITE_OK) {
    return rc;
  } else {
    if (!f->n) f->off = o;
    memcpy(f->buf + f->n, p, (size_t)n);
    f->n += (u32)n;
    rc = completes_commit ? walbuf_flush(f) : SQLITE_OK;
  }
  if (rc == SQLITE_OK) f->next = o + n;
  return rc;
}
static int walbuf_close(sqlite3_file *file) {
  walbuf_file *f = (walbuf_file *)file;
  int rc = walbuf_flush(f);
  free(f->buf);
  f->buf = 0;
  int rc2 = f->real->pMethods->xClose(f->real);
  return rc ? rc : rc2;
}
static int walbuf_read(sqlite3_file *f, void *p, int n, sqlite3_int64 o) { WALBUF_FLUSH(f); WTRACE("read %lld %d\n", (long long)o, n); return WALBUF_REAL(f)->pMethods->xRead(WALBUF_REAL(f), p, n, o); }
static int walbuf_truncate(sqlite3_file *f, sqlite3_int64 size) { WALBUF_FLUSH(f); WTRACE("truncate %lld\n", (long long)size); return WALBUF_REAL(f)->pMethods->xTruncate(WALBUF_REAL(f), size); }
static int walbuf_sync(sqlite3_file *f, int flags) { WALBUF_FLUSH(f); WTRACE("sync\n"); return WALBUF_REAL(f)->pMethods->xSync(WALBUF_REAL(f), flags); }
static int walbuf_size(sqlite3_file *f, sqlite3_int64 *size) { WALBUF_FLUSH(f); return WALBUF_REAL(f)->pMethods->xFileSize(WALBUF_REAL(f), size); }
static int walbuf_lock(sqlite3_file *f, int l) { WALBUF_FLUSH(f); return WALBUF_REAL(f)->pMethods->xLock(WALBUF_REAL(f), l); }
static int walbuf_unlock(sqlite3_file *f, int l) { WALBUF_FLUSH(f); return WALBUF_REAL(f)->pMethods->xUnlock(WALBUF_REAL(f), l); }
static int walbuf_reserved(sqlite3_file *f, int *r) { return WALBUF_REAL(f)->pMethods->xCheckReservedLock(WALBUF_REAL(f), r); }
static int walbuf_control(sqlite3_file *f, int op, void *a) { WALBUF_FLUSH(f); return WALBUF_REAL(f)->pMethods->xFileControl(WALBUF_REAL(f), op, a); }
static int walbuf_sector(sqlite3_file *f) { return WALBUF_REAL(f)->pMethods->xSectorSize(WALBUF_REAL(f)); }
static int walbuf_device(sqlite3_file *f) { return WALBUF_REAL(f)->pMethods->xDeviceCharacteristics(WALBUF_REAL(f)); }
/* Version 1: SQLite maps and locks shared memory only through the database file. */
static const sqlite3_io_methods walbuf_methods = {
  1, walbuf_close, walbuf_read, walbuf_write, walbuf_truncate, walbuf_sync, walbuf_size,
  walbuf_lock, walbuf_unlock, walbuf_reserved, walbuf_control, walbuf_sector, walbuf_device,
};

static int walbuf_open(sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *file, int flags, int *out) {
  (void)v;
  if (!(flags & SQLITE_OPEN_WAL)) return walbuf_base->xOpen(walbuf_base, name, file, flags, out);
  walbuf_file *f = (walbuf_file *)file;
  memset(f, 0, sizeof *f);
  f->real = (sqlite3_file *)(f + 1);
  if (!(f->buf = malloc(WALBUF_CAP))) return SQLITE_NOMEM;
  int rc = walbuf_base->xOpen(walbuf_base, name, f->real, flags, out);
  if (rc != SQLITE_OK) { free(f->buf); f->buf = 0; return rc; }
  f->next = -1;
  f->base.pMethods = &walbuf_methods;
#ifdef WALBUF_TRACE
  if (getenv("WALBUF_TRACE")) walbuf_trace_fd = open(getenv("WALBUF_TRACE"), O_WRONLY | O_CREAT | O_APPEND, 0644);
  WTRACE("open\n");
#endif
  return SQLITE_OK;
}
static const char *walbuf_register(void) {
  if (!DB_WALBUF) return 0;
  walbuf_base = sqlite3_vfs_find(0);
  walbuf_vfs = *walbuf_base;
  walbuf_vfs.zName = "walbuf";
  walbuf_vfs.szOsFile = (int)sizeof(walbuf_file) + walbuf_base->szOsFile;
  walbuf_vfs.xOpen = walbuf_open;
  walbuf_vfs.pAppData = 0;
  if (sqlite3_vfs_register(&walbuf_vfs, 0) != SQLITE_OK) db_die("vfs");
  return walbuf_vfs.zName;
}

static void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, walbuf_register()) != SQLITE_OK) db_die("open");
  db_exec("PRAGMA journal_mode = WAL");
  db_exec("PRAGMA synchronous = NORMAL");
  db_exec("PRAGMA temp_store = MEMORY");
  db_exec("PRAGMA cache_size = -" XSTR(DB_CACHE_KIB));
  db_exec("PRAGMA mmap_size = " XSTR(DB_MMAP_BYTES));
  sqlite3_wal_hook(db, wal_hook, 0);      /* replaces automatic checkpoints */
  db_bt = db->aDb[0].pBt;
  st_begin = db_prepare("BEGIN");
  st_commit = db_prepare("COMMIT");
  st_rollback = db_prepare("ROLLBACK");
  ckpt_start(path);
}

/* ---- batches --------------------------------------------------------------------------- */

/* Commits by any other connection. SQLite's data version (BTREE_DATA_VERSION) changes only
 * when another connection commits; this server's own commits and checkpoints leave it alone.
 * The invariant checks assume this process is the only writer, so a change here turns off
 * every plan that relies on them (see app_batch). */
static u32 db_version;
static int db_version_known, db_foreign_write;

/* A batch is one transaction: reads first (from the snapshot at batch start), then writes,
 * then COMMIT. Responses are released only after the batch ends. */
static void db_batch_begin(void) {
  if (step_reset(st_begin) != SQLITE_DONE) db_die("BEGIN");
  if (sqlite3BtreeBeginTrans(db_bt, 0, 0) != SQLITE_OK) db_die("read transaction");
  u32 v;
  sqlite3BtreeGetMeta(db_bt, BTREE_DATA_VERSION, &v);
  if (db_version_known && v != db_version && !db_foreign_write) {
    db_foreign_write = 1;
    fprintf(stderr, "another connection wrote to the database: native read plans disabled\n");
  }
  db_version = v;
  db_version_known = 1;
  db_in_batch = 1;
  db_wrote = 0;
}
/* The log starts over only when a write begins after every frame has been checkpointed. With
 * commits arriving continuously, the background checkpoints can keep missing that moment and
 * the log keeps growing. Past WAL_HIGH frames, while the background thread is idle, this
 * thread checkpoints the remainder itself between batches (it cannot be inside a transaction
 * here), so the next write restarts the log. That stalls the loop for one fsync, only in this
 * case. */
static void db_wal_bound(void) {
  if (wal_frames < WAL_HIGH || __atomic_load_n(&ckpt_wanted, __ATOMIC_ACQUIRE)) return;
  int nlog = 0, ndone = 0;
  int rc = sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
  if (rc == SQLITE_OK) wal_frames = 0;
  static i64 last;
  if (time(0) != last) { last = time(0); fprintf(stderr, "wal: %d frames, checkpointed in the request thread (%s)\n", nlog, rc == SQLITE_OK ? "ok" : sqlite3_errmsg(db)); }
}

/* Returns 0 if the batch committed. */
static int db_batch_end(void) {
  db_in_batch = 0;
  if (step_reset(st_commit) == SQLITE_DONE) { db_wal_bound(); return 0; }
  step_reset(st_rollback);
  return -1;
}

/* ---- cursors and records --------------------------------------------------------------- */

/* Stable storage is reused, but every transaction opens fresh SQLite cursors and
 * closes them before commit. No row, page reference, or position survives a batch. */
typedef struct { BtCursor storage; BtCursor *c; u8 *spill; u32 spill_cap; } cursor;

static void cursor_open(cursor *r, Pgno root, KeyInfo *ki) {
  r->c = &r->storage;
  sqlite3BtreeCursorZero(r->c);
  if (sqlite3BtreeCursor(db_bt, root, 0, ki, r->c) != SQLITE_OK) db_die("cursor");
}
static void cursor_close(cursor *c) {
  sqlite3BtreeCloseCursor(c->c);
  c->c = 0;
}

/* Row payload is borrowed until this cursor moves or reads another record. Each cursor
 * owns its spill allocation: reading a different table must not overwrite live fields. */
#include "record.c"

static int cur_record(cursor *c, rec_t *r) {
  u32 total = sqlite3BtreePayloadSize(c->c);
  u32 local;
  const u8 *p = sqlite3BtreePayloadFetch(c->c, &local);
  if (LIKELY(local == total)) { r->p = p; r->n = total; return 0; }
  if (total > c->spill_cap) {
    free(c->spill);
    c->spill_cap = total + 4096;
    if (!(c->spill = malloc(c->spill_cap))) abort();
  }
  if (sqlite3BtreePayload(c->c, 0, total, c->spill) != SQLITE_OK) return -1;
  r->p = c->spill; r->n = total;
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
