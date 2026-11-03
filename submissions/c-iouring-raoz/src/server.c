/* The $12 server challenge in C: io_uring event loops, SQLite in-process.
 *
 * - multishot accept straight into io_uring's registered file table (direct descriptors), multishot
 *   recv into provided buffers (idle connections hold no buffer)
 * - one loop thread does all the work until its file table is nearly full; then the next worker thread
 *   (its own ring and table) takes new connections. A table holds at most RLIMIT_NOFILE entries, so this
 *   is how one process holds more than 65535 keep-alive connections. Workers take turns on the database
 *   (a mutex held while a batch of completions is processed), so on one vCPU they never compete.
 * - responses are queued per connection and sent with one io_uring submission per loop iteration
 * - SQLite runs inline on the loop thread (every query is an index lookup in SQLite's page cache)
 * - writes made while handling one batch of completions share one transaction (group commit),
 *   committed before any response of that batch is sent
 * - SQLite runs with locking_mode=EXCLUSIVE (one process, one connection): no lock syscalls per query
 * - the feed is built from three range scans instead of 20 correlated subqueries (see h_feed)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"
#include "sha256.h"
#include "json.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ---------------------------------------------------------------- config */

#define RING_ENTRIES 1024
#define CQ_ENTRIES 8192     /* the kernel keeps overflowing completions, so this needn't cover a burst */
#define BUF_GROUP 0
#define NBUFS 1024          /* provided recv buffers per worker (power of two); each goes back right after use */
#define BUF_SIZE 2048
#define MAX_HEADER 16384    /* request head limit */
#define MAX_BODY (1 << 20)  /* request body limit */
#define OUT_HIGH 65536      /* stop reading a connection with this much output unsent, until half is sent */
#define STALL_S 30          /* close a connection whose pending request or response made no progress */
#define CKPT_FRAMES 1000    /* checkpoint when the WAL has this many frames */
#ifndef WAIT_NR
#define WAIT_NR 128         /* wake the loop once this many completions are pending... */
#endif
#ifndef WAIT_USEC
#define WAIT_USEC 4000      /* ...or this long after it went to sleep with work outstanding */
#endif
/* SQLite's own page cache beats mmap here: in WAL mode an mmap'd page still needs a pager wrapper
 * built on every fetch, while a cached page is one hash lookup. The hot set (newest posts, their likes,
 * users) is small, but likes land at random users in the large likes primary key: on a droplet 128 MiB
 * measured 8% less user CPU than 16 MiB. The cache only grows as far as pages are actually used. */
#ifndef MMAP_SIZE
#define MMAP_SIZE "0"
#endif
#ifndef CACHE_SIZE
#define CACHE_SIZE "-131072"
#endif
#define NWORKERS 8          /* loop threads; each holds up to RLIMIT_NOFILE connections */
/* Usable memory (see check_memory) at which to stop accepting new connections. This sets how many
 * users fit, so it is no larger than a run at capacity needs. Overload (more users than fit, still trying
 * to connect, with the vCPU saturated and requests piling up in socket buffers) can eat a few hundred MB
 * more within seconds; that is handled by shedding idle connections, not by a bigger reserve. */
#define MEM_STOP_MB 192
#define MEM_RESUME_MB 288   /* resume accepting above this */
#define MEM_SHED_MB 96      /* shed idle connections when usable memory is, or within 2 s will be, below this */
#define ACCEPT_STOP 1024    /* stop accepting when fewer free table slots than this remain */
#define ACCEPT_RESUME 2048  /* ...and resume when at least this many are free again */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_IGNORE };

static inline uint64_t ud(int op, int fd, uint32_t gen) { return (uint64_t)op | (uint64_t)fd << 8 | (uint64_t)gen << 32; }

/* ---------------------------------------------------------------- buffers */

typedef struct { char *p; uint32_t len, cap; } buf_t;

/* Connection buffers are one-page chunks from a per-thread arena, and go back as soon as they are empty,
 * so an idle keep-alive connection holds no memory beyond its conn_t. Most responses fit in a page; larger
 * ones move to malloc. Freed chunks are reused newest first while they are cache-hot. Past CHUNK_HOT free
 * chunks (after a burst), the pages go back to the kernel, which needs them for sockets. */
#define CHUNK 4096
#define CHUNK_MAX 65536     /* per thread: 256 MiB of address space, only touched as needed */
#define CHUNK_HOT 128
static __thread char *cbase, *cnext, *cend;
static __thread char *chot[CHUNK_HOT], **ccold;
static __thread int nhot, ncold;

