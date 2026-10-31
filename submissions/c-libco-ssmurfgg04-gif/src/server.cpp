/* server.cpp - the c-libco server: Tencent libco event loop, one process
 * per core, coroutine per connection.
 *
 * What is deliberately NOT here, compared to the io_uring submission:
 *   - no connection state machine (the coroutine IS the state machine)
 *   - no send/recv buffer rings, no multishot plumbing, no epoll fallback
 *   - no write-backoff bookkeeping (co_poll(POLLOUT) on EAGAIN)
 *   - no db writer thread, no group-commit queue (transactions run inline
 *     in the request coroutine; a commit always precedes the send)
 *
 * The HTTP parsing, JWT, JSON and SQLite layers are the same C files the
 * io_uring submission ships (http.c/json.c/jwt.c/util.c/db.c); this file
 * only provides the connection layer those files talk to (server.h).
 *
 * Hook usage: accepted fds are marked O_NONBLOCK through libco's fcntl
 * hook, so read()/write() are single raw syscalls and EAGAIN is waited
 * out with co_poll(). The vendored libco widens its hook fd map from
 * 102400 to 1048576 entries (stock libco silently degrades the hooks
 * past 102400 fds per process); see README.md.
 *
 * Fork model: N processes (default 2, LIBCO_PROCS), each with its own
 * SO_REUSEPORT listener, its own SQLite connection and its own event
 * loop. Coroutines never run in two processes at once, so per-process
 * state needs no locks.
 */
#define _GNU_SOURCE
#include "libco/co_routine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "db.h"
#include "http.h"
#include "json.h"
#include "jwt.h"
#include "server.h"
#include "util.h"

/* defined in co_hook_sys_call.cpp, not declared in any header (C++ linkage) */
int co_accept(int fd, struct sockaddr *addr, socklen_t *len);

extern "C" {
long long db_req_count(void);
long long db_req_sum_us(void);
long long db_req_max_us(void);
void db_req_reset(void);
long long db_nfeed(void);
long long db_npost(void);
long long db_nlike(void);
long long db_ncreate(void);
long long db_nhealth(void);
}

/* ---------------------------------------------------------------- config */

#define MAXFDS_DEFAULT 400000
#define POOL_CHUNK 8192
#define MAX_OUT_HIGH 65536      /* stop parsing a connection with this much unsent */

enum {
    B_OPEN = 1 << 0,
    B_CLOSING = 1 << 3,
    B_TXN_NOTED = 1 << 6,
};

typedef struct {
    buf_t in;       /* bytes of a request that is not complete yet */
    buf_t out;      /* queued responses not yet handed to the kernel */
    uint32_t out_off;
    uint32_t gen;
    uint8_t bits;
    uint8_t conn_hdr;
    uint8_t sent_continue;
} conn_t;

struct task_t {
    stCoRoutine_t *co;
    int fd;
    char rbuf[2048];
};

/* per-process state; single-threaded event loop, no locks */
static conn_t *conns;
static int g_maxfds = MAXFDS_DEFAULT;
static std::vector<task_t *> g_idle;
static stShareStack_t *g_ss;
static long long g_created, g_accepts, g_closes, g_open, g_reqs;
static long long g_wegain, g_regain, g_txn_fail;
static int g_lfd = -1;
static uint64_t g_start_ms;
static int g_nprocs = 2, g_stacks = 128;

/* Responses queued while the transaction is open: if the commit fails,
 * they are replaced with one 500 per connection. Same scheme as the
 * io_uring submission, scoped to one request here instead of one batch. */
static struct {
    int slot;
    uint32_t gen;
    uint32_t start;
} txn_resps[64];
static int n_txn_resps;

static inline conn_t *C(int slot) { return &conns[slot]; }

static long rss_mb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size, resident;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
    fclose(f);
    return resident * 4096 / 1048576;
}

/* ---------------------------------------------------------------- responses */

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
    case 503: S("HTTP/1.1 503 Service Unavailable\r\n");
    default: S("HTTP/1.1 500 Internal Server Error\r\n");
    }
#undef S
}

int conn_open(int slot) { return (C(slot)->bits & B_OPEN) != 0; }
uint32_t conn_gen(int slot) { return C(slot)->gen; }
uint32_t conn_out_len(int slot) { return C(slot)->out.len; }
void conn_truncate_out(int slot, uint32_t to) { C(slot)->out.len = to; }

/* Writes status line + headers and reserves n bytes for the body; returns
 * where the caller writes the body (out.len is already past it). */
