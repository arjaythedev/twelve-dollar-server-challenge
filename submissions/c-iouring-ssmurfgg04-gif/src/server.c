/* server.c - connections, responses, group commit, and two event loops:
 * io_uring (multishot accept/recv, buffer ring, direct descriptors) and an
 * epoll fallback for kernels without multishot (pre-5.19). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <liburing.h>

#include "db.h"
#include "http.h"
#include "json.h"
#include "jwt.h"
#include "server.h"
#include "util.h"

/* ---------------------------------------------------------------- config */

#define RING_ENTRIES 1024
#define CQ_ENTRIES 8192
#define BUF_GROUP 0
#define NBUFS 2048
#define BUF_SIZE 2048
#define MAX_OUT_HIGH 65536      /* stop reading a connection with this much unsent */
#define STALL_MS 30000
#define NWORKERS 8

#ifndef WAIT_NR
#define WAIT_NR 128             /* wake the loop once this many completions are ready... */
#endif
#ifndef WAIT_US
#define WAIT_US 4000            /* ...or this long after it went to sleep */
#endif

/* memory guardrails: usable MemAvailable in MB */
#define MEM_STOP_MB 192
#define MEM_RESUME_MB 288
#define MEM_SHED_MB 96

#define ACCEPT_STOP 1024        /* stop accepting with fewer free slots than this */
#define LISTENER_UD 0xffffffffffffffffULL
#define ACCEPT_RESUME 2048

enum {
    B_OPEN = 1 << 0,
    B_RECV_ON = 1 << 1,
    B_SENDING = 1 << 2,
    B_CLOSING = 1 << 3,
    B_SHUT = 1 << 4,
    B_PAUSED = 1 << 5,
    B_DIRTY = 1 << 7,
};

/* Connection state; an idle keep-alive holds nothing but this. */
typedef struct {
    buf_t in;       /* bytes of a request that is not complete yet */
    buf_t out;      /* queued responses not yet handed to the kernel */
    buf_t fly;      /* uring: the buffer the kernel is sending from */
    uint32_t fly_off;
    uint32_t out_off; /* epoll: send offset into out */
    uint32_t gen;
    uint32_t since_ms;
    uint8_t bits;
    uint8_t conn_hdr;
    uint8_t sent_continue;
} conn_t;

/* per worker */
static __thread conn_t *conns;
static __thread int nopen;
static __thread int accepting;
static __thread uint32_t accept_gen;
static __thread struct io_uring ring;
static __thread struct io_uring_buf_ring *bufring;
static __thread char *bufmem;
static __thread int buf_adds;
static __thread int epfd = -1;
static __thread int *dirty;
static __thread int ndirty;

static int listen_fd;
static int nslots;              /* file table slots (shared, set before workers start) */
static int g_mode_uring = 1;    /* 0: epoll fallback */
static atomic_int mem_low;      /* 0 ok, 1 not accepting, 2+ shedding */
static atomic_llong memcheck_q;
static uint64_t start_ms;
static int g_pgo_secs = 0;

static inline conn_t *C(int slot) { return &conns[slot]; }

/* user_data: op | slot << 8 | gen << 32 */
enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_IGNORE };
static inline uint64_t ud(int op, int slot, uint32_t gen)
{
    return (uint64_t)op | (uint64_t)slot << 8 | (uint64_t)gen << 32;
}

static void e_close_conn(int slot);
static void sweep_stalled(void);
static void u_pause_recv(int slot);
static __thread uint32_t shed_pos;

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

static void mark_dirty(int slot)
{
    if (!(C(slot)->bits & B_DIRTY)) {
        C(slot)->bits |= B_DIRTY;
        dirty[ndirty++] = slot;
    }
}

/* Writes status line + headers and reserves n bytes for the body; returns
 * where the caller writes the body (out.len is already past it). */