static char *chunk_get(void)
{
    if (likely(nhot)) return chot[--nhot];
    if (ncold) return ccold[--ncold];
    if (unlikely(!cbase)) {
        void *m = mmap(NULL, (size_t)CHUNK * CHUNK_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        ccold = malloc(CHUNK_MAX * sizeof *ccold);
        if (m == MAP_FAILED || !ccold) abort();
        cbase = cnext = m;
        cend = cbase + (size_t)CHUNK * CHUNK_MAX;
    }
    if (cnext < cend) { char *p = cnext; cnext += CHUNK; return p; }
    char *p = malloc(CHUNK); /* arena exhausted: plain heap memory, freed with free() */
    if (!p) abort();
    return p;
}
static inline int is_chunk(const char *p) { return p >= cbase && p < cend; }
static void chunk_put(char *p)
{
    if (unlikely(!is_chunk(p))) { free(p); return; }
    if (likely(nhot < CHUNK_HOT)) { chot[nhot++] = p; return; }
    madvise(p, CHUNK, MADV_DONTNEED);
    ccold[ncold++] = p;
}

static void buf_reserve(buf_t *b, size_t extra)
{
    if (likely(b->len + extra <= b->cap)) return;
    if (!b->cap && extra <= CHUNK) {
        b->p = chunk_get();
        b->cap = CHUNK;
        return;
    }
    size_t cap = b->cap ? b->cap : CHUNK;
    while (cap < b->len + extra) cap *= 2;
    char *p;
    if (b->cap == CHUNK) {
        if (!(p = malloc(cap))) abort();
        memcpy(p, b->p, b->len);
        chunk_put(b->p);
    } else if (!(p = realloc(b->p, cap))) {
        abort();
    }
    b->p = p;
    b->cap = cap;
}
static void buf_free(buf_t *b)
{
    if (b->cap == CHUNK) chunk_put(b->p);
    else free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}
static inline void buf_put(buf_t *b, const void *s, size_t n) { buf_reserve(b, n); memcpy(b->p + b->len, s, n); b->len += n; }
#define BUF_LIT(b, s) buf_put(b, s, sizeof(s) - 1)

static inline char *u64_to(char *o, uint64_t v)
{
    int n = 1;
    for (uint64_t x = v; x >= 10; x /= 10) n++;
    char *e = o + n;
    do { *--e = '0' + v % 10; v /= 10; } while (v);
    return o + n;
}

/* ---------------------------------------------------------------- connections */

typedef struct {
    buf_t in;        /* bytes of an incomplete request (empty most of the time) */
    buf_t out;       /* queued response bytes not yet handed to the kernel */
    buf_t fly;       /* bytes being sent */
    uint32_t fly_off;
    uint32_t gen;
    uint32_t since;  /* while input or output is pending: when it last made progress (seconds) */
    uint8_t open : 1, recv_on : 1, sending : 1, closing : 1, shut : 1, dirty : 1, sent_continue : 1;
    uint8_t paused : 1;    /* too much output queued: recv cancelled until the client reads */
    uint8_t txn_noted : 1; /* has a response in txn_resps for the open group transaction */
} conn_t;

static __thread time_t now_s; /* wall clock seconds, updated once per loop iteration */

/* Per worker thread. A connection is identified by its slot in the ring's file table. */
static __thread struct io_uring ring;
static __thread struct io_uring_buf_ring *bufring; /* NULL: legacy provided buffers (see worker_init) */
static __thread char *bufmem;
static __thread int buf_adds;
static __thread conn_t *conns;
static __thread int *dirty;
static __thread int ndirty;
static __thread int nopen, accepting;
static __thread uint32_t accept_gen;
static int nslots;      /* file table size per worker */
static int mem_low;     /* 1: not accepting; 2, 3: also shedding idle connections (see check_memory) */
static int64_t memcheck_q; /* quarter-second of the last memory check */
static int listen_fd;

static struct io_uring_sqe *get_sqe(void)
{
    struct io_uring_sqe *s = io_uring_get_sqe(&ring);
    while (unlikely(!s)) {
        io_uring_submit(&ring);
        s = io_uring_get_sqe(&ring);
    }
    return s;
}

static void arm_accept(void)
{
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_multishot_accept_direct(s, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(s, ud(OP_ACCEPT, 0, ++accept_gen));
    accepting = 1;
}

static void stop_accept(void)
{
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_cancel64(s, ud(OP_ACCEPT, 0, accept_gen), 0);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
    accepting = 0;
}

static void arm_recv(int fd)
{
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_recv_multishot(s, fd, NULL, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
    s->buf_group = BUF_GROUP;
    io_uring_sqe_set_data64(s, ud(OP_RECV, fd, conns[fd].gen));
    conns[fd].recv_on = 1;
}

static void submit_send(int fd)
{
    conn_t *c = &conns[fd];
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_send(s, fd, c->fly.p + c->fly_off, c->fly.len - c->fly_off, MSG_NOSIGNAL);
    s->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(s, ud(OP_SEND, fd, c->gen));
    c->sending = 1;
    c->since = now_s;
}

static inline void mark_dirty(int fd)
{
    if (!conns[fd].dirty) { conns[fd].dirty = 1; dirty[ndirty++] = fd; }
}

/* Shuts the socket down, which ends its multishot recv. */
static void shut_conn(int fd)
{
    conns[fd].shut = 1;
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_shutdown(s, fd, SHUT_RDWR);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, fd, 0));
    s->flags |= IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS;
}

/* Closes the socket once nothing is in flight; with a recv still armed, a shutdown ends it first. */
static void maybe_close(int fd)
{
    conn_t *c = &conns[fd];
    if (!c->closing || c->sending || c->out.len) return;
    if (c->recv_on) {
        if (!c->shut) shut_conn(fd);
        return;
    }
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_close_direct(s, fd);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, fd, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
    buf_free(&c->in); buf_free(&c->out); buf_free(&c->fly);
    c->open = 0;
    c->gen++;
    nopen--;
    if (!accepting && nslots - nopen >= ACCEPT_RESUME && !__atomic_load_n(&mem_low, __ATOMIC_RELAXED)) arm_accept();
}

/* ---------------------------------------------------------------- time */

static struct timespec start_ts;
static __thread char date_hdr[64];
static __thread size_t date_len;

static __thread double now_d; /* wall clock with sub-second precision (JWT exp/nbf, timers) */

static void update_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    now_d = ts.tv_sec + ts.tv_nsec * 1e-9;
    time_t t = ts.tv_sec;
    if (t == now_s) return;
    now_s = t;
    struct tm tm;
    gmtime_r(&t, &tm);
    date_len = strftime(date_hdr, sizeof date_hdr, "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
}

/* ---------------------------------------------------------------- database */

static sqlite3 *db;
static sqlite3_stmt *st_feed_ids, *st_like_range, *st_feed_rows;
static sqlite3_stmt *st_feed, *st_post, *st_create, *st_like, *st_exists, *st_ping, *st_begin, *st_commit, *st_rollback;
static int in_txn;
static const char *db_path;

#define POST_SELECT \
    "SELECT p.id, p.body, p.created_at, u.username, " \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) " \
    "FROM posts p JOIN users u ON u.id = p.user_id "

static sqlite3_stmt *prep(const char *sql)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s: %s\n", sqlite3_errmsg(db), sql);
        exit(1);
    }
    return s;
}

static void exec_or_die(sqlite3 *d, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(d, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

static int wal_frames;

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames)
{
    (void)arg; (void)d; (void)name;
    wal_frames = frames;
    return SQLITE_OK;
}

/* Copies WAL frames back into the database once the WAL is long enough. Runs on the loop thread
 * after a group commit: with locking_mode=EXCLUSIVE no other connection can do it. */
static void maybe_checkpoint(void)
{
    if (wal_frames < CKPT_FRAMES) return;
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    wal_frames = 0;
}

static void db_open(void)
{
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_busy_timeout(db, 5000);
    /* EXCLUSIVE before the first WAL access: the wal-index lives in heap memory instead of a shared
     * -shm file, so read transactions need no fcntl() lock calls. This process is the only user. */
    exec_or_die(db,
                "PRAGMA locking_mode=EXCLUSIVE;"
                "PRAGMA journal_mode=WAL;"
                "PRAGMA synchronous=NORMAL;"
                "PRAGMA mmap_size=" MMAP_SIZE ";"
                "PRAGMA cache_size=" CACHE_SIZE ";"
                "PRAGMA temp_store=MEMORY;"
                "PRAGMA foreign_keys=ON;"
                "PRAGMA journal_size_limit=67108864;"
                "PRAGMA wal_autocheckpoint=0;");
    sqlite3_wal_hook(db, wal_hook, NULL);
    st_feed = prep(POST_SELECT "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_post = prep(POST_SELECT "WHERE p.id = ?1");
    st_feed_ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    st_like_range = prep("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    st_feed_rows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id "
                        "WHERE p.id BETWEEN ?1 AND ?2");
    st_create = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    st_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
                   "ON CONFLICT (user_id, post_id) DO NOTHING");
    st_exists = prep("SELECT 1 FROM posts WHERE id = ?1");
    st_ping = prep("SELECT 1");
    st_begin = prep("BEGIN IMMEDIATE");
    st_commit = prep("COMMIT");
    st_rollback = prep("ROLLBACK");

    /* Pull the database file into the OS page cache so early requests don't wait on disk. */
    int fd = open(db_path, O_RDONLY);
    if (fd >= 0) { posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED); close(fd); }
}

/* Group commit: the first write of a loop iteration opens a transaction, end_batch() commits it. */
static int txn_begin(void)
{
    if (in_txn) return 1;
    int rc = sqlite3_step(st_begin);
    sqlite3_reset(st_begin);
    if (rc != SQLITE_DONE) return 0;
    in_txn = 1;
    return 1;
}

/* ---------------------------------------------------------------- responses */

static __thread buf_t body; /* scratch for response bodies */

static const char *status_line(int st, size_t *n)
{
#define S(x) do { *n = sizeof(x) - 1; return x; } while (0)
    switch (st) {
    case 200: S("HTTP/1.1 200 OK\r\n");
    case 201: S("HTTP/1.1 201 Created\r\n");
    case 400: S("HTTP/1.1 400 Bad Request\r\n");
    case 401: S("HTTP/1.1 401 Unauthorized\r\n");
    case 404: S("HTTP/1.1 404 Not Found\r\n");
    case 413: S("HTTP/1.1 413 Payload Too Large\r\n");
    case 431: S("HTTP/1.1 431 Request Header Fields Too Large\r\n");
    case 503: S("HTTP/1.1 503 Service Unavailable\r\n");
    default: S("HTTP/1.1 500 Internal Server Error\r\n");
    }
#undef S
}

/* Per-connection response record: which connections got a response that depends on this batch's
 * transaction (so a failed group commit can be turned into 500s). */
typedef struct { int fd; uint32_t gen; uint32_t start; } txn_resp_t;
static __thread txn_resp_t *txn_resps;
static __thread int ntxn_resps, txn_resps_cap;

/* Connection header mode for responses to the request being handled: 0 none (HTTP/1.1 keep-alive),
 * 1 "close" (the client asked for it, or HTTP/1.0 without keep-alive), 2 "keep-alive" (HTTP/1.0). */
static __thread int conn_hdr;

/* Every response produced while the group transaction is open may have read its uncommitted rows: the
 * first one on each connection is noted, so that if the commit fails, it and everything after it on that
 * connection can be replaced by a 500. */
