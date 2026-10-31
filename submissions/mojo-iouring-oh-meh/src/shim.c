// shim.c: the C entry points the Mojo server calls.
//
// Two halves, both plumbing rather than logic:
//   1. io_uring: liburing's prep helpers are inline C, so each one gets a plain
//      exported wrapper. Rings, their fixed-file tables, provided buffers and
//      eventfds live here; the event loop, parsing, routing, auth, JSON and
//      the query plans are in Mojo.
//   2. SQLite: the amalgamation is compiled into this translation unit so the
//      B-tree layer (SQLITE_PRIVATE = static) is reachable, and exported as a
//      handful of cursor primitives. Record decoding is done in Mojo.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <liburing.h>

// ============================================================== io_uring rings

// 16 because the Mojo side packs the ring index into 4 bits of user_data.
#define MAX_RINGS 16

struct ring {
  struct io_uring ring;
  unsigned char *bufs;   // nbufs * bufsize, contiguous
  uint64_t *pending;     // bitmap of buffer ids returned since the last flush
  unsigned nbufs, bufsize, npending;
  int efd;
  int efd_registered;
  int live;
};

static struct ring rings[MAX_RINGS];
static int epfd = -1;
static unsigned cfg_sq, cfg_table, cfg_nbufs, cfg_bufsize;

// Returns the usable fixed-file table size under the current hard fd limit.
unsigned sh_table_size(void) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 0;
  rlim_t lim = rl.rlim_max;
  if (rl.rlim_cur < lim && setrlimit(RLIMIT_NOFILE, &(struct rlimit){lim, lim}) != 0) lim = rl.rlim_cur;
  if (lim > (1u << 20)) lim = 1u << 20;
  return lim > 256 ? (unsigned)lim - 64 : (unsigned)lim;
}

// Configure (nothing is created yet). Returns 0 or -errno.
int sh_init(unsigned sq_entries, unsigned table, unsigned nbufs, unsigned bufsize) {
  cfg_sq = sq_entries;
  cfg_table = table;
  cfg_nbufs = nbufs;
  cfg_bufsize = bufsize;
  epfd = epoll_create1(EPOLL_CLOEXEC);
  return epfd < 0 ? -errno : 0;
}

// Take an SQE; if the queue is full, flush it and try once more.
#define SQE_OR_FAIL(i)                                      \
  struct io_uring_sqe *s = io_uring_get_sqe(&rings[i].ring); \
  if (!s) {                                                  \
    io_uring_submit(&rings[i].ring);                         \
    s = io_uring_get_sqe(&rings[i].ring);                    \
    if (!s) return -1;                                       \
  }

// The eventfd only matters once the loop sleeps in epoll across several rings
// (a registered eventfd is signalled on every completion round, which is wasted
// work while one ring is served with submit-and-wait), so it is registered late.
static int ring_register_efd(struct ring *r) {
  if (r->efd_registered) return 0;
  int rc = io_uring_register_eventfd(&r->ring, r->efd);
  if (rc == 0) r->efd_registered = 1;
  return rc;
}

int sh_ring_enable_efd(int i) { return ring_register_efd(&rings[i]); }