char *resp_alloc(int slot, int status, size_t n, int close_conn)
{
    conn_t *c = C(slot);
    if (!(c->bits & B_OPEN)) return NULL;
    if (c->conn_hdr == 1) close_conn = 1;

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
    mark_dirty(slot);
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
    mark_dirty(slot);
}
void srv_reset_continue(int slot) { C(slot)->sent_continue = 0; }
void srv_set_conn_hdr(int slot, int mode) { C(slot)->conn_hdr = (uint8_t)mode; }

/* ---------------------------------------------------------------- request flow */

void on_data(int slot, const char *data, size_t n)
{
    conn_t *c = C(slot);
#ifdef DBG
    fprintf(stderr, "DBG on_data slot=%d n=%zu bits=%x\n", slot, n, c->bits);
#endif
    if (c->bits & B_CLOSING) return;
    const char *p = data;
    if (c->in.len) {
        if (n) buf_put(&c->in, data, n);
        p = c->in.p;
        n = c->in.len;
    }
    size_t off = 0;
    uint32_t now = (uint32_t)now_mono_ms();
    while (off < n && !(c->bits & B_CLOSING)) {
        if (c->out.len - c->out_off > MAX_OUT_HIGH) {
            c->bits |= B_PAUSED;
            if (g_mode_uring) u_pause_recv(slot);
            break;
        }
        long long _t0 = now_mono_us();
        size_t r = handle_request(slot, p + off, n - off);
        db_req_note(now_mono_us() - _t0);
        if (!r) break;
        off += r;
        c->since_ms = now;
    }
    size_t rest = (c->bits & B_CLOSING) ? 0 : n - off;
    if (c->in.len) {
        if (rest) memmove(c->in.p, c->in.p + off, rest);
        c->in.len = (uint32_t)rest;
        if (!rest) buf_free(&c->in);
    } else if (rest) {
        buf_put(&c->in, p + off, rest);
        c->since_ms = now;
    }
}

/* ---------------------------------------------------------------- upkeep */

/* Usable memory: MemAvailable minus reclaimable slab and free CMA (which
 * sockets can never use), four times a second, by whichever worker is first. */
static void check_memory(void)
{
    char buf[4096];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = strstr(buf, "MemAvailable:");
    if (!p) return;
    long kb = strtol(p + 13, NULL, 10);
    char *r = strstr(buf, "SReclaimable:");
    if (r) kb -= strtol(r + 13, NULL, 10);
    char *cma = strstr(buf, "CmaFree:");
    if (cma) kb -= strtol(cma + 8, NULL, 10);

    int low = atomic_load(&mem_low);
    int now;
    if (kb < MEM_SHED_MB * 1024L / 2) now = 3;
    else if (kb < MEM_SHED_MB * 1024L) now = 2;
    else if (kb < MEM_STOP_MB * 1024L) now = 1;
    else if (kb > MEM_RESUME_MB * 1024L) now = 0;
    else now = low != 0;
    if (now != low) {
        atomic_store(&mem_low, now);
        static const char *what[] = { "accepting again", "not accepting new connections",
                                      "closing idle connections", "closing idle connections quickly" };
        fprintf(stderr, "usable memory %ld MB: %s\n", kb / 1024, what[now]);
    }
}

/* ---------------------------------------------------------------- io_uring */

static struct io_uring_sqe *get_sqe(void)
{
    struct io_uring_sqe *s = io_uring_get_sqe(&ring);
    while (!s) {
        io_uring_submit(&ring);
        s = io_uring_get_sqe(&ring);
    }
    return s;
}

