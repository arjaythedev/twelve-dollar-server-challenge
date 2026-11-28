/* io_uring transport (kernel plumbing only; the event loop, HTTP and handlers are Kotlin, see src/Server.kt).
 *
 * - One thread. Up to max_rings rings, each with its own sparse direct-descriptor table (a table holds at most
 *   RLIMIT_NOFILE entries, so a second ring holds the next 65k connections in the same process). Accepted sockets
 *   never get a normal fd: accept_direct with IORING_FILE_INDEX_ALLOC, recv/send/shutdown/close on the index.
 * - ACCEPT_DEPTH single-shot accepts in flight (multishot accept into a direct table lost connections for another
 *   entry); new accepts stop FD_MARGIN slots before a table is full and go to the next ring instead.
 * - Multishot recv with a provided-buffer ring per ring: an idle connection owns no buffer. Buffers go back to the
 *   ring right after Kotlin has handled the pass (ur_done). A receive that ran out of buffers (ENOBUFS) is re-armed
 *   after the pass's buffers are back (all of them by default; KN_REARM bounds it per pass).
 * - Sends copy the response into one malloc'd request (pointer-tagged user_data), resubmit on a short write, and
 *   queue later output behind an in-flight send so the order on the wire is kept.
 * - Secondary rings signal one eventfd that ring 0 reads, so the thread sleeps in one io_uring_enter.
 * Ideas from public MIT entries (no code copied): rust-iouring-kensac (rings spilling past the fd cap, 512 accepts in
 * flight, soft RLIMIT raised to the hard one, re-arm after ENOBUFS (bounded per pass there; KN_REARM here)), c-iouring-pavlyhalim (provided buffers). */
#include "liburing.h"
#include "kn.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef TCP_DEFER_ACCEPT
#define TCP_DEFER_ACCEPT 9
#endif

#define MAX_RINGS 8
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 32768
#define NBUF_DEFAULT 4096    /* provided buffers per ring (power of two, KN_NBUF). 1024 ran out at the ramp end on a
                                1 vCPU 6.8 VM (23k ENOBUFS, p99 1.2 s at 160k users); 4096: 3k ENOBUFS, p99 0.3-0.5 s */
#define BUFSZ 2048
#define ACCEPT_DEPTH 512
#define FD_MARGIN 256
#define REARM_DEFAULT 0      /* receives re-armed per pass after ENOBUFS (KN_REARM; 0 = all). 256 per pass left a
                                burst of 8-23k starved receives waiting 30-90 passes (seconds) on the VM */
#define DEFER_IDLE_US 1000   /* longest sleep on ring 0 while secondary rings (DEFER_TASKRUN) exist */

enum { K_ACCEPT = 1, K_RECV = 2, K_SEND = 3, K_CLOSE = 4, K_SHUT = 5, K_EVFD = 6, K_WATCH = 7 };

typedef struct conn {
    uint32_t gen;
    uint8_t open, sending, close_after, shut, rearm;
    uint8_t *pend;              /* output queued behind the in-flight send */
    int pend_len, pend_cap;
} conn;

typedef struct sreq {
    int len, off, slot;
    uint32_t gen;
    uint8_t buf[];
} sreq;

typedef struct ringx {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    uint8_t *bufs;
    int used, accepts, table, dirty;
    int nrecycle;
    conn *conns;
} ringx;

static ringx R[MAX_RINGS];
#define MAX_LISTENERS 8
static int nrings, maxrings, efd = -1, table_size;
/* KN_LISTENERS=n (1..8): n SO_REUSEPORT listeners on the port, accepts spread over them (default 1). */
static int lfds[MAX_LISTENERS], nlisten = 1, lacc[MAX_LISTENERS];
/* KN_DEFER=1: IORING_SETUP_DEFER_TASKRUN on every ring (completions posted only when the loop enters a ring). Off by
 * default: with the batch wait (wait for 256 completions) it measured worse at 256k users (reads p95 0.5 -> 40 ms,
 * 14.5 -> 16.5 us/req; either one alone was fine), so the rings use COOP_TASKRUN and the eventfd. */