// Create ring i (lazily). Returns 0 or -errno; a failed ring stays dead.
int sh_ring_open(int i) {
  if (i < 0 || i >= MAX_RINGS) return -EINVAL;
  struct ring *r = &rings[i];
  if (r->live) return 0;
  if (i > 0) {
    int rc = ring_register_efd(&rings[0]);
    if (rc < 0) return rc;
  }
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
            IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CLAMP;
  p.cq_entries = cfg_sq * 4;
  int rc = io_uring_queue_init_params(cfg_sq, &r->ring, &p);
  if (rc < 0) {
    fprintf(stderr, "ring %d: io_uring_queue_init: %s\n", i, strerror(-rc));
    return rc;
  }
  rc = io_uring_register_files_sparse(&r->ring, cfg_table);
  if (rc < 0) {
    fprintf(stderr, "ring %d: register_files_sparse(%u): %s\n", i, cfg_table, strerror(-rc));
    return rc;
  }
  r->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (r->efd < 0) return -errno;
  struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, r->efd, &ev) < 0) return -errno;
  if (i > 0) {
    rc = ring_register_efd(r);
    if (rc < 0) {
      fprintf(stderr, "ring %d: register_eventfd: %s\n", i, strerror(-rc));
      return rc;
    }
  }

  // Provided buffers, the classic IORING_OP_PROVIDE_BUFFERS kind: Ubuntu's 6.8 kernels
  // reject IORING_REGISTER_PBUF_RING, and this works with multishot recv on every 6.x.
  r->nbufs = cfg_nbufs;
  r->bufsize = cfg_bufsize;
  r->bufs = aligned_alloc(4096, (size_t)cfg_nbufs * cfg_bufsize);
  r->pending = calloc((cfg_nbufs + 63) / 64, sizeof(uint64_t));
  if (!r->bufs || !r->pending) return -ENOMEM;
  struct io_uring_sqe *s = io_uring_get_sqe(&r->ring);
  io_uring_prep_provide_buffers(s, r->bufs, (int)cfg_bufsize, (int)cfg_nbufs, 0, 0);
  rc = io_uring_submit_and_wait(&r->ring, 1);
  if (rc < 0) {
    fprintf(stderr, "ring %d: provide_buffers submit: %s\n", i, strerror(-rc));
    return rc;
  }
  struct io_uring_cqe *cqe = NULL;
  if (io_uring_peek_cqe(&r->ring, &cqe) != 0 || cqe->res < 0) {
    fprintf(stderr, "ring %d: provide_buffers: %s\n", i, cqe ? strerror(-cqe->res) : "no completion");
    return -EINVAL;
  }
  io_uring_cqe_seen(&r->ring, cqe);
  r->live = 1;
  return 0;
}

void *sh_buf_base(int i) { return rings[i].bufs; }

// Mark buffer bid as free; sh_buf_flush hands the batch back to the kernel.
void sh_buf_return(int i, unsigned bid) {
  struct ring *r = &rings[i];
  r->pending[bid >> 6] |= 1ull << (bid & 63);
  r->npending++;
}

// Re-provide every returned buffer, one SQE per contiguous run of ids. Buffers are
// consumed FIFO, so a batch's returns are mostly one or two runs. Returns 0, or -1 if
// the SQ was full even after a submit (the buffers stay pending for the next flush).
int sh_buf_flush(int i) {
  struct ring *r = &rings[i];
  if (!r->npending) return 0;
  unsigned words = (r->nbufs + 63) / 64;
  for (unsigned w = 0; w < words; w++) {
    while (r->pending[w]) {
      unsigned start = w * 64 + (unsigned)__builtin_ctzll(r->pending[w]);
      unsigned end = start;
      while (end + 1 < r->nbufs && (r->pending[(end + 1) >> 6] >> ((end + 1) & 63) & 1)) end++;
      SQE_OR_FAIL(i);
      io_uring_prep_provide_buffers(s, r->bufs + (size_t)start * r->bufsize, (int)r->bufsize,
                                    (int)(end - start + 1), 0, (int)start);
      s->flags |= IOSQE_CQE_SKIP_SUCCESS;
      for (unsigned b = start; b <= end; b++) r->pending[b >> 6] &= ~(1ull << (b & 63));
      r->npending -= end - start + 1;
    }
  }
  return 0;
}

int sh_listen(const char *host, int port, int backlog) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  int one = 1, err = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) err = EINVAL;
  else if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, backlog) < 0) err = errno;
  if (err) {
    close(fd);
    return -err;
  }
  return fd;
}

struct sh_cqe {
  int32_t res;
  uint32_t flags;
  uint64_t user_data;
};

int sh_submit(int i) { return io_uring_submit(&rings[i].ring); }

// Sleep until any live ring has work (or timeout_ms). Runs deferred task work on
// the rings that signalled and returns a bitmask of them.
unsigned sh_wait(int timeout_ms) {
  struct epoll_event evs[MAX_RINGS];
  int n = epoll_wait(epfd, evs, MAX_RINGS, timeout_ms);
  unsigned mask = 0;
  for (int k = 0; k < n; k++) {
    int i = (int)evs[k].data.u32;
    uint64_t v;
    (void)!read(rings[i].efd, &v, sizeof v);  // one read clears a non-semaphore eventfd
    io_uring_get_events(&rings[i].ring);
    mask |= 1u << i;
  }
  return mask;
}

// Single-ring fast path: one io_uring_enter submits pending SQEs and waits for a
// completion (deferred task work runs inside it). Returns the enter's result
// (number submitted, or -errno).
int sh_wait_one(int i) { return io_uring_submit_and_wait(&rings[i].ring, 1); }