static void note_txn_resp(int fd)
{
    if (ntxn_resps == txn_resps_cap) {
        txn_resps_cap = txn_resps_cap ? txn_resps_cap * 2 : 256;
        txn_resps = realloc(txn_resps, txn_resps_cap * sizeof *txn_resps);
    }
    txn_resps[ntxn_resps++] = (txn_resp_t){fd, conns[fd].gen, conns[fd].out.len};
    conns[fd].txn_noted = 1;
}

/* Writes the status line and headers for a body of n bytes into the connection's output buffer, with
 * room reserved for the body; returns where the body goes. The caller sets out.len past the body. */
static char *respond_head(int fd, int status, size_t n, int close_conn)
{
    if (conn_hdr == 1) close_conn = 1;
    conn_t *c = &conns[fd];
    if (in_txn && !c->txn_noted) note_txn_resp(fd);
    size_t sl;
    const char *s = status_line(status, &sl);
    buf_reserve(&c->out, sl + date_len + 128 + n);
    char *o = c->out.p + c->out.len;
    memcpy(o, s, sl); o += sl;
    memcpy(o, date_hdr, date_len); o += date_len;
    static const char h1[] = "Content-Type: application/json\r\nContent-Length: ";
    memcpy(o, h1, sizeof h1 - 1); o += sizeof h1 - 1;
    o = u64_to(o, n);
    if (close_conn) {
        static const char h2[] = "\r\nConnection: close\r\n\r\n";
        memcpy(o, h2, sizeof h2 - 1); o += sizeof h2 - 1;
    } else if (conn_hdr == 2) {
        static const char h3[] = "\r\nConnection: keep-alive\r\n\r\n";
        memcpy(o, h3, sizeof h3 - 1); o += sizeof h3 - 1;
    } else {
        memcpy(o, "\r\n\r\n", 4); o += 4;
    }
    if (close_conn) c->closing = 1;
    mark_dirty(fd);
    return o;
}

static void respond(int fd, int status, const char *b, size_t n, int close_conn)
{
    char *o = respond_head(fd, status, n, close_conn);
    memcpy(o, b, n);
    conns[fd].out.len = o + n - conns[fd].out.p;
}

#define ERR(fd, st, msg) respond(fd, st, "{\"error\":\"" msg "\"}", sizeof("{\"error\":\"" msg "\"}") - 1, 0)

/* Appends a post object from a row (id, body, created_at, username). */
static void put_post(buf_t *b, sqlite3_stmt *s, int64_t likes)
{
    const unsigned char *pb = sqlite3_column_text(s, 1);
    int nb = sqlite3_column_bytes(s, 1);
    const unsigned char *pc = sqlite3_column_text(s, 2);
    int nc = sqlite3_column_bytes(s, 2);
    const unsigned char *pu = sqlite3_column_text(s, 3);
    int nu = sqlite3_column_bytes(s, 3);
    buf_reserve(b, 160 + 6 * (size_t)(nb + nc + nu)); /* 99 fixed bytes, and 6n + 16 per json_escape */
    char *o = b->p + b->len;
    memcpy(o, "{\"id\":", 6); o += 6;
    o = u64_to(o, sqlite3_column_int64(s, 0));
    memcpy(o, ",\"body\":\"", 9); o += 9;
    o = json_escape(o, pb, nb);
    memcpy(o, "\",\"created_at\":\"", 16); o += 16;
    o = json_escape(o, pc, nc);
    memcpy(o, "\",\"author\":\"", 12); o += 12;
    o = json_escape(o, pu, nu);
    memcpy(o, "\",\"like_count\":", 15); o += 15;
    o = u64_to(o, likes);
    *o++ = '}';
    b->len = o - b->p;
}

static void internal_error(int fd)
{
    ERR(fd, 500, "internal server error");
}

/* The feed with the reference query: one correlated count(*) per post. */
static void h_feed_ref(int fd)
{
    body.len = 0;
    BUF_LIT(&body, "{\"posts\":[");
    int rc, first = 1;
    while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
        if (!first) buf_put(&body, ",", 1);
        first = 0;
        put_post(&body, st_feed, sqlite3_column_int64(st_feed, 4));
    }
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE) { internal_error(fd); return; }
    BUF_LIT(&body, "]}");
    respond(fd, 200, body.p, body.len, 0);
}

/* The same result in fewer B-tree descents. The 20 newest posts nearly always have adjacent ids, so:
 *   1. their ids, newest first, from the created_at index alone;
 *   2. one scan of likes_post_id_idx over [min id, max id] counts the likes of all of them;
 *   3. one ascending rowid range scan of posts (joined to users) fetches their rows;
 * then the posts are written out in the order of step 1. Nothing else runs on the connection between
 * the three statements, so they see the same data. If the ids are spread out, use the reference query. */
#ifndef FEED_SPAN
#define FEED_SPAN 256
#endif

static void h_feed(int fd)
{
    int64_t ids[20], likes[20] = {0};
    uint32_t seg[20], seglen[20];
    int8_t slot[FEED_SPAN];
    int n = 0, rc;
    while (n < 20 && (rc = sqlite3_step(st_feed_ids)) == SQLITE_ROW) ids[n++] = sqlite3_column_int64(st_feed_ids, 0);
    sqlite3_reset(st_feed_ids);
    if (n < 20 && rc != SQLITE_DONE) { internal_error(fd); return; }
    if (!n) { respond(fd, 200, "{\"posts\":[]}", 12, 0); return; }
    int64_t mn = ids[0], mx = ids[0];
    for (int k = 1; k < n; k++) { if (ids[k] < mn) mn = ids[k]; if (ids[k] > mx) mx = ids[k]; }
    if (mx - mn >= FEED_SPAN) { h_feed_ref(fd); return; }
    memset(slot, -1, mx - mn + 1);
    for (int k = 0; k < n; k++) slot[ids[k] - mn] = k;

    sqlite3_bind_int64(st_like_range, 1, mn);
    sqlite3_bind_int64(st_like_range, 2, mx);
    while ((rc = sqlite3_step(st_like_range)) == SQLITE_ROW) {
        int k = slot[sqlite3_column_int64(st_like_range, 0) - mn];
        if (k >= 0) likes[k]++;
    }
    sqlite3_reset(st_like_range);
    if (rc != SQLITE_DONE) { internal_error(fd); return; }

    int found = 0;
    body.len = 0;
    sqlite3_bind_int64(st_feed_rows, 1, mn);
    sqlite3_bind_int64(st_feed_rows, 2, mx);
    while ((rc = sqlite3_step(st_feed_rows)) == SQLITE_ROW) {
        int k = slot[sqlite3_column_int64(st_feed_rows, 0) - mn];
        if (k < 0) continue;
        seg[k] = body.len;
        put_post(&body, st_feed_rows, likes[k]);
        seglen[k] = body.len - seg[k];
        found++;
    }
    sqlite3_reset(st_feed_rows);
    if (rc != SQLITE_DONE) { internal_error(fd); return; }
    if (found != n) { h_feed_ref(fd); return; } /* cannot happen without a concurrent writer */

    /* assemble straight into the connection's output buffer */
    char *o = respond_head(fd, 200, 10 + body.len + (n - 1) + 2, 0);
    memcpy(o, "{\"posts\":[", 10); o += 10;
    for (int k = 0; k < n; k++) {
        if (k) *o++ = ',';
        memcpy(o, body.p + seg[k], seglen[k]);
        o += seglen[k];
    }
    memcpy(o, "]}", 2); o += 2;
    conns[fd].out.len = o - conns[fd].out.p;
}

static void h_post(int fd, int64_t id)
{
    sqlite3_bind_int64(st_post, 1, id);
    int rc = sqlite3_step(st_post);
    if (rc == SQLITE_ROW) {
        body.len = 0;
        BUF_LIT(&body, "{\"post\":");
        put_post(&body, st_post, sqlite3_column_int64(st_post, 4));
        BUF_LIT(&body, "}");
        sqlite3_reset(st_post);
        respond(fd, 200, body.p, body.len, 0);
    } else {
        sqlite3_reset(st_post);
        if (rc == SQLITE_DONE) ERR(fd, 404, "post not found");
        else internal_error(fd);
    }
}

