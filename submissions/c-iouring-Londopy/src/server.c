/* The $12 Server Challenge: one event-loop thread driving several io_uring
 * rings.
 *
 * Why several rings: the kernel caps an io_uring's direct-descriptor table at
 * RLIMIT_NOFILE (65,535 on the benchmark box), so holding 300k+ keep-alive
 * connections needs at least five tables. Each ring has its own SO_REUSEPORT
 * listener on the same port, so the kernel spreads new connections across
 * them, and one thread services all rings in turn.
 *
 * Per connection we keep a 40-byte record and no buffers while idle: a
 * multishot recv per connection pulls from a shared provided-buffer ring, and
 * buffers go straight back after each request is parsed.
 *
 * Reads are answered in the same pass they arrive in, inside one SQLite read
 * transaction per pass. Writes are queued and committed together at the end of
 * the pass (group commit); their responses go out only after COMMIT. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "json.h"
#include "jwt.h"

#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif

#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536
#define NBUF 2048 /* provided recv buffers per ring, at most (power of two) */
#define NBUF_INIT 256 /* ...to start with: the pool doubles whenever a receive finds it empty */
_Static_assert(NBUF <= 0xffff, "buffer ids and counts travel in 16-bit user_data fields");
#define BUFSZ 2048
#define MAX_REQ (64 * 1024)
#define CHUNK_SIZE (4u << 20)
#define RESP_RESERVE (384u << 10) /* worst-case single response (a feed of escaped 500-char bodies) */
#define HDR_ROOM 128

#define OUT_LIMIT (256u << 10)   /* stop answering a connection that owes this much unsent output */
#define PBUF_LIMIT (1u << 20)    /* ...and drop it if it then buffers this many unread request bytes */
#define PBUF_GLOBAL (128u << 20) /* past this much buffered input server-wide, drop the biggest bufferers */
static size_t g_pbuf_bytes;      /* allocated request-buffer bytes, all connections */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_SHUT, OP_PROVIDE };
enum { C_FREE = 0, C_OPEN, C_CLOSING };
enum {
    F_WRITE_PENDING = 1,   /* a POST waits for the group commit; later requests wait behind it */
    F_REARM = 2,           /* queued for a fresh multishot recv */
    F_SHUT = 4,            /* shutting down: ignore further input */
    F_CONTINUED = 8,       /* sent 100 Continue for the request being read */
    F_SHUT_INFLIGHT = 16,  /* SHUTDOWN SQE outstanding: the slot must not be closed yet */
    F_CLOSE_DEFERRED = 32, /* close once that SHUTDOWN completes */
    F_SENDING = 64,        /* a SEND SQE is outstanding */
    F_EOF = 128,           /* the client half-closed: finish what's owed, then close */
};

/* An output run queued behind the connection's in-flight SEND. io_uring doesn't
 * order two SENDs on one socket (a partial MSG_WAITALL send re-arms on POLLOUT
 * and a later one can overtake it), so each connection has at most one. */
struct pend {
    struct pend *next;
    const char *p;
    uint32_t n, chunk;
};

struct conn {
    char *pbuf;    /* bytes of an incomplete request (or ones held behind a pending write) */
    uint32_t plen, pcap;
    struct pend *pq; /* queued output, in order */
    uint32_t qbytes; /* bytes queued plus in flight */
    uint32_t sending_len; /* length of the in-flight SEND */
    uint16_t gen;
    uint8_t state, flags;
};

struct ring {
    int fd, reg; /* ring fd and its registered index */
    int idx;
    /* SQ */
    unsigned *sq_head, *sq_tail, *sq_flags, sq_mask;
    struct io_uring_sqe *sqes;
    unsigned sqe_tail, submitted;
    /* CQ */
    unsigned *cq_head, *cq_tail, *cq_flags, cq_mask;
    struct io_uring_cqe *cqes;
    /* provided buffers: a registered buffer ring (br), or, on kernels that
     * refuse to register one, legacy PROVIDE_BUFFERS with freed ids batched */
    struct io_uring_buf_ring *br;
    char *bufbase;
    unsigned br_tail;
    int legacy_bufs;
    uint16_t *freed;
    unsigned nfreed;
    unsigned nbuf_live; /* buffers handed to the kernel so far (ids 0..nbuf_live-1) */
    int bufs_short;     /* a receive hit ENOBUFS this drain: grow the pool */
    /* listener */
    int lfd;
    int accept_armed, accept_full;
    unsigned nfiles;
    struct conn *conns;
    uint32_t *rearm;
    unsigned nrearm;
};

static struct ring *g_rings;
static int g_nrings = 8;
static int g_evfd = -1; /* registered on every ring: wakes the idle loop (see idle_wait) */
static int g_debug; /* DEBUG=1: log why connections close and what got a 4xx */
static struct {
    uint64_t accepts, accept_errs, eofs, recv_errs, rearm_nobufs, rearm_final, sends, send_errs, closes,
        reqs, passes, taskrun_clears;
} st;
static unsigned g_pass_data; /* receives with request bytes in this loop pass */
static uint64_t g_win_reqs;  /* ...and in the nap controller's current window */

/* Build with -DPROF to time each stage with rdtsc (printed with DEBUG=1). */
#ifdef PROF
#include <x86intrin.h>
enum { P_ENTER, P_DRAIN, P_FEED, P_POST, P_READEND, P_COMMIT, P_N };
static const char *const prof_name[P_N] = {"enter", "drain", "feed", "post", "readend", "commit"};
static uint64_t prof_cyc[P_N], prof_cnt[P_N];
#define PROF_T0(k) uint64_t prof_t0_##k = __rdtsc()
#define PROF_T1(k) (prof_cyc[k] += __rdtsc() - prof_t0_##k, prof_cnt[k]++)
#else
#define PROF_T0(k) ((void)0)
#define PROF_T1(k) ((void)0)
#endif

static void dbg_bytes(const char *what, unsigned ring, unsigned slot, const char *p, size_t n) {
    char esc[512];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < sizeof esc; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 0x20 && c < 0x7f) esc[o++] = (char)c;
        else o += (size_t)snprintf(esc + o, sizeof esc - o, "\\x%02x", c);
    }
    esc[o] = 0;
    fprintf(stderr, "[r%u s%u] %s (%zu bytes): %s\n", ring, slot, what, n, esc);
}
static hmac_key g_jwt_key;
static int64_t g_now_s;    /* wall clock seconds, refreshed each pass */
static int64_t g_mono_us;  /* monotonic microseconds, refreshed each pass */
static int64_t g_start_mono_s;

/* ---------- response arena ---------- */

struct chunk {
    char *mem;
    uint32_t refs;
    uint8_t live;
};
static struct chunk *g_chunks;
static unsigned g_nchunks, g_chunk_cap;
static unsigned *g_free_chunks;
static unsigned g_nfree;
static unsigned g_cur; /* current chunk */
static uint32_t g_pos; /* next free byte in the current chunk */

static unsigned chunk_new(void) {
    if (g_nfree) return g_free_chunks[--g_nfree];
    if (g_nchunks == g_chunk_cap) {
        g_chunk_cap = g_chunk_cap ? g_chunk_cap * 2 : 16;
        if (g_chunk_cap > 65535) { fprintf(stderr, "arena exhausted\n"); abort(); }
        g_chunks = realloc(g_chunks, g_chunk_cap * sizeof *g_chunks);
        g_free_chunks = realloc(g_free_chunks, g_chunk_cap * sizeof *g_free_chunks);
    }
    unsigned i = g_nchunks++;
    g_chunks[i].mem = mmap(NULL, CHUNK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_chunks[i].mem == MAP_FAILED) { perror("mmap chunk"); abort(); }
    g_chunks[i].refs = 0;
    return i;
}

/* Free chunks beyond a small reserve give their memory back (they stay mapped
 * and fault back in if a burst needs them again). */
static void chunk_free(unsigned i) {
    if (g_nfree >= 2) madvise(g_chunks[i].mem, CHUNK_SIZE, MADV_DONTNEED);
    g_free_chunks[g_nfree++] = i;
}

static void chunk_release(unsigned i) {
    if (--g_chunks[i].refs == 0 && i != g_cur) chunk_free(i);
}

static void arena_ensure(void) {
    if (CHUNK_SIZE - g_pos >= RESP_RESERVE) return;
    unsigned old = g_cur;
    g_cur = chunk_new();
    g_pos = 0;
    if (g_chunks[old].refs == 0) chunk_free(old);
}

/* An outgoing byte run for one connection, contiguous in the current chunk. */
struct out {
    char *start, *end;
};

/* ---------- io_uring plumbing ---------- */

static int sys_setup(unsigned n, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, n, p); }
static int sys_enter(int fd, unsigned sub, unsigned minc, unsigned flags, void *arg, size_t sz) {
    return (int)syscall(__NR_io_uring_enter, fd, sub, minc, flags, arg, sz);
}
static int sys_register(int fd, unsigned op, void *arg, unsigned n) {
    return (int)syscall(__NR_io_uring_register, fd, op, arg, n);
}

