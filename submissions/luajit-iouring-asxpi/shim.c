/*
 * The only C in this submission: a thin layer over the unmodified SQLite amalgamation.
 *
 * It exposes SQLite's own B-tree cursor API (seek, first, next, previous, payload) and five prepared
 * statements to LuaJIT. No HTTP, JWT or record decoding happens here: Lua gets the record bytes
 * SQLite returns and does the rest. The one exception is td_json_clean, a byte scan that tells
 * the JSON writer how much of a string needs no escaping. Nothing here writes SQLite's private
 * structs; cursors are only moved through sqlite3Btree* functions.
 *
 * WAL checkpoints run on a second thread with its own connection, so the event loop never waits
 * for their fsyncs. Both connections use SQLite's "unix-excl" VFS: the process owns the file and
 * the WAL index lives in heap memory.
 */
#include "vendor/sqlite3.c"
#include <stdint.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>

enum { C_POSTS, C_USERS, C_FEED, C_LIKES, NCUR };

static sqlite3 *db;
static Btree *bt;
static Pgno root[NCUR];
static KeyInfo *keyinfo[NCUR];
static BtCursor *cur[NCUR];
/* One more users cursor per feed slot (see td_user_row). */
#define NUCUR 20
static BtCursor *ucur[NUCUR];
static BtCursor *pcur[NUCUR];    /* and one posts cursor (see td_post_row) */
static int in_txn;
static UnpackedRecord *likes_key;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback, *st_post, *st_like, *st_health;
static char errbuf[512];

/* checkpoint thread */
static sqlite3 *ckdb;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int ck_req;
static volatile int ck_running, ck_log = -1, ck_done = -1, ck_rounds;
static volatile int wal_frames;
static pid_t ck_tid;

static int fail(const char *what, int rc) {
  sqlite3_snprintf(sizeof errbuf, errbuf, "%s: %s (%d)", what, db ? sqlite3_errmsg(db) : "no db", rc);
  return -1;
}
const char *td_errmsg(void) { return errbuf; }

static int same_cols(Table *t, const char **names, int n) {
  if (!t || t->nCol != n) return 0;
  for (int i = 0; i < n; i++)
    if (sqlite3_stricmp(t->aCol[i].zCnName, names[i])) return 0;
  return 1;
}

static KeyInfo *index_keyinfo(Index *ix) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, ix->nColumn, 0);
  if (!k) return 0;
  for (int i = 0; i < ix->nColumn; i++) {
    if (ix->azColl[i] != sqlite3StrBINARY) return 0;
    k->aColl[i] = 0;
    k->aSortFlags[i] = ix->aSortOrder[i];
  }
  return k;
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int nframe) {
  (void)arg; (void)d; (void)name;
  wal_frames = nframe;
  return SQLITE_OK;
}

/* ---- one write per commit: a VFS over "unix-excl" that changes only the WAL file's writes ----
 *
 * SQLite writes a transaction's frames one after another, each as two writes (a 24-byte header,
 * then the page): fifteen or more pwrite calls per commit here. While td_commit runs COMMIT,
 * writes to the WAL that continue where the last one ended are collected in a buffer, and the
 * buffer goes to the file when the commit frame's page arrives (the frame header's database-size
 * field is non-zero only for that frame). That is still inside SQLite's own write call, so the
 * commit is in the kernel before SQLite publishes it in the WAL index and before COMMIT returns.
 * Any other write, and every read, size query, sync, truncate and close, writes the buffer out
 * first. Everything else, and the database file, is unix-excl's. This is SQLite's public VFS
 * interface, the pattern of its own vfstrace and multiplex shims. TD_WALBUF=0 turns it off. */
typedef struct TdFile {
  sqlite3_file base;
  int is_wal;
  int commit_frame;            /* the frame header just buffered is a commit frame's */
  unsigned char *buf;
  int n, cap;
  sqlite3_int64 off;           /* file offset of buf[0] */
} TdFile;
#define TD_REAL(p) ((sqlite3_file *)((p) + 1))
static sqlite3_vfs *td_base;   /* unix-excl */
static sqlite3_vfs td_vfs;
static int td_in_commit;
static TdFile *td_wal;         /* the main connection's WAL */

