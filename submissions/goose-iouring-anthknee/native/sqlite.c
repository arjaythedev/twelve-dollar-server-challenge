/* MIT. SQLite adapters derived from rust-iouring-kensac and goose-iouring-ahmadsh2007. */
/* GCC 13 LTO can report a false positive in sqlite3ColumnSetColl when it optimizes this helper. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
static __attribute__((noipa)) int sqlite3Strlen30(const char *);
#endif
#include "sqlite3.c"
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "goose.h"

/* PCACHE2 filter from PR #263. A B-tree split can park a page at the lock-page key. */
typedef struct {
  sqlite3_pcache *real;
  unsigned max_key, lock_page, parked;
} anth_cache;
static sqlite3_pcache_methods2 anth_pcache;

static int anth_pc_init(void *arg) { (void)arg; return anth_pcache.xInit(anth_pcache.pArg); }
static void anth_pc_shutdown(void *arg) { (void)arg; anth_pcache.xShutdown(anth_pcache.pArg); }
static sqlite3_pcache *anth_pc_create(int size, int extra, int purgeable) {
  anth_cache *p = sqlite3_malloc(sizeof *p);
  if (!p) return NULL;
  p->real = anth_pcache.xCreate(size, extra, purgeable);
  if (!p->real) { sqlite3_free(p); return NULL; }
  p->max_key = p->parked = 0;
  p->lock_page = 0x40000000u / (unsigned)size + 1;
  return (sqlite3_pcache *)p;
}
static void anth_pc_size(sqlite3_pcache *p, int n) { anth_pcache.xCachesize(((anth_cache *)p)->real, n); }
static int anth_pc_count(sqlite3_pcache *p) { return anth_pcache.xPagecount(((anth_cache *)p)->real); }
static void anth_pc_key(anth_cache *p, unsigned key) {
  if (key == p->lock_page) {
    if (p->parked != UINT_MAX) p->parked++;
  } else if (key > p->max_key) p->max_key = key;
}
static sqlite3_pcache_page *anth_pc_fetch(sqlite3_pcache *cache, unsigned key, int create) {
  anth_cache *p = (anth_cache *)cache;
  sqlite3_pcache_page *page = anth_pcache.xFetch(p->real, key, create);
  if (page && create) anth_pc_key(p, key);
  return page;
}
static void anth_pc_unpin(sqlite3_pcache *p, sqlite3_pcache_page *page, int discard) {
  anth_pcache.xUnpin(((anth_cache *)p)->real, page, discard);
}
static void anth_pc_rekey(sqlite3_pcache *cache, sqlite3_pcache_page *page, unsigned old, unsigned key) {
  anth_cache *p = (anth_cache *)cache;
  anth_pcache.xRekey(p->real, page, old, key);
  if (old == p->lock_page && p->parked && p->parked != UINT_MAX) p->parked--;
  anth_pc_key(p, key);
}
static void anth_pc_truncate(sqlite3_pcache *cache, unsigned limit) {
  anth_cache *p = (anth_cache *)cache;
  if (!p->parked && p->max_key < limit) return;
  anth_pcache.xTruncate(p->real, limit);
  if (limit && p->max_key >= limit) p->max_key = limit - 1;
  if (p->lock_page >= limit) p->parked = 0;
}
static void anth_pc_destroy(sqlite3_pcache *cache) {
  anth_cache *p = (anth_cache *)cache;
  anth_pcache.xDestroy(p->real);
  sqlite3_free(p);
}
static void anth_pc_shrink(sqlite3_pcache *p) { anth_pcache.xShrink(((anth_cache *)p)->real); }
static int anth_pc_install(void) {
  int rc = sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &anth_pcache);
  if (rc != SQLITE_OK) return rc;
  sqlite3_pcache_methods2 m = anth_pcache;
  m.pArg = NULL; m.xInit = anth_pc_init; m.xShutdown = anth_pc_shutdown;
  m.xCreate = anth_pc_create; m.xCachesize = anth_pc_size; m.xPagecount = anth_pc_count;
  m.xFetch = anth_pc_fetch; m.xUnpin = anth_pc_unpin; m.xRekey = anth_pc_rekey;
  m.xTruncate = anth_pc_truncate; m.xDestroy = anth_pc_destroy; m.xShrink = anth_pc_shrink;
  return sqlite3_config(SQLITE_CONFIG_PCACHE2, &m);
}