static void u_arm_accept(void)
{
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_multishot_accept_direct(s, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(s, ud(OP_ACCEPT, 0, ++accept_gen));
    accepting = 1;
}

static void u_stop_accept(void)
{
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_cancel64(s, ud(OP_ACCEPT, 0, accept_gen), 0);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
    accepting = 0;
}

static void u_arm_recv(int slot)
{
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_recv_multishot(s, slot, NULL, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
    s->buf_group = BUF_GROUP;
    io_uring_sqe_set_data64(s, ud(OP_RECV, slot, conns[slot].gen));
    conns[slot].bits |= B_RECV_ON;
}

static void u_pause_recv(int slot)
{
    conn_t *c = C(slot);
    if (!(c->bits & B_RECV_ON)) return;
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_cancel64(s, ud(OP_RECV, slot, c->gen), 0);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
}

static void u_submit_send(int slot)
{
    conn_t *c = C(slot);
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_send(s, slot, c->fly.p + c->fly_off, c->fly.len - c->fly_off, MSG_NOSIGNAL);
    s->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(s, ud(OP_SEND, slot, c->gen));
    c->bits |= B_SENDING;
    c->since_ms = (uint32_t)now_mono_ms();
}

static void u_shut_conn(int slot)
{
    conns[slot].bits |= B_SHUT;
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_shutdown(s, slot, SHUT_RDWR);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, slot, 0));
    s->flags |= IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS;
}

/* Close once nothing is in flight; a multishot recv still armed is ended
 * by a shutdown first (its last CQE routes back here). */
static void u_maybe_close(int slot)
{
    conn_t *c = C(slot);
    if (!(c->bits & B_CLOSING) || (c->bits & B_SENDING) || c->out.len) return;
    if (c->bits & B_RECV_ON) {
        if (!(c->bits & B_SHUT)) u_shut_conn(slot);
        return;
    }
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_close_direct(s, slot);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, slot, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
    buf_free(&c->in);
    buf_free(&c->out);
    buf_free(&c->fly);
    c->bits = 0;
    c->gen++;
    nopen--;
    if (!accepting && nslots - nopen >= ACCEPT_RESUME && !atomic_load(&mem_low)) u_arm_accept();
}

static void u_on_accept(struct io_uring_cqe *cqe, uint32_t gen)
{
    int slot = cqe->res;
    if (!(cqe->flags & IORING_CQE_F_MORE) && accepting && gen == accept_gen) {
        /* the multishot accept ended on its own (an error): arm it again */
        accepting = 0;
        if (nslots - nopen >= ACCEPT_STOP) u_arm_accept();
    }
    if (slot < 0) return;
    conn_t *c = &conns[slot];
    uint32_t g = c->gen;
    memset(c, 0, sizeof *c);
    c->gen = g;
    c->bits = B_OPEN;
    nopen++;
    u_arm_recv(slot);
    if (accepting && nslots - nopen < ACCEPT_STOP) u_stop_accept();
}

/* classic provided buffers, coalesced into runs (kernels that refuse buf rings) */
static __thread int ret_start, ret_n;

static void u_return_bufs(void)
{
    if (!ret_n) return;
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_provide_buffers(s, bufmem + (size_t)ret_start * BUF_SIZE,
                                 BUF_SIZE, ret_n, BUF_GROUP, ret_start);
    io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS;
    ret_n = 0;
}

static void u_recycle_buf(int bid)
{
    if (bufring) {
        io_uring_buf_ring_add(bufring, bufmem + (size_t)bid * BUF_SIZE, BUF_SIZE, bid,
                              io_uring_buf_ring_mask(NBUFS), buf_adds++);
        return;
    }
    if (ret_n && bid == ret_start + ret_n) { ret_n++; return; }
    u_return_bufs();
    ret_start = bid;
    ret_n = 1;
}

static void u_on_recv(struct io_uring_cqe *cqe, int slot, uint32_t gen)
{
    conn_t *c = &conns[slot];
    int res = cqe->res;
    int more = cqe->flags & IORING_CQE_F_MORE;

    if (cqe->flags & IORING_CQE_F_BUFFER) {
        int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        if (res > 0 && (c->bits & B_OPEN) && c->gen == gen)
            on_data(slot, bufmem + (size_t)bid * BUF_SIZE, (size_t)res);
        u_recycle_buf(bid);
    }
    if (!(c->bits & B_OPEN) || c->gen != gen) return;
    if (!more) {
        c->bits &= (uint8_t)~B_RECV_ON;
        if ((c->bits & B_PAUSED) && !(c->bits & B_CLOSING)) return; /* resume_conn re-arms */
        if (res == -ENOBUFS && !(c->bits & B_CLOSING)) { u_return_bufs(); u_arm_recv(slot); }
        else if (res > 0 && !(c->bits & B_CLOSING)) u_arm_recv(slot);
        else c->bits |= B_CLOSING;
    }
    if (c->bits & B_CLOSING) u_maybe_close(slot);
}