static int td_flush(TdFile *p) {
  int rc = SQLITE_OK, done = 0;
  while (rc == SQLITE_OK && done < p->n) {
    int amt = p->n - done;
    if (amt > 65536) amt = 65536;
    rc = TD_REAL(p)->pMethods->xWrite(TD_REAL(p), p->buf + done, amt, p->off + done);
    done += amt;
  }
  p->n = 0; p->commit_frame = 0;
  return rc;
}
static int tdClose(sqlite3_file *f) {
  TdFile *p = (TdFile *)f;
  int rc = p->n ? td_flush(p) : SQLITE_OK;
  int rc2 = TD_REAL(p)->pMethods->xClose(TD_REAL(p));
  if (td_wal == p) td_wal = 0;
  sqlite3_free(p->buf); p->buf = 0;
  return rc ? rc : rc2;
}
static int tdRead(sqlite3_file *f, void *z, int amt, sqlite3_int64 off) {
  TdFile *p = (TdFile *)f;
  if (p->n) { int rc = td_flush(p); if (rc) return rc; }
  return TD_REAL(p)->pMethods->xRead(TD_REAL(p), z, amt, off);
}
static int tdWrite(sqlite3_file *f, const void *z, int amt, sqlite3_int64 off) {
  TdFile *p = (TdFile *)f;
  if (p->is_wal && td_in_commit && p == td_wal) {
    if (p->n && off != p->off + p->n) { int rc = td_flush(p); if (rc) return rc; }
    if (p->n + amt > p->cap) {
      if (p->n) { int rc = td_flush(p); if (rc) return rc; }
      if (amt > p->cap) {
        int cap = 1 << 16;
        while (cap < amt) cap <<= 1;
        if (cap > (1 << 20)) return TD_REAL(p)->pMethods->xWrite(TD_REAL(p), z, amt, off);
        unsigned char *nb = sqlite3_realloc(p->buf, cap);
        if (!nb) return SQLITE_NOMEM;
        p->buf = nb; p->cap = cap;
      }
    }
    if (p->n == 0) p->off = off;
    memcpy(p->buf + p->n, z, amt);
    p->n += amt;
    if (amt == 24) {                         /* a frame header: bytes 4..7 are the commit mark */
      const unsigned char *h = z;
      p->commit_frame = (h[4] | h[5] | h[6] | h[7]) != 0;
    } else if (p->commit_frame) {            /* the commit frame's page: the transaction is whole */
      return td_flush(p);
    }
    return SQLITE_OK;
  }
  if (p->n) { int rc = td_flush(p); if (rc) return rc; }
  return TD_REAL(p)->pMethods->xWrite(TD_REAL(p), z, amt, off);
}
static int tdTruncate(sqlite3_file *f, sqlite3_int64 sz) {
  TdFile *p = (TdFile *)f;
  if (p->n) { int rc = td_flush(p); if (rc) return rc; }
  return TD_REAL(p)->pMethods->xTruncate(TD_REAL(p), sz);
}
static int tdSync(sqlite3_file *f, int flags) {
  TdFile *p = (TdFile *)f;
  if (p->n) { int rc = td_flush(p); if (rc) return rc; }
  return TD_REAL(p)->pMethods->xSync(TD_REAL(p), flags);
}
static int tdFileSize(sqlite3_file *f, sqlite3_int64 *sz) {
  TdFile *p = (TdFile *)f;
  if (p->n) { int rc = td_flush(p); if (rc) return rc; }
  return TD_REAL(p)->pMethods->xFileSize(TD_REAL(p), sz);
}
static int tdLock(sqlite3_file *f, int l) { return TD_REAL((TdFile *)f)->pMethods->xLock(TD_REAL((TdFile *)f), l); }
static int tdUnlock(sqlite3_file *f, int l) { return TD_REAL((TdFile *)f)->pMethods->xUnlock(TD_REAL((TdFile *)f), l); }
static int tdCheckReservedLock(sqlite3_file *f, int *r) { return TD_REAL((TdFile *)f)->pMethods->xCheckReservedLock(TD_REAL((TdFile *)f), r); }
static int tdFileControl(sqlite3_file *f, int op, void *a) { return TD_REAL((TdFile *)f)->pMethods->xFileControl(TD_REAL((TdFile *)f), op, a); }
static int tdSectorSize(sqlite3_file *f) { return TD_REAL((TdFile *)f)->pMethods->xSectorSize(TD_REAL((TdFile *)f)); }
static int tdDeviceCharacteristics(sqlite3_file *f) { return TD_REAL((TdFile *)f)->pMethods->xDeviceCharacteristics(TD_REAL((TdFile *)f)); }
static int tdShmMap(sqlite3_file *f, int pg, int sz, int ext, void volatile **pp) { return TD_REAL((TdFile *)f)->pMethods->xShmMap(TD_REAL((TdFile *)f), pg, sz, ext, pp); }
static int tdShmLock(sqlite3_file *f, int off, int n, int flags) { return TD_REAL((TdFile *)f)->pMethods->xShmLock(TD_REAL((TdFile *)f), off, n, flags); }
static void tdShmBarrier(sqlite3_file *f) { TD_REAL((TdFile *)f)->pMethods->xShmBarrier(TD_REAL((TdFile *)f)); }
static int tdShmUnmap(sqlite3_file *f, int del) { return TD_REAL((TdFile *)f)->pMethods->xShmUnmap(TD_REAL((TdFile *)f), del); }
static int tdFetch(sqlite3_file *f, sqlite3_int64 off, int amt, void **pp) { return TD_REAL((TdFile *)f)->pMethods->xFetch(TD_REAL((TdFile *)f), off, amt, pp); }
static int tdUnfetch(sqlite3_file *f, sqlite3_int64 off, void *pg) { return TD_REAL((TdFile *)f)->pMethods->xUnfetch(TD_REAL((TdFile *)f), off, pg); }
static const sqlite3_io_methods td_io = {
  3, tdClose, tdRead, tdWrite, tdTruncate, tdSync, tdFileSize, tdLock, tdUnlock, tdCheckReservedLock,
  tdFileControl, tdSectorSize, tdDeviceCharacteristics, tdShmMap, tdShmLock, tdShmBarrier, tdShmUnmap,
  tdFetch, tdUnfetch
};
static int tdOpen(sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *f, int flags, int *out) {
  TdFile *p = (TdFile *)f;
  (void)v;
  memset(p, 0, sizeof *p);
  int rc = td_base->xOpen(td_base, name, TD_REAL(p), flags, out);
  if (rc == SQLITE_OK && TD_REAL(p)->pMethods) {
    p->base.pMethods = &td_io;
    p->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
    if (p->is_wal && !td_wal) td_wal = p;
  }
  return rc;
}
static int tdDelete(sqlite3_vfs *v, const char *n, int s) { (void)v; return td_base->xDelete(td_base, n, s); }
static int tdAccess(sqlite3_vfs *v, const char *n, int fl, int *r) { (void)v; return td_base->xAccess(td_base, n, fl, r); }
static int tdFullPathname(sqlite3_vfs *v, const char *n, int nOut, char *out) { (void)v; return td_base->xFullPathname(td_base, n, nOut, out); }
/* registers the VFS; returns its name, or "unix-excl" if it is switched off */
static const char *td_vfs_name(void) {
  const char *e = getenv("TD_WALBUF");
  td_base = sqlite3_vfs_find("unix-excl");
  if (!td_base || (e && e[0] == '0') || td_base->iVersion < 1) return "unix-excl";
  td_vfs = *td_base;
  td_vfs.zName = "td-walbuf";
  td_vfs.pNext = 0;
  td_vfs.szOsFile = (int)sizeof(TdFile) + td_base->szOsFile;
  td_vfs.xOpen = tdOpen;
  td_vfs.xDelete = tdDelete;
  td_vfs.xAccess = tdAccess;
  td_vfs.xFullPathname = tdFullPathname;
  if (sqlite3_vfs_register(&td_vfs, 0)) return "unix-excl";
  return td_vfs.zName;
}