static int ring_enter(struct ring *r, unsigned minc, struct __kernel_timespec *ts) {
    unsigned flags = IORING_ENTER_GETEVENTS | IORING_ENTER_REGISTERED_RING;
    struct io_uring_getevents_arg arg;
    void *argp = NULL;
    size_t argsz = 0;
    if (ts) {
        memset(&arg, 0, sizeof arg);
        arg.ts = (uint64_t)(uintptr_t)ts;
        flags |= IORING_ENTER_EXT_ARG;
        argp = &arg;
        argsz = sizeof arg;
    }
    __atomic_store_n(r->sq_tail, r->sqe_tail, __ATOMIC_RELEASE);
    unsigned pending = r->sqe_tail - r->submitted;
    int ret = sys_enter(r->reg, pending, minc, flags, argp, argsz);
    if (ret < 0) {
        if (errno != ETIME && errno != EINTR && errno != EAGAIN && errno != EBUSY) {
            perror("io_uring_enter");
            abort();
        }
        return 0;
    }
    r->submitted += (unsigned)ret;
    return ret;
}

static struct io_uring_sqe *get_sqe(struct ring *r) {
    if (r->sqe_tail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= SQ_ENTRIES) {
        ring_enter(r, 0, NULL);
        while (r->sqe_tail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= SQ_ENTRIES) ring_enter(r, 0, NULL);
    }
    struct io_uring_sqe *s = &r->sqes[r->sqe_tail & r->sq_mask];
    memset(s, 0, sizeof *s);
    r->sqe_tail++;
    return s;
}

static inline uint64_t ud(unsigned op, unsigned chunk, unsigned gen, unsigned slot) {
    return (uint64_t)op << 56 | (uint64_t)chunk << 40 | (uint64_t)gen << 16 | slot;
}
#define UD_OP(u) ((unsigned)((u) >> 56))
#define UD_CHUNK(u) ((unsigned)((u) >> 40) & 0xffff)
#define UD_GEN(u) ((unsigned)((u) >> 16) & 0xffff)
#define UD_SLOT(u) ((unsigned)(u) & 0xffff)

static void arm_accept(struct ring *r) {
    struct io_uring_sqe *s = get_sqe(r);
    s->opcode = IORING_OP_ACCEPT;
    s->fd = r->lfd;
    s->ioprio = IORING_ACCEPT_MULTISHOT;
    s->file_index = IORING_FILE_INDEX_ALLOC;
    s->user_data = ud(OP_ACCEPT, 0, 0, 0);
    r->accept_armed = 1;
}

static void arm_recv(struct ring *r, unsigned slot) {
    struct io_uring_sqe *s = get_sqe(r);
    s->opcode = IORING_OP_RECV;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->ioprio = IORING_RECV_MULTISHOT;
    s->buf_group = 0;
    s->user_data = ud(OP_RECV, 0, r->conns[slot].gen, slot);
}

static void issue_send(struct ring *r, unsigned slot, const char *p, uint32_t n, unsigned chunk) {
    struct conn *c = &r->conns[slot];
    struct io_uring_sqe *s = get_sqe(r);
    s->opcode = IORING_OP_SEND;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE;
    s->addr = (uint64_t)(uintptr_t)p;
    s->len = n;
    s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
    s->user_data = ud(OP_SEND, chunk, c->gen, slot);
    c->flags |= F_SENDING;
    c->sending_len = n;
}

/* Sends p[0..n) (in arena chunk `chunk`) after anything already owed on this
 * connection. The chunk stays referenced until the bytes are sent or dropped. */
static void submit_send(struct ring *r, unsigned slot, const char *p, size_t n, unsigned chunk) {
    struct conn *c = &r->conns[slot];
    g_chunks[chunk].refs++;
    c->qbytes += (uint32_t)n;
    if (!(c->flags & F_SENDING)) {
        issue_send(r, slot, p, (uint32_t)n, chunk);
        return;
    }
    struct pend *e = malloc(sizeof *e);
    if (!e) abort();
    *e = (struct pend){NULL, p, (uint32_t)n, chunk};
    struct pend **t = &c->pq;
    while (*t) t = &(*t)->next;
    *t = e;
}

/* Output that will never be sent: give the chunk references back. */
static void pend_drop(struct conn *c) {
    for (struct pend *e = c->pq, *nx; e; e = nx) {
        nx = e->next;
        chunk_release(e->chunk);
        free(e);
    }
    c->pq = NULL;
}

static void conn_shutdown(struct ring *r, unsigned slot) {
    struct conn *c = &r->conns[slot];
    if (c->state != C_OPEN || (c->flags & F_SHUT)) return;
    /* The kernel runs SHUTDOWN from an io-wq worker and only resolves the fixed
     * slot when that worker gets to it. Until its completion arrives the slot
     * must stay allocated, or a new connection accepted into it gets shut down. */
    c->flags |= F_SHUT | F_SHUT_INFLIGHT;
    struct io_uring_sqe *s = get_sqe(r);
    s->opcode = IORING_OP_SHUTDOWN;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE;
    s->len = SHUT_RDWR;
    s->user_data = ud(OP_SHUT, 0, c->gen, slot);
}

static void submit_close(struct ring *r, unsigned slot) {
    struct io_uring_sqe *s = get_sqe(r);
    s->opcode = IORING_OP_CLOSE;
    s->file_index = slot + 1;
    s->user_data = ud(OP_CLOSE, 0, r->conns[slot].gen, slot);
}

static void conn_close(struct ring *r, unsigned slot) {
    struct conn *c = &r->conns[slot];
    if (c->state != C_OPEN) return;
    c->state = C_CLOSING;
    g_pbuf_bytes -= c->pcap;
    free(c->pbuf);
    c->pbuf = NULL;
    c->plen = c->pcap = 0;
    pend_drop(c);
    if (c->flags & F_SHUT_INFLIGHT) {
        c->flags |= F_CLOSE_DEFERRED; /* the OP_SHUT completion submits the CLOSE */
        return;
    }
    submit_close(r, slot);
}

/* After a half-close, the connection closes once nothing more is owed. It shuts
 * down first and the SHUTDOWN completion closes it: on 6.8 a fixed-file CLOSE
 * doesn't release the socket (so no FIN goes out) until every request armed
 * before it on the ring has finished, and long-lived multishot recvs on idle
 * connections can hold that off indefinitely. SHUTDOWN sends the FIN now. */
static void close_if_done(struct ring *r, unsigned slot) {
    struct conn *c = &r->conns[slot];
    if (c->state == C_OPEN && (c->flags & F_EOF) && !(c->flags & (F_WRITE_PENDING | F_SENDING)) && !c->pq)
        conn_shutdown(r, slot);
}

static void flush_out(struct ring *r, unsigned slot, struct out *o) {
    if (!o->start) return;
    submit_send(r, slot, o->start, (size_t)(o->end - o->start), g_cur);
    g_pos = (uint32_t)(((o->end - g_chunks[g_cur].mem) + 63) & ~63);
    o->start = o->end = NULL;
}

/* ---------- responses ---------- */

static const char *status_line(int st, size_t *n) {
#define S(code, txt) case code: *n = sizeof("HTTP/1.1 " #code " " txt "\r\nContent-Type: application/json\r\nContent-Length: ") - 1; \
    return "HTTP/1.1 " #code " " txt "\r\nContent-Type: application/json\r\nContent-Length: ";
    switch (st) {
    S(200, "OK")
    S(201, "Created")
    S(400, "Bad Request")
    S(401, "Unauthorized")
    S(404, "Not Found")
    S(503, "Service Unavailable")
    default: S(500, "Internal Server Error")
    }
#undef S
}

/* Where the next response body goes: after HDR_ROOM bytes of header space. */
static char *body_begin(struct ring *r, unsigned slot, struct out *o) {
    char *base = o->start ? o->end : g_chunks[g_cur].mem + g_pos;
    if ((size_t)(g_chunks[g_cur].mem + CHUNK_SIZE - base) < RESP_RESERVE) {
        flush_out(r, slot, o);
        arena_ensure();
        base = g_chunks[g_cur].mem + g_pos;
    }
    return base + HDR_ROOM;
}
static char *body_limit(void) { return g_chunks[g_cur].mem + CHUNK_SIZE; }

/* Puts the header in front of body[0..blen) and appends the whole response to o. */
static void body_finish(struct out *o, int status, char *body, char *bend) {
    size_t sl;
    const char *line = status_line(status, &sl);
    char num[24];
    char *ne = u64_to_dec(num, (uint64_t)(bend - body));
    size_t nl = (size_t)(ne - num);
    size_t hl = sl + nl + 4;
    char *h = body - hl;
    memcpy(h, line, sl);
    memcpy(h + sl, num, nl);
    memcpy(h + sl + nl, "\r\n\r\n", 4);
    if (!o->start) {
        o->start = h;
        o->end = bend;
    } else {
        size_t n = (size_t)(bend - h);
        memmove(o->end, h, n);
        o->end += n;
    }
}

static void respond_error(struct ring *r, unsigned slot, struct out *o, int status, const char *msg) {
    if (g_debug && status != 401 && status != 400) fprintf(stderr, "[r%d s%u] %d %s\n", r->idx, slot, status, msg);
    char *b = body_begin(r, slot, o);
    size_t ml = strlen(msg);
    memcpy(b, "{\"error\":\"", 10);
    memcpy(b + 10, msg, ml);
    memcpy(b + 10 + ml, "\"}", 2);
    body_finish(o, status, b, b + 12 + ml);
}