unsigned sh_reap(int i, struct sh_cqe *out, unsigned max) {
  struct io_uring *r = &rings[i].ring;
  struct io_uring_cqe *cqe;
  unsigned head, n = 0;
  io_uring_for_each_cqe(r, head, cqe) {
    if (n == max) break;
    out[n].res = cqe->res;
    out[n].flags = cqe->flags;
    out[n].user_data = cqe->user_data;
    n++;
  }
  io_uring_cq_advance(r, n);
  return n;
}

// One-shot accept straight into a free slot of ring i's table. Bounded by the caller
// to the slots actually free, so a full table never costs a connection (multishot
// accept would overrun it in a burst and drop the overflow with ENFILE).
int sh_prep_accept(int i, int lfd, uint64_t ud) {
  SQE_OR_FAIL(i);
  io_uring_prep_accept_direct(s, lfd, NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
  io_uring_sqe_set_data64(s, ud);
  return 0;
}
int sh_prep_recv(int i, int slot, uint64_t ud) {
  SQE_OR_FAIL(i);
  io_uring_prep_recv_multishot(s, slot, NULL, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud);
  return 0;
}
int sh_prep_send(int i, int slot, const void *buf, unsigned len, uint64_t ud) {
  SQE_OR_FAIL(i);
  io_uring_prep_send(s, slot, buf, len, MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, ud);
  return 0;
}
// Shutdown both directions, hard-linked to the close: the outstanding multishot
// receive then completes (with EOF or an error) instead of pinning the socket open
// past the close. The shutdown's own completion carries `ud_shutdown` and is ignored.
int sh_prep_close(int i, int slot, uint64_t ud_shutdown, uint64_t ud) {
  struct io_uring *r = &rings[i].ring;
  // Both entries or neither: an SQE taken and not prepared would be submitted as
  // whatever it held last time.
  if (io_uring_sq_space_left(r) < 2) {
    io_uring_submit(r);
    if (io_uring_sq_space_left(r) < 2) return -1;
  }
  struct io_uring_sqe *s1 = io_uring_get_sqe(r);
  struct io_uring_sqe *s2 = io_uring_get_sqe(r);
  io_uring_prep_shutdown(s1, slot, SHUT_RDWR);
  s1->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  io_uring_sqe_set_data64(s1, ud_shutdown);
  io_uring_prep_close_direct(s2, slot);
  io_uring_sqe_set_data64(s2, ud);
  return 0;
}

// ============================================================== misc libc

void *sh_malloc(size_t n) { return malloc(n); }
void *sh_realloc(void *p, size_t n) { return realloc(p, n); }
void sh_free(void *p) { free(p); }

int64_t sh_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t sh_unix_time(void) { return (int64_t)time(NULL); }
const char *sh_getenv(const char *k) { return getenv(k); }
void sh_log(const char *s) { fputs(s, stderr); fputc('\n', stderr); fflush(stderr); }

// MemAvailable from /proc/meminfo, in KB (0 if unknown).
int64_t sh_mem_available_kb(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return 0;
  char line[128];
  int64_t kb = 0;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "MemAvailable: %" SCNd64 " kB", &kb) == 1) break;
  fclose(f);
  return kb;
}

// ============================================================== SQLite

#include "sqlite3.c"