static int defer_taskrun;
/* KN_PASS=sweep: the pass structure of java-iouring-davidhsv (#393; ideas only): every ring DEFER_TASKRUN; each pass
 * first sweeps the secondary rings without waiting (submit, run their deferred completions), then waits on ring 0
 * alone, then collects every ring. Default (batch): b3's batch wake on COOP_TASKRUN rings (see ur_poll). */
static int pass_sweep;
static int64_t st_sweep_enters;
static uint64_t efd_val, watch_val;
static int watch_fd = -1;
static int rearm_q[1 << 20], rearm_head, rearm_tail;   /* global slots waiting for a recv re-arm */
static int NBUF = NBUF_DEFAULT, REARM_MAX = REARM_DEFAULT > 0 ? REARM_DEFAULT : 1 << 20;
static int64_t st_recv_eof, st_recv_err[160], st_send_err[160], st_send_zero, st_shuts;   /* why connections end (KN_STATS) */
static int64_t st_open, st_enobufs, st_accept_err, st_rearms;
static int64_t st_cqe[MAX_RINGS], st_acc_ok[MAX_RINGS];
static kn_ev *g_evs;
static int g_nev, g_max;

static inline uint64_t ud(int kind, int r, int idx, uint32_t gen) {
    return (uint64_t)kind | (uint64_t)r << 4 | (uint64_t)idx << 8 | (uint64_t)gen << 32;
}

static struct io_uring_sqe *sqe_of(int r) {
    struct io_uring_sqe *s = io_uring_get_sqe(&R[r].ring);
    if (!s) {
        io_uring_submit(&R[r].ring);
        s = io_uring_get_sqe(&R[r].ring);
    }
    R[r].dirty = 1;
    return s;
}

/* Provided-buffer ring, group 0. Registered by hand rather than with io_uring_setup_buf_ring(): Ubuntu's 6.8.0-136+
 * kernels invert the check on the reserved words and refuse a registration whose resv[] is all zero (Launchpad
 * #2162843; noted in rust-iouring-kensac's README), so an EINVAL is retried once with resv[0] = 1. */
static struct io_uring_buf_ring *buf_ring(struct io_uring *ring) {
    size_t sz = (size_t)NBUF * sizeof(struct io_uring_buf);
    void *mem = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap buf ring"); return 0; }
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr = (uint64_t)(uintptr_t)mem;
    reg.ring_entries = NBUF;
    reg.bgid = 0;
    int rc = io_uring_register_buf_ring(ring, &reg, 0);
    if (rc == -EINVAL) {
        reg.resv[0] = 1;
        rc = io_uring_register_buf_ring(ring, &reg, 0);
        if (rc == 0) fprintf(stderr, "buf ring registered with resv[0]=1 (kernel quirk)\n");
    }
    if (rc < 0) { fprintf(stderr, "register_buf_ring: %s\n", strerror(-rc)); munmap(mem, sz); return 0; }
    io_uring_buf_ring_init((struct io_uring_buf_ring *)mem);
    return (struct io_uring_buf_ring *)mem;
}