static void h_health(int fd)
{
    int rc = sqlite3_step(st_ping);
    sqlite3_reset(st_ping);
    body.len = 0;
    if (rc == SQLITE_ROW) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        BUF_LIT(&body, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        buf_reserve(&body, 24);
        body.len = u64_to(body.p + body.len, ts.tv_sec - start_ts.tv_sec) - body.p;
        BUF_LIT(&body, "}");
        respond(fd, 200, body.p, body.len, 0);
    } else {
        const char *m = sqlite3_errmsg(db);
        size_t ml = strlen(m);
        BUF_LIT(&body, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
        buf_reserve(&body, 6 * ml + 32);
        body.len = json_escape(body.p + body.len, (const unsigned char *)m, ml) - body.p;
        BUF_LIT(&body, "\"}");
        respond(fd, 503, body.p, body.len, 0);
    }
}

static void h_create(int fd, int64_t uid, const char *uname, size_t ulen, const char *b, size_t n)
{
    char *dec = NULL;
    jfield f[1] = {{.name = "body"}};
    int t = json_fields(b, n, f, 1);
    if (!t) { ERR(fd, 400, "malformed JSON body"); return; }
    if (t != JT_OTHER || f[0].type != JT_STRING) { ERR(fd, 400, "body is required"); return; }
    size_t raw = f[0].ve - f[0].v;
    char stackbuf[2048];
    char *s = raw <= sizeof stackbuf ? stackbuf : (dec = malloc(raw));
    size_t len = j_unescape(f[0].v, f[0].ve, s);
    /* Trim whitespace (as JavaScript's String.prototype.trim does). */
    const unsigned char *p = (const unsigned char *)s, *e = p + len;
    for (;;) {
        if (p < e && (*p == ' ' || (*p >= '\t' && *p <= '\r'))) { p++; continue; }
        if (e - p >= 2 && p[0] == 0xC2 && p[1] == 0xA0) { p += 2; continue; }
        if (e - p >= 3) {
            uint32_t u = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            if ((p[0] & 0xF0) == 0xE0 && (u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 ||
                                          u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF)) { p += 3; continue; }
        }
        break;
    }
    for (;;) {
        if (e > p && (e[-1] == ' ' || (e[-1] >= '\t' && e[-1] <= '\r'))) { e--; continue; }
        if (e - p >= 2 && e[-2] == 0xC2 && e[-1] == 0xA0) { e -= 2; continue; }
        if (e - p >= 3) {
            const unsigned char *q = e - 3;
            uint32_t u = (q[0] & 0x0F) << 12 | (q[1] & 0x3F) << 6 | (q[2] & 0x3F);
            if ((q[0] & 0xF0) == 0xE0 && (u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 ||
                                          u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF)) { e -= 3; continue; }
        }
        break;
    }
    if (p == e) { ERR(fd, 400, "body is required"); free(dec); return; }
    size_t chars = 0;
    for (const unsigned char *q = p; q < e; q++) chars += (*q & 0xC0) != 0x80;
    if (chars > 500) { ERR(fd, 400, "body must be at most 500 characters"); free(dec); return; }

    if (!txn_begin()) { internal_error(fd); free(dec); return; }
    sqlite3_bind_int64(st_create, 1, uid);
    sqlite3_bind_text(st_create, 2, (const char *)p, e - p, SQLITE_STATIC);
    int rc = sqlite3_step(st_create);
    if (rc != SQLITE_ROW) { sqlite3_reset(st_create); internal_error(fd); free(dec); return; }
    int64_t id = sqlite3_column_int64(st_create, 0);
    const unsigned char *ca = sqlite3_column_text(st_create, 1);
    int nca = sqlite3_column_bytes(st_create, 1);
    body.len = 0;
    buf_reserve(&body, 160 + 6 * ((e - p) + ulen + nca));
    char *o = body.p;
    memcpy(o, "{\"post\":{\"id\":", 14); o += 14;
    o = u64_to(o, id);
    memcpy(o, ",\"body\":\"", 9); o += 9;
    o = json_escape(o, p, e - p);
    memcpy(o, "\",\"created_at\":\"", 16); o += 16;
    o = json_escape(o, ca, nca);
    memcpy(o, "\",\"author\":\"", 12); o += 12;
    o = json_escape(o, (const unsigned char *)uname, ulen);
    memcpy(o, "\",\"like_count\":0}}", 18); o += 18;
    body.len = o - body.p;
    rc = sqlite3_step(st_create); /* finish the statement so the row is fully written */
    sqlite3_reset(st_create);
    free(dec);
    if (rc != SQLITE_DONE) { internal_error(fd); return; }
    respond(fd, 201, body.p, body.len, 0);
}

static void h_like(int fd, int64_t uid, int64_t pid)
{
    if (!txn_begin()) { internal_error(fd); return; }
    sqlite3_bind_int64(st_like, 1, uid);
    sqlite3_bind_int64(st_like, 2, pid);
    int rc = sqlite3_step(st_like);
    sqlite3_reset(st_like);
    if (rc != SQLITE_DONE) { internal_error(fd); return; }
    int inserted = sqlite3_changes(db) > 0;
    if (!inserted) {
        sqlite3_bind_int64(st_exists, 1, pid);
        rc = sqlite3_step(st_exists);
        sqlite3_reset(st_exists);
        if (rc == SQLITE_DONE) { ERR(fd, 404, "post not found"); return; }
        if (rc != SQLITE_ROW) { internal_error(fd); return; }
    }
    char b[96], *o = b;
    memcpy(o, "{\"liked\":true,\"already_liked\":", 30); o += 30;
    if (inserted) { memcpy(o, "false", 5); o += 5; } else { memcpy(o, "true", 4); o += 4; }
    memcpy(o, ",\"post_id\":", 11); o += 11;
    o = u64_to(o, pid);
    *o++ = '}';
    respond(fd, inserted ? 201 : 200, b, o - b, 0);
}

/* Commits this iteration's group transaction. If that fails, every response produced while it was open
 * (reads included: they may have seen its rows) is dropped, and the connection gets one 500 and closes. */
static void end_batch(void)
{
    if (!in_txn) return;
    in_txn = 0;
    int rc = sqlite3_step(st_commit);
    sqlite3_reset(st_commit);
    int failed = rc != SQLITE_DONE;
    if (failed) {
        sqlite3_step(st_rollback);
        sqlite3_reset(st_rollback);
    }
    for (int i = 0; i < ntxn_resps; i++) {
        int fd = txn_resps[i].fd;
        conn_t *c = &conns[fd];
        if (!c->open || c->gen != txn_resps[i].gen) continue;
        c->txn_noted = 0;
        if (!failed) continue;
        static const char m[] = "{\"error\":\"internal server error\"}";
        c->out.len = txn_resps[i].start;
        c->closing = 0; /* the 500 replaces whatever was queued, including a Connection: close */
        respond(fd, 500, m, sizeof m - 1, 1);
    }
    ntxn_resps = 0;
    maybe_checkpoint();
}

/* ---------------------------------------------------------------- auth */

static hmac_key jwt_key;

static signed char B64URL[256];

static void b64url_init(void)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    memset(B64URL, -1, sizeof B64URL);
    for (int i = 0; i < 64; i++) B64URL[(unsigned char)A[i]] = i;
}

/* Unpadded base64url decode; returns the length or -1. */
static long b64url_decode(const char *s, size_t n, char *out)
{
    if (n % 4 == 1) return -1;
    char *o = out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = B64URL[(unsigned char)s[i]];
        if (v < 0) return -1;
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; *o++ = acc >> bits; }
    }
    if (acc & ((1u << bits) - 1)) return -1; /* non-canonical trailing bits */
    return o - out;
}