/* ---------- write queue (group commit) ---------- */

enum { J_POST = 1, J_LIKE };
struct job {
    struct ring *r;
    uint16_t slot, gen;
    uint8_t kind;
    int64_t uid, pid;
    char *body, *uname; /* copies, from jobmem_copy */
    uint32_t blen, ulen;
};
/* Two job lists: while one batch is being answered (which can resume held
 * connections and queue new writes), new jobs go to the other. */
static struct job *g_jobs, *g_jobs_alt;
static unsigned g_njobs, g_jobcap, g_jobcap_alt;
static int64_t g_hold_since_us = -1;
static uint64_t g_hold_gen;
static uint32_t g_hold_frames = 8192;
static int64_t g_hold_max_us = 250000;
static int64_t g_commit_interval_us = 0;
static int64_t g_last_commit_us;
static int64_t g_pace_us; /* PACE_US: minimum loop-pass length when there's work */

/* Bump allocator for job strings. Growing adds a block (older jobs keep
 * pointers into older blocks); a reset frees all but the newest block. */
struct jblock {
    struct jblock *prev;
    size_t cap, used;
    char data[];
};
static struct jblock *g_jb;

static char *jobmem_copy(const char *p, size_t n) {
    if (!g_jb || g_jb->used + n > g_jb->cap) {
        size_t cap = g_jb ? g_jb->cap * 2 : (1 << 20);
        while (cap < n) cap *= 2;
        struct jblock *b = malloc(sizeof *b + cap);
        if (!b) abort();
        b->prev = g_jb;
        b->cap = cap;
        b->used = 0;
        g_jb = b;
    }
    char *d = g_jb->data + g_jb->used;
    memcpy(d, p, n);
    g_jb->used += (n + 7) & ~(size_t)7;
    return d;
}

static void jobmem_reset(void) {
    if (!g_jb) return;
    for (struct jblock *b = g_jb->prev; b;) {
        struct jblock *prev = b->prev;
        free(b);
        b = prev;
    }
    g_jb->prev = NULL;
    g_jb->used = 0;
}

static struct job *job_new(void) {
    if (g_njobs == g_jobcap) {
        g_jobcap = g_jobcap ? g_jobcap * 2 : 1024;
        g_jobs = realloc(g_jobs, g_jobcap * sizeof *g_jobs);
        if (!g_jobs) abort();
    }
    return &g_jobs[g_njobs++];
}

/* ---------- request handling ---------- */

struct req {
    const char *method, *path;
    size_t mlen, plen;
    const char *auth;
    size_t alen;
    const char *body;
    size_t blen;
    int has_auth;
};

static int parse_id(const char *s, size_t n, int64_t *id) {
    if (n == 0 || n > 18) {
        if (n > 18) {
            for (size_t i = 0; i < n; i++)
                if (s[i] < '0' || s[i] > '9') return -1;
            *id = 0; /* well-formed but out of range: not found */
            return 1;
        }
        return -1;
    }
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v <= 0) return -1;
    *id = v;
    return 0;
}

/* Auth per SPEC.md. Returns 0 and fills claims, or writes the 401 and returns -1. */
static int check_auth(struct ring *r, unsigned slot, struct out *o, const struct req *q, jwt_claims *cl) {
    if (!q->has_auth || q->alen < 7 || memcmp(q->auth, "Bearer ", 7) != 0) {
        respond_error(r, slot, o, 401, "missing bearer token");
        return -1;
    }
    const char *t = q->auth + 7;
    size_t tl = q->alen - 7;
    while (tl && t[0] == ' ') t++, tl--;
    int v = jwt_verify(&g_jwt_key, t, tl, g_now_s, cl);
    if (v == JWT_OK) return 0;
    respond_error(r, slot, o, 401, v == JWT_BAD_PAYLOAD ? "invalid token payload" : "invalid or expired token");
    return -1;
}

struct body_ctx {
    const char *v, *ve;
};
static void body_cb(void *ctx, const char *k, size_t kl, const char *v, const char *ve) {
    struct body_ctx *b = ctx;
    if (kl == 4 && memcmp(k, "body", 4) == 0) {
        b->v = v;
        b->ve = ve;
    }
}

static inline int is_ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* Validates a POST /posts body. On success returns 0 with the trimmed text in
 * buf[*off .. *off + *n). Else writes the 400 and returns -1. */
/* Valid UTF-8 with no NUL. The schema's CHECK (length(body) BETWEEN 1 AND 500)
 * is SQLite's length(), which stops at a NUL and counts malformed sequences its
 * own way; on valid UTF-8 without NUL it is exactly one per non-continuation
 * byte, which is what the length check below counts. The B-tree write path
 * relies on that: it checks constraints here, not in SQLite. */