/* WAL coalescing from PR #263. Readers need the complete commit frame when SQLite publishes it. */
#define ANTH_WAL_CAP 0x1ffff /* unix VFS write-size limit */
#define ANTH_CKPT_CAP (64 * 1024)
#define ANTH_VFS "anthknee-wal"
typedef struct {
  sqlite3_file base;
  sqlite3_file *real;
  unsigned char *bytes;
  sqlite3_int64 offset, end;
  int length, page_size, is_wal, expect_page, commit, error;
  bool full_checkpoint, group_pages;
} anth_file;
static sqlite3_vfs *anth_vfs_base;

static void anth_wal_header(anth_file *f, const void *data, int n, sqlite3_int64 off) {
  if (!f->is_wal || off != 0 || n < 12) return;
  const unsigned char *b = data;
  unsigned magic = sqlite3Get4byte(b), size = sqlite3Get4byte(b + 8);
  f->page_size = (magic == 0x377f0682 || magic == 0x377f0683) &&
      size >= 512 && size <= 65536 && !(size & (size - 1)) ? (int)size : 0;
}
static int anth_flush(anth_file *f) {
  if (f->error) return f->error;
  if (!f->length) return SQLITE_OK;
  int rc = f->real->pMethods->xWrite(f->real, f->bytes, f->length, f->offset);
  if (rc == SQLITE_OK) f->length = 0;
  else f->error = rc;
  return rc;
}
#define AF(p) ((anth_file *)(p))
#define AF_FLUSH(p) do { int rc_ = anth_flush(AF(p)); if (rc_ != SQLITE_OK) return rc_; } while (0)
static int anth_f_close(sqlite3_file *p) {
  anth_file *f = AF(p);
  int rc = anth_flush(f);
  int closed = f->real->pMethods->xClose(f->real);
  sqlite3_free(f->bytes);
  f->bytes = NULL;
  return rc != SQLITE_OK ? rc : closed;
}
static int anth_f_read(sqlite3_file *p, void *data, int n, sqlite3_int64 off) {
  AF_FLUSH(p);
  anth_file *f = AF(p);
  /* A checksum rewrite reads a frame and replaces only its header. */
  f->expect_page = 0; f->end = -1;
  int rc = f->real->pMethods->xRead(f->real, data, n, off);
  if (rc == SQLITE_OK) anth_wal_header(f, data, n, off);
  return rc;
}
static int anth_f_write(sqlite3_file *p, const void *data, int n, sqlite3_int64 off) {
  anth_file *f = AF(p);
  if (f->group_pages) {
    if (f->error) return f->error;
    if (n > 0 && n < ANTH_CKPT_CAP) {
      if (f->length && (off != f->offset + f->length || f->length + n > ANTH_CKPT_CAP ||
                        f->length >= 16 * n)) AF_FLUSH(p);
      if (!f->bytes) f->bytes = sqlite3_malloc(ANTH_CKPT_CAP);
      if (f->bytes) {
        if (!f->length) f->offset = off;
        memcpy(f->bytes + f->length, data, (size_t)n);
        f->length += n;
        return SQLITE_OK;
      }
    }
    AF_FLUSH(p);
    f->group_pages = false;
  }
  if (!f->is_wal) return f->real->pMethods->xWrite(f->real, data, n, off);
  if (f->error) return f->error;
  int header = f->page_size && n == 24 && off == f->end && off >= 32 &&
      (off - 32) % (f->page_size + 24) == 0;
  int page = f->expect_page && off == f->end && n == f->page_size;
  if ((!header && !page) || (f->length && off != f->end) || f->length + n > ANTH_WAL_CAP) AF_FLUSH(p);
  if ((!header && !page) || (!f->bytes && !(f->bytes = sqlite3_malloc(ANTH_WAL_CAP)))) {
    AF_FLUSH(p);
    f->expect_page = f->commit = 0;
    int rc = f->real->pMethods->xWrite(f->real, data, n, off);
    if (rc == SQLITE_OK) anth_wal_header(f, data, n, off);
    else f->error = rc;
    f->end = off + n;
    return rc;
  }
  if (!f->length) f->offset = off;
  memcpy(f->bytes + f->length, data, (size_t)n);
  f->length += n; f->end = off + n;
  if (header) {
    f->expect_page = 1;
    f->commit = sqlite3Get4byte((const unsigned char *)data + 4) != 0;
  } else {
    f->expect_page = 0;
    if (f->commit) { f->commit = 0; return anth_flush(f); }
  }
  return SQLITE_OK;
}
static int anth_f_truncate(sqlite3_file *p, sqlite3_int64 n) {
  AF_FLUSH(p);
  AF(p)->expect_page = 0; AF(p)->end = -1;
  return AF(p)->real->pMethods->xTruncate(AF(p)->real, n);
}
static int anth_f_sync(sqlite3_file *p, int flags) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xSync(AF(p)->real, flags);
}
static int anth_f_size(sqlite3_file *p, sqlite3_int64 *n) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xFileSize(AF(p)->real, n);
}
static int anth_f_lock(sqlite3_file *p, int lock) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xLock(AF(p)->real, lock);
}
static int anth_f_unlock(sqlite3_file *p, int lock) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xUnlock(AF(p)->real, lock);
}
static int anth_f_reserved(sqlite3_file *p, int *out) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xCheckReservedLock(AF(p)->real, out);
}
static int anth_f_control(sqlite3_file *p, int op, void *arg) {
  if (op == SQLITE_FCNTL_CKPT_START) AF(p)->group_pages = AF(p)->full_checkpoint;
  if (op == SQLITE_FCNTL_CKPT_DONE) AF(p)->group_pages = false;
  AF_FLUSH(p); return AF(p)->real->pMethods->xFileControl(AF(p)->real, op, arg);
}
static int anth_f_sector(sqlite3_file *p) { return AF(p)->real->pMethods->xSectorSize(AF(p)->real); }
static int anth_f_device(sqlite3_file *p) { return AF(p)->real->pMethods->xDeviceCharacteristics(AF(p)->real); }
static int anth_f_map(sqlite3_file *p, int page, int size, int extend, void volatile **out) {
  return AF(p)->real->pMethods->xShmMap(AF(p)->real, page, size, extend, out);
}
static int anth_f_shmlock(sqlite3_file *p, int off, int n, int flags) {
  return AF(p)->real->pMethods->xShmLock(AF(p)->real, off, n, flags);
}
static void anth_f_barrier(sqlite3_file *p) { AF(p)->real->pMethods->xShmBarrier(AF(p)->real); }
static int anth_f_unmap(sqlite3_file *p, int del) { return AF(p)->real->pMethods->xShmUnmap(AF(p)->real, del); }
static int anth_f_fetch(sqlite3_file *p, sqlite3_int64 off, int n, void **out) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xFetch(AF(p)->real, off, n, out);
}
static int anth_f_unfetch(sqlite3_file *p, sqlite3_int64 off, void *page) {
  AF_FLUSH(p); return AF(p)->real->pMethods->xUnfetch(AF(p)->real, off, page);
}
static const sqlite3_io_methods anth_file_methods = {
  3, anth_f_close, anth_f_read, anth_f_write, anth_f_truncate, anth_f_sync, anth_f_size,
  anth_f_lock, anth_f_unlock, anth_f_reserved, anth_f_control, anth_f_sector, anth_f_device,
  anth_f_map, anth_f_shmlock, anth_f_barrier, anth_f_unmap, anth_f_fetch, anth_f_unfetch
};
#define ANTH_FILE_OFFSET ((sizeof(anth_file) + 7) & ~(size_t)7)
static int anth_vfs_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *p, int flags, int *out) {
  (void)vfs;
  anth_file *f = AF(p);
  memset(f, 0, sizeof *f);
  f->end = -1;
  f->real = (sqlite3_file *)((char *)p + ANTH_FILE_OFFSET);
  int rc = anth_vfs_base->xOpen(anth_vfs_base, name, f->real, flags, out);
  if (!f->real->pMethods) return rc;
  if (f->real->pMethods->iVersion < 3) {
    f->real->pMethods->xClose(f->real);
    return SQLITE_CANTOPEN;
  }
  f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
  p->pMethods = &anth_file_methods;
  return rc;
}
static int anth_vfs_install(void) {
  anth_vfs_base = sqlite3_vfs_find("unix-excl");
  if (!anth_vfs_base) return SQLITE_NOTFOUND;
  static sqlite3_vfs v;
  v = *anth_vfs_base;
  v.pNext = NULL; v.zName = ANTH_VFS; v.xOpen = anth_vfs_open;
  v.szOsFile = (int)(ANTH_FILE_OFFSET + (size_t)anth_vfs_base->szOsFile);
  return sqlite3_vfs_register(&v, 0);
}