static int ring_create(int r) {
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    /* DEFER_TASKRUN: a ring posts its completions only when the loop enters it, in one batch per pass. A secondary
     * ring then never signals the eventfd while the loop sleeps on ring 0, so with more than one ring ur_poll never
     * sleeps longer than DEFER_IDLE_US and enters every ring each pass (without that cap an idle server left
     * connections accepted by ring 1 unanswered: test-rings.sh). */
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE |
              (defer_taskrun ? IORING_SETUP_DEFER_TASKRUN : IORING_SETUP_COOP_TASKRUN) | IORING_SETUP_TASKRUN_FLAG;
    p.cq_entries = CQ_ENTRIES;
    int rc = io_uring_queue_init_params(SQ_ENTRIES, &R[r].ring, &p);
    if (rc < 0) {   /* older kernel: plain ring */
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_CQSIZE;
        p.cq_entries = CQ_ENTRIES;
        rc = io_uring_queue_init_params(SQ_ENTRIES, &R[r].ring, &p);
    }
    if (rc < 0) { fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-rc)); return -1; }
    io_uring_register_ring_fd(&R[r].ring);
    rc = io_uring_register_files_sparse(&R[r].ring, table_size);
    if (rc < 0) { fprintf(stderr, "register_files_sparse(%d): %s\n", table_size, strerror(-rc)); return -1; }
    R[r].br = buf_ring(&R[r].ring);
    if (!R[r].br) return -1;
    R[r].bufs = aligned_alloc(4096, (size_t)NBUF * BUFSZ);
    R[r].conns = calloc(table_size, sizeof(conn));
    if (!R[r].bufs || !R[r].conns) return -1;
    for (int i = 0; i < NBUF; i++)
        io_uring_buf_ring_add(R[r].br, R[r].bufs + (size_t)i * BUFSZ, BUFSZ, (unsigned short)i, io_uring_buf_ring_mask(NBUF), i);
    io_uring_buf_ring_advance(R[r].br, NBUF);
    R[r].table = table_size;
    if (r > 0) {
        if (efd < 0) {
            efd = eventfd(0, EFD_CLOEXEC);
            if (efd < 0) return -1;
            struct io_uring_sqe *s = sqe_of(0);
            io_uring_prep_read(s, efd, &efd_val, sizeof efd_val, 0);
            io_uring_sqe_set_data64(s, ud(K_EVFD, 0, 0, 0));
        }
        io_uring_register_eventfd(&R[r].ring, efd);
    }
    return 0;
}

/* Locks this binary's own file-backed mappings (code, read-only data) in RAM: the box has no swap, so near the memory
 * wall reclaim can only take file pages, and the loop would stall on major faults reading its own code back.
 * Within the default 8 MB RLIMIT_MEMLOCK (the binary is ~2.6 MB). Idea credit: c-iouring-pavlyhalim,
 * mojo-iouring-oh-meh, fsharp-uring-delneg. KN_MLOCK=0 turns it off. */
static void mlock_self(void) {
    if (getenv("KN_MLOCK") && getenv("KN_MLOCK")[0] == '0') return;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[4352];
    size_t locked = 0;
    int fails = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8];
        int off = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perm, &off) < 3 || !off) continue;
        char *path = line + off;
        path[strcspn(path, "\n")] = 0;
        if (strcmp(path, exe) != 0 || perm[2] == 'w') continue;   /* our own code and read-only data */
        if (mlock((void *)lo, hi - lo) == 0) locked += hi - lo; else fails++;
    }
    fclose(f);
    fprintf(stderr, "mlock: %zu KB of the binary locked%s\n", locked / 1024, fails ? " (some mappings failed)" : "");
}

/* PGO-instrumented builds only (build.sh's training run): write the profile when the training run stops us with
 * SIGTERM. The symbol is weak, so in the real (non-instrumented) build it is NULL and SIGTERM keeps its default. */
extern int __llvm_profile_write_file(void) __attribute__((weak));
static void on_term(int sig) {
    (void)sig;
    __llvm_profile_write_file();
    _exit(0);
}