static int utf8_text_ok(const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        if (c < 0x80) {
            if (c == 0) return 0;
            i++;
            continue;
        }
        size_t len;
        unsigned min;
        if ((c & 0xE0) == 0xC0) len = 2, min = 0x80, c &= 0x1F;
        else if ((c & 0xF0) == 0xE0) len = 3, min = 0x800, c &= 0x0F;
        else if ((c & 0xF8) == 0xF0) len = 4, min = 0x10000, c &= 0x07;
        else return 0;
        if (n - i < len) return 0;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return 0;
            c = c << 6 | (p[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0; /* overlong, too big, surrogate */
        i += len;
    }
    return 1;
}

static int read_post_body(struct ring *r, unsigned slot, struct out *o, const struct req *q, char *buf,
                          size_t *off, size_t *n) {
    struct body_ctx b = {0, 0};
    int k = json_object_each(q->body, q->body + q->blen, body_cb, &b);
    if (k < 0) {
        respond_error(r, slot, o, 400, "malformed JSON body");
        return -1;
    }
    size_t len = 0;
    if (k != 0 || !b.v || *b.v != '"' || !json_string(b.v, b.ve, buf, &len)) {
        respond_error(r, slot, o, 400, "body is required");
        return -1;
    }
    size_t s = 0, e = len;
    while (s < e && is_ws((unsigned char)buf[s])) s++;
    while (e > s && is_ws((unsigned char)buf[e - 1])) e--;
    if (s == e) {
        respond_error(r, slot, o, 400, "body is required");
        return -1;
    }
    if (!utf8_text_ok((const unsigned char *)buf + s, e - s)) {
        respond_error(r, slot, o, 400, "malformed JSON body"); /* JSON text is UTF-8 */
        return -1;
    }
    size_t chars = 0;
    for (size_t i = s; i < e; i++) chars += ((unsigned char)buf[i] & 0xC0) != 0x80;
    if (chars > 500) {
        respond_error(r, slot, o, 400, "body must be at most 500 characters");
        return -1;
    }
    *off = s;
    *n = e - s;
    return 0;
}

static char g_bodybuf[MAX_REQ];

/* Handles one complete request. Returns 1 if a write was queued (the
 * connection must wait for the commit before more requests are handled). */
static int handle(struct ring *r, unsigned slot, struct out *o, const struct req *q) {
    const char *p = q->path;
    size_t pl = q->plen;
    const char *qm = memchr(p, '?', pl);
    if (qm) pl = (size_t)(qm - p);

    if (q->mlen == 3 && memcmp(q->method, "GET", 3) == 0) {
        if (pl == 5 && memcmp(p, "/feed", 5) == 0) {
            char *b = body_begin(r, slot, o);
            PROF_T0(P_FEED);
            char *e = db_feed_json(b, body_limit());
            PROF_T1(P_FEED);
            if (e == DB_ERR || !e) respond_error(r, slot, o, 500, "internal server error");
            else body_finish(o, 200, b, e);
            return 0;
        }
        if (pl > 7 && memcmp(p, "/posts/", 7) == 0) {
            int64_t id;
            int k = parse_id(p + 7, pl - 7, &id);
            if (k < 0) {
                if (memchr(p + 7, '/', pl - 7)) respond_error(r, slot, o, 404, "not found");
                else respond_error(r, slot, o, 400, "invalid post id");
                return 0;
            }
            char *b = body_begin(r, slot, o);
            PROF_T0(P_POST);
            char *e = k ? NULL : db_post_json(id, b, body_limit());
            PROF_T1(P_POST);
            if (e == DB_ERR) respond_error(r, slot, o, 500, "internal server error");
            else if (!e) respond_error(r, slot, o, 404, "post not found");
            else body_finish(o, 200, b, e);
            return 0;
        }
        if (pl == 7 && memcmp(p, "/health", 7) == 0) {
            char err[256];
            char *b = body_begin(r, slot, o);
            if (db_health(err, sizeof err) == 0) {
                char *e = b;
                memcpy(e, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36);
                e = i64_to_dec(e + 36, g_mono_us / 1000000 - g_start_mono_s);
                *e++ = '}';
                body_finish(o, 200, b, e);
            } else {
                char *e = b;
                memcpy(e, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"", 49);
                e = json_escape(e + 49, err, strlen(err));
                memcpy(e, "\"}", 2);
                body_finish(o, 503, b, e + 2);
            }
            return 0;
        }
        if (pl == 6 && memcmp(p, "/posts", 6) == 0) {
            respond_error(r, slot, o, 404, "not found");
            return 0;
        }
        respond_error(r, slot, o, 404, "not found");
        return 0;
    }

    if (q->mlen == 4 && memcmp(q->method, "POST", 4) == 0) {
        int is_create = pl == 6 && memcmp(p, "/posts", 6) == 0;
        int is_like = pl > 12 && memcmp(p, "/posts/", 7) == 0 && memcmp(p + pl - 5, "/like", 5) == 0;
        if (!is_create && !is_like) {
            respond_error(r, slot, o, 404, "not found");
            return 0;
        }
        jwt_claims cl;
        if (check_auth(r, slot, o, q, &cl)) return 0;
        struct conn *c = &r->conns[slot];
        if (is_like) {
            int64_t id;
            int k = parse_id(p + 7, pl - 12, &id);
            if (k < 0) {
                respond_error(r, slot, o, 400, "invalid post id");
                return 0;
            }
            if (k > 0) {
                respond_error(r, slot, o, 404, "post not found");
                return 0;
            }
            struct job *j = job_new();
            *j = (struct job){.r = r, .slot = (uint16_t)slot, .gen = c->gen, .kind = J_LIKE, .uid = cl.uid, .pid = id};
        } else {
            size_t off, n;
            if (q->blen > sizeof g_bodybuf) {
                respond_error(r, slot, o, 400, "body must be at most 500 characters");
                return 0;
            }
            if (read_post_body(r, slot, o, q, g_bodybuf, &off, &n)) return 0;
            struct job *j = job_new();
            *j = (struct job){.r = r, .slot = (uint16_t)slot, .gen = c->gen, .kind = J_POST, .uid = cl.uid};
            j->body = jobmem_copy(g_bodybuf + off, n);
            j->blen = (uint32_t)n;
            j->uname = jobmem_copy(cl.uname, cl.uname_len);
            j->ulen = (uint32_t)cl.uname_len;
        }
        c->flags |= F_WRITE_PENDING;
        return 1;
    }

    respond_error(r, slot, o, 404, "not found");
    return 0;
}

static inline int hdr_is(const char *name, size_t nl, const char *want, size_t wl) {
    return nl == wl && strncasecmp(name, want, wl) == 0;
}

/* Parses one request from p[0..n). Returns its total length, 0 if incomplete,
 * -1 if unparseable. Sets *expect_continue when headers are complete but the
 * body isn't, and the client asked for 100-continue. */
static long parse_request(const char *p, size_t n, struct req *q, int *expect_continue) {
    const char *he = memmem(p, n, "\r\n\r\n", 4);
    if (!he) return n > 16384 ? -1 : 0;
    const char *end = he + 2; /* end of the last header line's CRLF */
    const char *sp1 = memchr(p, ' ', (size_t)(end - p));
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(end - sp1 - 1));
    const char *le = memchr(p, '\n', (size_t)(end - p));
    if (!sp2 || !le || sp2 > le) return -1;
    q->method = p;
    q->mlen = (size_t)(sp1 - p);
    q->path = sp1 + 1;
    q->plen = (size_t)(sp2 - sp1 - 1);
    q->has_auth = 0;
    q->auth = NULL;
    q->alen = 0;
    size_t clen = 0;
    int expect = 0;
    const char *l = le + 1;
    while (l < end) {
        const char *e = memchr(l, '\n', (size_t)(end - l));
        if (!e) return -1;
        const char *ve = e;
        if (ve > l && ve[-1] == '\r') ve--;
        const char *colon = memchr(l, ':', (size_t)(ve - l));
        if (colon) {
            size_t nl = (size_t)(colon - l);
            const char *v = colon + 1;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            const char *vend = ve;
            while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;
            switch (l[0] | 0x20) {
            case 'a':
                if (hdr_is(l, nl, "authorization", 13)) {
                    q->has_auth = 1;
                    q->auth = v;
                    q->alen = (size_t)(vend - v);
                }
                break;
            case 'c':
                if (hdr_is(l, nl, "content-length", 14)) {
                    clen = 0;
                    if (v == vend || vend - v > 9) return -1;
                    for (const char *d = v; d < vend; d++) {
                        if (*d < '0' || *d > '9') return -1;
                        clen = clen * 10 + (size_t)(*d - '0');
                    }
                }
                break;
            case 'e':
                if (hdr_is(l, nl, "expect", 6) && vend - v == 12 && strncasecmp(v, "100-continue", 12) == 0)
                    expect = 1;
                break;
            case 't':
                if (hdr_is(l, nl, "transfer-encoding", 17)) return -1; /* chunked bodies unsupported */
                break;
            }
        }
        l = e + 1;
    }
    if (clen > MAX_REQ) return -1;
    size_t hlen = (size_t)(he + 4 - p);
    if (n < hlen + clen) {
        *expect_continue = expect;
        return 0;
    }
    q->body = p + hlen;
    q->blen = clen;
    return (long)(hlen + clen);
}

static void pbuf_store(struct conn *c, const char *p, size_t n) {
    if (n == 0) { /* idle connections hold no buffer */
        g_pbuf_bytes -= c->pcap;
        free(c->pbuf);
        c->pbuf = NULL;
        c->plen = c->pcap = 0;
        return;
    }
    if (c->pcap < n) {
        size_t cap = c->pcap ? c->pcap : 1024;
        while (cap < n) cap *= 2;
        char *nb = malloc(cap);
        if (!nb) abort();
        free(c->pbuf);
        g_pbuf_bytes += cap - c->pcap;
        c->pbuf = nb;
        c->pcap = (uint32_t)cap;
    }
    memmove(c->pbuf, p, n);
    c->plen = (uint32_t)n;
}

static void pbuf_append(struct conn *c, const char *p, size_t n) {
    size_t need = (size_t)c->plen + n;
    if (need > c->pcap) {
        size_t cap = c->pcap ? c->pcap : 1024;
        while (cap < need) cap *= 2;
        char *nb = realloc(c->pbuf, cap);
        if (!nb) abort();
        g_pbuf_bytes += cap - c->pcap;
        c->pbuf = nb;
        c->pcap = (uint32_t)cap;
    }
    memcpy(c->pbuf + c->plen, p, n);
    c->plen += (uint32_t)n;
}

/* Whether buffering n more request bytes would be too much: per connection,
 * or (past a server-wide budget) for any connection already holding more than
 * one maximum-size request. */
static inline int pbuf_over(const struct conn *c, size_t n) {
    size_t after = (size_t)c->plen + n;
    return after > PBUF_LIMIT || (g_pbuf_bytes > PBUF_GLOBAL && after > MAX_REQ + 16384);
}

static inline int conn_backlogged(const struct conn *c, const struct out *o) {
    return c->qbytes + (o->start ? (uint32_t)(o->end - o->start) : 0) > OUT_LIMIT;
}

/* Parses and answers every complete request in data[0..n); keeps the rest.
 * Stops early behind a pending write, or while the client isn't reading what
 * it's already owed (resumed from the send completion). */
static void process(struct ring *r, unsigned slot, const char *data, size_t n) {
    struct conn *c = &r->conns[slot];
    struct out o = {0, 0};
    size_t off = 0;
    int bad = 0;
    while (off < n && !(c->flags & F_WRITE_PENDING) && !conn_backlogged(c, &o)) {
        struct req q;
        int expect = 0;
        long k = parse_request(data + off, n - off, &q, &expect);
        if (k < 0) {
            if (g_debug) dbg_bytes("unparseable request", (unsigned)r->idx, slot, data + off, n - off);
            bad = 1;
            break;
        }
        if (k == 0) {
            if (expect && !(c->flags & F_CONTINUED)) {
                c->flags |= F_CONTINUED;
                char *b = body_begin(r, slot, &o);
                static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                char *h = b - (sizeof cont - 1);
                memcpy(h, cont, sizeof cont - 1);
                if (!o.start) o.start = h, o.end = b;
                else { memmove(o.end, h, sizeof cont - 1); o.end += sizeof cont - 1; }
            }
            break;
        }
        c->flags &= (uint8_t)~F_CONTINUED;
        handle(r, slot, &o, &q);
        off += (size_t)k;
    }
    flush_out(r, slot, &o);
    if (bad || (data != c->pbuf && pbuf_over(c, n - off)) || n - off > PBUF_LIMIT) {
        pbuf_store(c, NULL, 0);
        conn_shutdown(r, slot);
        return;
    }
    if (data == c->pbuf) {
        if (off) pbuf_store(c, c->pbuf + off, n - off);
    } else {
        pbuf_store(c, data + off, n - off);
    }
}

static void on_data(struct ring *r, unsigned slot, const char *data, size_t n) {
    struct conn *c = &r->conns[slot];
    int hold = c->qbytes > OUT_LIMIT || (c->flags & F_WRITE_PENDING);
    if (hold || c->plen) {
        if (pbuf_over(c, n)) { /* a client that sends without reading */
            pbuf_store(c, NULL, 0);
            conn_shutdown(r, slot);
            return;
        }
        pbuf_append(c, data, n);
        if (!hold) process(r, slot, c->pbuf, c->plen);
    } else {
        process(r, slot, data, n);
    }
}