static void b64url_encode32(const uint8_t in[32], char out[43])
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int j = 0;
    for (int i = 0; i < 30; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        out[j++] = A[v >> 18]; out[j++] = A[v >> 12 & 63]; out[j++] = A[v >> 6 & 63]; out[j++] = A[v & 63];
    }
    uint32_t v = in[30] << 16 | in[31] << 8;
    out[j++] = A[v >> 18]; out[j++] = A[v >> 12 & 63]; out[j++] = A[v >> 6 & 63];
}

typedef struct { int64_t uid; char name[2048]; size_t name_len; } auth_t; /* name: up to the whole payload */

/* Parses a JSON number field as a finite double. */
static int json_number(const jfield *f, double *out)
{
    char nb[64];
    size_t nl = f->ve - f->v;
    if (nl >= sizeof nb) return 0;
    memcpy(nb, f->v, nl);
    nb[nl] = 0;
    *out = strtod(nb, NULL);
    return isfinite(*out);
}

/* Returns 0 on success, otherwise the 401 message kind: 1 missing, 2 invalid/expired, 3 bad payload. */
static int check_auth(const char *h, size_t hn, auth_t *a)
{
    if (!h || hn < 7 || memcmp(h, "Bearer ", 7)) return 1;
    const char *t = h + 7, *te = h + hn;
    const char *d1 = memchr(t, '.', te - t);
    if (!d1) return 2;
    const char *d2 = memchr(d1 + 1, '.', te - d1 - 1);
    if (!d2 || memchr(d2 + 1, '.', te - d2 - 1)) return 2;
    /* signature first: HMAC over "header.payload" */
    if (te - d2 - 1 != 43) return 2;
    uint8_t mac[32];
    char enc[43];
    hmac_sha256(&jwt_key, t, d2 - t, mac);
    b64url_encode32(mac, enc);
    unsigned diff = 0;
    for (int i = 0; i < 43; i++) diff |= (unsigned char)enc[i] ^ (unsigned char)d2[1 + i];
    if (diff) return 2;

    char hb[512], pb[2048];
    if ((size_t)(d1 - t) > sizeof hb * 4 / 3 || (size_t)(d2 - d1 - 1) > sizeof pb * 4 / 3) return 2;
    long hl = b64url_decode(t, d1 - t, hb);
    if (hl < 0) return 2;
    jfield hf[1] = {{.name = "alg"}};
    if (json_fields(hb, hl, hf, 1) != JT_OTHER || hf[0].type != JT_STRING ||
        hf[0].ve - hf[0].v != 5 || memcmp(hf[0].v, "HS256", 5))
        return 2;
    long pl = b64url_decode(d1 + 1, d2 - d1 - 1, pb);
    if (pl < 0) return 2;
    jfield pf[4] = {{.name = "sub"}, {.name = "username"}, {.name = "exp"}, {.name = "nbf"}};
    if (json_fields(pb, pl, pf, 4) != JT_OTHER) return 2;
    /* exp is required; exp and nbf are finite NumericDates, compared with sub-second precision */
    double exp, nbf;
    if (pf[2].type != JT_NUMBER || !json_number(&pf[2], &exp) || exp <= now_d) return 2;
    if (pf[3].type != JT_NONE && (pf[3].type != JT_NUMBER || !json_number(&pf[3], &nbf) || nbf > now_d)) return 2;
    if (pf[0].type != JT_STRING || pf[1].type != JT_STRING) return 3;
    const char *s = pf[0].v, *se = pf[0].ve;
    if (s == se) return 3;
    int64_t uid = 0;
    for (; s < se; s++) {
        if (*s < '0' || *s > '9' || uid > (INT64_MAX - (*s - '0')) / 10) return 3;
        uid = uid * 10 + (*s - '0');
    }
    if (uid <= 0) return 3;
    a->uid = uid;
    if ((size_t)(pf[1].ve - pf[1].v) > sizeof a->name) return 3;
    a->name_len = j_unescape(pf[1].v, pf[1].ve, a->name);
    return 0;
}

static void auth_error(int fd, int kind)
{
    if (kind == 1) ERR(fd, 401, "missing bearer token");
    else if (kind == 2) ERR(fd, 401, "invalid or expired token");
    else ERR(fd, 401, "invalid token payload");
}

/* ---------------------------------------------------------------- HTTP */