void *sq_open(const char *path) {
  sqlite3 *db = NULL;
  sqlite3_initialize();
  // unix-excl: this process owns the file, the WAL index lives in heap memory and
  // no fcntl lock is taken per transaction. Both connections (loop + checkpointer)
  // are in this process, which is what that VFS is for.
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, "unix-excl") != SQLITE_OK) {
    fprintf(stderr, "sqlite3_open: %s\n", db ? sqlite3_errmsg(db) : "?");
    sqlite3_close(db);
    return NULL;
  }
  // No mmap: mapped pages bypass SQLite's page cache, so every page touch would
  // consult the WAL index first (walFindFrame was 12% of CPU). With a 32 MB page
  // cache the hot set (users, the tails of posts and likes_post_id_idx) is a cache hit.
  const char *pragmas =
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-32768;"
      "PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY; PRAGMA wal_autocheckpoint=0;";
  char *err = NULL;
  if (sqlite3_exec(db, pragmas, 0, 0, &err) != SQLITE_OK) {
    fprintf(stderr, "pragmas: %s\n", err);  // WAL + NORMAL is the design (rule 6); refuse to run without it
    sqlite3_free(err);
    sqlite3_close(db);
    return NULL;
  }
  return db;
}
// A throwaway connection without mmap and with a tiny cache, for the startup scan.
void *sq_open_plain(const char *path) {
  sqlite3 *db = NULL;
  sqlite3_initialize();
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, "unix-excl") != SQLITE_OK ||
      sqlite3_exec(db, "PRAGMA mmap_size=0; PRAGMA cache_size=-512;", 0, 0, 0) != SQLITE_OK) {
    sqlite3_close(db);
    return NULL;
  }
  return db;
}
int sq_close(void *db) { return sqlite3_close(db); }
// Ask the kernel to drop our database file's pages from the page cache (after the
// startup scan touched all of them). Only this process's own file is affected.
void sh_drop_file_cache(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  close(fd);
}
int sq_exec(void *db, const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0); }
const char *sq_errmsg(void *db) { return sqlite3_errmsg(db); }
void *sq_prepare(void *db, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, NULL) != SQLITE_OK) {
    fprintf(stderr, "prepare: %s -- %s\n", sqlite3_errmsg(db), sql);
    return NULL;
  }
  return st;
}
int sq_step(void *st) { return sqlite3_step(st); }
int sq_reset(void *st) { return sqlite3_reset(st); }
int sq_finalize(void *st) { return sqlite3_finalize(st); }
int64_t sq_col_int(void *st, int i) { return sqlite3_column_int64(st, i); }
const unsigned char *sq_col_text(void *st, int i) { return sqlite3_column_text(st, i); }
int sq_col_bytes(void *st, int i) { return sqlite3_column_bytes(st, i); }
int sq_bind_int(void *st, int i, int64_t v) { return sqlite3_bind_int64(st, i, v); }
// The bound text must stay alive until the statement is reset.
int sq_bind_text(void *st, int i, const char *p, int n) {
  return sqlite3_bind_text(st, i, p, n, SQLITE_STATIC);
}
int sq_changes(void *db) { return sqlite3_changes(db); }

// Frames currently in the WAL (0 if not in WAL mode).
int64_t sq_wal_frames(void *db) {
  Pager *pPager = ((sqlite3 *)db)->aDb[0].pBt->pBt->pPager;
  return pPager->pWal ? (int64_t)pPager->pWal->hdr.mxFrame : 0;
}
// Backfill what is left and reset the WAL to empty. Returns SQLITE_OK, or BUSY if the
// checkpointer thread holds the checkpoint lock right now (caller retries later).
int sq_wal_truncate(void *db) {
  int log = 0, ckpt = 0;
  return sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log, &ckpt);
}

// ---- B-tree layer. All calls on one connection, from one thread, inside a
// transaction (sq_exec "BEGIN" then bt_begin_read). Cursors are closed before COMMIT.

static Btree *main_bt(void *db) { return ((sqlite3 *)db)->aDb[0].pBt; }

// Starts the B-tree read transaction inside the SQL BEGIN; SQL COMMIT ends it.
int bt_begin_read(void *db) { return sqlite3BtreeBeginTrans(main_bt(db), 0, NULL); }

// A cursor object is allocated once and (re)opened per transaction.
struct bt_cur {
  BtCursor *c;
  KeyInfo *ki;           // index cursors only
  UnpackedRecord *rec;   // index cursors only: a one-integer seek key
  int open;
};