/* ---------- group commit ---------- */

static void commit_writes(void) {
    if (!g_njobs) return;
    if (g_commit_interval_us && g_mono_us - g_last_commit_us < g_commit_interval_us) return;
    /* Hold writes while the WAL is over its limit and a checkpoint is catching up,
     * so the next commit can restart the log instead of growing it. */
    if (db_wal_frames() >= g_hold_frames) {
        if (g_hold_since_us < 0) {
            g_hold_since_us = g_mono_us;
            g_hold_gen = db_ckpt_generation();
        }
        if (db_ckpt_generation() == g_hold_gen && g_mono_us - g_hold_since_us < g_hold_max_us) return;
    }
    g_hold_since_us = -1;
    g_last_commit_us = g_mono_us;

    int ok = db_write_begin() == 0;
    /* results: status per job, plus post id / created_at for creates */
    static int *st;
    static int64_t *ids;
    static char (*created)[32];
    static size_t *clens;
    static unsigned cap;
    if (cap < g_njobs) {
        cap = g_njobs * 2;
        st = realloc(st, cap * sizeof *st);
        ids = realloc(ids, cap * sizeof *ids);
        created = realloc(created, cap * sizeof *created);
        clens = realloc(clens, cap * sizeof *clens);
    }
    for (unsigned i = 0; ok && i < g_njobs; i++) {
        struct job *j = &g_jobs[i];
        if (j->kind == J_LIKE) {
            int k = db_insert_like(j->uid, j->pid);
            st[i] = k == 1 ? 201 : k == 0 ? 200 : k == -1 ? 404 : 500;
        } else {
            st[i] = db_insert_post(j->uid, j->body, j->blen, &ids[i], created[i], &clens[i]) == 0 ? 201 : 500;
        }
    }
    if (ok && db_write_commit() != 0) ok = 0;
    if (!ok) {
        db_write_rollback();
        for (unsigned i = 0; i < g_njobs; i++) st[i] = 500;
    }

    /* Swap lists: resuming held connections below may queue new jobs. */
    struct job *jobs = g_jobs;
    unsigned n = g_njobs, jcap = g_jobcap;
    g_jobs = g_jobs_alt;
    g_jobcap = g_jobcap_alt;
    g_jobs_alt = jobs;
    g_jobcap_alt = jcap;
    g_njobs = 0;
    for (unsigned i = 0; i < n; i++) {
        struct job *j = &jobs[i];
        struct ring *r = j->r;
        struct conn *c = &r->conns[j->slot];
        if (c->state != C_OPEN || c->gen != j->gen) continue;
        struct out o = {0, 0};
        if (st[i] == 500) {
            respond_error(r, j->slot, &o, 500, "internal server error");
        } else if (st[i] == 404) {
            respond_error(r, j->slot, &o, 404, "post not found");
        } else if (j->kind == J_LIKE) {
            char *b = body_begin(r, j->slot, &o), *e = b;
            memcpy(e, "{\"liked\":true,\"already_liked\":", 30);
            e += 30;
            if (st[i] == 200) memcpy(e, "true", 4), e += 4;
            else memcpy(e, "false", 5), e += 5;
            memcpy(e, ",\"post_id\":", 11);
            e = i64_to_dec(e + 11, j->pid);
            *e++ = '}';
            body_finish(&o, st[i], b, e);
        } else {
            char *b = body_begin(r, j->slot, &o), *e = b;
            memcpy(e, "{\"post\":{\"id\":", 14);
            e = i64_to_dec(e + 14, ids[i]);
            memcpy(e, ",\"body\":\"", 9);
            e = json_escape(e + 9, j->body, j->blen);
            memcpy(e, "\",\"created_at\":\"", 16);
            e = json_escape(e + 16, created[i], clens[i]);
            memcpy(e, "\",\"author\":\"", 12);
            e = json_escape(e + 12, j->uname, j->ulen);
            memcpy(e, "\",\"like_count\":0}}", 18);
            e += 18;
            body_finish(&o, 201, b, e);
        }
        flush_out(r, j->slot, &o);
        c->flags &= (uint8_t)~F_WRITE_PENDING;
        if (c->plen) process(r, j->slot, c->pbuf, c->plen);
        close_if_done(r, j->slot);
    }
    /* jobs queued while resuming keep their copies in g_jobmem, so only reset
     * it once nothing is queued */
    if (!g_njobs) jobmem_reset();
}

/* ---------- completions ---------- */

static void on_cqe(struct ring *r, struct io_uring_cqe *cqe) {
    uint64_t u = cqe->user_data;
    unsigned op = UD_OP(u), slot = UD_SLOT(u), gen = UD_GEN(u);
    int res = cqe->res;
    switch (op) {
    case OP_ACCEPT: {
        if (!(cqe->flags & IORING_CQE_F_MORE)) r->accept_armed = 0;
        if (res < 0) {
            st.accept_errs++;
            if (g_debug) fprintf(stderr, "[r%d] accept error %d\n", r->idx, res);
        }
        if (res >= 0) {
            st.accepts++;
            unsigned s = (unsigned)res;
            if (g_debug && r->conns[s].state == C_OPEN)
                fprintf(stderr, "[r%d s%u] accept reused a slot we think is open!\n", r->idx, s);
            struct conn *c = &r->conns[s];
            c->gen++;
            c->state = C_OPEN;
            c->flags = 0;
            c->plen = 0;
            c->pq = NULL;
            c->qbytes = c->sending_len = 0;
            arm_recv(r, s);
        } else if (res == -ENFILE) {
            r->accept_full = 1;
        }
        if (!r->accept_armed && !r->accept_full) arm_accept(r);
        break;
    }
    case OP_RECV: {
        struct conn *c = &r->conns[slot];
        int live = c->state == C_OPEN && c->gen == gen;
        if (res > 0) {
            unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            char *data = r->bufbase + (size_t)bid * BUFSZ;
            if (live && !(c->flags & F_SHUT)) {
                g_pass_data++;
                g_win_reqs++;
                on_data(r, slot, data, (size_t)res);
            }
            if (r->legacy_bufs) {
                r->freed[r->nfreed++] = (uint16_t)bid;
            } else {
                struct io_uring_buf *b = &r->br->bufs[r->br_tail & (NBUF - 1)];
                b->addr = (uint64_t)(uintptr_t)data;
                b->len = BUFSZ;
                b->bid = (uint16_t)bid;
                r->br_tail++;
            }
        }
        if (!(cqe->flags & IORING_CQE_F_MORE) && live) {
            if (res > 0 || res == -ENOBUFS) {
                if (!(c->flags & F_REARM)) {
                    c->flags |= F_REARM;
                    r->rearm[r->nrearm++] = slot;
                }
            } else if (res == 0 && !(c->flags & F_SHUT)) {
                /* Half-close: answer what the client already sent, then close. */
                st.eofs++;
                c->flags |= F_EOF;
                close_if_done(r, slot);
            } else {
                if (g_debug && res < 0) fprintf(stderr, "[r%d s%u] recv error %d, closing\n", r->idx, slot, res);
                if (res < 0) st.recv_errs++;
                conn_close(r, slot);
            }
            if (res == -ENOBUFS) st.rearm_nobufs++;
            else if (res > 0) st.rearm_final++;
        }
        if (res == -ENOBUFS) r->bufs_short = 1;
        break;
    }
    case OP_SEND: {
        st.sends++;
        if (res < 0) st.send_errs++;
        chunk_release(UD_CHUNK(u));
        struct conn *c = &r->conns[slot];
        if (c->gen != gen || c->state != C_OPEN) break; /* a closed connection's send */
        c->flags &= (uint8_t)~F_SENDING;
        c->qbytes -= c->sending_len;
        /* MSG_WAITALL: anything short of the full length means the socket failed */
        if (res < 0 || (uint32_t)res != c->sending_len) {
            if (g_debug) fprintf(stderr, "[r%d s%u] send %d of %u, shutting down\n", r->idx, slot, res, c->sending_len);
            pend_drop(c);
            c->qbytes = 0;
            conn_shutdown(r, slot);
            break;
        }
        if (c->pq) {
            struct pend *e = c->pq;
            c->pq = e->next;
            issue_send(r, slot, e->p, e->n, e->chunk);
            free(e);
        }
        /* resume requests held back while the client wasn't reading */
        if (c->plen && !(c->flags & (F_WRITE_PENDING | F_SHUT)) && c->qbytes <= OUT_LIMIT)
            process(r, slot, c->pbuf, c->plen);
        close_if_done(r, slot);
        break;
    }
    case OP_CLOSE: {
        st.closes++;
        struct conn *c = &r->conns[slot];
        if (c->gen == gen && c->state == C_CLOSING) c->state = C_FREE;
        if (r->accept_full) {
            r->accept_full = 0;
            if (!r->accept_armed) arm_accept(r);
        }
        break;
    }
    case OP_PROVIDE: { /* only failures complete (IOSQE_CQE_SKIP_SUCCESS) */
        /* Requeue the range (bid in the slot field, count in the chunk field) so
         * flush_freed retries it at the end of this drain; dropping it would
         * shrink the pool for good. */
        static unsigned long fails;
        if (++fails <= 10 || fails % 1000 == 0)
            fprintf(stderr, "[r%d] PROVIDE_BUFFERS failed: %d (%lu so far), retrying\n", r->idx, res, fails);
        unsigned n = UD_CHUNK(u);
        if (r->nfreed + n > NBUF) { /* every buffer is in exactly one place, so this can't happen */
            fprintf(stderr, "[r%d] freed list overflow (%u + %u), dropping the excess\n", r->idx, r->nfreed, n);
            n = NBUF - r->nfreed;
        }
        for (unsigned i = 0; i < n; i++) r->freed[r->nfreed++] = (uint16_t)(slot + i);
        break;
    }
    case OP_SHUT: {
        struct conn *c = &r->conns[slot];
        if (c->gen != gen) break; /* can't happen: the slot stays allocated until now */
        c->flags &= (uint8_t)~F_SHUT_INFLIGHT;
        if (c->state == C_CLOSING && (c->flags & F_CLOSE_DEFERRED)) {
            c->flags &= (uint8_t)~F_CLOSE_DEFERRED;
            submit_close(r, slot);
        } else if (c->state == C_OPEN && (c->flags & F_EOF)) {
            conn_close(r, slot); /* the recv already ended; nothing else will close it */
        }
        break;
    }
    default:
        break;
    }
}