/* our own descriptors of the two files, for syncing and cache advice from the checkpoint thread.
   Never closed: closing any descriptor of a file drops the process's locks on it. */
static int own_db_fd = -1, own_wal_fd = -1;
static char wal_path[1024];

static void *ck_main(void *arg) {
  (void)arg;
  struct sched_param sp = {0};
  ck_tid = (pid_t)syscall(SYS_gettid);
  sched_setscheduler(ck_tid, SCHED_IDLE, &sp);     /* runs when the event loop sleeps */
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    while (!ck_req) pthread_cond_wait(&ck_cv, &ck_mu);
    ck_req = 0;
    pthread_mutex_unlock(&ck_mu);
    int nlog = -1, ndone = -1;
    sqlite3_wal_checkpoint_v2(ckdb, 0, SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
    if (nlog >= 0 && ndone < nlog) {
      /* An incomplete round: SQLite only syncs the database file at the end of a round that
         copied the whole log, which here is the round that runs while writes are held. Sync
         what this round wrote now, off the loop, so that sync has little left to do. */
      if (own_db_fd >= 0) fdatasync(own_db_fd);
    } else if (nlog >= 0) {
      /* The whole log is copied: nobody reads these pages again, so let the kernel drop them
         instead of keeping up to hundreds of megabytes of old log in its cache. */
      if (own_wal_fd < 0 && wal_path[0]) own_wal_fd = open(wal_path, O_RDONLY | O_CLOEXEC);
      if (own_wal_fd >= 0) posix_fadvise(own_wal_fd, 0, 0, POSIX_FADV_DONTNEED);
    }
    ck_log = nlog; ck_done = ndone;
    ck_rounds++;
    __atomic_store_n(&ck_running, 0, __ATOMIC_RELEASE);
  }
  return 0;
}

