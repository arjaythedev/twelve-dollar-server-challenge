// The only C in this server besides SQLite: database glue for server.asm. It #includes the unmodified
// SQLite amalgamation because the B-tree functions and structures the read path needs are static inside
// it and have no public API. Everything here is ported from the database section of our C entry
// (submissions/c-iouring-unorig-username/server.c): db_open, btree_setup, the cursor helpers, the
// checkpoint worker thread and the WAL write coalescing.
//
// What lives here: opening the database (page cache, VFS system-call hooks, pragmas, WAL hook, the
// checkpoint thread), checking the schema the assembly's record decoder assumes, starting and ending the
// B-tree read transaction and its four cursors, cursor moves, the record bytes under a cursor, and a view
// of the likes cursor's page. What does not: decoding records, counting likes, the feed plan, every SQL
// statement (prepared and stepped by the assembly through the public sqlite3_* API), the checkpoint
// policy (when to ask, when to hold writes), JSON, HTTP, JWT and io_uring.
//
// SQLite's own state is never written from here except through sqlite3Btree* calls, plus the likes
// cursor's cell index, which td_likes_setix moves within the current leaf exactly as the C entry does.
#include "sqlite3.c"
#include <fcntl.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>

#ifndef CACHE_KB
#define CACHE_KB 32768  // SQLite page cache
#endif
#ifndef WALBUF
#define WALBUF 1        // 1: a commit's WAL frames go out in one pwrite (see wal_pwrite)
#endif
#ifndef WALKICK_MB
#define WALKICK_MB 8    // the assembly queues writeback of the log every this many MB; 0: off
#endif
#define STR2(x) #x
#define STR(x) STR2(x)

sqlite3 *td_db;            // the loop's connection: the assembly prepares and steps its statements on it
int td_bt_ok;              // 0: the schema is not the one the decoder knows, every read goes through SQL
int td_wal_frames;         // frames in the log, from the WAL hook; the assembly zeroes it after a full copy
int td_wal_asks = -1;      // eventfd: the loop asks for a checkpoint pass
int td_wal_replies = -1;   // eventfd: the pass's outcome (1 frames left, 2 all copied), read by a ring
int td_kick_fd = -1;       // the -wal file, for the assembly's sync_file_range writeback kick
int td_walkick_mb = WALKICK_MB;

static void *huge_alloc(size_t n) {  // on 2 MB pages where the kernel offers them (THP madvise mode)
  size_t hp = 2u << 20; n = (n + hp - 1) & ~(hp - 1);
  char *p = mmap(NULL, n + hp, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  char *a = (char *)(((uintptr_t)p + hp - 1) & ~(hp - 1));
  madvise(a, n, MADV_HUGEPAGE);
  return a;
}

// ---------- B-tree cursors: 0 posts, 1 users, 2 posts_created_at_id_idx, 3 likes_post_id_idx ----------
static Btree *bt;
static BtCursor *cur[4];
static Pgno root[4];
static KeyInfo *ki[4];
static int cur_open;

static void close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < 4; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}
// Called right after the assembly's BEGIN succeeded: the read side of the batch transaction. 1 ok, 0 failed
// (the caller rolls back).
int td_begin(void) {
  if (!td_bt_ok) return 1;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return 0;
  for (int i = 0; i < 4; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, root[i], 0, ki[i], cur[i]) != SQLITE_OK) {
      while (i--) sqlite3BtreeCloseCursor(cur[i]);
      return 0;
    }
  }
  return cur_open = 1;
}
// Before COMMIT, or once SQLite has ended the transaction by itself.
void td_end(void) { close_cursors(); }