static void provide_buffers(struct ring *r, unsigned bid, unsigned count) {
    struct io_uring_sqe *s = get_sqe(r);
    s->opcode = IORING_OP_PROVIDE_BUFFERS;
    s->fd = (int)count;
    s->addr = (uint64_t)(uintptr_t)(r->bufbase + (size_t)bid * BUFSZ);
    s->len = BUFSZ;
    s->off = bid;
    s->buf_group = 0;
    s->flags = IOSQE_CQE_SKIP_SUCCESS;
    s->user_data = ud(OP_PROVIDE, count, 0, bid); /* the range, for a retry if it fails */
}

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

/* Legacy mode: hand this pass's freed buffers back, one SQE per run of
 * consecutive ids. They go out ahead of the next enter's task work. */
static void flush_freed(struct ring *r) {
    if (!r->nfreed) return;
    if (r->nfreed > 1) qsort(r->freed, r->nfreed, sizeof *r->freed, cmp_u16);
    unsigned start = r->freed[0], len = 1;
    for (unsigned i = 1; i < r->nfreed; i++) {
        if (r->freed[i] == start + len) {
            len++;
            continue;
        }
        provide_buffers(r, start, len);
        start = r->freed[i];
        len = 1;
    }
    provide_buffers(r, start, len);
    r->nfreed = 0;
}

/* The pool starts small and doubles each time a receive finds it empty, so
 * resident buffer memory follows the largest burst actually seen rather than
 * the cap: the kernel takes buffers in ring order, so every buffer handed over
 * ends up touched. */
static void grow_bufs(struct ring *r) {
    r->bufs_short = 0;
    unsigned add = r->nbuf_live;
    if (add > NBUF - r->nbuf_live) add = NBUF - r->nbuf_live;
    for (unsigned i = 0; i < add; i++) {
        unsigned bid = r->nbuf_live + i;
        if (r->legacy_bufs) {
            r->freed[r->nfreed++] = (uint16_t)bid; /* flush_freed hands them over */
        } else {
            struct io_uring_buf *b = &r->br->bufs[r->br_tail & (NBUF - 1)];
            b->addr = (uint64_t)(uintptr_t)(r->bufbase + (size_t)bid * BUFSZ);
            b->len = BUFSZ;
            b->bid = (uint16_t)bid;
            r->br_tail++;
        }
    }
    r->nbuf_live += add;
}

static unsigned drain_cq(struct ring *r) {
    unsigned head = *r->cq_head, n = 0;
    for (;;) {
        unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail) break;
        while (head != tail) {
            on_cqe(r, &r->cqes[head & r->cq_mask]);
            head++;
            n++;
        }
        __atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
    }
    if (r->bufs_short) grow_bufs(r);
    if (r->legacy_bufs) flush_freed(r);
    else __atomic_store_n(&r->br->tail, (uint16_t)r->br_tail, __ATOMIC_RELEASE);
    return n;
}

static void do_rearms(struct ring *r) {
    unsigned n = r->nrearm, done = 0;
    for (; done < n && done < 4096; done++) {
        unsigned slot = r->rearm[done];
        struct conn *c = &r->conns[slot];
        c->flags &= (uint8_t)~F_REARM;
        if (c->state != C_OPEN) continue;
        if (c->flags & F_SHUT) conn_close(r, slot); /* no recv left to see the shutdown */
        else arm_recv(r, slot);
    }
    if (done < n) memmove(r->rearm, r->rearm + done, (n - done) * sizeof *r->rearm);
    r->nrearm = n - done;
}

/* ---------- setup ---------- */

static void *xmap(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static int make_listener(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one)) { perror("SO_REUSEPORT"); exit(1); }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* inherited by accepted sockets */
    const char *cc = getenv("TCP_CC");
    if (cc && *cc && setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, cc, (socklen_t)strlen(cc)))
        perror("TCP_CONGESTION");
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", host); exit(1); }
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) { perror("bind"); exit(1); }
    if (listen(fd, 65535)) { perror("listen"); exit(1); }
    return fd;
}

static void ring_init(struct ring *r, int idx, unsigned nfiles, const char *host, int port) {
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
              IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_NO_SQARRAY;
    p.cq_entries = CQ_ENTRIES;
    int fd = sys_setup(SQ_ENTRIES, &p);
    if (fd < 0 && errno == EINVAL) { /* kernels before 6.6: no NO_SQARRAY */
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
                  IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_SUBMIT_ALL;
        p.cq_entries = CQ_ENTRIES;
        fd = sys_setup(SQ_ENTRIES, &p);
    }
    if (fd < 0) { perror("io_uring_setup"); exit(1); }
    if (!(p.features & IORING_FEAT_SINGLE_MMAP)) { fprintf(stderr, "kernel too old\n"); exit(1); }
    r->fd = fd;
    r->idx = idx;
    size_t sqsz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cqsz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t sz = sqsz > cqsz ? sqsz : cqsz;
    char *ring = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (ring == MAP_FAILED) { perror("mmap ring"); exit(1); }
    r->sq_head = (unsigned *)(ring + p.sq_off.head);
    r->sq_tail = (unsigned *)(ring + p.sq_off.tail);
    r->sq_flags = (unsigned *)(ring + p.sq_off.flags);
    r->sq_mask = *(unsigned *)(ring + p.sq_off.ring_mask);
    if (!(p.flags & IORING_SETUP_NO_SQARRAY)) {
        unsigned *arr = (unsigned *)(ring + p.sq_off.array);
        for (unsigned i = 0; i < p.sq_entries; i++) arr[i] = i;
    }
    r->cq_head = (unsigned *)(ring + p.cq_off.head);
    r->cq_tail = (unsigned *)(ring + p.cq_off.tail);
    r->cq_mask = *(unsigned *)(ring + p.cq_off.ring_mask);
    r->cq_flags = (unsigned *)(ring + p.cq_off.flags);
    r->cqes = (struct io_uring_cqe *)(ring + p.cq_off.cqes);
    r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) { perror("mmap sqes"); exit(1); }
    r->sqe_tail = r->submitted = *r->sq_tail;

    struct io_uring_rsrc_update up = {.offset = (unsigned)-1, .data = (uint64_t)fd};
    if (sys_register(fd, IORING_REGISTER_RING_FDS, &up, 1) != 1) { perror("register ring fd"); exit(1); }
    r->reg = (int)up.offset;

    struct io_uring_rsrc_register rr;
    memset(&rr, 0, sizeof rr);
    rr.nr = nfiles;
    rr.flags = IORING_RSRC_REGISTER_SPARSE;
    if (sys_register(fd, IORING_REGISTER_FILES2, &rr, sizeof rr)) { perror("register files"); exit(1); }
    r->nfiles = nfiles;

    /* The idle eventfd starts switched off; idle_wait turns it on only while
     * the loop sleeps. Without it the loop falls back to sleeping on ring 0. */
    __atomic_store_n(r->cq_flags, *r->cq_flags | IORING_CQ_EVENTFD_DISABLED, __ATOMIC_RELAXED);
    if (g_evfd >= 0 && sys_register(fd, IORING_REGISTER_EVENTFD, &g_evfd, 1)) {
        perror("register eventfd");
        for (int i = 0; i < idx; i++) sys_register(g_rings[i].fd, IORING_UNREGISTER_EVENTFD, NULL, 0);
        close(g_evfd);
        g_evfd = -1;
    }

    /* Receive buffers. Prefer a registered buffer ring in user memory (what
     * other entries run on the box), else kernel-allocated. Ubuntu kernels
     * 6.8.0-136..147 (LP #2162843) invert the reserved-field check: they
     * reject the correct all-zero struct with EINVAL and accept a nonzero
     * resv[0], so retry with that; a correct kernel never reaches the retry.
     * Last resort: legacy PROVIDE_BUFFERS, which every 6.x kernel has. */
    r->bufbase = xmap((size_t)NBUF * BUFSZ);
    size_t brsz = NBUF * sizeof(struct io_uring_buf);
    r->br = NULL;
    if (getenv("LEGACY_BUFS") == NULL || atoi(getenv("LEGACY_BUFS")) == 0) {
        for (int attempt = 0; attempt < 4 && !r->br; attempt++) {
            int kernel_mem = attempt >= 2, inverted = attempt & 1;
            struct io_uring_buf_reg reg;
            memset(&reg, 0, sizeof reg);
            reg.ring_entries = NBUF;
            reg.bgid = 0;
            if (inverted) reg.resv[0] = 1;
            void *mem = NULL;
            if (kernel_mem) {
                reg.flags = IOU_PBUF_RING_MMAP;
            } else {
                mem = xmap(brsz);
                reg.ring_addr = (uint64_t)(uintptr_t)mem;
            }
            if (sys_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1) != 0) {
                if (mem) munmap(mem, brsz);
                if (errno != EINVAL) break;
                continue;
            }
            if (kernel_mem) {
                r->br = mmap(NULL, brsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
                             (off_t)(IORING_OFF_PBUF_RING | ((uint64_t)reg.bgid << IORING_OFF_PBUF_SHIFT)));
                if (r->br == MAP_FAILED) { perror("mmap pbuf ring"); exit(1); }
            } else {
                r->br = mem;
            }
            if (idx == 0 && inverted)
                fprintf(stderr, "buffer ring registered via the reserved-field workaround (LP #2162843)\n");
        }
    }
    r->nbuf_live = NBUF_INIT;
    if (r->br) {
        for (unsigned i = 0; i < NBUF_INIT; i++) {
            r->br->bufs[i].addr = (uint64_t)(uintptr_t)(r->bufbase + (size_t)i * BUFSZ);
            r->br->bufs[i].len = BUFSZ;
            r->br->bufs[i].bid = (uint16_t)i;
        }
        r->br_tail = NBUF_INIT;
        __atomic_store_n(&r->br->tail, (uint16_t)NBUF_INIT, __ATOMIC_RELEASE);
    } else {
        r->legacy_bufs = 1;
        r->freed = xmap(NBUF * sizeof *r->freed);
        provide_buffers(r, 0, NBUF_INIT);
        r->sqes[(r->sqe_tail - 1) & r->sq_mask].flags = 0; /* want this one's completion */
        ring_enter(r, 1, NULL);
        struct io_uring_cqe *c = &r->cqes[*r->cq_head & r->cq_mask];
        if (c->res < 0) { fprintf(stderr, "PROVIDE_BUFFERS: %s\n", strerror(-c->res)); exit(1); }
        __atomic_store_n(r->cq_head, *r->cq_head + 1, __ATOMIC_RELEASE);
    }

    r->conns = xmap((size_t)nfiles * sizeof(struct conn));
    r->rearm = xmap((size_t)nfiles * sizeof(uint32_t));
    r->lfd = make_listener(host, port);
    arm_accept(r);
}