char *resp_alloc(int slot, int status, size_t n, int close_conn)
{
    conn_t *c = C(slot);
    if (!(c->bits & B_OPEN)) return NULL;
    if (c->conn_hdr == 1) close_conn = 1;

    if (db_in_txn() && !(c->bits & B_TXN_NOTED) &&
        n_txn_resps < (int)(sizeof txn_resps / sizeof txn_resps[0])) {
        txn_resps[n_txn_resps].slot = slot;
        txn_resps[n_txn_resps].gen = c->gen;
        txn_resps[n_txn_resps].start = c->out.len;
        n_txn_resps++;
        c->bits |= B_TXN_NOTED;
    }

    size_t sl;
    const char *s = status_line(status, &sl);
    if (buf_reserve(&c->out, sl + 128 + n)) {
        c->bits |= B_CLOSING;
        return NULL;
    }
    char *o = c->out.p + c->out.len;
    memcpy(o, s, sl);
    o += sl;
    memcpy(o, "Content-Type: application/json\r\nContent-Length: ", 48);
    o += 48;
    o = u64_to(o, n);
    if (close_conn) {
        memcpy(o, "\r\nConnection: close\r\n\r\n", 23);
        o += 23;
        c->bits |= B_CLOSING;
    } else if (c->conn_hdr == 2) {
        memcpy(o, "\r\nConnection: keep-alive\r\n\r\n", 28);
        o += 28;
    } else {
        memcpy(o, "\r\n\r\n", 4);
        o += 4;
    }
    char *body = o;
    c->out.len = (uint32_t)(o + n - c->out.p);
    return body;
}

void respond(int slot, int status, const char *b, size_t n, int close_conn)
{
    char *o = resp_alloc(slot, status, n, close_conn);
    if (o && n) memcpy(o, b, n);
}

static void err_body(int slot, int status, const char *msg, int close_conn)
{
    char b[256];
    int n = snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    respond(slot, status, b, (size_t)n, close_conn);
}

void srv_err(int slot, int status, const char *msg) { err_body(slot, status, msg, 0); }
void srv_err_500(int slot) { err_body(slot, 500, "internal server error", 0); }
void srv_err_close(int slot, int status, const char *msg) { err_body(slot, status, msg, 1); }