static void u_on_send(struct io_uring_cqe *cqe, int slot, uint32_t gen)
{
    conn_t *c = &conns[slot];
    if (!(c->bits & B_OPEN) || c->gen != gen) return;
    c->bits &= (uint8_t)~B_SENDING;
    int res = cqe->res;
    if (res < 0) {
        if (res == -EAGAIN || res == -EINTR) { u_submit_send(slot); return; }
        c->bits |= B_CLOSING;
        c->out.len = 0;
        c->fly.len = 0;
        if ((c->bits & B_RECV_ON) && !(c->bits & B_SHUT)) u_shut_conn(slot);
        u_maybe_close(slot);
        return;
    }
    c->fly_off += (uint32_t)res;
    if (c->fly_off < c->fly.len) { u_submit_send(slot); return; }
    buf_free(&c->fly);
    c->fly_off = 0;
    if ((c->bits & B_PAUSED) && c->out.len <= MAX_OUT_HIGH / 2) {
        c->bits &= (uint8_t)~B_PAUSED;
        if (c->in.len) on_data(slot, NULL, 0);
        if (!(c->bits & B_PAUSED) && !(c->bits & B_RECV_ON) && !(c->bits & B_CLOSING))
            u_arm_recv(slot);
    }
    if (c->out.len) mark_dirty(slot);
    else if (c->bits & B_CLOSING) u_maybe_close(slot);
}

/* Hands queued responses to the kernel: swap out -> fly and send. */
static void u_flush_dirty(void)
{
    for (int i = 0; i < ndirty; i++) {
        int slot = dirty[i];
        conn_t *c = &conns[slot];
        c->bits &= (uint8_t)~B_DIRTY;
        if (!(c->bits & B_OPEN) || (c->bits & B_SENDING)) continue;
        if (c->out.len) {
            buf_free(&c->fly);
            c->fly = c->out;
            c->out = (buf_t){ 0 };
            c->fly_off = 0;
            u_submit_send(slot);
        } else if (c->bits & B_CLOSING) {
            u_maybe_close(slot);
        }
    }
    ndirty = 0;
}