static int64_t env_int(const char *name, int64_t def) {
    const char *v = getenv(name);
    return v && *v ? strtoll(v, NULL, 10) : def;
}

/* Anything for the next pass to do on any ring? */
static int rings_pending(void) {
    for (int i = 0; i < g_nrings; i++) {
        struct ring *r = &g_rings[i];
        if (r->sqe_tail != r->submitted || r->nrearm ||
            (__atomic_load_n(r->sq_flags, __ATOMIC_RELAXED) & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) ||
            *r->cq_head != __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE))
            return 1;
    }
    return 0;
}

/* Sleeps until any ring has work, or for wait_us.
 *
 * With DEFER_TASKRUN, completions queue as task work that the kernel announces
 * by setting IORING_SQ_TASKRUN and, when an eventfd is registered, signalling
 * it. That signal is switched off (IORING_CQ_EVENTFD_DISABLED) except while we
 * sleep here, so a busy loop pays next to nothing for it (on 6.8, an
 * uncontended lock and a flag check per CQ flush). Switching it on and then
 * checking TASKRUN, both behind full barriers (the kernel sets TASKRUN with an
 * atomic op before it reads the flag), means a completion either shows up in
 * the check or signals the eventfd. */
static void idle_wait(int64_t wait_us) {
    if (g_evfd < 0) {
        struct __kernel_timespec kts = {0, wait_us * 1000};
        ring_enter(&g_rings[0], 1, &kts);
        return;
    }
    for (int i = 0; i < g_nrings; i++)
        __atomic_store_n(g_rings[i].cq_flags, *g_rings[i].cq_flags & ~IORING_CQ_EVENTFD_DISABLED, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!rings_pending()) {
        struct pollfd pfd = {.fd = g_evfd, .events = POLLIN};
        struct timespec ts = {0, wait_us * 1000};
        ppoll(&pfd, 1, &ts, NULL);
    }
    for (int i = 0; i < g_nrings; i++)
        __atomic_store_n(g_rings[i].cq_flags, *g_rings[i].cq_flags | IORING_CQ_EVENTFD_DISABLED, __ATOMIC_RELAXED);
    uint64_t v;
    if (read(g_evfd, &v, sizeof v) < 0 && errno != EAGAIN) perror("read eventfd"); /* reset it */
}

static int64_t mono_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* The self-tuning nap (the default; PACE_US=<us> fixes it, PACE_US=0 turns it
 * off). Batching completions and sends through the kernel is what makes a
 * request cheap on a virtio-net box, so a busy pass pauses before the next one.
 * Three loops pick the pause, all measured in 50 ms windows:
 *  - whether to nap: the share of the time the loop isn't asleep. It starts
 *    napping above NAP_ON and stops below NAP_OFF, so a quiet server answers
 *    at once;
 *  - whether napping is hurting: clients that wait for each answer before
 *    sending the next one (a benchmark loop, a smoke test) send fewer requests
 *    when the server pauses, whereas users with think time keep arriving at the
 *    same rate. So when napping starts, the request rate of the first windows is
 *    compared with the window before: if it fell by more than NAP_THROTTLE, the
 *    loop stops napping and leaves it off for a while (1 s, doubling up to 16 s,
 *    reset once napping has held for 2 s);
 *  - how long: while napping, it watches how many requests each pass collects
 *    and stretches the nap when passes come in under NAP_TARGET, shrinks it when
 *    they come in over, between NAP_MIN_US and NAP_MAX_US. At saturation a pass
 *    outlasts any nap, so the loop stops sleeping by itself. */
#define NAP_MIN_US 500
#define NAP_MAX_US 3000
#define NAP_TARGET 64.0
#define NAP_ON 0.20
#define NAP_OFF 0.08
#define NAP_THROTTLE 0.75   /* napping is hurting if the request rate falls below this share */
#define NAP_PROBE_US 150000 /* ...within this long of starting */
#define NAP_PROBE_MIN_RATE 2000.0 /* req/s: below ~100 requests a window, a 25% drop is noise */
/* ...and above this the check is off. No handful of waiting clients sends that
 * much, and a server that busy needs its nap: a VM stall (the guest frozen while
 * requests pile up outside) looks just like throttled clients, and backing off
 * then would cost ~40% more CPU per request right where the load is highest. */
#define NAP_PROBE_MAX_RATE 20000.0
static int g_pace_auto = 1;
static int g_napping;
static double g_nap_us = 2000, g_batch_ewma = NAP_TARGET, g_busy, g_rate_before;
static int64_t g_win_start_us, g_win_sleep_us, g_nap_since_us, g_nap_block_until_us, g_nap_last_fire_us,
    g_nap_backoff_us = 1000000;
static uint64_t g_nap_backoffs;

/* Called at the start of every pass (g_mono_us is the pass start). */
static void track_window(void) {
    int64_t win = g_mono_us - g_win_start_us;
    if (win < 50000) return;
    double busy = 1.0 - (double)g_win_sleep_us / (double)win;
    double rate = (double)g_win_reqs * 1e6 / (double)win;
    g_busy = busy < 0 ? 0 : busy;
    if (!g_pace_auto) { /* PACE_US fixed or off: nothing to decide */
    } else if (!g_napping) {
        if (g_busy > NAP_ON && g_mono_us >= g_nap_block_until_us) {
            g_napping = 1;
            g_nap_since_us = g_mono_us;
            g_rate_before = rate;
        }
    } else if (g_mono_us - g_nap_since_us <= NAP_PROBE_US && g_rate_before >= NAP_PROBE_MIN_RATE &&
               g_rate_before <= NAP_PROBE_MAX_RATE && rate < NAP_THROTTLE * g_rate_before && g_busy < NAP_ON) {
        /* The clients were waiting on our naps: fewer requests, and the server
         * went nearly idle. A rate that dips while the server stays busy is a
         * stall or an overload, and napping is exactly what an overloaded
         * server needs, so that never backs off. (A load that simply stops
         * looks the same for these 150 ms; its backoff is harmless.) */
        g_napping = 0;
        g_nap_backoffs++;
        /* a backoff earned long ago (a smoke test before the real load) shouldn't carry over */
        if (g_mono_us - g_nap_last_fire_us > 2 * g_nap_backoff_us) g_nap_backoff_us = 1000000;
        g_nap_last_fire_us = g_mono_us;
        g_nap_block_until_us = g_mono_us + g_nap_backoff_us;
        if (g_nap_backoff_us < 16000000) g_nap_backoff_us *= 2;
    } else if (g_busy < NAP_OFF) {
        g_napping = 0; /* the load went away */
    } else if (g_mono_us - g_nap_since_us > 2000000) {
        g_nap_backoff_us = 1000000; /* napping has held up: forget earlier backoffs */
    }
    g_win_start_us = g_mono_us;
    g_win_sleep_us = 0;
    g_win_reqs = 0;
}