void *bt_cursor_alloc(void *db, int nkey) {
  sqlite3 *d = db;
  struct bt_cur *k = calloc(1, sizeof *k);
  if (!k) return NULL;
  k->c = calloc(1, (size_t)sqlite3BtreeCursorSize());
  if (!k->c) return NULL;
  sqlite3BtreeCursorZero(k->c);
  if (nkey > 0) {
    k->ki = sqlite3KeyInfoAlloc(d, nkey, 1);  // nkey key columns + rowid; BINARY collation, ASC
    if (!k->ki) return NULL;
    k->rec = sqlite3VdbeAllocUnpackedRecord(k->ki);
    if (!k->rec) return NULL;
    k->rec->nField = 1;
    k->rec->default_rc = 1;  // SeekGE: entries equal on the prefix compare as "greater", so the
                             // cursor lands just before the first of them (then bt_next if res<0)
    memset(k->rec->aMem, 0, sizeof(Mem));
    k->rec->aMem[0].flags = MEM_Null;
    k->rec->aMem[0].db = d;
  }
  return k;
}
int bt_open(void *db, void *kc, int root) {
  struct bt_cur *k = kc;
  int rc = sqlite3BtreeCursor(main_bt(db), (Pgno)root, 0, k->ki, k->c);
  k->open = rc == SQLITE_OK;
  return rc;
}
void bt_close(void *kc) {
  struct bt_cur *k = kc;
  if (k->open) sqlite3BtreeCloseCursor(k->c);
  k->open = 0;
}
int bt_last(void *kc, int *empty) { return sqlite3BtreeLast(((struct bt_cur *)kc)->c, empty); }
int bt_prev(void *kc) { return sqlite3BtreePrevious(((struct bt_cur *)kc)->c, 0); }  // SQLITE_DONE at start
int bt_next(void *kc) { return sqlite3BtreeNext(((struct bt_cur *)kc)->c, 0); }      // SQLITE_DONE at end
int bt_moveto(void *kc, int64_t rowid, int *res) {
  return sqlite3BtreeTableMoveto(((struct bt_cur *)kc)->c, rowid, 0, res);
}
// Seek an index cursor for key (first column). Mirrors OP_SeekGE: afterwards, if
// *res < 0 the cursor is on an entry smaller than key and the caller does bt_next;
// then the cursor is on the first entry whose first column >= key (or at EOF).
int bt_idx_seek_int(void *kc, int64_t key, int *res) {
  struct bt_cur *k = kc;
  sqlite3VdbeMemSetInt64(&k->rec->aMem[0], key);
  return sqlite3BtreeIndexMoveto(k->c, k->rec, res);
}
int bt_eof(void *kc) { return sqlite3BtreeEof(((struct bt_cur *)kc)->c); }
int64_t bt_rowid(void *kc) { return sqlite3BtreeIntegerKey(((struct bt_cur *)kc)->c); }
uint32_t bt_payload_size(void *kc) { return sqlite3BtreePayloadSize(((struct bt_cur *)kc)->c); }
// PayloadFetch requires the cell to be parsed already (3.54 asserts info.nSize>0);
// PayloadSize does that parse when needed, so call it first.
const void *bt_payload_fetch(void *kc, uint32_t *amt) {
  BtCursor *c = ((struct bt_cur *)kc)->c;
  (void)sqlite3BtreePayloadSize(c);
  return sqlite3BtreePayloadFetch(c, amt);
}
int bt_payload(void *kc, uint32_t off, uint32_t n, void *buf) {
  return sqlite3BtreePayload(((struct bt_cur *)kc)->c, off, n, buf);
}

// ---- Checkpoint thread: its own connection, passive checkpoints on a timer so
// the event loop never waits on an fsync.

static struct {
  char *path;
  int interval_ms;
} ck;

static void *ck_main(void *arg) {
  (void)arg;
  sqlite3 *db = NULL;
  if (sqlite3_open_v2(ck.path, &db, SQLITE_OPEN_READWRITE, "unix-excl") != SQLITE_OK) {
    fprintf(stderr, "checkpointer: open failed\n");
    sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db, 50);
  // A connection that has never run a statement has not opened the WAL, and a
  // checkpoint on it is a no-op (frame counts come back -1). One read fixes that.
  sqlite3_exec(db, "SELECT 1 FROM sqlite_master LIMIT 1", 0, 0, 0);
  int last_log = -1, idle = 0, reported = 0;
  for (;;) {
    // back off to 10x the interval when the WAL hasn't changed (idle server)
    usleep((useconds_t)ck.interval_ms * 1000 * (idle > 5 ? 10 : 1));
    int log = 0, done = 0;
    int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
    if (!reported && log > 0) {
      fprintf(stderr, "checkpointer: first passive checkpoint rc=%d wal_frames=%d backfilled=%d\n", rc, log, done);
      fflush(stderr);
      reported = 1;
    }
    if (log == last_log) idle++; else idle = 0;
    last_log = log;
  }
  return NULL;
}

int sh_checkpointer_start(const char *path, int interval_ms) {
  ck.path = strdup(path);
  ck.interval_ms = interval_ms;
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  return pthread_create(&t, &a, ck_main, NULL);
}