static int u_worker_init(int id)
{
    conns = calloc((size_t)nslots, sizeof *conns);
    dirty = malloc((size_t)nslots * sizeof *dirty);
    if (!conns || !dirty) {
        fprintf(stderr, "worker %d: out of memory\n", id);
        return -1;
    }

    int rc = -ENOMEM;
    for (unsigned sq = RING_ENTRIES, cq = CQ_ENTRIES; sq >= 64; sq /= 2, cq /= 2) {
        struct io_uring_params prm;
        memset(&prm, 0, sizeof prm);
        prm.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN |
                    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE;
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
    if (rc < 0) {
        fprintf(stderr, "worker %d: io_uring_queue_init: %s\n", id, strerror(-rc));
        return -1;
    }
    io_uring_register_ring_fd(&ring);
    rc = io_uring_register_files_sparse(&ring, (unsigned)nslots);
    if (rc < 0) {
        fprintf(stderr, "worker %d: register_files_sparse(%d): %s\n", id, nslots, strerror(-rc));
        io_uring_queue_exit(&ring);
        return -1;
    }

    bufmem = mmap(NULL, (size_t)NBUFS * BUF_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bufmem == MAP_FAILED) {
        fprintf(stderr, "worker %d: out of memory (buffers)\n", id);
        io_uring_queue_exit(&ring);
        return -1;
    }
    int err = -EINVAL;
    bufring = io_uring_setup_buf_ring(&ring, NBUFS, BUF_GROUP, 0, &err);
    if (bufring) {
        for (int i = 0; i < NBUFS; i++) u_recycle_buf(i);
        io_uring_buf_ring_advance(bufring, buf_adds);
        buf_adds = 0;
    } else {
        /* classic provided buffers: multishot recv selects from them the
         * same way; each buffer is handed back with an SQE */
        struct io_uring_sqe *s = get_sqe();
        io_uring_prep_provide_buffers(s, bufmem, BUF_SIZE, NBUFS, BUF_GROUP, 0);
        io_uring_sqe_set_data64(s, ud(OP_IGNORE, 0, 0));
        struct io_uring_cqe *cqe = NULL;
        rc = io_uring_submit_and_wait(&ring, 1);
        if (rc >= 0) rc = io_uring_wait_cqe(&ring, &cqe);
        if (rc >= 0) { rc = cqe->res; io_uring_cqe_seen(&ring, cqe); }
        if (rc < 0) {
            fprintf(stderr, "worker %d: provided buffers: %s (buf ring: %s)\n",
                    id, strerror(-rc), strerror(-err));
            io_uring_queue_exit(&ring);
            return -1;
        }
        if (id == 0) fprintf(stderr, "buffer ring unavailable (%s), classic provided buffers\n", strerror(-err));
    }
    return 0;
}

static void *u_worker_main(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint64_t last_sweep = now_mono_ms();
    unsigned last_n = 0;
    int rc = 0;
    if (u_worker_init(id)) return NULL;
    db_thread_init();

    u_arm_accept();
    io_uring_submit(&ring);
    fprintf(stderr, "worker %d: ready (%d slots)\n", id, nslots);

    for (;;) {
        struct io_uring_cqe *cqe;
        unsigned head, n = 0;
        if (WAIT_NR > 1 && last_n >= 4) {
            struct __kernel_timespec ts = { 0, WAIT_US * 1000 };
            rc = io_uring_submit_and_wait_timeout(&ring, &cqe, WAIT_NR, &ts, NULL);
            if (rc == -ETIME) rc = 0;
        } else {
            struct __kernel_timespec ts = { 2, 0 };
            rc = io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &ts, NULL);
            if (rc == -ETIME) rc = 0;
        }
        if (rc < 0 && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY) {
            static __thread time_t last_err;
            time_t nows = time(NULL);
            if (nows != last_err) { last_err = nows; fprintf(stderr, "worker %d: enter: %s\n", id, strerror(-rc)); }
            usleep(1000);
        }

        int64_t q = (int64_t)(now_realtime() * 4);
        int64_t last = atomic_load(&memcheck_q);
        if (q != last && atomic_compare_exchange_strong(&memcheck_q, &last, q)) check_memory();
        int low = atomic_load(&mem_low);
        if (low) {
            if (accepting) u_stop_accept();
        } else if (!accepting && nslots - nopen >= ACCEPT_RESUME) {
            u_arm_accept();
            io_uring_submit(&ring);
        }

        io_uring_for_each_cqe(&ring, head, cqe) {
            n++;
            uint64_t u = cqe->user_data;
            int op = (int)(u & 0xff), slot = (int)((u >> 8) & 0xffffff);
            uint32_t gen = (uint32_t)(u >> 32);
            switch (op) {
            case OP_ACCEPT: u_on_accept(cqe, gen); break;
            case OP_RECV: u_on_recv(cqe, slot, gen); break;
            case OP_SEND: u_on_send(cqe, slot, gen); break;
            default: break;
            }
        }
        io_uring_cq_advance(&ring, n);
        last_n = n;
        if (buf_adds) { io_uring_buf_ring_advance(bufring, buf_adds); buf_adds = 0; }
        u_return_bufs();
        u_flush_dirty();

        uint64_t now = now_mono_ms();
        if (now - last_sweep >= 5000 || (low >= 2 && now - last_sweep >= 250)) {
            last_sweep = now;
            sweep_stalled();
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- sweep */

/* Close connections whose partial request or unsent response made no
 * progress for STALL_MS. Idle keep-alives are left alone (except under
 * memory pressure, where some are shed). */
static void sweep_stalled(void)
{
    uint32_t now = (uint32_t)now_mono_ms();
    int low = atomic_load(&mem_low);
    int shed = low >= 2 ? nopen / (low == 3 ? 20 : 50) + 1 : 0;
    for (int slot = 0; slot < nslots; slot++) {
        conn_t *c = &conns[slot];
        if (!(c->bits & B_OPEN) || (c->bits & B_CLOSING)) continue;
        int stalled = (c->in.len || (c->bits & B_SENDING) || c->out.len) &&
                      now - c->since_ms > STALL_MS;
        int idle_shed = 0;
        if (shed > 0 && !c->in.len && !c->out.len && !(c->bits & B_SENDING)) {
            if ((uint32_t)slot == shed_pos) { idle_shed = 1; shed--; }
        }
        if (stalled || idle_shed) {
            c->bits |= B_CLOSING;
            c->out.len = 0;
            if (g_mode_uring) {
                if ((c->bits & B_RECV_ON) && !(c->bits & B_SHUT)) u_shut_conn(slot);
                u_maybe_close(slot);
            } else {
                e_close_conn(slot);
            }
        }
    }
    shed_pos = (shed_pos + 1) % (uint32_t)(nslots ? nslots : 1);
}

/* ---------------------------------------------------------------- epoll */

static void e_mod(int slot, int want_out)
{
    struct epoll_event ev;
    ev.events = EPOLLIN | (want_out ? EPOLLOUT : 0);
    ev.data.u64 = (uint64_t)conns[slot].gen << 32 | (uint32_t)slot;
    epoll_ctl(epfd, EPOLL_CTL_MOD, slot, &ev);
}

static void e_close_conn(int slot)
{
    conn_t *c = C(slot);
    if (!(c->bits & B_OPEN)) return;
    c->bits &= (uint8_t)~(B_RECV_ON | B_SENDING | B_PAUSED | B_DIRTY);
    buf_free(&c->in);
    buf_free(&c->out);
    c->out_off = 0;
    close(slot);
    c->bits = 0;
    c->gen++;
    nopen--;
}

/* Sends what it can right now; registers EPOLLOUT for the rest. */
static void e_flush_dirty(void)
{
    for (int i = 0; i < ndirty; i++) {
        int slot = dirty[i];
        conn_t *c = &conns[slot];
        c->bits &= (uint8_t)~B_DIRTY;
        if (!(c->bits & B_OPEN) || (c->bits & B_SENDING)) continue;
        if (!c->out.len) {
            if (c->bits & B_CLOSING) e_close_conn(slot);
            continue;
        }
#ifdef DBG
        fprintf(stderr, "DBG flush slot=%d out.len=%u\n", slot, c->out.len);
#endif
        for (;;) {
            ssize_t w = send(slot, c->out.p + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) { c->bits |= B_SENDING; e_mod(slot, 1); break; }
                e_close_conn(slot);
                break;
            }
            c->out_off += (uint32_t)w;
            c->since_ms = (uint32_t)now_mono_ms();
            if (c->out_off >= c->out.len) {
                c->out.len = 0;
                c->out_off = 0;
                if (c->bits & B_PAUSED) {
                    c->bits &= (uint8_t)~B_PAUSED;
                    if (c->in.len) on_data(slot, NULL, 0); /* buffered requests */
                }
                if ((c->bits & B_CLOSING) && !c->out.len) e_close_conn(slot);
                break;
            }
        }
    }
    ndirty = 0;
}

static int e_worker_init(int id)
{
    conns = calloc((size_t)nslots, sizeof *conns);
    dirty = malloc((size_t)nslots * sizeof *dirty);
    if (!conns || !dirty) {
        fprintf(stderr, "worker %d: out of memory\n", id);
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return -1;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.u64 = LISTENER_UD;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        /* EPOLLEXCLUSIVE fails for nested wakeups: plain share is fine */
        ev.events = EPOLLIN;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
            perror("epoll_ctl listener");
            return -1;
        }
    }
    return 0;
}

static void *e_worker_main(void *arg)
{
    int id = (int)(intptr_t)arg;
    if (e_worker_init(id)) return NULL;
    db_thread_init();
    struct epoll_event evs[256];
    uint64_t last_sweep = now_mono_ms();
    char rbuf[16384];

    fprintf(stderr, "worker %d: ready (epoll, %d slots)\n", id, nslots);

    for (;;) {
        int n = epoll_wait(epfd, evs, 256, 1000);
        if (n < 0 && errno != EINTR) {
            perror("epoll_wait");
            usleep(1000);
            continue;
        }

        int64_t q = (int64_t)(now_realtime() * 4);
        int64_t last = atomic_load(&memcheck_q);
        if (q != last && atomic_compare_exchange_strong(&memcheck_q, &last, q)) check_memory();
        int low = atomic_load(&mem_low);

        for (int i = 0; i < n; i++) {
            uint32_t slot = (uint32_t)(evs[i].data.u64 & 0xffffffff);
            uint32_t gen = (uint32_t)(evs[i].data.u64 >> 32);
#ifdef DBG
            fprintf(stderr, "DBG ev data=%llx events=%x\n", (unsigned long long)evs[i].data.u64, evs[i].events);
#endif
            if (evs[i].data.u64 == LISTENER_UD) {
                if (!(evs[i].events & EPOLLIN)) continue;
                for (;;) {
                    if (low || nopen >= nslots - ACCEPT_STOP) break;
                    int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
                    if (fd < 0) break;
#ifdef DBG
                    fprintf(stderr, "DBG accepted fd=%d\n", fd);
#endif
                    conn_t *c = &conns[fd];
                    uint32_t g = c->gen;
                    memset(c, 0, sizeof *c);
                    c->gen = g;
                    c->bits = B_OPEN | B_RECV_ON;
                    nopen++;
                    struct epoll_event ev;
                    ev.events = EPOLLIN;
                    ev.data.u64 = (uint64_t)c->gen << 32 | (uint32_t)fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
                }
                continue;
            }
            if (slot >= (uint32_t)nslots) continue;
            conn_t *c = &conns[slot];
            if (!(c->bits & B_OPEN) || c->gen != gen) continue;
            uint32_t events = evs[i].events;
            if (events & (EPOLLERR | EPOLLHUP)) {
                c->bits |= B_CLOSING;
                e_close_conn(slot);
                continue;
            }
            if (events & EPOLLOUT) {
                c->bits &= (uint8_t)~B_SENDING;
                e_mod(slot, 0);
                mark_dirty(slot);
                /* fall through: more input may be readable too */
            }
            if ((events & EPOLLIN) && !(c->bits & (B_CLOSING | B_PAUSED))) {
                for (;;) {
                    ssize_t r = read(slot, rbuf, sizeof rbuf);
                    if (r > 0) {
                        on_data(slot, rbuf, (size_t)r);
                        if (c->bits & (B_CLOSING | B_PAUSED)) break;
                        if ((size_t)r < sizeof rbuf) break; /* drained */
                        continue;
                    }
                    if (r == 0) { c->bits |= B_CLOSING; e_close_conn(slot); }
                    else if (errno == EINTR) continue;
                    else if (errno == EAGAIN) break;
                    else { c->bits |= B_CLOSING; e_close_conn(slot); }
                    break;
                }
            }
        }
        e_flush_dirty();

        uint64_t now = now_mono_ms();
        if (now - last_sweep >= 5000 || (low >= 2 && now - last_sweep >= 250)) {
            last_sweep = now;
            sweep_stalled();
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- main */

static int make_listener(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        exit(1);
    }
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* inherited by accepted sockets on Linux */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int idle = 120, intvl = 10, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    for (int tries = 0;; tries++) {
        if (!bind(fd, res->ai_addr, res->ai_addrlen) && !listen(fd, 1024)) break;
        if (errno != EADDRINUSE || tries >= 500) {
            perror("bind/listen");
            exit(1);
        }
        if (!tries) fprintf(stderr, "port busy, waiting for it to be released\n");
        usleep(100000);
    }
    freeaddrinfo(res);
    return fd;
}

/* Probe whether this kernel supports the multishot ops we want. */
static int probe_uring(void)
{
    struct io_uring r;
    struct io_uring_params prm;
    memset(&prm, 0, sizeof prm);
    prm.flags = IORING_SETUP_CQSIZE;
    prm.cq_entries = 64;
    int rc = io_uring_queue_init_params(64, &r, &prm);
    if (rc < 0) {
        fprintf(stderr, "io_uring unavailable (%s), using epoll\n", strerror(-rc));
        return 0;
    }
    rc = io_uring_register_files_sparse(&r, 8);
    if (rc < 0) {
        fprintf(stderr, "io_uring file tables unavailable (%s), using epoll\n", strerror(-rc));
        io_uring_queue_exit(&r);
        return 0;
    }
    struct io_uring_sqe *s = io_uring_get_sqe(&r);
    io_uring_prep_multishot_accept_direct(s, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(s, 0xfeed);
    rc = io_uring_submit(&r);
    int ok = 0;
    if (rc == 1) {
        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts = { 0, 2000000 }; /* 2 ms */
        rc = io_uring_wait_cqe_timeout(&r, &cqe, &ts);
        if (rc == 0 && cqe) {
            /* -EINVAL: kernel too old for multishot accept direct */
            ok = cqe->res != -EINVAL;
            io_uring_cqe_seen(&r, cqe);
        } else {
            ok = 1; /* no completion in time: the op armed fine (listener idle) */
        }
    }
    io_uring_queue_exit(&r);
    if (!ok) fprintf(stderr, "kernel lacks multishot io_uring, using epoll\n");
    return ok;
}

int main(int argc, char **argv)
{
    int force_epoll = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--epoll")) force_epoll = 1;
        else if (!strcmp(argv[i], "--pgo") && i + 1 < argc) g_pgo_secs = atoi(argv[++i]);
    }

    const char *host = getenv("HOST"), *port = getenv("PORT");
    const char *secret = getenv("JWT_SECRET"), *db_path = getenv("SQLITE_PATH");
    if (!host) host = "0.0.0.0";
    if (!port) port = "3000";
    if (!db_path || !secret) {
        fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    start_ms = now_mono_ms();
    jwt_b64url_init();
    jwt_init(secret, strlen(secret));

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
    nslots = (int)(rl.rlim_cur > 65535 ? 65535 : rl.rlim_cur);
    if (nslots <= ACCEPT_RESUME) {
        fprintf(stderr, "RLIMIT_NOFILE too low (%d)\n", nslots);
        return 1;
    }

    listen_fd = make_listener(host, port);
    g_mode_uring = !force_epoll && probe_uring();
    if (!g_mode_uring && (int)rl.rlim_cur > nslots) {
        /* epoll indexes conns by fd: keep fds within the table */
        rl.rlim_cur = (rlim_t)nslots;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    db_open(db_path);

    fprintf(stderr, "listening on %s:%s (%s)\n", host, port,
            g_mode_uring ? "io_uring" : "epoll");

    int started = 0;
    for (int i = 0; i < NWORKERS; i++) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1 << 20);
        if (pthread_create(&t, &at, g_mode_uring ? u_worker_main : e_worker_main,
                           (void *)(intptr_t)i)) {
            if (!started) { perror("pthread_create"); return 1; }
            break;
        }
        started++;
        pthread_detach(t);
    }

    if (g_pgo_secs > 0) {
        sleep((unsigned)g_pgo_secs);
        return 0; /* clean exit so the gcda profile data is written */
    }
    for (;;) pause();
}