int srv_sent_continue(int slot) { return C(slot)->sent_continue; }
void srv_send_continue(int slot)
{
    conn_t *c = C(slot);
    buf_put(&c->out, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    c->sent_continue = 1;
}
void srv_reset_continue(int slot) { C(slot)->sent_continue = 0; }
void srv_set_conn_hdr(int slot, int mode) { C(slot)->conn_hdr = (uint8_t)mode; }

/* ---------------------------------------------------------------- request flow */

/* Commit the transaction (if any) and fix up the responses that were
 * queued while it was open: on failure they become a single 500 each. */
static void end_txn(void)
{
    if (!db_in_txn()) {
        for (int i = 0; i < n_txn_resps; i++)
            C(txn_resps[i].slot)->bits &= (uint8_t)~B_TXN_NOTED;
        n_txn_resps = 0;
        return;
    }
    int ok = db_commit();
    if (!ok) g_txn_fail++;
    for (int i = 0; i < n_txn_resps; i++) {
        int slot = txn_resps[i].slot;
        conn_t *c = C(slot);
        if ((c->bits & B_TXN_NOTED) && c->gen == txn_resps[i].gen && (c->bits & B_OPEN))
            c->bits &= (uint8_t)~B_TXN_NOTED;
        if (ok) continue;
        if (!(c->bits & B_OPEN) || c->gen != txn_resps[i].gen) continue;
        c->out.len = txn_resps[i].start;
        c->bits &= (uint8_t)~B_CLOSING;
        srv_err_500(slot);
    }
    n_txn_resps = 0;
}

void on_data(int slot, const char *data, size_t n)
{
    conn_t *c = C(slot);
    if (c->bits & B_CLOSING) return;
    const char *p = data;
    if (c->in.len) {
        if (n) buf_put(&c->in, data, n);
        p = c->in.p;
        n = c->in.len;
    }
    size_t off = 0;
    while (off < n && !(c->bits & B_CLOSING)) {
        if (c->out.len - c->out_off > MAX_OUT_HIGH) break;   /* flush first */
        long long t0 = now_mono_us();
        size_t r = handle_request(slot, p + off, n - off);
        db_req_note(now_mono_us() - t0);
        if (!r) break;
        off += r;
        g_reqs++;
    }
    size_t rest = (c->bits & B_CLOSING) ? 0 : n - off;
    if (c->in.len) {
        if (rest) memmove(c->in.p, c->in.p + off, rest);
        c->in.len = (uint32_t)rest;
        if (!rest) buf_free(&c->in);
    } else if (rest) {
        buf_put(&c->in, p + off, rest);
    }
}

/* ---------------------------------------------------------------- coroutines */

static void grow_pool(int n);   /* below: the acceptor grows the pool on demand */

static void park_task(task_t *t)
{
    t->fd = -1;
    g_idle.push_back(t);
    co_yield_ct();
}

static void drop_conn(task_t *t)
{
    conn_t *c = &conns[t->fd];
    close(t->fd);
    if (c->in.p) buf_free(&c->in);
    if (c->out.p) buf_free(&c->out);
    c->bits = 0;
    c->conn_hdr = 0;
    c->sent_continue = 0;
    c->out_off = 0;
    c->in.len = c->in.cap = 0;
    c->out.len = c->out.cap = 0;
    c->gen++;
    g_closes++;
    g_open--;
}

/* One turn of the connection loop: flush, parse (leftover and/or fresh
 * bytes), commit, repeat. Returns 0 when the connection is done.
 *
 * The no-busy-spin rule: on_data("") on leftover bytes is only retried
 * when the previous pass made progress (consumed bytes or produced
 * output). With no progress we always end up in co_poll(POLLIN), so an
 * incomplete request parks the coroutine instead of spinning the loop. */
static int conn_step(task_t *t)
{
    int fd = t->fd;
    conn_t *c = &conns[fd];

    /* flush queued responses first */
    while (c->out.len > c->out_off) {
        int w = write(fd, c->out.p + c->out_off, c->out.len - c->out_off);
        if (w > 0) { c->out_off += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            g_wegain++;
            struct pollfd pf = { .fd = fd, .events = POLLOUT | POLLERR | POLLHUP };
            co_poll(co_get_epoll_ct(), &pf, 1, 5000);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return 0;
    }
    c->out.len = 0;
    c->out_off = 0;
    if (c->bits & B_CLOSING) return 0;

    /* try to parse leftover bytes (possible now that out is drained) */
    if (c->in.len) {
        uint32_t before = c->in.len;
        on_data(fd, "", 0);
        end_txn();   /* a transaction from this request commits before its bytes go out */
        if (c->out.len > 0 || c->in.len < before) return 1;
        /* no progress: the request is incomplete, wait for more bytes */
    }

    struct pollfd pf = { .fd = fd, .events = POLLIN | POLLERR | POLLHUP };
    co_poll(co_get_epoll_ct(), &pf, 1, -1);
    int r = read(fd, t->rbuf, sizeof t->rbuf);
    if (r == 0) return 0;
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) { g_regain++; return 1; }
        if (errno == EINTR) return 1;
        return 0;
    }
    on_data(fd, t->rbuf, (size_t)r);
    end_txn();
    return 1;
}

static void *conn_routine(void *arg)
{
    co_enable_hook_sys();
    task_t *t = (task_t *)arg;

    for (;;) {
        if (t->fd == -1) { park_task(t); continue; }
        while (t->fd != -1 && conn_step(t)) {}
        drop_conn(t);
        park_task(t);
    }
    return NULL;
}

static void *accept_routine(void *)
{
    co_enable_hook_sys();
    for (;;) {
        struct sockaddr_in addr;
        socklen_t len = sizeof addr;
        int fd = co_accept(g_lfd, (struct sockaddr *)&addr, &len);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) usleep(1000);
            struct pollfd pf = { .fd = g_lfd, .events = POLLIN | POLLERR | POLLHUP };
            co_poll(co_get_epoll_ct(), &pf, 1, 1000);
            continue;
        }
        if (fd >= g_maxfds) { close(fd); continue; }
        if (g_idle.empty()) {
            /* pool exhausted: grow it (the cap above bounds memory) */
            grow_pool(POOL_CHUNK);
            if (g_idle.empty()) { close(fd); continue; }
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int fl = fcntl(fd, F_GETFL, 0);        /* through libco's fcntl hook:
                                                  the fd map records O_NONBLOCK
                                                  so read/write stay raw */
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        conns[fd].bits = B_OPEN;
        g_accepts++;
        g_open++;
        task_t *t = g_idle.back();
        g_idle.pop_back();
        t->fd = fd;
        co_resume(t->co);
    }
    return NULL;
}