// 1 on a row, 0 empty, -1 error
int td_first(int c) { int empty = 1; if (sqlite3BtreeFirst(cur[c], &empty) != SQLITE_OK) return -1; return !empty; }
// 1 on a row, 0 past the end, -1 error
int td_next(int c) { int rc = sqlite3BtreeNext(cur[c], 0); return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1; }
// Is rowid id in table cursor c? 1 yes, 0 no, -1 the seek failed.
int td_seek(int c, int64_t id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(cur[c], id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && cur[c]->eState == CURSOR_VALID;
}
// The record bytes under cursor c, valid until it moves; NULL if the record spills onto overflow pages.
const unsigned char *td_rec(int c, uint32_t *n) {
  BtCursor *k = cur[c];
  getCellInfo(k);
  if (k->info.nLocal != k->info.nPayload) return NULL;
  *n = k->info.nPayload;
  return k->info.pPayload;
}
// Moves the likes cursor to the first entry for a post at or above lo. 1 on an entry, 0 past the end, -1 on error.
int td_likes_from(int64_t lo) {
  BtCursor *k = cur[3];
  Mem key; UnpackedRecord want; int cmp = 0;
  memset(&key, 0, sizeof key); key.flags = MEM_Int; key.u.i = lo;
  memset(&want, 0, sizeof want); want.pKeyInfo = k->pKeyInfo; want.aMem = &key; want.nField = 1;
  want.default_rc = 1;  // an entry equal on post_id compares above the key, so the seek stops before the first of them
  if (sqlite3BtreeIndexMoveto(k, &want, &cmp) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(k)) return 0;
  if (cmp >= 0) return 1;
  int rc = sqlite3BtreeNext(k, 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}
// The likes cursor's current page, so the assembly can tally a leaf's cells without a call per cell.
// A cell starts at data + (mask & big-endian u16 at cellidx[2 * i]) + childptr.
typedef struct {
  const unsigned char *data, *cellidx, *end;
  uint32_t mask, ncell, ix, leaf, childptr, maxlocal;
} td_pv;
void td_page(td_pv *v) {
  BtCursor *k = cur[3]; MemPage *pg = k->pPage;
  v->data = pg->aData; v->cellidx = pg->aCellIdx; v->end = pg->aDataEnd;
  v->mask = pg->maskPage; v->ncell = pg->nCell; v->ix = k->ix; v->leaf = pg->leaf;
  v->childptr = pg->childPtrSize; v->maxlocal = pg->maxLocal;
}
// After tallying cells up to ix of the current page, the next td_next(3) continues after it.
void td_likes_setix(uint32_t ix) { cur[3]->ix = (u16)ix; }

static Pgno rootpage(const char *name) {
  sqlite3_stmt *st; Pgno r = 0;
  if (sqlite3_prepare_v2(td_db, "SELECT rootpage FROM sqlite_schema WHERE name = ?", -1, &st, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW) r = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return r;
}
static int layout_is(const char *sql, const char *want) {  // the decoder assumes these exact columns
  sqlite3_stmt *st; int ok = 0;
  if (sqlite3_prepare_v2(td_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
  if (sqlite3_step(st) == SQLITE_ROW) { const char *t = (const char *)sqlite3_column_text(st, 0); ok = t && !strcmp(t, want); }
  sqlite3_finalize(st);
  return ok;
}
static KeyInfo *key_info(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(td_db, nkey, nextra);
  if (!k) return NULL;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = td_db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}
static void btree_setup(void) {  // leaves td_bt_ok 0 (SQL for every read) on anything unexpected
  td_bt_ok = ENC(td_db) == SQLITE_UTF8
    && layout_is("SELECT group_concat(name) FROM pragma_table_info('posts')", "id,user_id,body,created_at")
    && layout_is("SELECT group_concat(name) FROM pragma_table_info('users')", "id,username,created_at")
    && layout_is("SELECT group_concat(name) FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id")
    && layout_is("SELECT group_concat(name) FROM pragma_index_info('likes_post_id_idx')", "post_id")
    && layout_is("SELECT group_concat(\"desc\") FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1")
    && layout_is("SELECT group_concat(\"desc\") FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "0")
    && (root[0] = rootpage("posts")) && (root[1] = rootpage("users"))
    && (root[2] = rootpage("posts_created_at_id_idx")) && (root[3] = rootpage("likes_post_id_idx"))
    && (ki[2] = key_info(2, 1, 1)) && (ki[3] = key_info(1, 1, 0));
  if (!td_bt_ok) { fprintf(stderr, "unexpected schema: reads use SQL\n"); return; }
  bt = td_db->aDb[0].pBt;
  for (int i = 0; i < 4; i++) cur[i] = malloc(sqlite3BtreeCursorSize());
}

// ---------- checkpoints ----------
// The log is copied back by this worker thread at low priority, on its own connection, so the loop never
// waits for an fsync. The loop (assembly) alone decides: it asks for a pass on td_wal_asks and the outcome
// comes back on td_wal_replies, which the loop reads as a ring completion.
enum { WAL_LEFT = 1, WAL_ALL };
static sqlite3 *wal_db;
static int wal_hook(void *u, sqlite3 *d, const char *n, int frames) { td_wal_frames = frames; return SQLITE_OK; }
static void *wal_worker(void *a) {
  uint64_t go, outcome;
  setpriority(PRIO_PROCESS, (id_t)gettid(), 19);
  while (read(td_wal_asks, &go, 8) == 8) {
    int log = -1, done = -2, rc = sqlite3_wal_checkpoint_v2(wal_db, "main", SQLITE_CHECKPOINT_PASSIVE, &log, &done);
    outcome = rc == SQLITE_OK && log == done ? WAL_ALL : WAL_LEFT;
    if (write(td_wal_replies, &outcome, 8) != 8) return NULL;
  }
  return NULL;
}

// SQLite writes each log frame as two pwrites, its 24-byte header and then the page, so a commit of n pages cost 2n
// syscalls. On the loop thread a commit's frames are gathered here and go out in one pwrite once the commit frame
// (nonzero size field in its header) is complete. SQLite updates the log index only after that, so no reader can
// look for a frame still held here; a read or stat of the same file writes them out first anyway.
static __thread int on_loop;
static char *wbuf; static size_t wlen, wcap; static off_t woff; static int wfd = -1, wcommit;
static ssize_t (*sys_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*sys_pread)(int, void *, size_t, off_t);
static int (*sys_fstat)(int, struct stat *);
static int wflush(void) {
  for (size_t done = 0; done < wlen;) {
    ssize_t r = sys_pwrite(wfd, wbuf + done, wlen - done, woff + done);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { wlen = 0; wfd = -1; if (!r) errno = EIO; return -1; }
    done += r;
  }
  wlen = 0; wfd = -1; return 0;
}
static ssize_t wal_pwrite(int fd, const void *p, size_t n, off_t off) {
  if (!on_loop) return sys_pwrite(fd, p, n, off);
  if (wlen && (fd != wfd || off != woff + (off_t)wlen) && wflush()) return -1;
  if (!wlen && n != 24) return sys_pwrite(fd, p, n, off);  // only a frame header starts a run
  if (!wlen) { wfd = fd; woff = off; }
  if (wlen + n > wcap) { wcap = (wlen + n) * 2; if (!(wbuf = realloc(wbuf, wcap))) abort(); }
  memcpy(wbuf + wlen, p, n); wlen += n;
  const unsigned char *h = p;
  if (n == 24) wcommit = h[4] | h[5] | h[6] | h[7];
  else if ((wcommit || wlen >= (1 << 20)) && wflush()) return -1;
  return n;
}
static ssize_t wal_pread(int fd, void *p, size_t n, off_t off) {
  if (wlen && fd == wfd && on_loop && wflush()) return -1;
  return sys_pread(fd, p, n, off);
}
static int wal_fstat(int fd, struct stat *s) {
  if (wlen && fd == wfd && on_loop && wflush()) return -1;
  return sys_fstat(fd, s);
}

// Opens the database for the loop thread (the caller) and starts the checkpoint thread. Exits on failure.
void td_open(const char *path) {
  {  // one contiguous page cache on huge pages instead of a malloc per page
    int hdr = 0; sqlite3_config(SQLITE_CONFIG_PCACHE_HDRSZ, &hdr);
    int sz = (4096 + hdr + 7) & ~7, npg = CACHE_KB / 4;
    sqlite3_config(SQLITE_CONFIG_PAGECACHE, huge_alloc((size_t)sz * npg), sz, npg);
  }
  sqlite3_initialize();  // built with SQLITE_OMIT_AUTOINIT
  sqlite3_vfs *v = sqlite3_vfs_find("unix-excl");  // the unix VFSes share one system call table
  on_loop = 1;
  if (WALBUF) {
    sys_pwrite = (void *)v->xGetSystemCall(v, "pwrite"); sys_pread = (void *)v->xGetSystemCall(v, "pread");
    sys_fstat = (void *)v->xGetSystemCall(v, "fstat");
    v->xSetSystemCall(v, "pwrite", (sqlite3_syscall_ptr)wal_pwrite); v->xSetSystemCall(v, "pread", (sqlite3_syscall_ptr)wal_pread);
    v->xSetSystemCall(v, "fstat", (sqlite3_syscall_ptr)wal_fstat);
  }
  // unix-excl: one process owns the file, the WAL index lives in its heap, and a transaction takes no file locks.
  int fl = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path, &td_db, fl, "unix-excl") != SQLITE_OK) { fprintf(stderr, "open %s: %s\n", path, sqlite3_errmsg(td_db)); exit(1); }
  sqlite3_busy_timeout(td_db, 5000);
  char *e = NULL;
  if (sqlite3_exec(td_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=0; PRAGMA cache_size=-" STR(CACHE_KB) "; PRAGMA wal_autocheckpoint=0;"
                   "SELECT count(*) FROM users WHERE id = 1;", NULL, NULL, &e) != SQLITE_OK) { fprintf(stderr, "pragma: %s\n", e); exit(1); }
  sqlite3_wal_hook(td_db, wal_hook, NULL);
  if (WALKICK_MB) { char w[4096]; snprintf(w, sizeof w, "%s-wal", path); td_kick_fd = open(w, O_RDONLY | O_CLOEXEC); }
  td_wal_asks = eventfd(0, EFD_CLOEXEC); td_wal_replies = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (td_wal_asks < 0 || td_wal_replies < 0 || sqlite3_open_v2(path, &wal_db, fl, "unix-excl") != SQLITE_OK ||
      sqlite3_exec(wal_db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=16;", NULL, NULL, NULL) != SQLITE_OK) { fprintf(stderr, "checkpoint connection failed\n"); exit(1); }
  pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 << 10);
  if (pthread_create(&t, &a, wal_worker, NULL)) { fprintf(stderr, "checkpoint thread failed\n"); exit(1); }
  btree_setup();
}