/* Starts one PASSIVE checkpoint round on the thread. 1 = started, 0 = one is already running. */
int td_ckpt_start(void) {
  if (__atomic_load_n(&ck_running, __ATOMIC_ACQUIRE)) return 0;
  ck_running = 1;
  pthread_mutex_lock(&ck_mu);
  ck_req = 1;
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
  return 1;
}
/* The thread normally runs at idle priority, so a checkpoint never takes the CPU from the event
   loop. When the loop is saturated that would starve it and let the log grow without bound:
   on = 1 gives it normal priority until the log is small again. */
int td_ckpt_boost(int on) {
  struct sched_param sp = {0};
  if (!ck_tid) return -1;
  return sched_setscheduler(ck_tid, on ? SCHED_OTHER : SCHED_IDLE, &sp);
}
int td_ckpt_running(void) { return __atomic_load_n(&ck_running, __ATOMIC_ACQUIRE); }
/* frames left in the log after the last round (0 = everything is in the database file) */
int td_ckpt_left(void) { return (ck_log < 0 || ck_done < 0) ? -1 : ck_log - ck_done; }
int td_ckpt_rounds(void) { return ck_rounds; }
/* frames the last round had copied into the database file when it ended */
int td_ckpt_done(void) { return ck_done; }

int td_open(const char *path, int cache_kb, int autockpt) {
  int rc;
  char sql[256];
  (void)autockpt;
  sqlite3_initialize();
  const char *vfs = td_vfs_name();
  own_db_fd = open(path, O_RDWR | O_CLOEXEC);
  sqlite3_snprintf(sizeof wal_path, wal_path, "%s-wal", path);
  rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs);
  if (rc) return fail("open", rc);
  sqlite3_snprintf(sizeof sql, sql,
    "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
    "PRAGMA cache_size=-%d; PRAGMA wal_autocheckpoint=0;", cache_kb);
  rc = sqlite3_exec(db, sql, 0, 0, 0);
  if (rc) return fail("pragma", rc);
  sqlite3_wal_hook(db, wal_hook, 0);
  rc = sqlite3_open_v2(path, &ckdb, SQLITE_OPEN_READWRITE, "unix-excl");
  if (rc) return fail("open (checkpoint connection)", rc);
  rc = sqlite3_exec(ckdb, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=-2048;", 0, 0, 0);
  if (rc) return fail("pragma (checkpoint connection)", rc);
  pthread_t th;
  if (pthread_create(&th, 0, ck_main, 0)) return fail("checkpoint thread", 0);
  /* loads the schema */
  rc = sqlite3_exec(db, "SELECT 1 FROM posts, users, likes LIMIT 0", 0, 0, 0);
  if (rc) return fail("schema", rc);

  static const char *posts_cols[] = {"id", "user_id", "body", "created_at"};
  static const char *users_cols[] = {"id", "username", "created_at"};
  Table *posts = sqlite3FindTable(db, "posts", 0);
  Table *users = sqlite3FindTable(db, "users", 0);
  Index *feed = sqlite3FindIndex(db, "posts_created_at_id_idx", 0);
  Index *likes = sqlite3FindIndex(db, "likes_post_id_idx", 0);
  if (!same_cols(posts, posts_cols, 4) || !same_cols(users, users_cols, 3) || !feed || !likes
      || !HasRowid(posts) || !HasRowid(users) || posts->iPKey != 0 || users->iPKey != 0
      || feed->nColumn != 3 || feed->aiColumn[0] != 3 || feed->aiColumn[1] != 0
      || !feed->aSortOrder[0] || !feed->aSortOrder[1]
      || likes->nColumn != 2 || likes->aiColumn[0] != 1) {
    sqlite3_snprintf(sizeof errbuf, errbuf, "unexpected schema");
    return -1;
  }
  bt = db->aDb[0].pBt;
  root[C_POSTS] = posts->tnum;
  root[C_USERS] = users->tnum;
  root[C_FEED] = feed->tnum;
  root[C_LIKES] = likes->tnum;
  keyinfo[C_FEED] = index_keyinfo(feed);
  keyinfo[C_LIKES] = index_keyinfo(likes);
  if (!keyinfo[C_FEED] || !keyinfo[C_LIKES]) return fail("keyinfo", 0);
  likes_key = sqlite3VdbeAllocUnpackedRecord(keyinfo[C_LIKES]);
  if (!likes_key) return fail("unpacked", 0);
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) return fail("cursor", 0);
  }
  for (int i = 0; i < NUCUR; i++) {
    ucur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    pcur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!ucur[i] || !pcur[i]) return fail("cursor", 0);
  }