int ur_init(const char *host, int port, int max_rings) {
    signal(SIGPIPE, SIG_IGN);
    if (__llvm_profile_write_file) signal(SIGTERM, on_term);
    mlock_self();
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    table_size = rl.rlim_cur > 65535 ? 65535 : (int)rl.rlim_cur;
    if (getenv("KN_TABLE")) table_size = atoi(getenv("KN_TABLE"));   /* testing the ring spill with small tables */
    if (getenv("KN_NBUF")) { int n = atoi(getenv("KN_NBUF")); if (n >= 64 && n <= 32768 && !(n & (n - 1))) NBUF = n; }
    if (getenv("KN_REARM")) { int n = atoi(getenv("KN_REARM")); REARM_MAX = n > 0 ? n : 1 << 20; }
    if (table_size < 2 * FD_MARGIN) table_size = 2 * FD_MARGIN;
    maxrings = max_rings < 1 ? 1 : max_rings > MAX_RINGS ? MAX_RINGS : max_rings;
    defer_taskrun = getenv("KN_DEFER") && getenv("KN_DEFER")[0] == '1';
    pass_sweep = getenv("KN_PASS") && !strcmp(getenv("KN_PASS"), "sweep");
    if (pass_sweep) defer_taskrun = 1;

    if (getenv("KN_LISTENERS")) nlisten = atoi(getenv("KN_LISTENERS"));
    if (nlisten < 1) nlisten = 1;
    if (nlisten > MAX_LISTENERS) nlisten = MAX_LISTENERS;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", host); return -1; }
    for (int l = 0; l < nlisten; l++) {
        int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (lfd < 0) { perror("socket"); return -1; }
        int one = 1, defer = 5;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (nlisten > 1) setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        setsockopt(lfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* accepted sockets inherit it */
        if (!getenv("KN_NO_DEFER")) setsockopt(lfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof defer);
        /* A restart right after a stop can find the port still in LISTEN: on 6.8 kernels the old process's io_uring
         * is torn down asynchronously after exit, and its pending accepts keep the old listener alive until then
         * (seen on a Vultr Ubuntu 24.04 VM, 6.8.0-147: "bind: Address already in use" one second after a stop with
         * ~1k open connections). SO_REUSEADDR does not cover a listener, so retry for up to 30 s. (With several
         * SO_REUSEPORT listeners the old process's listener does not block the bind; only the first one retries.) */
        for (int tries = 0;; tries++) {
            if (bind(lfd, (struct sockaddr *)&a, sizeof a) == 0) {
                if (tries) fprintf(stderr, "bind: port free after %d.%d s\n", tries / 10, tries % 10);
                break;
            }
            if (errno != EADDRINUSE || tries >= 300) { perror("bind"); return -1; }
            if (tries == 0) fprintf(stderr, "bind: address in use, retrying for up to 30 s\n");
            usleep(100 * 1000);
        }
        if (listen(lfd, 65535) < 0) { perror("listen"); return -1; }
        lfds[l] = lfd;
    }
    if (ring_create(0)) return -1;
    nrings = 1;
    return 0;
}

/* ------------------------------------------------------------------ per-connection operations */
static void arm_recv(int r, int idx) {
    struct io_uring_sqe *s = sqe_of(r);
    io_uring_prep_recv_multishot(s, idx, 0, 0, 0);
    s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->buf_group = 0;
    io_uring_sqe_set_data64(s, ud(K_RECV, r, idx, R[r].conns[idx].gen));
}

static void shut(int r, int idx) {
    conn *c = &R[r].conns[idx];
    if (c->shut) return;
    c->shut = 1; st_shuts++;
    struct io_uring_sqe *s = sqe_of(r);
    io_uring_prep_shutdown(s, idx, SHUT_RDWR);   /* the recv then ends with 0 and conn_closed() frees the slot */
    s->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(s, ud(K_SHUT, r, idx, c->gen));
}

static void submit_send(int r, int idx, conn *c, const uint8_t *p, int len) {
    sreq *q = malloc(sizeof(sreq) + (size_t)len);
    if (!q) { shut(r, idx); return; }
    memcpy(q->buf, p, (size_t)len);
    q->len = len; q->off = 0; q->slot = r << 16 | idx; q->gen = c->gen;
    c->sending = 1;
    struct io_uring_sqe *s = sqe_of(r);
    io_uring_prep_send(s, idx, q->buf, (size_t)len, MSG_NOSIGNAL);
    s->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(s, (uint64_t)(uintptr_t)q | K_SEND);
}

static conn *live(int slot, int gen, int *r, int *idx) {
    *r = slot >> 16; *idx = slot & 0xffff;
    if (*r >= nrings || *idx >= R[*r].table) return 0;
    conn *c = &R[*r].conns[*idx];
    return c->open && !c->shut && c->gen == (uint32_t)gen ? c : 0;
}

void ur_send(int slot, int gen, const uint8_t *p, int len) {
    int r, idx;
    conn *c = live(slot, gen, &r, &idx);
    if (!c || len <= 0) return;
    if (!c->sending) { submit_send(r, idx, c, p, len); return; }
    if (c->pend_len + len > c->pend_cap) {
        int cap = c->pend_cap ? c->pend_cap : 1024;
        while (cap < c->pend_len + len) cap *= 2;
        uint8_t *n = realloc(c->pend, (size_t)cap);
        if (!n) { shut(r, idx); return; }
        c->pend = n; c->pend_cap = cap;
    }
    memcpy(c->pend + c->pend_len, p, (size_t)len);
    c->pend_len += len;
}

void ur_close_after(int slot, int gen) {
    int r, idx;
    conn *c = live(slot, gen, &r, &idx);
    if (!c) return;
    if (c->sending) c->close_after = 1; else shut(r, idx);
}

static void emit(int kind, int r, int idx, uint32_t gen, uint8_t *ptr, int len) {
    kn_ev *e = &g_evs[g_nev++];
    e->kind = kind; e->slot = r << 16 | idx; e->gen = (int32_t)gen; e->len = len; e->ptr = ptr;
}

static void conn_closed(int r, int idx) {
    conn *c = &R[r].conns[idx];
    if (!c->open) return;
    uint32_t gen = c->gen;
    c->open = 0; c->gen++; c->sending = 0; c->close_after = 0; c->shut = 0; c->rearm = 0;
    free(c->pend); c->pend = 0; c->pend_len = c->pend_cap = 0;
    R[r].used--; st_open--;
    struct io_uring_sqe *s = sqe_of(r);
    io_uring_prep_close_direct(s, (unsigned)idx);
    io_uring_sqe_set_data64(s, ud(K_CLOSE, r, idx, 0));
    emit(EV_CLOSED, r, idx, gen, 0, 0);
}

static void recycle(int r, int bid) {
    ringx *x = &R[r];
    io_uring_buf_ring_add(x->br, x->bufs + (size_t)bid * BUFSZ, BUFSZ, (unsigned short)bid, io_uring_buf_ring_mask(NBUF), x->nrecycle++);
}

static void flush_recycled(void) {
    for (int r = 0; r < nrings; r++) {
        if (R[r].nrecycle) { io_uring_buf_ring_advance(R[r].br, R[r].nrecycle); R[r].nrecycle = 0; }
    }
}

static void queue_rearm(int r, int idx) {
    conn *c = &R[r].conns[idx];
    if (c->rearm) return;
    c->rearm = 1;
    rearm_q[rearm_tail++ & ((1 << 20) - 1)] = r << 16 | idx;
}

/* ------------------------------------------------------------------ completions */
static void top_up_accepts(void) {
    int inflight = 0;
    for (int r = 0; r < nrings; r++) inflight += R[r].accepts;
    for (int r = 0; r < maxrings && inflight < ACCEPT_DEPTH; r++) {
        if (r == nrings) {
            if (ring_create(r)) { maxrings = r; break; }
            nrings++;
            fprintf(stderr, "ring %d opened (%lld connections)\n", r, (long long)st_open);
        }
        ringx *x = &R[r];
        int room = x->table - FD_MARGIN - x->used - x->accepts;
        while (room-- > 0 && inflight < ACCEPT_DEPTH) {
            int l = 0;   /* the listener with the fewest accepts in flight */
            for (int k = 1; k < nlisten; k++) if (lacc[k] < lacc[l]) l = k;
            lacc[l]++;
            struct io_uring_sqe *s = sqe_of(r);
            io_uring_prep_accept_direct(s, lfds[l], 0, 0, 0, IORING_FILE_INDEX_ALLOC);
            io_uring_sqe_set_data64(s, ud(K_ACCEPT, r, l, 0));
            x->accepts++; inflight++;
        }
    }
}

static int accept_failed;

static void on_cqe(int r, struct io_uring_cqe *cqe) {
    uint64_t u = cqe->user_data;
    int kind = (int)(u & 15);
    int res = cqe->res;
    if (kind == K_SEND) {
        sreq *q = (sreq *)(uintptr_t)(u & ~(uint64_t)15);
        int sr = q->slot >> 16, idx = q->slot & 0xffff;
        conn *c = &R[sr].conns[idx];
        int mine = c->open && c->gen == q->gen;
        if (res > 0 && mine && q->off + res < q->len) {   /* short write: send the rest */
            q->off += res;
            struct io_uring_sqe *s = sqe_of(sr);
            io_uring_prep_send(s, idx, q->buf + q->off, (size_t)(q->len - q->off), MSG_NOSIGNAL);
            s->flags |= IOSQE_FIXED_FILE;
            io_uring_sqe_set_data64(s, u);
            return;
        }
        free(q);
        if (!mine) return;
        c->sending = 0;
        if (res <= 0) { if (res == 0) st_send_zero++; else st_send_err[-res < 160 ? -res : 159]++; shut(sr, idx); return; }
        if (c->pend_len) {
            /* swap the queued output into a fresh send */
            int n = c->pend_len;
            c->pend_len = 0;
            submit_send(sr, idx, c, c->pend, n);
        } else if (c->close_after) shut(sr, idx);
        return;
    }
    int idx = (int)(u >> 8 & 0xffff);
    uint32_t gen = (uint32_t)(u >> 32);
    ringx *x = &R[r];
    switch (kind) {
    case K_ACCEPT:
        x->accepts--;
        lacc[idx]--;
        if (res >= 0) {
            conn *c = &x->conns[res];
            c->gen++; c->open = 1; c->sending = c->close_after = c->shut = c->rearm = 0;
            x->used++; st_open++; st_acc_ok[r]++;
            arm_recv(r, res);
            emit(EV_OPEN, r, res, c->gen, 0, 0);
        } else {
            st_accept_err++;
            accept_failed = 1;
            if (st_accept_err < 10 || st_accept_err % 10000 == 0)
                fprintf(stderr, "accept: %s (%lld so far)\n", strerror(-res), (long long)st_accept_err);
        }
        return;
    case K_RECV: {
        conn *c = &x->conns[idx];
        int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
        int bid = has_buf ? (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT) : -1;
        if (!c->open || c->gen != gen) { if (has_buf) recycle(r, bid); return; }
        if (res > 0) {
            emit(EV_DATA, r, idx, gen, x->bufs + (size_t)bid * BUFSZ, res);
            if (!(cqe->flags & IORING_CQE_F_MORE)) queue_rearm(r, idx);
            return;
        }
        if (has_buf) recycle(r, bid);
        if (res == -ENOBUFS) { st_enobufs++; queue_rearm(r, idx); return; }
        if (res == 0) st_recv_eof++; else st_recv_err[-res < 160 ? -res : 159]++;
        conn_closed(r, idx);   /* EOF, reset, or our shutdown */
        return;
    }
    case K_WATCH: {
        struct io_uring_sqe *s = sqe_of(0);
        io_uring_prep_read(s, watch_fd, &watch_val, sizeof watch_val, 0);
        io_uring_sqe_set_data64(s, ud(K_WATCH, 0, 0, 0));
        if (res > 0) emit(EV_WAKE, 0, 0, 0, 0, 0);
        return;
    }
    case K_EVFD: {
        struct io_uring_sqe *s = sqe_of(0);
        io_uring_prep_read(s, efd, &efd_val, sizeof efd_val, 0);
        io_uring_sqe_set_data64(s, ud(K_EVFD, 0, 0, 0));
        return;
    }
    default:   /* K_CLOSE, K_SHUT */
        return;
    }
}


static int backlog;   /* the last pass left completions in a queue (event array full) */

/* Posts a DEFER_TASKRUN ring's pending completions. One GETEVENTS enter runs only a bounded number of them on newer
 * kernels (about 20 without a wait), so repeat while the kernel still flags task work (IORING_SETUP_TASKRUN_FLAG).
 * Without this, secondary rings fell behind at 256k users (p99 510 ms at 37 % CPU). */
static void drain_deferred(int r) {
    for (int k = 0; k < 256; k++) {
        if (r > 0 || k > 0) io_uring_get_events(&R[r].ring);
        if (!(IO_URING_READ_ONCE(*R[r].ring.sq.kflags) & IORING_SQ_TASKRUN)) break;
        if (io_uring_cq_ready(&R[r].ring) > CQ_ENTRIES / 2) break;   /* plenty to do already */
    }
}

/* KN_PASS=sweep: a ring that is not waited on. Submits what is queued and runs its deferred completions without
 * sleeping; a ring with nothing queued and no task work flagged costs no system call. An enter that asks for no
 * completions runs only a small batch of task work on newer kernels, so if the flag is still up a second enter asks
 * for many completions with a zero timeout (it runs them all and returns at once). */
static void sweep(int r) {
    struct io_uring *q = &R[r].ring;
    unsigned busy = IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW;
    R[r].dirty = 0;
    if (!io_uring_sq_ready(q) && !(IO_URING_READ_ONCE(*q->sq.kflags) & busy)) return;
    io_uring_submit_and_get_events(q);
    st_sweep_enters++;
    if (IO_URING_READ_ONCE(*q->sq.kflags) & IORING_SQ_TASKRUN) {
        struct __kernel_timespec ts = {0, 0};
        struct io_uring_cqe *cqe;
        io_uring_wait_cqes(q, &cqe, 4096, &ts, 0);
        st_sweep_enters++;
    }
}

static int collect(void);

static int poll_sweep(int wait_nr, int64_t timeout_us) {
    for (int r = nrings - 1; r > 0; r--) sweep(r);
    /* An idle server with secondary rings must still wake to sweep them (deferred rings never signal): cap the sleep. */
    if (nrings > 1 && (timeout_us < 0 || timeout_us > DEFER_IDLE_US)) timeout_us = DEFER_IDLE_US;
    struct io_uring_cqe *cqe;
    if (backlog) io_uring_submit_and_get_events(&R[0].ring);
    else if (timeout_us < 0) io_uring_submit_and_wait(&R[0].ring, 1);
    else {
        struct __kernel_timespec ts = {timeout_us / 1000000, (timeout_us % 1000000) * 1000};
        io_uring_submit_and_wait_timeout(&R[0].ring, &cqe, (unsigned)(wait_nr < 1 ? 1 : wait_nr), &ts, 0);
    }
    R[0].dirty = 0;
    return collect();
}

int ur_poll(kn_ev *evs, int max, int wait_nr, int64_t timeout_us) {
    g_evs = evs; g_nev = 0; g_max = max;
    if (!accept_failed) top_up_accepts();
    accept_failed = 0;
    if (pass_sweep) return poll_sweep(wait_nr, timeout_us);
    /* 1. Hand the secondary rings their new requests (sends of the last pass) before sleeping. */
    for (int r = 1; r < nrings; r++) {
        if (R[r].dirty || io_uring_sq_ready(&R[r].ring)) io_uring_submit(&R[r].ring);
        R[r].dirty = 0;
    }
    /* 2. Sleep on ring 0 alone: wait_nr completions or timeout_us (batch wake). Completions of the secondary rings
     *    are not looked at here, or they would cut every wait short once a second ring exists. */
    struct io_uring_cqe *cqe;
    if (defer_taskrun && nrings > 1 && (timeout_us < 0 || timeout_us > DEFER_IDLE_US)) {
        timeout_us = DEFER_IDLE_US;
        if (wait_nr < 1) wait_nr = 1;
    }
    if (backlog || io_uring_cq_ready(&R[0].ring)) io_uring_submit(&R[0].ring);
    else if (timeout_us < 0) io_uring_submit_and_wait(&R[0].ring, 1);
    else {
        struct __kernel_timespec ts = {timeout_us / 1000000, (timeout_us % 1000000) * 1000};
        io_uring_submit_and_wait_timeout(&R[0].ring, &cqe, (unsigned)(wait_nr < 1 ? 1 : wait_nr), &ts, 0);
    }
    R[0].dirty = 0;
    /* 3. Collect what the secondary rings completed meanwhile (with DEFER_TASKRUN they post only when entered). */
    if (defer_taskrun)
        for (int r = 0; r < nrings; r++) drain_deferred(r);
    return collect();
}

/* Turns every ring's ready completions into events (a CQE can add up to two: a close emits one; keep room). */
static int collect(void) {
    struct io_uring_cqe *cqe;
    backlog = 0;
    for (int r = 0; r < nrings; r++) {
        unsigned head, n = 0;
        io_uring_for_each_cqe(&R[r].ring, head, cqe) {
            if (g_nev + 2 > g_max) { backlog = 1; break; }
            on_cqe(r, cqe);
            n++;
            st_cqe[r]++;
        }
        io_uring_cq_advance(&R[r].ring, n);
    }
    flush_recycled();
    return g_nev;
}

void ur_done(void) {
    for (int i = 0; i < g_nev; i++) {
        kn_ev *e = &g_evs[i];
        if (e->kind == EV_DATA) {
            int r = e->slot >> 16;
            recycle(r, (int)((e->ptr - R[r].bufs) / BUFSZ));
        }
    }
    flush_recycled();
    g_nev = 0;
    /* Bounded re-arm: the buffers of this pass are back; re-arm at most REARM_MAX receives. */
    for (int k = 0; k < REARM_MAX && rearm_head != rearm_tail; k++) {
        int s = rearm_q[rearm_head++ & ((1 << 20) - 1)];
        int r = s >> 16, idx = s & 0xffff;
        conn *c = &R[r].conns[idx];
        if (!c->rearm) continue;
        c->rearm = 0;
        if (!c->open || c->shut) continue;
        arm_recv(r, idx);
        st_rearms++;
    }
}



int64_t ur_stat(int which) {
    if (which >= 100 && which < 100 + MAX_RINGS) {   /* per ring: used, accepts in flight, CQEs, accepts done */
        int r = which - 100;
        if (r >= nrings) return -1;
        if (r == 0) {
            fprintf(stderr, "  ends: recv_eof=%lld shuts=%lld send_zero=%lld", (long long)st_recv_eof, (long long)st_shuts, (long long)st_send_zero);
            for (int e = 1; e < 160; e++) if (st_recv_err[e]) fprintf(stderr, " recv_e%d=%lld", e, (long long)st_recv_err[e]);
            for (int e = 1; e < 160; e++) if (st_send_err[e]) fprintf(stderr, " send_e%d=%lld", e, (long long)st_send_err[e]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "  ring %d: used=%d accepts=%d cqes=%lld accepted=%lld sq_ready=%u cq_ready=%u flags=%x\n", r, R[r].used,
                R[r].accepts, (long long)st_cqe[r], (long long)st_acc_ok[r], io_uring_sq_ready(&R[r].ring),
                io_uring_cq_ready(&R[r].ring), *R[r].ring.sq.kflags);
        return 0;
    }
    switch (which) {
    case 0: return st_open;
    case 1: return st_enobufs;
    case 2: return st_accept_err;
    case 3: return nrings;
    case 4: return st_rearms;
    case 5: return st_sweep_enters;
    default: return 0;
    }
}

int ur_watch_eventfd(int fd) {
    watch_fd = fd;
    struct io_uring_sqe *s = sqe_of(0);
    io_uring_prep_read(s, watch_fd, &watch_val, sizeof watch_val, 0);
    io_uring_sqe_set_data64(s, ud(K_WATCH, 0, 0, 0));
    return 0;
}

int64_t kn_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int64_t kn_wall_s(void) {
    return (int64_t)time(0);
}