typedef struct { const uint8_t *data; int64_t len; } bytes_t;
typedef struct { uint32_t *data; int64_t len; } counts_t;
typedef struct { const int64_t *data; int64_t len; } author_ids_t;
typedef struct { int64_t *data; int64_t len; } record_offsets_t;

enum { T_POSTS, T_USERS, T_FEED, T_LIKES, T_LIKE_EDGE, NTREES };

static sqlite3 *conn;
static Btree *btree;
static Pgno root[NTREES];
static KeyInfo *keys[NTREES];
static BtCursor *curs[NTREES];
static bool tx_open, tx_lost;
static sqlite3_stmt *q_begin, *q_commit, *q_rollback, *q_post, *q_like, *q_ping, *q_feed;
static volatile int wal_size;

static void say(gs_rref err, const char *what) {
  const char *m = conn ? sqlite3_errmsg(conn) : "no database";
  gs_bld_append(err, what, (int64_t)strlen(what));
  gs_bld_append(err, ": ", 2);
  gs_bld_append(err, m, (int64_t)strlen(m));
}

static int on_wal(void *u, sqlite3 *d, const char *n, int frames) {
  (void)u; (void)d; (void)n;
  wal_size = frames;
  return SQLITE_OK;
}

static void one_text(const char *sql, char *out, size_t cap) {
  sqlite3_stmt *s;
  out[0] = 0;
  if (sqlite3_prepare_v2(conn, sql, -1, &s, 0) != SQLITE_OK) return;
  if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0))
    snprintf(out, cap, "%s", (const char *)sqlite3_column_text(s, 0));
  sqlite3_finalize(s);
}