#define PREP(st, q) if ((rc = sqlite3_prepare_v3(db, q, -1, SQLITE_PREPARE_PERSISTENT, &st, 0))) return fail(q, rc)
  PREP(st_begin, "BEGIN");
  PREP(st_commit, "COMMIT");
  PREP(st_rollback, "ROLLBACK");
  PREP(st_post, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
  PREP(st_like, "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING");
  PREP(st_health, "SELECT 1");
  return 0;
}

static void close_cursors(void) {
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  for (int i = 0; i < NUCUR; i++) sqlite3BtreeCloseCursor(ucur[i]);
  for (int i = 0; i < NUCUR; i++) sqlite3BtreeCloseCursor(pcur[i]);
}

/* One transaction per event-loop pass: reads share a snapshot, writes are group-committed. */
int td_begin(void) {
  int rc = sqlite3_step(st_begin);
  sqlite3_reset(st_begin);
  if (rc != SQLITE_DONE) return fail("begin", rc);
  rc = sqlite3BtreeBeginTrans(bt, 0, 0);
  if (rc) { sqlite3_step(st_rollback); sqlite3_reset(st_rollback); return fail("begin read", rc); }
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    rc = sqlite3BtreeCursor(bt, root[i], 0, keyinfo[i], cur[i]);
    if (rc) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      sqlite3_step(st_rollback); sqlite3_reset(st_rollback);
      return fail("cursor", rc);
    }
  }
  for (int i = 0; i < NUCUR; i++) {
    sqlite3BtreeCursorZero(ucur[i]);
    sqlite3BtreeCursorZero(pcur[i]);
    rc = sqlite3BtreeCursor(bt, root[C_USERS], 0, 0, ucur[i]);
    if (!rc) {
      rc = sqlite3BtreeCursor(bt, root[C_POSTS], 0, 0, pcur[i]);
      if (rc) sqlite3BtreeCloseCursor(ucur[i]);
    }
    if (rc) {
      for (int j = 0; j < NCUR; j++) sqlite3BtreeCloseCursor(cur[j]);
      for (int j = 0; j < i; j++) { sqlite3BtreeCloseCursor(ucur[j]); sqlite3BtreeCloseCursor(pcur[j]); }
      sqlite3_step(st_rollback); sqlite3_reset(st_rollback);
      return fail("cursor", rc);
    }
  }
  in_txn = 1;
  return 0;
}