/* Parses a post id path segment: >0 ok, 0 invalid, -1 too large (cannot exist). */
static int64_t parse_id(const char *s, size_t n)
{
    if (!n) return 0;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (v > (INT64_MAX - 9) / 10) {
            for (; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static inline int ieq(const char *a, const char *lit, size_t n)
{
    for (size_t i = 0; i < n; i++) if ((a[i] | 0x20) != lit[i]) return 0;
    return 1;
}

static void route(int fd, const char *m, size_t ml, const char *path, size_t pl, const char *auth, size_t authl,
                  const char *b, size_t bl)
{
    int get = ml == 3 && !memcmp(m, "GET", 3);
    int post = ml == 4 && !memcmp(m, "POST", 4);
    if (pl >= 7 && !memcmp(path, "/posts/", 7)) {
        const char *seg = path + 7;
        size_t sl = pl - 7;
        const char *slash = memchr(seg, '/', sl);
        if (!slash && get) {
            int64_t id = parse_id(seg, sl);
            if (!id) ERR(fd, 400, "invalid post id");
            else if (id < 0) ERR(fd, 404, "post not found");
            else h_post(fd, id);
            return;
        }
        if (slash && post && (size_t)(path + pl - slash) == 5 && !memcmp(slash, "/like", 5)) {
            auth_t a;
            int k = check_auth(auth, authl, &a);
            if (k) { auth_error(fd, k); return; }
            int64_t id = parse_id(seg, slash - seg);
            if (!id) ERR(fd, 400, "invalid post id");
            else if (id < 0) ERR(fd, 404, "post not found");
            else h_like(fd, a.uid, id);
            return;
        }
    } else if (pl == 5 && !memcmp(path, "/feed", 5)) {
        if (get) { h_feed(fd); return; }
    } else if (pl == 6 && !memcmp(path, "/posts", 6)) {
        if (post) {
            auth_t a;
            int k = check_auth(auth, authl, &a);
            if (k) { auth_error(fd, k); return; }
            h_create(fd, a.uid, a.name, a.name_len, b, bl);
            return;
        }
    } else if (pl == 7 && !memcmp(path, "/health", 7)) {
        if (get) { h_health(fd); return; }
    }
    ERR(fd, 404, "not found");
}

static void bad_request(int fd, int status)
{
    if (status == 413) respond(fd, 413, "{\"error\":\"payload too large\"}", 29, 1);
    else if (status == 431) respond(fd, 431, "{\"error\":\"headers too large\"}", 29, 1);
    else respond(fd, 400, "{\"error\":\"bad request\"}", 23, 1);
}

/* Decodes a chunked body starting at b into chunk_body: chunk sizes with optional extensions, then
 * trailer fields, which are ignored. Returns 1 and the end of the message in *next, 0 if incomplete,
 * -1 if malformed, -2 if too large. Bodies are small, so an incomplete one is decoded again later. */
static __thread buf_t chunk_body;
static ssize_t dechunk(const char *b, const char *end, const char **next)
{
    chunk_body.len = 0;
    if (end - b > 2 * MAX_BODY) return -2; /* bounds what chunk extensions can make us buffer */
    const char *x = b;
    for (;;) {
        const char *lf = memchr(x, '\n', end - x);
        if (!lf) return end - x > 1024 ? -1 : 0;
        if (lf == x || lf[-1] != '\r') return -1;
        size_t size = 0;
        const char *h = x;
        for (; h < lf - 1 && *h != ';'; h++) {
            int d = *h >= '0' && *h <= '9' ? *h - '0' : (*h | 0x20) >= 'a' && (*h | 0x20) <= 'f' ? (*h | 0x20) - 'a' + 10 : -1;
            if (d < 0) return -1;
            size = size * 16 + d;
            if (size > MAX_BODY) return -2;
        }
        if (h == x) return -1;
        x = lf + 1;
        if (!size) break;
        if (chunk_body.len + size > MAX_BODY) return -2;
        if ((size_t)(end - x) < size + 2) return 0;
        if (x[size] != '\r' || x[size + 1] != '\n') return -1;
        buf_put(&chunk_body, x, size);
        x += size + 2;
    }
    for (;;) { /* trailer section, up to an empty line */
        const char *lf = memchr(x, '\n', end - x);
        if (!lf) return end - x > MAX_HEADER ? -1 : 0;
        if (lf == x || lf[-1] != '\r') return -1;
        int empty = lf == x + 1;
        x = lf + 1;
        if (empty) { *next = x; return 1; }
    }
}

/* Handles one request at the start of [p, p+n). Returns bytes consumed, 0 if incomplete. */
static size_t handle_request(int fd, const char *p, size_t n)
{
    const char *he = memmem(p, n, "\r\n\r\n", 4);
    if (!he) {
        if (n > MAX_HEADER) { bad_request(fd, 431); return n; }
        return 0;
    }
    const char *end = p + n;
    const char *le = memchr(p, '\r', he + 2 - p);
    if (!le || le[1] != '\n' || memchr(p, '\n', le - p)) { bad_request(fd, 400); return n; }
    /* request line: METHOD SP TARGET SP VERSION */
    const char *sp1 = memchr(p, ' ', le - p);
    if (!sp1) { bad_request(fd, 400); return n; }
    const char *tgt = sp1 + 1;
    const char *sp2 = memchr(tgt, ' ', le - tgt);
    if (!sp2 || le - sp2 - 1 != 8 || memcmp(sp2 + 1, "HTTP/1.", 7)) { bad_request(fd, 400); return n; }
    int http11 = sp2[8] == '1', keepalive = http11;
    const char *q = memchr(tgt, '?', sp2 - tgt);
    size_t pl = (q ? q : sp2) - tgt;

    const char *auth = NULL;
    size_t authl = 0;
    size_t clen = 0;
    int has_cl = 0, chunked = 0, expect = 0;
    for (const char *l = le + 2; l < he + 2;) {
        const char *e = memchr(l, '\r', he + 2 - l);
        if (!e || e[1] != '\n' || memchr(l, '\n', e - l)) { bad_request(fd, 400); return n; }
        const char *colon = memchr(l, ':', e - l);
        if (colon) {
            size_t nl = colon - l;
            const char *v = colon + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            const char *ve = e;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            switch (nl) {
            case 13:
                if (ieq(l, "authorization", 13)) { auth = v; authl = ve - v; }
                break;
            case 14:
                if (ieq(l, "content-length", 14)) {
                    if (v == ve) { bad_request(fd, 400); return n; }
                    size_t cl = 0;
                    for (const char *x = v; x < ve; x++) {
                        if (*x < '0' || *x > '9') { bad_request(fd, 400); return n; }
                        cl = cl * 10 + (*x - '0');
                        if (cl > MAX_BODY) { bad_request(fd, 413); return n; }
                    }
                    if (has_cl && cl != clen) { bad_request(fd, 400); return n; } /* conflicting lengths */
                    clen = cl;
                    has_cl = 1;
                }
                break;
            case 10:
                if (ieq(l, "connection", 10)) {
                    if (ve - v == 5 && ieq(v, "close", 5)) keepalive = 0;
                    else if (ve - v == 10 && ieq(v, "keep-alive", 10)) keepalive = 1;
                }
                break;
            case 17:
                if (ieq(l, "transfer-encoding", 17)) {
                    if (chunked || ve - v != 7 || !ieq(v, "chunked", 7)) { bad_request(fd, 400); return n; }
                    chunked = 1;
                }
                break;
            case 6:
                if (ieq(l, "expect", 6) && ve - v == 12 && ieq(v, "100-continue", 12)) expect = 1;
                break;
            }
        }
        l = e + 2;
    }
    if (chunked && has_cl) { bad_request(fd, 400); return n; } /* ambiguous framing */
    const char *b = he + 4, *next = b + clen;
    if (chunked) {
        ssize_t r = dechunk(b, end, &next);
        if (r < 0) { bad_request(fd, r == -2 ? 413 : 400); return n; }
        if (r == 0) next = NULL;
        b = chunk_body.p;
        clen = chunk_body.len;
    } else if ((size_t)(end - b) < clen) {
        next = NULL;
    }
    if (!next) { /* incomplete body */
        conn_t *c = &conns[fd];
        if (expect && !c->sent_continue) {
            c->sent_continue = 1;
            buf_put(&c->out, "HTTP/1.1 100 Continue\r\n\r\n", 25);
            mark_dirty(fd);
        }
        return 0;
    }
    conns[fd].sent_continue = 0;
    conn_hdr = !keepalive ? 1 : http11 ? 0 : 2;
    route(fd, p, sp1 - p, tgt, pl, auth, authl, b, clen);
    conn_hdr = 0;
    return next - p;
}

/* A client that pipelines requests without reading the responses: stop receiving from it, so TCP
 * flow control holds it back instead of our memory. Bytes already received wait in c->in. */
static void pause_conn(int fd)
{
    conn_t *c = &conns[fd];
    c->paused = 1;
    if (!c->recv_on) return;
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_cancel64(s, ud(OP_RECV, fd, c->gen), 0);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
}

static void on_data(int fd, const char *data, size_t n)
{
    conn_t *c = &conns[fd];
    if (c->closing) return;
    const char *p = data;
    if (c->in.len) {
        if (n) buf_put(&c->in, data, n);
        p = c->in.p;
        n = c->in.len;
    }
    size_t off = 0;
    while (off < n && !c->closing) {
        if (unlikely(c->out.len + c->fly.len - c->fly_off > OUT_HIGH)) { pause_conn(fd); break; }
        size_t r = handle_request(fd, p + off, n - off);
        if (!r) break;
        off += r;
        c->since = now_s;
    }
    size_t rest = c->closing ? 0 : n - off;
    if (c->in.len) {
        if (rest) memmove(c->in.p, c->in.p + off, rest);
        c->in.len = rest;
        if (!rest) buf_free(&c->in);
    } else if (rest) {
        buf_put(&c->in, p + off, rest);
        c->since = now_s;
    }
}

/* Half the paused output went out: handle the requests already received, then receive again. */
static void resume_conn(int fd)
{
    conn_t *c = &conns[fd];
    c->paused = 0;
    if (c->in.len) on_data(fd, NULL, 0);
    if (!c->paused && !c->recv_on && !c->closing) arm_recv(fd);
}

/* ---------------------------------------------------------------- event loop */

static void on_accept(struct io_uring_cqe *cqe, uint32_t gen)
{
    int slot = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE) && accepting && gen == accept_gen) {
        /* the multishot accept ended on its own (an error): arm it again */
        accepting = 0;
        if (nslots - nopen >= ACCEPT_STOP) arm_accept();
    }
    if (slot < 0) return;
    conn_t *c = &conns[slot];
    uint32_t g = c->gen;
    memset(c, 0, sizeof *c);
    c->gen = g;
    c->open = 1;
    nopen++;
    arm_recv(slot);
    /* a full table would make the kernel drop accepted connections: hand new ones to the next worker */
    if (accepting && nslots - nopen < ACCEPT_STOP) stop_accept();
}

/* Classic provided buffers are handed out first-in first-out, so the ones used in a batch mostly have
 * consecutive ids: give them back as runs, one PROVIDE_BUFFERS per run. */
static __thread int ret_start, ret_n;

static void return_bufs(void)
{
    if (!ret_n) return;
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_provide_buffers(s, bufmem + (size_t)ret_start * BUF_SIZE, BUF_SIZE, ret_n, BUF_GROUP, ret_start);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
    ret_n = 0;
}

/* Gives a recv buffer back to the kernel once its data has been consumed. */
static void recycle_buf(int bid)
{
    if (likely(bufring)) {
        io_uring_buf_ring_add(bufring, bufmem + (size_t)bid * BUF_SIZE, BUF_SIZE, bid, io_uring_buf_ring_mask(NBUFS), buf_adds++);
        return;
    }
    if (ret_n && bid == ret_start + ret_n) { ret_n++; return; }
    return_bufs();
    ret_start = bid;
    ret_n = 1;
}

static void on_recv(struct io_uring_cqe *cqe, int fd, uint32_t gen)
{
    conn_t *c = &conns[fd];
    int res = cqe->res;
    int more = cqe->flags & IORING_CQE_F_MORE;
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        if (res > 0 && c->open && c->gen == gen) on_data(fd, bufmem + (size_t)bid * BUF_SIZE, res);
        recycle_buf(bid);
    }
    if (!c->open || c->gen != gen) return;
    if (!more) {
        c->recv_on = 0;
        if (c->paused && !c->closing) return; /* cancelled by pause_conn; resume_conn arms it again */
        if (res == -ENOBUFS && !c->closing) { return_bufs(); arm_recv(fd); }
        else if (res > 0 && !c->closing) arm_recv(fd);
        else c->closing = 1;
    }
    if (c->closing) maybe_close(fd);
}