static sqlite3_stmt *prepare(const char *sql) {
  sqlite3_stmt *s = 0;
  return sqlite3_prepare_v3(conn, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) == SQLITE_OK ? s : 0;
}

static KeyInfo *index_keys(int nkey, bool desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(conn, nkey, 1);
  if (!k) return 0;
  for (int i = 0; i <= nkey; i++) k->aColl[i] = conn->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

static sqlite3 *ck_conn;
static pthread_mutex_t ck_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_wake = PTHREAD_COND_INITIALIZER;
static int ck_asked;
static int ck_status;          /* 0 idle, 1 running, 2 complete, 3 frames remain */
static int ck_quit;
static pthread_t ck_worker;

/* A busy lock can leave a partial checkpoint without a final sync to flush buffered pages. */
static int ck_busy(void *arg, int attempts) {
  (void)attempts;
  AF(arg)->full_checkpoint = false;
  return 0;
}

static void *ck_thread(void *arg) {
  anth_file *file = AF(arg);
#ifdef __linux__
  if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10)) perror("checkpoint nice");
#endif
  for (;;) {
    pthread_mutex_lock(&ck_lock);
    while (!ck_asked && !ck_quit) pthread_cond_wait(&ck_wake, &ck_lock);
    if (ck_quit) { pthread_mutex_unlock(&ck_lock); return NULL; }
    ck_asked = 0;
    pthread_mutex_unlock(&ck_lock);
    int in_log = -1, copied = -1;
    file->full_checkpoint = true;
    int rc = sqlite3_wal_checkpoint_v2(ck_conn, "main", SQLITE_CHECKPOINT_FULL, &in_log, &copied);
    /* A buffered write error from xTruncate prevents SQLite from advancing a full checkpoint. */
    assert(!file->error || rc != SQLITE_OK);
    file->full_checkpoint = file->group_pages = false;
    file->length = file->error = 0;
    __atomic_store_n(&ck_status, rc == SQLITE_OK && in_log >= 0 && in_log == copied ? 2 : 3, __ATOMIC_RELEASE);
  }
  return 0;
}

void sd_checkpoint_request(void) {
  if (__atomic_load_n(&ck_status, __ATOMIC_ACQUIRE) == 1) return;
  __atomic_store_n(&ck_status, 1, __ATOMIC_RELEASE);
  pthread_mutex_lock(&ck_lock);
  ck_asked = 1;
  pthread_cond_signal(&ck_wake);
  pthread_mutex_unlock(&ck_lock);
}

void sd_checkpoint_interrupt(void) { sqlite3_interrupt(ck_conn); }

int32_t sd_checkpoint_poll(void) {
  int s = __atomic_load_n(&ck_status, __ATOMIC_ACQUIRE);
  if (s >= 2) __atomic_store_n(&ck_status, 0, __ATOMIC_RELEASE);
  return s;
}

int64_t sd_wal_frames(void) { return wal_size; }

void sd_wal_reset(void) { wal_size = 0; }