int td_commit(void) {
  if (!in_txn) return 0;
  in_txn = 0;
  close_cursors();
  td_in_commit = 1;
  int rc = sqlite3_step(st_commit);
  td_in_commit = 0;
  if (td_wal && td_wal->n && td_flush(td_wal) && rc == SQLITE_DONE) rc = SQLITE_IOERR;
  sqlite3_reset(st_commit);
  if (rc != SQLITE_DONE) {
    fail("commit", rc);
    if (!sqlite3_get_autocommit(db)) { sqlite3_step(st_rollback); sqlite3_reset(st_rollback); }
    return -1;
  }
  return 0;
}

/* Copies the record the cursor is on into buf. Returns its length, or -1. */
static int payload(int c, uint8_t *buf, uint32_t cap) {
  u32 amt, sz = sqlite3BtreePayloadSize(cur[c]);
  if (sz == 0 || sz > cap) return -1;
  const void *d = sqlite3BtreePayloadFetch(cur[c], &amt);
  if (amt >= sz) memcpy(buf, d, sz);
  else if (sqlite3BtreePayload(cur[c], 0, sz, buf)) return -1;   /* spills to overflow pages */
  return (int)sz;
}

/* Seek a table B-tree by rowid. Returns the record's length (the bytes are in buf), 0 if there is
   no such row, -1 on an error. */
int td_row(int c, int64_t rowid, uint8_t *buf, uint32_t cap) {
  int res;
  if (sqlite3BtreeTableMoveto(cur[c], rowid, 0, &res)) return -1;
  if (res) return 0;
  return payload(c, buf, cap);
}

/* The users row `rowid` through users cursor `k` (0 .. NUCUR-1), like td_row.
 *
 * The feed looks up twenty authors per request, a root-to-leaf descent each. Within one
 * transaction the feed's posts, and so their authors, are mostly the same from one request to the
 * next. The caller gives every feed post its own cursor (post id modulo NUCUR), so the next
 * request usually asks a cursor for the row it is already on, and sqlite3BtreeTableMoveto returns
 * at once: its own "already there" check, nothing done here. The record is fetched from SQLite's
 * page for every request, as with any other cursor; only the cursor's position is kept, and only
 * until the transaction ends (the cursors are closed at COMMIT). */
static int slot_row(BtCursor *c, int64_t rowid, uint8_t *buf, uint32_t cap) {
  int res;
  if (sqlite3BtreeTableMoveto(c, rowid, 0, &res)) return -1;
  if (res) return 0;
  u32 amt, sz = sqlite3BtreePayloadSize(c);
  if (sz == 0 || sz > cap) return -1;
  const void *d = sqlite3BtreePayloadFetch(c, &amt);
  if (amt >= sz) memcpy(buf, d, sz);
  else if (sqlite3BtreePayload(c, 0, sz, buf)) return -1;
  return (int)sz;
}
int td_user_row(int k, int64_t rowid, uint8_t *buf, uint32_t cap) { return slot_row(ucur[k], rowid, buf, cap); }

/* The posts row `rowid` through posts cursor `k`, in the same way: a feed post keeps its cursor
   on its own row from one request to the next. An INSERT into posts makes SQLite save the
   position of every cursor on that table, and the next use of each then searches from the root
   once: correct, and still far fewer searches than one per request. */
int td_post_row(int k, int64_t rowid, uint8_t *buf, uint32_t cap) { return slot_row(pcur[k], rowid, buf, cap); }

/* 1 = row exists */
int td_exists(int c, int64_t rowid) {
  int res;
  if (sqlite3BtreeTableMoveto(cur[c], rowid, 0, &res)) return -1;
  return res == 0;
}

/* Walk a B-tree in key order. Returns the entry's length, 0 at the end, -1 on an error. */
int td_first(int c, uint8_t *buf, uint32_t cap) {
  int res;
  if (sqlite3BtreeFirst(cur[c], &res)) return -1;
  if (res) return 0;
  return payload(c, buf, cap);
}

int td_next(int c, uint8_t *buf, uint32_t cap) {
  int rc = sqlite3BtreeNext(cur[c], 0);
  if (rc == SQLITE_DONE) return 0;
  if (rc) return -1;
  return payload(c, buf, cap);
}

int td_prev(int c, uint8_t *buf, uint32_t cap) {
  int rc = sqlite3BtreePrevious(cur[c], 0);
  if (rc == SQLITE_DONE) return 0;
  if (rc) return -1;
  return payload(c, buf, cap);
}