static void *stats_routine(void *)
{
    co_enable_hook_sys();
    long long t = 0, prev_reqs = 0, prev_acc = 0, prev_close = 0, prev_weg = 0;
    for (;;) {
        poll(NULL, 0, 5000);   /* hooked poll: coroutine-safe sleep */
        t += 5;
        db_checkpoint();
        fprintf(stderr,
            "libcostats: t=%llds pid=%d open=%lld acc=%lld (+%lld) closes=%lld (+%lld) "
            "reqs=%lld (+%lld) | feed=%lld post=%lld like=%lld create=%lld health=%lld | "
            "db avg=%lldus max=%lldus wal=%lldKB txn_fail=%lld | "
            "weagain=%lld (+%lld) reag=%lld | cos=%lld idle=%zu rss=%ldMB\n",
            t, (int)getpid(), g_open, g_accepts, g_accepts - prev_acc,
            g_closes, g_closes - prev_close,
            g_reqs, g_reqs - prev_reqs,
            db_nfeed(), db_npost(), db_nlike(), db_ncreate(), db_nhealth(),
            db_req_count() ? db_req_sum_us() / db_req_count() : 0,
            db_req_max_us(), db_wal_bytes() / 1024, g_txn_fail,
            g_wegain, g_wegain - prev_weg, g_regain,
            g_created, g_idle.size(), rss_mb());
        fflush(stderr);
        db_req_reset();
        prev_reqs = g_reqs; prev_acc = g_accepts;
        prev_close = g_closes; prev_weg = g_wegain;
    }
    return NULL;
}

/* ---------------------------------------------------------------- boot */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void grow_pool(int n)
{
    stCoRoutineAttr_t attr;
    memset(&attr, 0, sizeof attr);
    attr.stack_size = 0;
    attr.share_stack = g_ss;
    for (int i = 0; i < n && g_created < g_maxfds; i++) {
        task_t *t = (task_t *)calloc(1, sizeof *t);
        if (!t) { perror("calloc"); exit(1); }
        t->fd = -1;
        co_create(&t->co, &attr, conn_routine, t);
        g_created++;
        co_resume(t->co);   /* runs until it parks itself in g_idle */
    }
}

static int make_listener(const char *host, const char *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(port));
    if (!host || !*host || !strcmp(host, "0.0.0.0") || !strcmp(host, "0"))
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        fprintf(stderr, "unsupported HOST %s\n", host);
        exit(1);
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 65536) < 0) { perror("listen"); exit(1); }
    set_nonblock(fd);
    return fd;
}

static int child_main(const char *host, const char *port, const char *db_path,
                      const char *secret)
{
    db_thread_init();
    db_open(db_path);
    jwt_b64url_init();
    jwt_init(secret, strlen(secret));
    signal(SIGPIPE, SIG_IGN);

    g_lfd = make_listener(host, port);

    g_ss = co_alloc_sharestack(g_stacks, 128 * 1024);
    grow_pool(POOL_CHUNK);

    stCoRoutine_t *co = NULL;
    co_create(&co, NULL, accept_routine, NULL);
    co_resume(co);
    co_create(&co, NULL, stats_routine, NULL);
    co_resume(co);

    fprintf(stderr, "c-libco: pid %d on %s:%s (%d procs, %d shared stacks, "
            "%d fds max)\n",
            (int)getpid(), host, port, g_nprocs, g_stacks, g_maxfds);
    co_eventloop(co_get_epoll_ct(), NULL, NULL);
    return 0;
}

int main(void)
{
    const char *host = getenv("HOST"), *port = getenv("PORT");
    const char *secret = getenv("JWT_SECRET"), *db_path = getenv("SQLITE_PATH");
    const char *p;
    if (!host) host = "0.0.0.0";
    if (!port) port = "3000";
    if (!db_path || !secret) {
        fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
        return 1;
    }
    if ((p = getenv("LIBCO_PROCS")) && atoi(p) > 0) g_nprocs = atoi(p);
    else {
        /* default: a single process on small-RAM boxes (the challenge droplet
         * is 1 vCPU / 2GB, and one process keeps the SQLite page cache and
         * wal-index unique), two otherwise */
        long long kb = 0;
        FILE *f = fopen("/proc/meminfo", "r");
        if (f && fscanf(f, "MemTotal: %lld kB", &kb) == 1 && kb > 0 && kb < 3500 * 1024)
            g_nprocs = 1;
        if (f) fclose(f);
    }
    if ((p = getenv("LIBCO_STACKS")) && atoi(p) > 0) g_stacks = atoi(p);
    if ((p = getenv("LIBCO_MAXCONN")) && atoi(p) > 0) g_maxfds = atoi(p);

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < (rlim_t)g_maxfds) g_maxfds = (int)rl.rlim_cur;

    conns = (conn_t *)calloc((size_t)g_maxfds, sizeof *conns);
    if (!conns) { perror("calloc conns"); return 1; }
    g_start_ms = now_mono_ms();

    for (int i = 0; i < g_nprocs; i++) {
        pid_t pid = fork();
        if (pid == 0) return child_main(host, port, db_path, secret);
        if (pid < 0) { perror("fork"); return 1; }
    }
    while (wait(NULL) > 0) {}
    return 0;
}