static void on_send(struct io_uring_cqe *cqe, int fd, uint32_t gen)
{
    conn_t *c = &conns[fd];
    if (!c->open || c->gen != gen) return;
    c->sending = 0;
    int res = cqe->res;
    if (res < 0) {
        if (res == -EAGAIN || res == -EINTR) { submit_send(fd); return; }
        c->closing = 1;
        c->out.len = 0;
        c->fly.len = 0;
        if (c->recv_on && !c->shut) shut_conn(fd);
        maybe_close(fd);
        return;
    }
    c->fly_off += res;
    if (c->fly_off < c->fly.len) { submit_send(fd); return; }
    buf_free(&c->fly);
    c->fly_off = 0;
    if (c->paused && c->out.len <= OUT_HIGH / 2 && !c->closing) resume_conn(fd);
    if (c->out.len) mark_dirty(fd);
    else {
        if (c->closing) maybe_close(fd);
    }
}

/* Every few seconds: close connections whose partial request or unsent response has not moved for
 * STALL_S (a slow-loris client, or one that stopped reading). Idle keep-alive connections stay open. */
static void sweep_stalled(void)
{
    uint32_t now = now_s;
    for (int fd = 0; fd < nslots; fd++) {
        conn_t *c = &conns[fd];
        if (!c->open || c->closing || !(c->in.len || c->sending || c->out.len)) continue;
        if (now - c->since < STALL_S) continue;
        c->closing = 1;
        c->out.len = 0;
        if (!c->shut) shut_conn(fd);
        maybe_close(fd);
    }
}

/* Hands queued responses to the kernel: swap out -> fly and send. */
static void flush_dirty(void)
{
    for (int i = 0; i < ndirty; i++) {
        int fd = dirty[i];
        conn_t *c = &conns[fd];
        c->dirty = 0;
        if (!c->open || c->sending) continue;
        if (c->out.len) {
            buf_free(&c->fly);
            c->fly = c->out;
            c->out = (buf_t){0};
            c->fly_off = 0;
            submit_send(fd);
        } else if (c->closing) {
            maybe_close(fd);
        }
    }
    ndirty = 0;
}

static int make_listener(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE}, *res;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc)); exit(1); }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* Inherited by accepted sockets. Keepalive reaps peers that vanished without a FIN or RST getting
     * through, so dead connections don't hold memory forever; live clients are never idle that long. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int idle = 120, intvl = 10, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    /* A previous instance's listening socket can outlive its process for a few seconds: the kernel tears
     * an io_uring down asynchronously, and pending accepts keep the socket open until then. Retry. */
    for (int tries = 0;; tries++) {
        if (!bind(fd, res->ai_addr, res->ai_addrlen) && !listen(fd, 1024)) break;
        if (errno != EADDRINUSE || tries >= 500) { perror("bind/listen"); exit(1); }
        if (tries == 0) fprintf(stderr, "port busy, waiting for it to be released\n");
        usleep(100000);
    }
    freeaddrinfo(res);
    return fd;
}

static pthread_mutex_t db_mu = PTHREAD_MUTEX_INITIALIZER;
static sem_t started;

#include "train.h"

static int worker_ok; /* set by a worker that initialized, read by main after `started` */

/* Called four times a second, by whichever worker gets there first. Every keep-alive connection costs about 4 KB of kernel
 * memory, so with enough users the box would run out of RAM: stop accepting before that happens.
 * MemAvailable overstates what sockets can get, so two things are subtracted: reclaimable slab, because
 * about 1 KB of each socket (its inode and dentry) is in there and can't be freed while it is open; and
 * free CMA pages, which only movable (user) allocations may use, never slab (on Ubuntu's 7.0 AWS kernel
 * that is ~400 MB of a 2 GB machine). */
static void check_memory(void)
{
    char buf[4096];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    const char *p = strstr(buf, "MemAvailable:"), *r = strstr(buf, "SReclaimable:"), *cma = strstr(buf, "CmaFree:");
    if (!p) return;
    long kb = strtol(p + 13, NULL, 10) - (r ? strtol(r + 13, NULL, 10) : 0) - (cma ? strtol(cma + 8, NULL, 10) : 0);
    /* how fast it is falling (smoothed), projected 2 s ahead */
    static long prev_kb;
    static double prev_t, slope;
    double dt = now_d - prev_t;
    if (prev_t > 0 && dt > 0) slope = 0.5 * slope + 0.5 * (kb - prev_kb) / dt;
    prev_kb = kb;
    prev_t = now_d;
    long ahead = kb + (long)(slope < 0 ? slope * 2 : 0);

    int low = __atomic_load_n(&mem_low, __ATOMIC_RELAXED), now;
    long shed = MEM_SHED_MB * 1024L;
    if (kb < shed / 2 || (low && ahead < shed / 2)) now = 3;   /* shed hard */
    else if (kb < shed || (low && ahead < shed)) now = 2;     /* shed */
    else if (kb < MEM_STOP_MB * 1024L) now = 1;
    else if (kb > MEM_RESUME_MB * 1024L) now = 0;
    else now = low != 0;
    if (now == low) return;
    __atomic_store_n(&mem_low, now, __ATOMIC_RELAXED);
    static const char *what[] = {"accepting again", "not accepting new connections", "closing idle connections",
                                 "closing idle connections quickly"};
    fprintf(stderr, "usable memory %ld MB (in 2 s: %ld MB): %s\n", kb / 1024, ahead / 1024, what[now]);
}

/* Under critical memory pressure: close some idle keep-alive connections (no request in progress). */
static __thread int shed_pos;
static void shed_idle(int k)
{
    for (int i = 0; i < nslots && k > 0; i++) {
        int fd = shed_pos;
        shed_pos = (shed_pos + 1) % nslots;
        conn_t *c = &conns[fd];
        if (!c->open || c->closing || c->in.len || c->out.len || c->sending) continue;
        c->closing = 1;
        maybe_close(fd);
        k--;
    }
}