/* rowid of the entry a table cursor is on */
int64_t td_rowid(int c) { return sqlite3BtreeIntegerKey(cur[c]); }

/* Positions the likes_post_id_idx cursor on its first entry with post_id >= the argument, like
   OP_SeekGE. 1 = on an entry, 0 = none, -1 = error. */
static int likes_seek(int64_t post_id) {
  UnpackedRecord *k = likes_key;
  int res, rc;
  k->aMem[0].u.i = post_id;
  k->aMem[0].flags = MEM_Int;
  k->nField = 1;
  k->default_rc = 1;
  k->errCode = 0;
  k->eqSeen = 0;
  if (sqlite3BtreeIndexMoveto(cur[C_LIKES], k, &res)) return -1;
  if (res < 0) {
    rc = sqlite3BtreeNext(cur[C_LIKES], 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc) return -1;
  }
  return 1;
}

int td_likes_seek(int64_t post_id, uint8_t *buf, uint32_t cap) {
  int r = likes_seek(post_id);
  if (r != 1) return r;
  return payload(C_LIKES, buf, cap);
}

/* count(*) FROM likes WHERE post_id = ?: seek likes_post_id_idx, step while the key matches. */
int td_count_likes(int64_t post_id) {
  BtCursor *c = cur[C_LIKES];
  UnpackedRecord *k = likes_key;
  int rc, cnt = 0;
  int r = likes_seek(post_id);
  if (r != 1) return r;
  k->default_rc = 0;
  for (;;) {
    u32 amt, sz = sqlite3BtreePayloadSize(c);
    const void *d = sqlite3BtreePayloadFetch(c, &amt);
    if (amt < sz) return -1;
    if (sqlite3VdbeRecordCompare((int)sz, d, k)) break;
    cnt++;
    rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) break;
    if (rc) return -1;
  }
  return cnt;
}

/* 0 = ok (id and created_at filled in), -1 = error */
int td_insert_post(int64_t user_id, const char *body, int n, int64_t *id, char *created, int created_cap) {
  int rc, ok = -1;
  sqlite3_bind_int64(st_post, 1, user_id);
  sqlite3_bind_text(st_post, 2, body, n, SQLITE_STATIC);
  rc = sqlite3_step(st_post);
  if (rc == SQLITE_ROW) {
    const unsigned char *t = sqlite3_column_text(st_post, 1);
    int tn = sqlite3_column_bytes(st_post, 1);
    *id = sqlite3_column_int64(st_post, 0);
    if (t && tn < created_cap) {
      memcpy(created, t, tn);
      created[tn] = 0;
      rc = sqlite3_step(st_post);
      if (rc == SQLITE_DONE) ok = 0;
    }
  }
  if (ok) fail("insert post", rc);
  sqlite3_reset(st_post);
  return ok;
}

/* 1 = new like, 0 = already liked, -1 = error */
int td_insert_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(st_like, 1, user_id);
  sqlite3_bind_int64(st_like, 2, post_id);
  int rc = sqlite3_step(st_like);
  sqlite3_reset(st_like);
  if (rc != SQLITE_DONE) return fail("insert like", rc);
  return sqlite3_changes(db);
}

int td_health(void) {
  int rc = sqlite3_step(st_health);
  sqlite3_reset(st_health);
  return rc == SQLITE_ROW ? 0 : fail("health", rc);
}

/* Length of the prefix of p[0 .. n) that JSON can take as it is: up to the first byte below 0x20,
   '"' or '\\'. Eight bytes at a time: a word is flagged if any of its bytes is one of those. */
int td_json_clean(const uint8_t *p, int n) {
  const uint64_t L01 = 0x0101010101010101ULL, L80 = 0x8080808080808080ULL;
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t x;
    memcpy(&x, p + i, 8);
    uint64_t y = x ^ (L01 * 0x22), z = x ^ (L01 * 0x5c);
    if ((((x - L01 * 0x20) & ~x) | ((y - L01) & ~y) | ((z - L01) & ~z)) & L80) break;
  }
  for (; i < n; i++) {
    uint8_t c = p[i];
    if (c < 0x20 || c == '"' || c == '\\') break;
  }
  return i;
}

/* frames in the WAL after the last write commit */
int td_wal_frames(void) { return wal_frames; }