int32_t sd_open(bytes_t path_b, gs_rref err) {
  char path[4096], got[256];
  if (path_b.len <= 0 || path_b.len >= (int64_t)sizeof path) { say(err, "SQLITE_PATH"); return -1; }
  memcpy(path, path_b.data, (size_t)path_b.len);
  path[path_b.len] = 0;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  const char *vfs = ANTH_VFS;
  if (anth_pc_install() != SQLITE_OK || sqlite3_initialize() != SQLITE_OK || anth_vfs_install() != SQLITE_OK) {
    const char *m = "SQLite page-cache/VFS setup failed";
    gs_bld_append(err, m, (int64_t)strlen(m));
    return -1;
  }
  if (sqlite3_open_v2(path, &conn, flags, vfs)) { say(err, "open"); return -1; }
  if (sqlite3_exec(conn,
                   "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-131072;"
                   "PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=0;"
                   "SELECT count(*) FROM sqlite_schema;", 0, 0, 0)) {
    say(err, "pragmas");
    return -1;
  }
  sqlite3_wal_hook(conn, on_wal, 0);
  static const char *want[][2] = {
    {"SELECT group_concat(name, ' ') FROM pragma_table_info('posts')", "id user_id body created_at"},
    {"SELECT group_concat(name, ' ') FROM pragma_table_info('users')", "id username created_at"},
    {"SELECT group_concat(name || ':' || desc, ' ') FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key",
     "created_at:1 id:1"},
    {"SELECT group_concat(name, ' ') FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT pk FROM pragma_table_info('posts') WHERE name = 'id'", "1"},
    {"SELECT type FROM pragma_table_info('posts') WHERE name = 'id'", "INTEGER"},
    {"SELECT type FROM pragma_table_info('users') WHERE name = 'id'", "INTEGER"},
  };
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
    one_text(want[i][0], got, sizeof got);
    if (strcmp(got, want[i][1]) != 0) {
      char m[512];
      snprintf(m, sizeof m, "unexpected schema (%s gave \"%s\", not \"%s\")", want[i][0], got, want[i][1]);
      gs_bld_append(err, m, (int64_t)strlen(m));
      return -1;
    }
  }
  if (ENC(conn) != SQLITE_UTF8) { say(err, "the database's text encoding is not UTF-8"); return -1; }
  static const char *trees[T_LIKE_EDGE] = {"posts", "users", "posts_created_at_id_idx", "likes_post_id_idx"};
  for (int i = 0; i < T_LIKE_EDGE; i++) {
    char sql[160];
    snprintf(sql, sizeof sql, "SELECT rootpage FROM sqlite_schema WHERE name = '%s'", trees[i]);
    one_text(sql, got, sizeof got);
    root[i] = (Pgno)atol(got);
    if (!root[i]) { say(err, "root page"); return -1; }
  }
  keys[T_FEED] = index_keys(2, true);
  keys[T_LIKES] = index_keys(1, false);
  root[T_LIKE_EDGE] = root[T_LIKES];
  keys[T_LIKE_EDGE] = keys[T_LIKES];
  q_begin = prepare("BEGIN");
  q_commit = prepare("COMMIT");
  q_rollback = prepare("ROLLBACK");
  q_post = prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  q_like = prepare("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  q_ping = prepare("SELECT 1");
  q_feed = prepare("SELECT p.id, p.body, p.created_at, u.username, "
                   "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                   "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  if (!keys[T_FEED] || !keys[T_LIKES] || !q_begin || !q_commit || !q_rollback || !q_post || !q_like ||
      !q_ping || !q_feed) {
    say(err, "prepare");
    return -1;
  }
  btree = conn->aDb[0].pBt;
  for (int i = 0; i < NTREES; i++)
    if (!(curs[i] = sqlite3_malloc(sqlite3BtreeCursorSize()))) { say(err, "cursor"); return -1; }
  if (sqlite3_open_v2(path, &ck_conn, flags, vfs) ||
      sqlite3_exec(ck_conn, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=16;", 0, 0, 0)) {
    say(err, "checkpoint connection");
    return -1;
  }
  sqlite3_file *ck_file = NULL;
  if (sqlite3_file_control(ck_conn, "main", SQLITE_FCNTL_FILE_POINTER, &ck_file) != SQLITE_OK ||
      !ck_file || ck_file->pMethods != &anth_file_methods ||
      sqlite3_busy_handler(ck_conn, ck_busy, ck_file) != SQLITE_OK) {
    say(err, "checkpoint VFS"); return -1;
  }
  pthread_attr_t a;
  int thread_rc = pthread_attr_init(&a);
  if (!thread_rc) {
    thread_rc = pthread_attr_setstacksize(&a, 256 << 10);
    if (!thread_rc) thread_rc = pthread_create(&ck_worker, &a, ck_thread, ck_file);
    int destroy_rc = pthread_attr_destroy(&a);
    if (destroy_rc) {
      fprintf(stderr, "checkpoint attributes: %s\n", strerror(destroy_rc));
      exit(1);
    }
  }
  if (thread_rc) {
    const char *m = strerror(thread_rc);
    gs_bld_append(err, "checkpoint thread: ", 19);
    gs_bld_append(err, m, (int64_t)strlen(m));
    return -1;
  }
  return 0;
}

static int run(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc;
}

static void close_cursors(void) {
  for (int i = 0; i < NTREES; i++) sqlite3BtreeCloseCursor(curs[i]);
}

int32_t sd_begin(void) {
  if (tx_lost) return -1;
  if (tx_open) {
    if (!sqlite3_get_autocommit(conn)) return 0;
    tx_lost = true;
    return -1;
  }
  if (run(q_begin) != SQLITE_DONE) return -1;
  if (sqlite3BtreeBeginTrans(btree, 0, 0) != SQLITE_OK) { run(q_rollback); return -1; }
  for (int i = 0; i < NTREES; i++) {
    sqlite3BtreeCursorZero(curs[i]);
    if (sqlite3BtreeCursor(btree, root[i], 0, keys[i], curs[i]) != SQLITE_OK) {
      while (--i >= 0) sqlite3BtreeCloseCursor(curs[i]);
      run(q_rollback);
      return -1;
    }
  }
  tx_open = true;
  return 0;
}

uint8_t sd_commit(void) {
  bool ok = true;
  if (tx_open) {
    close_cursors();
    tx_open = false;
    if (tx_lost || run(q_commit) != SQLITE_DONE) ok = false;
    if (!ok) fprintf(stderr, "commit failed: %s (%d)\n", sqlite3_errmsg(conn), sqlite3_extended_errcode(conn));
    if (!ok && !sqlite3_get_autocommit(conn)) run(q_rollback);
  } else if (tx_lost) {
    ok = false;
  }
  tx_lost = false;
  return ok;
}

static void after_failed_write(void) {
  if (tx_open && sqlite3_get_autocommit(conn)) tx_lost = true;
}

int32_t sd_seek(int32_t t, int64_t key) {
  int where = 0;
  if (sqlite3BtreeTableMoveto(curs[t], key, 0, &where) != SQLITE_OK) return -1;
  return where == 0 && curs[t]->eState == CURSOR_VALID ? 1 : 0;
}

int32_t sd_seek_ge(int32_t t, int64_t key) {
  BtCursor *c = curs[t];
  Mem v;
  UnpackedRecord probe;
  int where = 0;
  memset(&v, 0, sizeof v);
  v.flags = MEM_Int;
  v.u.i = key;
  memset(&probe, 0, sizeof probe);
  probe.pKeyInfo = c->pKeyInfo;
  probe.aMem = &v;
  probe.nField = 1;
  probe.default_rc = 1; /* Entries with the same leading key sort after this probe. */
  if (sqlite3BtreeIndexMoveto(c, &probe, &where) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (where < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
  }
  return 1;
}

int32_t sd_first(int32_t t) {
  int empty = 1;
  if (sqlite3BtreeFirst(curs[t], &empty) != SQLITE_OK) return -1;
  return empty ? 0 : 1;
}

int32_t sd_next(int32_t t) {
  int rc = sqlite3BtreeNext(curs[t], 0);
  return rc == SQLITE_OK ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int64_t sd_row(int32_t t, gs_rref out) {
  BtCursor *c = curs[t];
  u32 n = sqlite3BtreePayloadSize(c), local = 0;
  const void *p = sqlite3BtreePayloadFetch(c, &local);
  if (local == n) {
    gs_bld_append(out, p, n);
  } else {
    if (sqlite3BtreePayload(c, 0, n, out.stk->top) != SQLITE_OK) return -1;
    out.stk->top += n;
    out.hdr->len += n;
  }
  return n;
}

static int author_cell(MemPage *page, i64 id) {
  int low = 0, high = page->nCell;
  while (low < high) {
    int mid = low + (high - low) / 2;
    const u8 *key = findCellPastPtr(page, mid);
    if (page->leaf) {
      while (key < page->aDataEnd && *key >= 0x80) key++;
      if (key >= page->aDataEnd) return -1;
      key++;
    }
    if (key >= page->aDataEnd) return -1;
    i64 rowid = sqlite3VarintValue(key);
    if (rowid == id) return mid;
    if (rowid < id) low = mid + 1;
    else high = mid;
  }
  return page->leaf ? -1 : low;
}

/* The parent separator predicts a leaf slot. Only an exact rowid match can use it. */
static bool author_from_fence(MemPage *leaf, i64 id, i64 fence, bool upper, CellInfo *row) {
  u64 distance = upper ? (u64)fence - (u64)id : (u64)id - (u64)fence - 1;
  if (distance >= leaf->nCell) return false;
  int cell = upper ? leaf->nCell - 1 - (int)distance : (int)distance;
  leaf->xParseCell(leaf, findCell(leaf, cell), row);
  return row->nKey == id;
}

/* The batched lookup follows PR #245. Each username points into a pinned leaf page until the copy below. */
uint8_t sd_feed_author_names(author_ids_t ids, record_offsets_t offsets, gs_rref out) {
  enum { MAX_AUTHORS = 20 };
  assert(tx_open && ids.len > 0 && ids.len <= MAX_AUTHORS && offsets.len == ids.len + 1);
  Pgno next[MAX_AUTHORS];
  i64 fence[MAX_AUTHORS];
  bool upper[MAX_AUTHORS];
  MemPage *pinned[MAX_AUTHORS], *pages[MAX_AUTHORS];
  bytes_t names[MAX_AUTHORS];
  for (int i = 0; i < ids.len; i++) next[i] = root[T_USERS];

  int npinned = 0;
  for (int depth = 0; depth < BTCURSOR_MAX_DEPTH; depth++) {
    for (int i = 0; i < ids.len; i++) {
      int j = 0;
      while (j < npinned && pinned[j]->pgno != next[i]) j++;
      if (j == npinned) {
        if (next[i] < 2 ||
            getAndInitPage(btree->pBt, next[i], &pinned[j], PAGER_GET_READONLY) != SQLITE_OK) goto fallback;
        npinned++;
        MemPage *page = pinned[j];
        if (!page->intKey || !page->nCell || page->nOverflow || page->leaf != pinned[0]->leaf) goto fallback;
        __builtin_prefetch(page->aCellIdx);
      }
      pages[i] = pinned[j];
    }
    bool leaf = pinned[0]->leaf;
    for (int i = 0; i < ids.len; i++) {
      MemPage *page = pages[i];
      if (leaf) {
        CellInfo record;
        if (!depth || !author_from_fence(page, ids.data[i], fence[i], upper[i], &record)) {
          int cell = author_cell(page, ids.data[i]);
          if (cell < 0) goto fallback;
          page->xParseCell(page, findCell(page, cell), &record);
        }
        const u8 *p = record.pPayload;
        if (record.nLocal != record.nPayload || record.nPayload < 3 || p > page->aDataEnd ||
            (size_t)(page->aDataEnd - p) < record.nPayload) goto fallback;
        /* The rowid alias uses no data bytes. The username starts just after the record header. */
        unsigned header = p[0], kind = p[2];
        if (header < 3 || header >= 128 || header > record.nPayload || p[1] != 0 ||
            kind < 13 || kind >= 128 || !(kind & 1)) goto fallback;
        unsigned length = (kind - 13) / 2;
        if (length > record.nPayload - header) goto fallback;
        names[i] = (bytes_t){p + header, length};
      } else {
        int cell = author_cell(page, ids.data[i]);
        if (cell < 0) goto fallback;
        upper[i] = cell < page->nCell;
        const u8 *edge = findCell(page, upper[i] ? cell : cell - 1);
        if (edge >= page->aDataEnd || (size_t)(page->aDataEnd - edge) <= 4) goto fallback;
        fence[i] = sqlite3VarintValue(edge + 4);
        next[i] = upper[i] ? get4byte(edge) : get4byte(page->aData + page->hdrOffset + 8);
      }
    }
    if (leaf) {
      offsets.data[0] = out.hdr->len;
      for (int i = 0; i < ids.len; i++) {
        gs_bld_append(out, names[i].data, names[i].len);
        offsets.data[i + 1] = out.hdr->len;
      }
    }
    while (npinned) releasePageNotNull(pinned[--npinned]);
    if (leaf) return 1;
  }
fallback:
  while (npinned) releasePageNotNull(pinned[--npinned]);
  return 0;
}

static inline int first_int(const u8 *rec, const u8 *end, i64 *v) {
  if (rec + 2 > end || rec[0] >= 0x80 || rec[1] >= 0x80) return 0;
  if (rec[0] < 2 || rec[0] > (size_t)(end - rec)) return 0;
  const u8 *val = rec + rec[0];
  switch (rec[1]) {
  case 8: *v = 0; return 1;
  case 9: *v = 1; return 1;
  case 1: if (val + 1 > end) return 0; *v = (i8)val[0]; return 1;
  case 2: if (val + 2 > end) return 0; *v = (i16)(val[0] << 8 | val[1]); return 1;
  case 3: if (val + 3 > end) return 0; *v = ((int32_t)((u32)val[0] << 24 | (u32)val[1] << 16 | (u32)val[2] << 8)) >> 8; return 1;
  case 4: if (val + 4 > end) return 0; *v = (int32_t)((u32)val[0] << 24 | (u32)val[1] << 16 | (u32)val[2] << 8 | val[3]); return 1;
  default: return 0;
  }
}

/* The result is 0 on success, 1 for a cursor retry, or -1 on a database error. */
int32_t sd_like_counts(int64_t lo, int64_t hi, counts_t counts) {
  BtCursor *c = curs[T_LIKES];
  if (hi < lo || hi - lo >= counts.len) return 1;
  BtCursor *edge = curs[T_LIKE_EDGE];
  int empty = 1;
  if (sqlite3BtreeLast(edge, &empty) != SQLITE_OK) return -1;
  if (empty) return 0;
  u32 local = 0;
  const u8 *last = sqlite3BtreePayloadFetch(edge, &local);
  i64 last_id;
  if (local >= 2 && first_int(last, last + local, &last_id) && last_id < lo) return 0;
  int at = sd_seek_ge(T_LIKES, lo);
  if (at <= 0) return at;
  for (;;) {
    MemPage *pg = c->pPage;
    if (!pg->leaf) return 1;
    int i = c->ix;
    for (; i < pg->nCell; i++) {
      u8 *cell = pg->aData + (get2byte(&pg->aCellIdx[2 * i]) & pg->maskPage);
      u32 size;
      const u8 *rec = cell + getVarint32(cell, size);
      i64 id;
      if (!first_int(rec, pg->aDataEnd, &id)) return 1;
      if (id > hi) return 0;
      if (id >= lo) counts.data[id - lo]++;
    }
    c->ix = (u16)(pg->nCell - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

int32_t sd_add_post(int64_t user_id, bytes_t body, int64_t *id, gs_rref created) {
  sqlite3_bind_int64(q_post, 1, user_id);
  sqlite3_bind_text(q_post, 2, (const char *)body.data, (int)body.len, SQLITE_STATIC);
  int ok = sqlite3_step(q_post) == SQLITE_ROW;
  if (ok) {
    *id = sqlite3_column_int64(q_post, 0);
    const unsigned char *t = sqlite3_column_text(q_post, 1);
    int n = sqlite3_column_bytes(q_post, 1);
    ok = t && n > 0 && n < 64;
    if (ok) gs_bld_append(created, t, n);
    ok = ok && sqlite3_step(q_post) == SQLITE_DONE;
  }
  sqlite3_reset(q_post);
  sqlite3_clear_bindings(q_post);
  if (!ok) {
    fprintf(stderr, "insert failed: %s (%d)\n", sqlite3_errmsg(conn), sqlite3_extended_errcode(conn));
    after_failed_write();
  }
  return ok ? 0 : -1;
}

int32_t sd_add_like(int64_t user_id, int64_t post_id) {
  sqlite3_bind_int64(q_like, 1, user_id);
  sqlite3_bind_int64(q_like, 2, post_id);
  if (run(q_like) != SQLITE_DONE) { after_failed_write(); return -1; }
  return sqlite3_changes(conn) > 0 ? 1 : 0;
}

int32_t sd_ping(gs_rref err) {
  int ok = sqlite3_step(q_ping) == SQLITE_ROW && sqlite3_column_int(q_ping, 0) == 1;
  sqlite3_reset(q_ping);
  if (ok) return 0;
  const char *m = sqlite3_errmsg(conn);
  gs_bld_append(err, m, (int64_t)strlen(m));
  return -1;
}

#pragma pack(push, 1)
typedef struct { int64_t id, likes, body_len, created_len, author_len; } feed_row_t;
#pragma pack(pop)
int32_t sd_feed_step(feed_row_t *row, gs_rref text) {
  int rc = sqlite3_step(q_feed);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(q_feed);
    return rc == SQLITE_DONE ? 0 : -1;
  }
  row->id = sqlite3_column_int64(q_feed, 0);
  row->likes = sqlite3_column_int64(q_feed, 4);
  int64_t lens[3];
  for (int k = 0; k < 3; k++) {
    const unsigned char *s = sqlite3_column_text(q_feed, k + 1);
    lens[k] = sqlite3_column_bytes(q_feed, k + 1);
    gs_bld_append(text, s, lens[k]);
  }
  row->body_len = lens[0]; row->created_len = lens[1]; row->author_len = lens[2];
  return 1;
}

void sd_shutdown(void) {
  pthread_mutex_lock(&ck_lock);
  ck_quit = 1;
  pthread_cond_signal(&ck_wake);
  pthread_mutex_unlock(&ck_lock);
  int rc = pthread_join(ck_worker, NULL);
  if (rc) { fprintf(stderr, "checkpoint join: %s\n", strerror(rc)); exit(1); }
}