/* Sets up this thread's ring, file table and recv buffers. Returns 0, or -1 after logging why. */
static int worker_init(int id)
{
    conns = calloc(nslots, sizeof *conns);
    dirty = calloc(nslots, sizeof *dirty);
    if (!conns || !dirty) { fprintf(stderr, "worker %d: out of memory\n", id); return -1; }

    /* Newer kernels charge ring memory to RLIMIT_MEMLOCK: on ENOMEM, retry smaller. On EINVAL (an
     * older kernel), retry without the optional flags. */
    int rc = -ENOMEM;
    for (unsigned sq = RING_ENTRIES, cq = CQ_ENTRIES; sq >= 64; sq /= 2, cq /= 2) {
        struct io_uring_params prm = {0};
        prm.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER |
                    IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE;
        prm.cq_entries = cq;
        rc = io_uring_queue_init_params(sq, &ring, &prm);
        if (rc == -EINVAL) {
            memset(&prm, 0, sizeof prm);
            prm.flags = IORING_SETUP_CQSIZE;
            prm.cq_entries = cq;
            rc = io_uring_queue_init_params(sq, &ring, &prm);
        }
        if (rc != -ENOMEM) break;
    }
    if (rc < 0) { fprintf(stderr, "worker %d: io_uring_queue_init: %s\n", id, strerror(-rc)); return -1; }
    io_uring_register_ring_fd(&ring);
    rc = io_uring_register_files_sparse(&ring, nslots);
    if (rc < 0) {
        fprintf(stderr, "worker %d: io_uring_register_files_sparse(%d): %s\n", id, nslots, strerror(-rc));
        io_uring_queue_exit(&ring);
        return -1;
    }
    bufmem = mmap(NULL, (size_t)NBUFS * BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bufmem == MAP_FAILED) { fprintf(stderr, "worker %d: out of memory\n", id); io_uring_queue_exit(&ring); return -1; }
    /* Recv buffers: a provided-buffer ring where the kernel allows it. Some kernels refuse to register
     * one (Ubuntu 24.04's 6.8.0-1xx returns EINVAL), so fall back to classic provided buffers, which
     * multishot recv selects from the same way; each buffer is then handed back with an SQE. */
    int err = -EINVAL;
#ifndef FORCE_LEGACY_BUFS /* testing aid */
    bufring = io_uring_setup_buf_ring(&ring, NBUFS, BUF_GROUP, 0, &err);
#endif
    if (bufring) {
        for (int i = 0; i < NBUFS; i++) recycle_buf(i);
        io_uring_buf_ring_advance(bufring, buf_adds);
        buf_adds = 0;
    } else {
        struct io_uring_sqe *s = get_sqe();
        io_uring_prep_provide_buffers(s, bufmem, BUF_SIZE, NBUFS, BUF_GROUP, 0);
        io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
        struct io_uring_cqe *cqe;
        rc = io_uring_submit_and_wait(&ring, 1);
        if (rc >= 0) rc = io_uring_wait_cqe(&ring, &cqe);
        if (rc >= 0) { rc = cqe->res; io_uring_cqe_seen(&ring, cqe); }
        if (rc < 0) {
            fprintf(stderr, "worker %d: provided buffers: %s (buffer ring: %s)\n", id, strerror(-rc), strerror(-err));
            io_uring_queue_exit(&ring);
            return -1;
        }
        if (id == 0) fprintf(stderr, "buffer ring unavailable (%s), using classic provided buffers\n", strerror(-err));
    }
    return 0;
}

static void *worker_main(void *arg)
{
    int id = (int)(intptr_t)arg;
    time_t last_sweep = 0;
    double last_shed = 0;
    update_clock();
    if (worker_init(id)) {
        worker_ok = 0;
        sem_post(&started);
        return NULL;
    }
    /* Workers queue up on the listener in order, and accept wakeups are exclusive: worker 0 gets every
     * connection while it is accepting. */
    arm_accept();
    io_uring_submit(&ring);
    worker_ok = 1;
    sem_post(&started);
    int rc;

    unsigned last_n = 0;
    for (;;) {
        /* Batching: under load, let completions pile up a little (DEFER_TASKRUN wakes us only when
         * WAIT_NR are pending or the timeout passes), so one loop iteration serves many requests. When
         * the last iteration was nearly idle, just wait for the next event. */
        if (WAIT_NR > 1 && last_n >= 4) {
            struct io_uring_cqe *c;
            struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = WAIT_USEC * 1000};
            rc = io_uring_submit_and_wait_timeout(&ring, &c, WAIT_NR, &ts, NULL);
            if (rc == -ETIME) rc = 0;
        } else {
            /* nearly idle: wait for one event, but wake every few seconds for the stalled-connection
             * sweep and the memory check */
            struct io_uring_cqe *c;
            struct __kernel_timespec ts = {.tv_sec = 2, .tv_nsec = 0};
            rc = io_uring_submit_and_wait_timeout(&ring, &c, 1, &ts, NULL);
            if (rc == -ETIME) rc = 0;
        }
        if (unlikely(rc < 0 && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY)) {
            /* e.g. ENOMEM, or EBADR after the kernel had to drop overflowed completions: keep serving */
            static __thread time_t last_err;
            if (now_s != last_err) { last_err = now_s; fprintf(stderr, "worker %d: io_uring_enter: %s\n", id, strerror(-rc)); }
            usleep(1000);
        }
        /* the first worker to see a quarter second pass since the last check does the next one */
        int64_t q = (int64_t)(now_d * 4), last = __atomic_load_n(&memcheck_q, __ATOMIC_RELAXED);
        if (q != last && __atomic_compare_exchange_n(&memcheck_q, &last, q, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) check_memory();
        int low = __atomic_load_n(&mem_low, __ATOMIC_RELAXED);
        if (unlikely(low)) {
            if (accepting) stop_accept();
            if (low >= 2 && now_d - last_shed >= 0.25) { last_shed = now_d; shed_idle(nopen / (low == 3 ? 20 : 50) + 1); }
        } else if (!accepting && nslots - nopen >= ACCEPT_RESUME) {
            arm_accept();
        }
        update_clock();
        pthread_mutex_lock(&db_mu);
        struct io_uring_cqe *cqe;
        unsigned head, n = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            n++;
            uint64_t u = cqe->user_data;
            int op = u & 0xff, fd = (u >> 8) & 0xffffff;
            uint32_t gen = u >> 32;
            switch (op) {
            case OP_ACCEPT: on_accept(cqe, gen); break;
            case OP_RECV: on_recv(cqe, fd, gen); break;
            case OP_SEND: on_send(cqe, fd, gen); break;
            }
        }
        io_uring_cq_advance(&ring, n);
        last_n = n;
        if (buf_adds) { io_uring_buf_ring_advance(bufring, buf_adds); buf_adds = 0; }
        return_bufs();
        end_batch();
        pthread_mutex_unlock(&db_mu);
        flush_dirty();
        if (unlikely(now_s - last_sweep >= 5)) { last_sweep = now_s; sweep_stalled(); }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--pgo-train")) {
        b64url_init();
        return pgo_train(argv[2]);
    }
    const char *host = getenv("HOST"), *port = getenv("PORT"), *secret = getenv("JWT_SECRET");
    db_path = getenv("SQLITE_PATH");
    if (!host) host = "0.0.0.0";
    if (!port) port = "3000";
    if (!db_path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    /* If memory ever runs out anyway (overload: every socket may hold a few KB of unread requests no
     * matter the TCP memory limits), be the OOM killer's first choice: our sockets go with us, which frees
     * the memory at once, and start.sh starts the server again. System services stay untouched. */
    int adj = open("/proc/self/oom_score_adj", O_WRONLY);
    if (adj >= 0) { if (write(adj, "1000", 4) != 4) {} close(adj); }

    /* One malloc arena for all workers: they take turns anyway, and per-thread arenas would each keep
     * their own high-water mark of SQLite and response memory. */
    mallopt(M_ARENA_MAX, 1);
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    b64url_init();
    hmac_sha256_key(&jwt_key, (const uint8_t *)secret, strlen(secret));

    /* A registered file table may have up to RLIMIT_NOFILE entries. */
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
    nslots = rl.rlim_cur > 65535 ? 65535 : (int)rl.rlim_cur;
#ifdef SLOT_LIMIT /* testing aid: spread few connections over several workers */
    nslots = SLOT_LIMIT;
#endif
    if (nslots <= ACCEPT_RESUME) { fprintf(stderr, "RLIMIT_NOFILE too low (%d)\n", nslots); return 1; }

    db_open();
    listen_fd = make_listener(host, port);
    sem_init(&started, 0, 0);
    int nworkers = 0;
    for (int i = 0; i < NWORKERS; i++) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1 << 20);
        if (pthread_create(&t, &at, worker_main, (void *)(intptr_t)i)) { perror("pthread_create"); return 1; }
        sem_wait(&started);
        if (!worker_ok) {
            if (i == 0) return 1;
            break; /* run with the workers we have: fewer connections, same speed */
        }
        nworkers++;
    }
    fprintf(stderr, "listening on %s:%s (%d workers x %d connections)\n", host, port, nworkers, nslots);
    for (;;) pause();
}