/* The pause for this pass, in us from its start (0 = none). */
static int64_t nap_length(void) {
    if (!g_pace_auto) return g_pace_us;
    if (!g_napping || !g_pass_data) return 0;
    g_batch_ewma += 0.125 * ((double)g_pass_data - g_batch_ewma);
    g_nap_us *= g_batch_ewma < NAP_TARGET ? 1.02 : 0.98;
    if (g_nap_us < NAP_MIN_US) g_nap_us = NAP_MIN_US;
    if (g_nap_us > NAP_MAX_US) g_nap_us = NAP_MAX_US;
    return (int64_t)g_nap_us;
}

static void clocks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    g_now_s = ts.tv_sec;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_mono_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *path = getenv("SQLITE_PATH");
    const char *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST");
    int port = (int)env_int("PORT", 3000);
    if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
    if (!host || !*host) host = "0.0.0.0";

    g_nrings = (int)env_int("RINGS", 8);
    g_hold_frames = (uint32_t)env_int("HOLD_FRAMES", 8192);
    g_hold_max_us = env_int("HOLD_MS", 250) * 1000;
    /* Batch writes: commits at most every COMMIT_US. Each commit rewrites the same
     * tail pages (posts/likes appends), so committing per pass multiplies WAL volume. */
    g_commit_interval_us = env_int("COMMIT_US", 5000);
    g_debug = (int)env_int("DEBUG", 0);
    /* Pacing: a busy pass pauses, so completions and sends go through the kernel
     * in batches. On a virtio-net VM at 150k users, a fixed 2 ms cut the
     * server's CPU per request from ~25 to ~15 us (p50 0.2 -> 1.8 ms). By default
     * the nap tunes itself (see nap_length); PACE_US=<us> fixes it, 0 = off. */
    const char *pace_env = getenv("PACE_US");
    if (pace_env && *pace_env && strcmp(pace_env, "auto") != 0) {
        g_pace_auto = 0;
        g_pace_us = strtoll(pace_env, NULL, 10);
    }
    hmac_key_init(&g_jwt_key, (const unsigned char *)secret, strlen(secret));

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
    unsigned nfiles = rl.rlim_cur > 65535 ? 65535 : (unsigned)rl.rlim_cur;
    if (nfiles < 1024) nfiles = 1024;

    db_open(path);
    db_set_verify((int)env_int("VERIFY", 0));
    db_start_checkpointer((uint32_t)env_int("CKPT_FRAMES", 2048));

    clocks();
    g_start_mono_s = g_mono_us / 1000000;
    g_cur = chunk_new();
    g_pos = 0;

    g_rings = calloc((size_t)g_nrings, sizeof *g_rings);
    g_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    for (int i = 0; i < g_nrings; i++) ring_init(&g_rings[i], i, nfiles, host, port);
    fprintf(stderr, "listening on %s:%d with %d rings x %u connections (%s receive buffers, %s idle wake)\n", host,
            port, g_nrings, nfiles, g_rings[0].legacy_bufs ? "legacy provided" : "ring-mapped",
            g_evfd >= 0 ? "eventfd" : "ring-0");

    int64_t next_stats = 0;
    g_win_start_us = mono_now_us();
    for (;;) {
        clocks();
        if (g_debug && g_mono_us >= next_stats) {
            next_stats = g_mono_us + 5000000;
            fprintf(stderr,
                    "stats: accepts=%lu accept_errs=%lu eofs=%lu recv_errs=%lu closes=%lu rearm_nobufs=%lu "
                    "rearm_final=%lu sends=%lu send_errs=%lu chunks=%u free=%u cur_refs=%u wal=%u taskrun_clears=%lu "
                    "bufs=%u cache_misses=%lu busy_pct=%d napping=%d nap_us=%d batch=%d nap_backoffs=%lu\n",
                    st.accepts, st.accept_errs, st.eofs, st.recv_errs, st.closes, st.rearm_nobufs, st.rearm_final,
                    st.sends, st.send_errs, g_nchunks, g_nfree, g_chunks[g_cur].refs, db_wal_frames(),
                    st.taskrun_clears, g_rings[0].nbuf_live, db_cache_misses(), (int)(g_busy * 100 + 0.5), g_napping,
                    (int)g_nap_us, (int)(g_batch_ewma + 0.5), g_nap_backoffs);
#ifdef PROF
            /* cycles per call, and calls; the TSC rate turns cycles into time */
            fprintf(stderr, "prof (cycles/call x calls):");
            for (int k = 0; k < P_N; k++) {
                fprintf(stderr, " %s=%lu x %lu", prof_name[k], prof_cnt[k] ? prof_cyc[k] / prof_cnt[k] : 0,
                        prof_cnt[k]);
                prof_cyc[k] = prof_cnt[k] = 0;
            }
            static uint64_t tsc0;
            static int64_t mono0;
            uint64_t tsc = __rdtsc();
            double mhz = mono0 ? (double)(tsc - tsc0) / (double)(g_mono_us - mono0) : 0;
            tsc0 = tsc;
            mono0 = g_mono_us;
            fprintf(stderr, " passes=%lu tsc=%.0fMHz\n", st.passes, mhz);
            st.passes = 0;
#endif
        }
        track_window();
        st.passes++;
        unsigned events = 0;
        g_pass_data = 0;
        for (int i = 0; i < g_nrings; i++) {
            struct ring *r = &g_rings[i];
            unsigned sqf = __atomic_load_n(r->sq_flags, __ATOMIC_RELAXED);
            if (r->sqe_tail != r->submitted || (sqf & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW))) {
                PROF_T0(P_ENTER);
                ring_enter(r, 0, NULL);
                /* 6.8 can leave TASKRUN set with nothing queued: work that arrives
                 * while task work runs sets it, and the same run's retry loop then
                 * consumes that work. Only a run clears the flag, and an enter runs
                 * nothing on an empty list, so the loop would never sleep again.
                 * The kernel never reads the flag, so clear it ourselves and enter
                 * once more: that runs anything queued before the clear, and
                 * anything queued after it sets the flag again. */
                if ((sqf & IORING_SQ_TASKRUN) && *r->cq_head == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE) &&
                    (__atomic_load_n(r->sq_flags, __ATOMIC_RELAXED) & IORING_SQ_TASKRUN)) {
                    __atomic_fetch_and(r->sq_flags, ~IORING_SQ_TASKRUN, __ATOMIC_SEQ_CST);
                    ring_enter(r, 0, NULL);
                    st.taskrun_clears++;
                }
                PROF_T1(P_ENTER);
            }
            PROF_T0(P_DRAIN);
            events += drain_cq(r);
            PROF_T1(P_DRAIN);
        }
        PROF_T0(P_READEND);
        db_read_end();
        PROF_T1(P_READEND);
        PROF_T0(P_COMMIT);
        commit_writes();
        PROF_T1(P_COMMIT);
        for (int i = 0; i < g_nrings; i++)
            if (g_rings[i].nrearm) do_rearms(&g_rings[i]);
        int64_t pace;
        /* Nothing ready: sleep for up to 1 ms (or until the next commit is due),
         * waking as soon as any ring has work. Never spin while writes wait: on one
         * vCPU a spinning loop starves the checkpoint thread they depend on. */
        if (!events) {
            int64_t wait_us = 1000;
            if (g_njobs && g_hold_since_us < 0) {
                int64_t due = g_last_commit_us + g_commit_interval_us - g_mono_us;
                if (due < wait_us) wait_us = due;
            }
            if (wait_us > 0 && !rings_pending()) {
                int64_t t0 = mono_now_us();
                idle_wait(wait_us);
                g_win_sleep_us += mono_now_us() - t0;
            }
        } else if ((pace = nap_length()) > 0 && g_pass_data) {
            /* Pacing: let completions pile up so each enter does more work. Submit
             * what this pass queued on every ring first, so no response waits out
             * the pause. Only passes that received requests pause: one that only
             * collected send completions goes straight on to the idle wait, which
             * wakes on the next request. The pause never runs past a due commit. */
            for (int i = 0; i < g_nrings; i++)
                if (g_rings[i].sqe_tail != g_rings[i].submitted) ring_enter(&g_rings[i], 0, NULL);
            int64_t now_us = mono_now_us();
            int64_t left = g_mono_us + pace - now_us;
            if (g_njobs && g_hold_since_us < 0) {
                int64_t due = g_last_commit_us + g_commit_interval_us - now_us;
                if (due < left) left = due;
            }
            if (left > 0) {
                struct timespec d = {left / 1000000, (left % 1000000) * 1000};
                nanosleep(&d, NULL);
                g_win_sleep_us += mono_now_us() - now_us;
            }
        }
    }
}
