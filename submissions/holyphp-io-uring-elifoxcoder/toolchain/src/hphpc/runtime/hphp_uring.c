/* hphp_uring.c — the generic stream-server transport: sockets, epoll and the
 * io_uring backend with direct descriptors and provided buffers.
 *
 * Ported from the twelve-dollar challenge runtime (runtime/hphp_std.c there)
 * without algorithm changes. Linked into a program only when the generated
 * code mentions one of its symbols (see cc_build_in_dir in main.c). The
 * io_uring part is Linux-only; everywhere else the epoll/poll path is used.
 */
#if !defined(_WIN32)
#define _GNU_SOURCE 1
#endif
#include "hphp_modules.h"

/* used by stream_close before its definition below */
static void hp_sock_graceful_close(hp_sock s);

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sched.h>
#endif

/* ------------------------------------------------------------------ */
/* sockets — the transport layer PHP calls "streams"                   */
/*                                                                     */
/*   $srv = stream_socket_server("tcp://127.0.0.1:9001");  -> handle   */
/*   $c   = stream_socket_client("tcp://127.0.0.1:9001");  -> handle   */
/*   $c   = stream_socket_accept($srv);        -> handle, -1 if none   */
/*   stream_set_blocking($c, false);           -> bool                 */
/*   stream_ready(array $handles, int $ms);    -> the readable ones    */
/*   stream_recv($c, int $max);                -> string, "" = nothing */
/*   stream_send($c, string $data);            -> bytes sent           */
/*   stream_eof($c); stream_peer($c); stream_close($c);                */
/*                                                                     */
/* Handles are plain integers (PHP exposes fds the same way) and every  */
/* accepted socket is non-blocking, so a server loop polls with         */
/* stream_ready() and one slow client can never stall the others.       */
/* ------------------------------------------------------------------ */

static bool hp_net_up = false;

static void hp_net_init(void) {
    if (hp_net_up) return;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#else
    /* A peer that resets the connection must not take the server down with
     * SIGPIPE in the middle of stream_send(). */
    signal(SIGPIPE, SIG_IGN);
#endif
    hp_net_up = true;
}

/* Per-socket state. EOF is sticky: once the peer closes, it stays closed
 * even if we stop reading. */
typedef struct { intptr_t h; bool eof; bool listener; } hp_sockrec;
/* Indexed by descriptor: a server with 15k connections must not pay a linear
 * scan on every recv/send. Slots are grown on demand. */
static hp_sockrec *g_sock;
static size_t g_sock_cap;

static hp_sockrec *sockrec(intptr_t h, bool create) {
    if (h < 0 || h > 0x7fffffff) return NULL;
    if ((size_t)h >= g_sock_cap) {
        if (!create) return NULL;
        size_t ncap = g_sock_cap ? g_sock_cap : 256;
        while (ncap <= (size_t)h) ncap <<= 1;
        hp_sockrec *n = (hp_sockrec *)realloc(g_sock, ncap * sizeof *n);
        if (!n) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
        memset(n + g_sock_cap, 0, (ncap - g_sock_cap) * sizeof *n);
        g_sock = n;
        g_sock_cap = ncap;
    }
    if (!g_sock[h].h) {
        if (!create) return NULL;
        g_sock[h].h = h ? h : -1;   /* used marker; fd 0 is not a socket */
        g_sock[h].eof = false;
    }
    return &g_sock[h];
}

static void sock_forget(intptr_t h) {
    if (h < 0 || (size_t)h >= g_sock_cap) return;
    g_sock[h].h = 0;
    g_sock[h].eof = false;
    g_sock[h].listener = false;
}

static hp_sock sock_of(hval v) { return (hp_sock)(intptr_t)hp_val_to_int(v); }

static void sock_nonblocking(hp_sock s, bool nonblock) {
#ifdef _WIN32
    u_long v = nonblock ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(s, F_SETFL, nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static void sock_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* "tcp://host:port" | "host:port" | ":port" */
static bool sock_split_addr(hstr *addr, char *host, size_t hsz, char *port, size_t psz) {
    const char *p = addr ? addr->data : "";
    const char *scheme = strstr(p, "://");
    if (scheme) p = scheme + 3;
    const char *colon = strrchr(p, ':');
    if (!colon) return false;
    size_t hl = (size_t)(colon - p);
    if (hl >= hsz) hl = hsz - 1;
    memcpy(host, p, hl);
    host[hl] = 0;
    snprintf(port, psz, "%s", colon + 1);
    return port[0] != 0;
}

static hval hp_stream_socket_server_impl(hval addr, int reuseport) {
    hp_net_init();
#if defined(_WIN32) || !defined(SO_REUSEPORT)
    if (reuseport) return hp_of_bool(false);
#endif
    char host[256], port[32];
    if (!sock_split_addr(hp_val_to_str(addr), host, sizeof host, port, sizeof port))
        return hp_of_int(-1);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host[0] ? host : NULL, port, &hints, &res) != 0 || !res)
        return hp_of_int(-1);
    hp_sock s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == HP_SOCK_BAD) { freeaddrinfo(res); return hp_of_int(-1); }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    if (reuseport) {
#if !defined(_WIN32) && defined(SO_REUSEPORT)
        if (setsockopt(s, SOL_SOCKET, SO_REUSEPORT,
                       (const char *)&yes, sizeof yes) != 0) {
            freeaddrinfo(res);
            hp_sock_close(s);
            return hp_of_bool(false);
        }
#endif
    }
    int backlog = reuseport ? 65535 : 4096;
    if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(s, backlog) != 0) {
        freeaddrinfo(res);
        hp_sock_close(s);
        return hp_of_bool(false);   /* PHP-style: failure is falsy */
    }
    freeaddrinfo(res);
    sock_nonblocking(s, true);
    hp_sockrec *sr = sockrec((intptr_t)s, true);
    if (sr) sr->listener = true;
    return hp_of_int((int64_t)(intptr_t)s);
}



hval hpbi_stream_socket_server_reuseport(hval addr) {
    return hp_stream_socket_server_impl(addr, 1);
}



static _Thread_local int g_accept_errno = 0;



#if defined(__linux__) && defined(SYS_io_uring_setup)
/* Defined with the io_uring backend further down; accept2 runs before it. */
static int hp_uring_active(void);
static int hp_ur_direct_on(void);
static hp_sock hp_ur_acceptq_pop(void);
static bool hp_ur_is_direct(hp_sock h);
static hstr *hp_uring_take_recv(hp_sock fd, size_t want);
static int64_t hp_uring_send2(hp_sock fd, hstr *a, hstr *b);
#endif

/* stream_socket_accept2(srv, timeoutMs): poll-then-accept, PHP-style.
 * Returns the client handle, or false when nothing arrived within the
 * timeout. Never blocks longer than timeoutMs (in 5 ms slices). */




/* Disable Nagle on a connected socket: responses are small and the peer
 * waits for each one, so coalescing only adds latency. */
hval hpbi_stream_set_nodelay(hval s) {
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_bool(false);
#if defined(__linux__) && defined(SYS_io_uring_setup)
    /* a direct descriptor has no process fd; the listener already set
     * TCP_NODELAY and accepted sockets inherit it */
    if (hp_ur_is_direct(fd)) return hp_of_bool(true);
#endif
    int one = 1;
    return hp_of_bool(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                                 (const char *)&one, sizeof one) == 0);
}

/* Set TCP socket buffer ceilings. Linux accepted sockets inherit these values
 * from their listening socket; this lets direct-descriptor accept paths cap
 * per-connection kernel memory without needing a process fd for each socket. */
hval hpbi_stream_set_socket_buffers(hval s, hval recv_bytes, hval send_bytes) {
    hp_sock fd = sock_of(s);
    int64_t rcv64 = hp_val_to_int(recv_bytes);
    int64_t snd64 = hp_val_to_int(send_bytes);
    /* A size <= 0 leaves that buffer at the kernel default (autotuned). */
    if (fd == HP_SOCK_BAD || rcv64 > INT_MAX || snd64 > INT_MAX ||
        (rcv64 <= 0 && snd64 <= 0))
        return hp_of_bool(false);
    if (rcv64 > 0) {
        int rcv = (int)rcv64;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                       (const char *)&rcv, sizeof rcv) != 0)
            return hp_of_bool(false);
    }
    if (snd64 > 0) {
        int snd = (int)snd64;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                       (const char *)&snd, sizeof snd) != 0)
            return hp_of_bool(false);
    }
    return hp_of_bool(true);
}


hval hpbi_stream_recv_release(hval s) {
    return hp_of_bool(hp_stream_recv_release_t(hp_val_to_int(s)) != 0);
}

hval hpbi_accept_emfile(void) {
#if defined(_WIN32)
    return hp_of_bool(g_accept_errno == WSAEMFILE);
#else
    return hp_of_bool(g_accept_errno == EMFILE || g_accept_errno == ENFILE);
#endif
}



/* The process's descriptor limit, so the server sheds connections only when
 * it is really about to run out (0 = unknown). */
hval hpbi_nofile_limit(void) {
#if defined(_WIN32)
    return hp_of_int(0);
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return hp_of_int(0);
    return hp_of_int((int64_t)rl.rlim_cur);
#endif
}

/* Two buffers, one writev(2), no blocking: the header and the body go out
 * together without copying the body. Returns bytes written, -1 would-block,
 * -2 error. */
hval hpbi_stream_send2_nb(hval s, hval a, hval b) {
    hp_net_init();
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_int(-2);
    hstr *x = hp_val_to_str(a);
    hstr *y = hp_val_to_str(b);
#if defined(__linux__) && defined(SYS_io_uring_setup)
    if (hp_uring_active()) return hp_of_int(hp_uring_send2(fd, x, y));
#endif
#if defined(_WIN32)
    /* no writev: one concatenated send (the caller buffers the remainder the
     * same way, the bytes are identical) */
    size_t n = x->len + y->len;
    char *tmp = malloc(n ? n : 1);
    if (!tmp) return hp_of_int(-2);
    memcpy(tmp, x->data, x->len);
    memcpy(tmp + x->len, y->data, y->len);
    int r = (int)send(fd, tmp, (int)n, 0);
    free(tmp);
    if (r >= 0) return hp_of_int(r);
    return hp_of_int(hp_sock_wouldblock() ? -1 : -2);
#else
    struct iovec iov[2];
    iov[0].iov_base = x->data;
    iov[0].iov_len = x->len;
    iov[1].iov_base = y->data;
    iov[1].iov_len = y->len;
    ssize_t r = writev(fd, iov, 2);
    if (r >= 0) return hp_of_int((int64_t)r);
    return hp_of_int(hp_sock_wouldblock() ? -1 : -2);
#endif
}

/* Two buffers, one syscall: the header and the body of a response without
 * copying the body into a new string. Returns the bytes written. */
hval hpbi_stream_send2(hval s, hval a, hval b) {
    hp_net_init();
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_int(0);
    hstr *x = hp_val_to_str(a);
    hstr *y = hp_val_to_str(b);
    size_t total = x->len + y->len;
    size_t sent = 0;
    int waited_ms = 0;
    while (sent < total) {
        long long r;
#if defined(_WIN32)
        if (sent < x->len) r = send(fd, x->data + sent, (int)(x->len - sent), 0);
        else r = send(fd, y->data + (sent - x->len), (int)(total - sent), 0);
#else
        if (sent < x->len) {
            struct iovec iov[2];
            iov[0].iov_base = x->data + sent;
            iov[0].iov_len = x->len - sent;
            iov[1].iov_base = y->data;
            iov[1].iov_len = y->len;
            r = (long long)writev(fd, iov, 2);
        } else {
            r = send(fd, y->data + (sent - x->len), (int)(total - sent), 0);
        }
#endif
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && hp_sock_wouldblock() && waited_ms < 2000) {
            sock_sleep_ms(1);
            waited_ms++;
            continue;
        }
        if (r < 0) sockrec((intptr_t)fd, true)->eof = true;
        break;
    }
    return hp_of_int((int64_t)sent);
}

/* Shared poll body: fills `out` with the ready handles. The pollfd array is
 * kept between calls so a busy loop does not malloc per round. */
static void hp_poll_into(harr *out, harr *handles, int to) {
    static _Thread_local struct pollfd *fds = NULL;
    static _Thread_local size_t fdscap = 0;
    if (!handles) return;
    int64_t n = hp_arr_len(handles);
    if (n <= 0) return;
    if ((size_t)n > fdscap) {
        size_t ncap = (size_t)n < 64 ? 64 : (size_t)n;
        struct pollfd *nf = (struct pollfd *)realloc(fds, ncap * sizeof *nf);
        if (!nf) return;
        fds = nf;
        fdscap = ncap;
    }
    int64_t nvalid = 0;
    for (int64_t i = 0; i < n; i++) {
        hval item = hp_arr_get(handles, hp_of_int(i));
        hp_sock fd = sock_of(item);
        if (fd == HP_SOCK_BAD) continue;
        fds[nvalid].fd = fd;
        fds[nvalid].events = POLLIN;
        fds[nvalid].revents = 0;
        nvalid++;
    }
    if (nvalid == 0) return;
    int r;
#ifdef _WIN32
    r = WSAPoll(fds, (ULONG)nvalid, to);
#else
    r = poll(fds, (nfds_t)nvalid, to);
#endif
    if (r <= 0) return;
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++) {
        hval item = hp_arr_get(handles, hp_of_int(i));
        hp_sock fd = sock_of(item);
        if (fd == HP_SOCK_BAD) continue;
        short rev = (short)fds[k++].revents;
        if (!rev) continue;
        /* POLLHUP often arrives with POLLIN when the peer writes and closes;
         * stream_recv stays the single authority for EOF (recv()==0). */
        if (rev & (POLLIN | POLLERR | POLLHUP))
            hp_arr_push(out, item);
    }
}

/* stream_ready_into($out, $handles, $timeoutMs): like stream_ready, but
 * reuses the caller's array. A server loop that polls every round then
 * allocates nothing at all here. */
hval hpbi_stream_ready_into(hval outv, hval handles, hval timeout_ms) {
    hp_net_init();
    harr *out = (outv.tag == HV_ARR) ? outv.u.a : NULL;
    if (!out) return hp_of_arr(hp_arr_new());
    out->len = 0;
    out->next_index = 0;
    out->is_map = false;
    if (handles.tag != HV_ARR || !handles.u.a) return hp_of_arr(out);
    int to = (int)hp_val_to_int(timeout_ms);
    if (to < 0) to = 0;
    hp_poll_into(out, handles.u.a, to);
    return hp_of_arr(out);
}

/* ---- epoll-based readiness (Linux) ----
 * stream_ready() polls an explicit array, which is O(n) in the number of
 * connections. A server that holds thousands of client sockets needs the
 * kernel to hand back only the ready ones. */
#if !defined(_WIN32)
static _Thread_local int g_epfd = -1;
#endif

/* ---- io_uring backend (Linux) ----------------------------------------------
 * Transparent backend behind the epoll/stream primitives. The ring carries one
 * armed recv per live connection plus the pending response sends, so one
 * io_uring_enter per poll cycle replaces one epoll_wait plus one recv (and one
 * send) syscall per request. Built on raw syscalls so nothing depends on
 * liburing; if the kernel cannot create a ring (no io_uring, or the sandbox
 * forbids it) every primitive falls back to the epoll path below.
 * HPHP_URING=0 disables the backend. */
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/mman.h>
#endif
#if defined(__linux__) && defined(SYS_io_uring_setup)
#define HP_UR_CAN 1
#endif

#ifdef HP_UR_CAN

/* opcode numbers from the IORING_OP_ enum (index from 0): POLL_ADD=6,
 * SEND=26, RECV=27 — verified against /usr/include/liburing/io_uring.h */
#define HP_UR_OP_POLL     6
#define HP_UR_OP_ACCEPT   13
#define HP_UR_OP_CLOSE    19
#define HP_UR_OP_SHUTDOWN 34
#define HP_UR_OP_SEND     26
#define HP_UR_OP_RECV     27
#define HP_UR_OFF_CQ      0x8000000ULL
#define HP_UR_OFF_SQES    0x10000000ULL
#define HP_UR_OFF_PBUF    0x80000000ULL
#define HP_UR_OFF_PBUF_SHIFT 16
#define HP_UR_ENTER_GET   1
#define HP_UR_ENTER_SQ_WAKEUP 2
/* IORING_ENTER_EXT_ARG is bit 3 (verified against io_uring.h + the probe) */
#define HP_UR_ENTER_EXT   8
/* io_uring SQ flags: buffered CQ overflow and pending task work. */
#define HP_UR_SQ_CQ_OVERFLOW (1U << 1)
#define HP_UR_SQ_TASKRUN  (1U << 2)
/* IORING_SETUP_CQSIZE = bit 3, IORING_SETUP_CLAMP = bit 4 (uapi io_uring.h) */
#define HP_UR_SETUP_CQSIZE (1U << 3)
#define HP_UR_SETUP_CLAMP   (1U << 4)
#define HP_UR_SETUP_SQPOLL  (1U << 1)
#define HP_UR_SETUP_SUBMIT_ALL (1U << 7)
/* Cooperative task run: process completions in io_uring_enter instead of
 * task_work on every syscall exit. DEFER_TASKRUN (6.1+) keeps all task work
 * in enter; SINGLE_ISSUER is required by it. */
#define HP_UR_SETUP_COOP_TASKRUN  (1U << 8)
#define HP_UR_SETUP_TASKRUN_FLAG  (1U << 9)
#define HP_UR_SETUP_SINGLE_ISSUER (1U << 12)
#define HP_UR_SETUP_DEFER_TASKRUN (1U << 13)
#define HP_UR_SQ_NEED_WAKEUP (1U << 0)
#define HP_UR_KIND_RECV   1
#define HP_UR_KIND_SEND   2
#define HP_UR_KIND_POLL   3
#define HP_UR_KIND_ACCEPT 4
#define HP_UR_KIND_CLOSE  5
#define HP_UR_KIND_READ   6
#define HP_UR_KIND_WAKE   7
#define HP_UR_KIND_CANCEL 8
#define HP_UR_WAKE_UD(r)  (((uint64_t)(r) << 8) | HP_UR_KIND_WAKE)
#define HP_UR_OP_POLL_ADD 6
#define HP_UR_POLL_ADD_MULTI 1
#define HP_UR_POLLRDHUP 0x2000U
/* Bound app dispatch per poll round by default. High-concurrency experiments
 * may raise the budget so a backlog can drain without repeated loop turns. */
#ifndef HP_UR_READ_ANSWER_MAX
#define HP_UR_READ_ANSWER_MAX 2048U
#endif
#define HP_UR_KIND        0xff
#define HP_UR_OP_ASYNC_CANCEL 14
#define HP_UR_ASYNC_CANCEL_USERDATA (1U << 4)
#define HP_UR_ASYNC_CANCEL_ALL (1U << 0)
#define HP_UR_REGISTER_FILES        2
#define HP_UR_REGISTER_FILES_UPDATE 6
#define HP_UR_REGISTER_FILES2       13   /* sparse table registration */
#define HP_UR_RSRC_REGISTER_SPARSE  (1U << 0)
#define HP_UR_REGISTER_EVENTFD      4
#define HP_UR_REGISTER_EVENTFD_ASYNC 7
#define HP_UR_REGISTER_PBUF_RING    22
/* Match the Rust leader: a small SQ is enough because submitted multishot
 * receives remain in the kernel; a deep CQ absorbs completion bursts. */
#define HP_UR_SQ_ENTRIES  4096
#define HP_UR_CQ_ENTRIES  65536
/* Per-connection recv target. Requests are a few hundred bytes (headers plus
 * at most a 500-character body); 1 KB holds them, and at 60k connections the
 * buffers total 60 MB instead of 240 MB -- less memory and a smaller cache
 * working set. Larger requests arrive in chunks and the app concatenates
 * partial buffers, so nothing is lost. */
#define HP_UR_RLEN_DEF    1024
/* Bound the multi-ring completion wait to one millisecond by default.
 * Keep both values tunable so A/B runs can compare policies on one binary. */
#define HP_UR_BATCH       64
#define HP_UR_BATCH_US    1000

/* io_uring ABI bits used by the direct-descriptor path. */
#define HP_UR_IOSQE_FIXED_FILE    (1U << 0)
#define HP_UR_IOSQE_IO_HARDLINK  (1U << 4)
#define HP_UR_IOSQE_BUFFER_SELECT (1U << 5)
#define HP_UR_IOSQE_CQE_SKIP_SUCCESS (1U << 6)
#define HP_UR_ACCEPT_MULTISHOT    (1U << 0)
#define HP_UR_ACCEPT_DEPTH        512U
#define HP_UR_ACCEPT_DEPTH_MAX    (HP_UR_SQ_ENTRIES / 2U)
#define HP_UR_RECV_MULTISHOT      (1U << 1)
#define HP_UR_RECVSEND_POLL_FIRST (1U << 0)
#define HP_UR_CQE_F_BUFFER         (1U << 0)
#define HP_UR_CQE_F_MORE           (1U << 1)
#define HP_UR_CQE_BUFFER_SHIFT     16
#define HP_UR_FILE_INDEX_ALLOC     (~0U)
#define HP_UR_IOU_PBUF_RING_MMAP   (1U)
/* Direct descriptor handles live above every real fd (LimitNOFILE=65535 on
 * the box), so hp_ur_st_of() can tell the two apart without a second lookup
 * and the app-facing handle stays small enough for an int array. */
#define HP_UR_HANDLE_BASE          0x10000U
/* The kernel rejects a file-table registration larger than RLIMIT_NOFILE
 * (the box sets 65535), so that is the default and the runtime clamps any
 * HPHP_DIRECT_FILES override to the same limit. */
#define HP_UR_DIRECT_FILES_DEF     65535U
/* Provided-buffer ring: keep the in-flight receive window bounded per ring.
 * 4k buffers are replenished as requests are consumed; HPHP_PBUF_N can tune
 * the count for deployments with a tighter memory budget. */
#define HP_UR_PBUF_N_DEF           4096

/* Recv buffer size per connection, overridable at run time (HPHP_RECV_LEN)
 * so the size can be A/B-tested on one binary. */
static _Thread_local int hp_ur_rlen_cfg = 0;
static size_t hp_ur_rlen(void) {
    if (!hp_ur_rlen_cfg) {
        const char *e = getenv("HPHP_RECV_LEN");
        int v = (e && *e) ? atoi(e) : HP_UR_RLEN_DEF;
        if (v < 64) v = 64;
        if (v > 65536) v = 65536;
        hp_ur_rlen_cfg = v;
    }
    return (size_t)hp_ur_rlen_cfg;
}

/* raw ABI, from include/uapi/linux/io_uring.h */
struct hp_uring_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t ioprio;
    int32_t  fd;
    union { uint64_t off; uint64_t addr2; };
    union { uint64_t addr; };
    uint32_t len;
    union {
        uint32_t rw_flags;
        uint32_t fsync_flags;
        uint16_t poll_events;
        uint32_t poll32_events;
        uint32_t splice_flags;
        uint32_t accept_flags;
        uint32_t cancel_flags;
    };
    uint64_t user_data;
    union { uint16_t buf_index; uint16_t buf_group; };
    uint16_t personality;
    union { uint32_t file_index; uint16_t addr_len; };
    uint64_t pad[2];
};
struct hp_uring_cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
};
typedef char hp_uring_sqe_size_check[
    (sizeof(struct hp_uring_sqe) == 64) ? 1 : -1];

/* Provided-buffer ring (uapi structs io_uring_buf / io_uring_buf_ring /
 * io_uring_buf_reg) and the register-FILES_UPDATE argument. */
struct hp_uring_buf {
    uint64_t addr;
    uint32_t len;
    uint16_t bid;
    uint16_t resv;
};
/* The ring is a header (resv/tail) followed by io_uring_buf entries. The
 * entries are addressed through the ring base; C11 rejects a flexible array
 * member inside a union, so the header is the only declared member. */
struct hp_uring_buf_ring {
    union {
        struct { uint64_t resv1; uint32_t resv2; uint16_t resv3; uint16_t tail; };
        uint8_t raw[16];
    };
};
struct hp_uring_buf_reg {
    uint64_t ring_addr;
    uint32_t ring_entries;
    uint16_t bgid;
    uint16_t flags;
    uint64_t resv[3];
};
struct hp_uring_files_update {
    uint32_t offset;
    uint32_t resv;
    uint64_t fds;
};
struct hp_uring_rsrc_register {
    uint32_t nr;
    uint32_t flags;
    uint64_t resv2;
    uint64_t data;
    uint64_t tags;
};
struct hp_uring_sq_ring {                 /* io_sqring_offsets */
    uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
    uint64_t user_addr;
};
struct hp_uring_cq_ring {                 /* io_cqring_offsets */
    uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
    uint64_t user_addr;
};
struct hp_uring_setup_params {
    uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
             features, wq_fd;
    uint32_t resv[3];
    struct hp_uring_sq_ring sq_off;
    struct hp_uring_cq_ring cq_off;
};

static int hp_diag_enabled(void);
static uint64_t hp_diag_now_ns(void);

struct hp_urchunk {
    struct hp_urchunk *next;
    char   *data;
    size_t  off, len, cap;
};

/* Keep the ordinary response chunk unchanged; append its queue timestamp only
 * in diagnostic processes, where HPHP_DIAG is fixed for the process lifetime. */
static uint64_t *hp_ur_chunk_diag_ns(struct hp_urchunk *c) {
    if (!hp_diag_enabled()) return NULL;
    return (uint64_t *)((char *)c + sizeof *c);
}

/* Recycle only the capacity of completed response buffers. The bytes are
 * overwritten before the next response is sent; no response content is
 * retained or reused. A small thread-local pool matches the single-threaded
 * event loop and bounds retained memory. */
/* A response is one chunk: the struct itself is the same size for every
 * request, so a tiny per-thread free list keeps the malloc/free churn off
 * the hot path (the payload buffer still has its own lifetime -- it is the
 * builder buffer, handed over by stream_send_sb, or a fresh copy). */
static _Thread_local struct hp_urchunk *g_chunk_spare = NULL;
static _Thread_local int g_chunk_spare_n = 0;
static struct hp_urchunk *hp_ur_chunk_alloc(void) {
    struct hp_urchunk *c = g_chunk_spare;
    if (c) {
        g_chunk_spare = c->next;
        g_chunk_spare_n--;
        return c;
    }
    return (struct hp_urchunk *)malloc(sizeof *c +
        (hp_diag_enabled() ? sizeof(uint64_t) : 0));
}
static void hp_ur_chunk_release(struct hp_urchunk *c) {
    hp_resp_buf_release(c->data, c->cap);
    if (g_chunk_spare_n < 4096) {
        c->next = g_chunk_spare;
        g_chunk_spare = c;
        g_chunk_spare_n++;
    } else {
        free(c);
    }
}

/* Per-connection (per-fd) state: one recv buffer (recycled between
 * completions) and at most one in-flight send chunk per fd; everything is
 * freed on close, and the epoch stamped into user_data makes completions
 * that arrive after a close plus fd reuse drop. */
struct hp_urfds {
    struct hp_urchunk *shead, *stail;     /* queued send chain */
    hstr *rh;                             /* recv target as a live hstr:
                                              the kernel writes data[], the
                                              app gets it without a copy */
    size_t rcap;
    size_t rlen;                          /* fresh bytes available */
    int64_t cdeadline;                    /* idle/close deadline */
    uint32_t epoch;
    uint32_t rborrow_bid;
    uint16_t diridx;                      /* slot in the registered file table */
    unsigned int squeued:7;               /* queue is capped at 64 chunks */
    unsigned int rarmed:1;                /* a recv SQE is queued or in flight */
    unsigned int rready:1;                /* rbuf holds unconsumed data */
    unsigned int reof:1;
    unsigned int listener:1;
    unsigned int pollarmed:1;
    unsigned int sarmed:1;                /* a send SQE is queued or in flight */
    unsigned int rtagged:1;               /* parked in the retry ring */
    unsigned int closeafter:1;            /* close deferred until sends drain */
    unsigned int ctagged:1;               /* listed for the deadline check */
    unsigned int epolled:1;               /* readiness via epoll, no recv SQE */
    unsigned int ringconn:1;              /* this connection holds a ring slot */
    unsigned int direct:1;                /* direct descriptor (direct fds) */
    unsigned int slot_live:1;             /* counted against its ring's accept table */
    unsigned int rmshot:1;                /* recv SQE is multishot */
    unsigned int rreadq:1;                /* queued for a bounded single-shot drain */
    unsigned int rreading:1;              /* bounded single-shot recv is in flight */
    unsigned int ranswerq:1;              /* ready, but deferred by the answer cap */
    unsigned int rborrowed:1;             /* rh is an in-place view of a provided buffer */
    unsigned int rdispatch:1;              /* already added to this poll's app-ready list */
    unsigned int rdispatch_valid:1;        /* distinguishes a fresh connection from pass bit 0 */
    unsigned int rpoll:1;                  /* direct socket uses poll + one-shot receive */
};
_Static_assert(sizeof(struct hp_urfds) <= 64,
               "per-connection runtime state must stay compact");

/* Overflow readiness: the kernel caps the SQ ring at 32768 entries, so above
 * ~28k connections one armed recv per connection no longer fits. Those
 * connections register in a plain epoll set instead (the ring still carries
 * the sends), which lets the server hold up to the fd limit. */
#define HP_UR_RING_MAX_DEFAULT 28000
static _Thread_local int hp_ur_epfd = -1;
static _Thread_local int hp_ur_ep_conns = 0;
static _Thread_local int hp_ur_ring_conns = 0;
static _Thread_local int hp_ur_ring_max = -1;
static _Thread_local int hp_ur_busy = 0;   /* the last wakeup had completions */
/* Optional fair CQ drain start for same-binary scheduler A/B checks. */
static _Thread_local int hp_ur_drain_rr;
static _Thread_local unsigned int hp_ur_drain_start;
/* Batching is tunable at runtime so the box can decide its own trade. The
 * 2,048 answer ceiling matches the leader's bounded per-turn response batch:
 * it lets a busy loop retire a useful burst without unbounded monopolization.
 * HPHP_BATCH_N completions waited for, HPHP_BATCH_US waiting ceiling,
 * HPHP_ANSWER_BUDGET requests handled per event-loop turn.
 * N=1 / US=0 restores the one-completion-per-wait behavior. */
static _Thread_local long hp_ur_batch = HP_UR_BATCH;
static _Thread_local long hp_ur_batch_us = HP_UR_BATCH_US;
static _Thread_local uint32_t hp_ur_answer_budget_limit = HP_UR_READ_ANSWER_MAX;
static _Thread_local unsigned int hp_ur_ready_bit;
/* Dedupe can reduce redundant ready handles, but at high VU it also delayed
 * useful reads in the same-binary A/B. Keep it opt-in until a full pass shows
 * better completed throughput and tail latency together. */
static _Thread_local int hp_ur_ready_dedupe = 0;
static _Thread_local int hp_ur_defer_all = 0;
static _Thread_local int hp_ur_force_enter = 0;
static _Thread_local int hp_ur_epoll_batch_us = 0;
static _Thread_local uint32_t hp_ur_cq_budget = 0;
static _Thread_local int hp_ur_ring0_wait = 1;
/* Mirror the leader's event-loop policy: ring 0 only performs its bounded
 * wait while both ready/recovery work and CQ completions are absent. */
static _Thread_local int hp_ur_ring0_busy_aware = 1;
/* When the SQ is full, Rust's push helper publishes SQEs without GETEVENTS;
 * normal batch entry handles deferred task work later. Keep the old blocking
 * flush available for same-binary A/B checks. */
static _Thread_local int hp_ur_sq_flush_getevents = 1;
/* The bounded one-shot accept queue mirrors the Rust leader. An explicit 0
 * keeps the earlier multishot listener available for A/B comparison. */
static _Thread_local uint32_t hp_ur_accept_depth = HP_UR_ACCEPT_DEPTH;
/* Optional Rust-style one-accept-lane handover. Only one ring accepts at a
 * time; it yields while its table still has room for all in-flight accepts. */
static _Thread_local int hp_ur_accept_handover = 0;
static _Thread_local int hp_ur_accept_parallel = 0;
static _Thread_local int hp_ur_accept_lane = -1;
static _Thread_local int hp_ur_accept_last_lane = -1;
static _Thread_local int hp_ur_accept_diag = 0;
static _Thread_local int hp_ur_taskrun = 1;
/* Optional kernel-side SQ polling experiment. Keep disabled by default: its
 * kernel thread can cost more CPU than it saves on a one-vCPU host. */
static _Thread_local int hp_ur_sqpoll = 0;
static _Thread_local int hp_ur_recv_poll = -1;

static _Thread_local int hp_ur_st = 0;    /* 0 untried, 1 active, -1 unusable */
static _Thread_local struct hp_urfds *hp_ur_fds;
static _Thread_local uint32_t hp_ur_fdscap;
static _Thread_local int hp_ur_pushfail;    /* retry-ring overflow: run scan */
static _Thread_local int hp_ur_dbg;         /* HPHP_URING_DEBUG=1 */
static _Thread_local uint32_t *hp_ur_retry; /* fds waiting for a re-arm slot */
static _Thread_local uint32_t hp_ur_retcap, hp_ur_rethead, hp_ur_rettail;
struct hp_ur_readq_ent { uint32_t fd, epoch; };
static _Thread_local struct hp_ur_readq_ent *hp_ur_readq;
static _Thread_local uint32_t hp_ur_readq_cap, hp_ur_readq_head, hp_ur_readq_tail;
static _Thread_local struct hp_ur_readq_ent *hp_ur_answerq;
static _Thread_local uint32_t hp_ur_answerq_cap, hp_ur_answerq_head, hp_ur_answerq_tail;
static _Thread_local int hp_ur_circular_queues;
static _Thread_local uint32_t hp_ur_readq_max, hp_ur_answerq_max;
static _Thread_local uint64_t hp_ur_readq_moved, hp_ur_answerq_moved;
static _Thread_local uint32_t hp_ur_readq_total;   /* entries across all rings' recovery FIFOs */
static _Thread_local uint32_t hp_ur_answer_budget;
static _Thread_local int hp_ur_recovery_mode;
static _Thread_local int hp_ur_readq_debugged;
static _Thread_local int hp_ur_readq_diag;
static _Thread_local uint32_t *hp_ur_closing; /* fds waiting for a send drain */
static _Thread_local uint32_t hp_ur_closecap, hp_ur_closecnt;

/* One io_uring instance. A single connection is tied to the ring that owns
 * its direct descriptor, so every low-level op runs against hp_ur_cur. The
 * macros below keep the existing call sites unchanged while the actual
 * state lives per ring. */
struct hp_uring_ring {
    int fd;
    int efd;              /* CQE eventfd registered into epoll, -1 = none */
    int defer_taskrun;    /* only deferred rings need an enter to post task_work */
    int sqpoll;           /* kernel SQ thread consumes published SQEs */
    unsigned char *sqes;
    struct hp_uring_cqe *cqes;
    uint32_t *sqarr;
    volatile uint32_t *sq_head, *sq_tail, *cq_head, *cq_tail;
    volatile uint32_t *sq_flags;
    uint32_t sqtail, cqhead, sqmask, cqmask;
    struct hp_urfds *dirs;
    uint32_t dircap;
    struct hp_uring_buf_ring *pbuf;
    char *pbuf_mem;
    size_t pbuf_mapsz;
    uint32_t pbuf_n, pbuf_mask, pbuf_tail, pbuf_blen;
    size_t pbuf_stride;
    uint32_t read_inflight;               /* buffer-select recovery reads in flight */
    /* Receive-recovery FIFO for this ring's connections (power-of-two ring
     * buffer). Kept per ring so a ring without free provided buffers is
     * skipped as a whole instead of rotating every queued entry. */
    struct hp_ur_readq_ent *rq;
    uint32_t rq_cap, rq_head, rq_tail;
    uint32_t accept_armed;                 /* single-shot accepts in flight */
    uint32_t live;                         /* installed direct descriptors */
    uint32_t accept_margin;                /* reserve for accepts already in flight */
    uint16_t accepts_by_listener[16];      /* one-shot accepts on each reuseport fd */
    int accept_resume;                     /* table reopened after the close hysteresis */
    int accepting;                          /* Rust ring state: keep its accept queue filled */
    int accept_full;      /* this ring's direct table is full */
    int wake_ready;       /* ring 0 observed this ring's registered eventfd */
    /* Opt-in per-ring timings. recv timestamps are allocated only for a
     * diagnostic run; normal service connections retain the same footprint. */
    uint64_t *diag_recv_ready_ns;
    uint64_t *diag_answerq_ns;
    uint64_t diag_enters, diag_enter_ns, diag_enter_max_ns;
    uint64_t diag_submit_only, diag_submit_ns;
    uint64_t diag_drain_cqes, diag_drain_ns;
    uint64_t diag_accept_cqes, diag_recv_cqes, diag_send_cqes, diag_poll_cqes;
    uint64_t diag_recv_to_app_ns, diag_recv_to_app_count, diag_recv_to_app_max_ns;
    uint64_t diag_answerq_wait_ns, diag_answerq_wait_count, diag_answerq_wait_max_ns;
    uint64_t diag_send_complete_ns, diag_send_complete_count, diag_send_complete_max_ns;
    /* The accept SQE keeps these alive while the multishot op is armed. */
    struct sockaddr_storage accept_addr;
    socklen_t accept_len;
};
#define HP_UR_RINGS_MAX        8
#define HP_UR_LISTENERS_MAX    16
#define HP_UR_SLOTS_PER_RING   65536U
static _Thread_local struct hp_uring_ring hp_ur_rings[HP_UR_RINGS_MAX];
static _Thread_local int hp_ur_nrings;
static _Thread_local struct hp_uring_ring *hp_ur_cur;   /* set at setup */
static _Thread_local int hp_ur_ring_wake; /* extra ring eventfds polled by ring 0 */

#define hp_ur_fd          (hp_ur_cur->fd)
#define hp_ur_sqes        (hp_ur_cur->sqes)
#define hp_ur_cqes        (hp_ur_cur->cqes)
#define hp_ur_sqarr       (hp_ur_cur->sqarr)
#define hp_ur_sq_head     (hp_ur_cur->sq_head)
#define hp_ur_sq_tail     (hp_ur_cur->sq_tail)
#define hp_ur_cq_head     (hp_ur_cur->cq_head)
#define hp_ur_cq_tail     (hp_ur_cur->cq_tail)
#define hp_ur_sqtail      (hp_ur_cur->sqtail)
#define hp_ur_cqhead      (hp_ur_cur->cqhead)
#define hp_ur_sqmask      (hp_ur_cur->sqmask)
#define hp_ur_cqmask      (hp_ur_cur->cqmask)
#define hp_ur_dirs        (hp_ur_cur->dirs)
#define hp_ur_dircap      (hp_ur_cur->dircap)
#define hp_ur_pbuf        (hp_ur_cur->pbuf)
#define hp_ur_pbuf_mem    (hp_ur_cur->pbuf_mem)
#define hp_ur_pbuf_mapsz  (hp_ur_cur->pbuf_mapsz)
#define hp_ur_pbuf_n      (hp_ur_cur->pbuf_n)
#define hp_ur_pbuf_mask   (hp_ur_cur->pbuf_mask)
#define hp_ur_pbuf_tail   (hp_ur_cur->pbuf_tail)
#define hp_ur_pbuf_blen   (hp_ur_cur->pbuf_blen)
#define hp_ur_pbuf_stride (hp_ur_cur->pbuf_stride)

/* ---- direct descriptors / provided buffers (HPHP_DIRECT=1) ---------------- */
static _Thread_local int hp_ur_direct = -1;   /* -1 untried, 0 off, 1 on */
static _Thread_local uint32_t *hp_ur_acceptq; /* accepted direct handles */
static _Thread_local uint32_t hp_ur_acceptq_cap, hp_ur_acceptq_len, hp_ur_acceptq_pos;
/* Listener state: accept on one ring at a time, spilling when its table fills. */
static _Thread_local hp_sock hp_ur_listen_fd = -1;
static _Thread_local int hp_ur_listen_on = 0;
static _Thread_local hp_sock hp_ur_listen_fds[HP_UR_LISTENERS_MAX];
static _Thread_local uint32_t hp_ur_listen_count;
static _Thread_local uint32_t hp_ur_accept_listener_cursor;

static int hp_ur_listener_find(hp_sock fd) {
    for (int i = 0; i < HP_UR_LISTENERS_MAX; i++)
        if (hp_ur_listen_fds[i] == fd) return i;
    return -1;
}

static bool hp_ur_listener_add(hp_sock fd) {
    if (hp_ur_listener_find(fd) >= 0) return true;
    for (int i = 0; i < HP_UR_LISTENERS_MAX; i++) {
        if (hp_ur_listen_fds[i] != 0) continue;
        hp_ur_listen_fds[i] = fd;
        hp_ur_listen_count++;
        if (hp_ur_listen_fd < 0) hp_ur_listen_fd = fd;
        hp_ur_listen_on = 1;
        return true;
    }
    return false;
}

static hp_sock hp_ur_listener_next(void) {
    for (int i = 0; i < HP_UR_LISTENERS_MAX; i++) {
        uint32_t at = hp_ur_accept_listener_cursor++ % HP_UR_LISTENERS_MAX;
        if (hp_ur_listen_fds[at] > 0) return hp_ur_listen_fds[at];
    }
    return -1;
}

/* hp_st lives in hphp_std.c so the SQLite module works without the transport. */   /* runtime counters, see hphp_rt.h */
static _Thread_local uint64_t hp_ur_readq_enqueues, hp_ur_readq_completions;

static int hp_uring_active(void) { return hp_ur_st == 1; }
static int hp_ur_direct_on(void) { return hp_ur_direct == 1; }

/* Direct multishot receives use a kernel-provided buffer ring. Keep only a
 * bounded pool of app-side aggregation buffers: idle connections do not need
 * a private 2 KB hstr each. The app returns a buffer after synchronous parse;
 * the pool retains capacity, never request contents. */
#define HP_UR_RECV_POOL_MAX 8192U
static _Thread_local hstr *hp_ur_recv_pool;
static _Thread_local uint32_t hp_ur_recv_pool_count;
static _Thread_local int hp_ur_recv_pool_enabled = -1;
static _Thread_local int hp_ur_recv_zerocopy = -1;

static int hp_ur_recv_zerocopy_on(void) {
    if (hp_ur_recv_zerocopy < 0) {
        const char *e = getenv("HPHP_RECV_ZEROCOPY");
        hp_ur_recv_zerocopy = e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
    }
    return hp_ur_recv_zerocopy;
}

static int hp_ur_recvpool_on(void) {
    if (hp_ur_recv_pool_enabled < 0) {
        const char *e = getenv("HPHP_RECV_POOL");
        hp_ur_recv_pool_enabled = !(e && e[0] == '0');
    }
    return hp_ur_recv_pool_enabled;
}

static hstr *hp_ur_recvbuf_take(void) {
    hstr *h;
    if (hp_ur_recv_pool) {
        h = hp_ur_recv_pool;
        hp_ur_recv_pool = *(hstr **)h->data;
        if (hp_ur_recv_pool_count) hp_ur_recv_pool_count--;
    } else {
        size_t cap = hp_ur_rlen();
        h = (hstr *)hp_alloc(sizeof(hstr) + cap + 1);
        h->cap = cap + 1;
    }
    h->len = 0;
    h->hash = 0;
    h->rc = 1;
    h->data[0] = 0;
    return h;
}

static void hp_ur_recvbuf_put(hstr *h) {
    if (!h) return;
    h->len = 0;
    h->hash = 0;
    if (hp_ur_recv_pool_count >= HP_UR_RECV_POOL_MAX) {
        hp_free(h);
        return;
    }
    *(hstr **)h->data = hp_ur_recv_pool;
    hp_ur_recv_pool = h;
    hp_ur_recv_pool_count++;
}

/* Direct handles encode (ring, slot): BASE + ring * 65536 + slot. Real fds
 * occupy ring 0's SQ but never share the direct handle space. */
static int hp_ur_ring_of(hp_sock h) {
    if (hp_ur_direct != 1 || h < (hp_sock)HP_UR_HANDLE_BASE) return -1;
    uint32_t off = (uint32_t)(h - (hp_sock)HP_UR_HANDLE_BASE);
    uint32_t r = off / HP_UR_SLOTS_PER_RING;
    if ((int)r >= hp_ur_nrings) return -1;
    return (int)r;
}
static bool hp_ur_is_direct(hp_sock h) { return hp_ur_ring_of(h) >= 0; }

static struct hp_urfds *hp_ur_st_of(hp_sock fd) {
    int r = hp_ur_ring_of(fd);
    if (r >= 0) {
        uint32_t i = (uint32_t)(fd - (hp_sock)HP_UR_HANDLE_BASE) %
                     HP_UR_SLOTS_PER_RING;
        struct hp_uring_ring *rg = &hp_ur_rings[r];
        if (!rg->dirs || i >= rg->dircap) return NULL;
        if (!rg->dirs[i].direct) return NULL;
        return &rg->dirs[i];
    }
    if (fd < 0 || (uint32_t)fd >= hp_ur_fdscap) return NULL;
    return &hp_ur_fds[fd];
}

/* Select the ring that owns this handle (ring 0 for real fds). */
static void hp_ur_use_ring(hp_sock fd) {
    int r = hp_ur_ring_of(fd);
    hp_ur_cur = (r >= 0) ? &hp_ur_rings[r] : &hp_ur_rings[0];
}

static void hp_ur_diag_recv_ready(hp_sock fd) {
    if (!hp_diag_enabled()) return;
    int r = hp_ur_ring_of(fd);
    if (r < 0) return;
    struct hp_uring_ring *rg = &hp_ur_rings[r];
    uint32_t slot = (uint32_t)(fd - (hp_sock)HP_UR_HANDLE_BASE) %
                    HP_UR_SLOTS_PER_RING;
    if (rg->diag_recv_ready_ns && !rg->diag_recv_ready_ns[slot]) {
        uint64_t now = hp_diag_now_ns();
        rg->diag_recv_ready_ns[slot] = now ? now : 1;
    }
}

static void hp_ur_diag_recv_taken(hp_sock fd) {
    if (!hp_diag_enabled()) return;
    int r = hp_ur_ring_of(fd);
    if (r < 0) return;
    struct hp_uring_ring *rg = &hp_ur_rings[r];
    uint32_t slot = (uint32_t)(fd - (hp_sock)HP_UR_HANDLE_BASE) %
                    HP_UR_SLOTS_PER_RING;
    if (!rg->diag_recv_ready_ns || !rg->diag_recv_ready_ns[slot]) return;
    uint64_t now = hp_diag_now_ns();
    uint64_t t0 = rg->diag_recv_ready_ns[slot];
    uint64_t age = now >= t0 ? now - t0 : 0;
    rg->diag_recv_ready_ns[slot] = 0;
    rg->diag_recv_to_app_ns += age;
    rg->diag_recv_to_app_count++;
    if (age > rg->diag_recv_to_app_max_ns)
        rg->diag_recv_to_app_max_ns = age;
}
/* A direct slot has actually left its registered file table: resume accepts
 * after the same hysteresis used when a ring fills. */
static void hp_ur_slot_maybe_resume(struct hp_uring_ring *rg) {
    /* Hysteresis matches Rust: pause near the end, resume only after a
     * substantial number of slots are free again. */
    if (rg->accept_full && rg->live + 2 * rg->accept_margin < rg->dircap) {
        rg->accept_full = 0;
        rg->accept_resume = 1;
    }
}

/* Synchronous removal (the SQ-full fallback) still has the live slot state. */
static void hp_ur_slot_freed(hp_sock fd) {
    int r = hp_ur_ring_of(fd);
    if (r < 0) return;
    struct hp_uring_ring *rg = &hp_ur_rings[r];
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (st && st->slot_live) {
        st->slot_live = 0;
        if (rg->live) rg->live--;
    }
    hp_ur_slot_maybe_resume(rg);
}

/* Async close CQEs may arrive after this connection state has been detached,
 * or even after the same direct slot has been reused. Account only the old
 * close here; never touch the current slot's generation/state. */
static void hp_ur_slot_close_done(hp_sock fd) {
    int r = hp_ur_ring_of(fd);
    if (r < 0) return;
    struct hp_uring_ring *rg = &hp_ur_rings[r];
    if (rg->live) rg->live--;
    hp_ur_slot_maybe_resume(rg);
}

/* An enter is only needed when the ring has SQEs to submit, deferred task
 * work to flush, or completions waiting. Idle rings cost no syscall. */
static int hp_ur_ring_needs_enter(struct hp_uring_ring *rg) {
    if (hp_ur_force_enter && (rg->live || rg->accept_armed)) return 1;
    uint32_t flags = rg->sq_flags ?
        __atomic_load_n(rg->sq_flags, __ATOMIC_ACQUIRE) : 0;
    /* Both flags require GETEVENTS even without new SQEs, including the
     * COOP_TASKRUN fallback and an already-awake SQPOLL ring. */
    if (flags & (HP_UR_SQ_CQ_OVERFLOW | HP_UR_SQ_TASKRUN)) return 1;
    if (rg->sqpoll && rg->sq_flags &&
        !(flags & HP_UR_SQ_NEED_WAKEUP))
        return 0;
    if (rg->sqtail != __atomic_load_n(rg->sq_head, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

/* Grow the deferred-close and retry queues. They are simple FIFOs, so their
 * capacity tracks the number of pending entries, not the handle range. */
static bool hp_ur_trackgrow(uint32_t want) {
    if (want <= hp_ur_retcap && want <= hp_ur_closecap) return true;
    uint32_t ncap = hp_ur_retcap ? hp_ur_retcap : 1024;
    while (ncap < want) {
        if (ncap >= (1U << 22)) return false;
        ncap <<= 1;
    }
    if (ncap > hp_ur_retcap) {
        uint32_t *nr = (uint32_t *)malloc((size_t)ncap * sizeof *nr);
        if (!nr) return false;
        uint32_t n = 0;
        for (uint32_t k = hp_ur_rethead; k != hp_ur_rettail;
             k = (k + 1) & (hp_ur_retcap ? (hp_ur_retcap - 1) : 0))
            nr[n++] = hp_ur_retry[k];
        free(hp_ur_retry);
        hp_ur_retry = nr;
        hp_ur_rethead = 0;
        hp_ur_rettail = n;
        hp_ur_retcap = ncap;
    }
    if (ncap > hp_ur_closecap) {
        uint32_t *nc = (uint32_t *)realloc(hp_ur_closing,
                                           (size_t)ncap * sizeof *nc);
        if (!nc) return false;
        hp_ur_closing = nc;
        hp_ur_closecap = ncap;
    }
    return true;
}

static void hp_ur_fdgrow(hp_sock fd) {
    uint32_t want = (uint32_t)fd + 1;
    if (want <= hp_ur_fdscap) return;
    uint32_t ncap = hp_ur_fdscap ? hp_ur_fdscap : 4096;
    while (ncap < want) {
        if (ncap > ((uint32_t)1 << 26)) { hp_ur_st = -1; return; }
        ncap *= 2;
    }
    struct hp_urfds *nf = (struct hp_urfds *)realloc(hp_ur_fds,
                       (size_t)ncap * sizeof *nf);
    if (!nf) { hp_ur_st = -1; return; }
    memset(nf + hp_ur_fdscap, 0, (size_t)(ncap - hp_ur_fdscap) * sizeof *nf);
    hp_ur_fds = nf;
    hp_ur_fdscap = ncap;
    /* keep the retry ring at least as wide as the fd table: one slot per
     * connection so a fully saturated ring still tracks every pending fd */
    if (ncap > hp_ur_retcap) {
        uint32_t *nr = (uint32_t *)malloc((size_t)ncap * sizeof *nr);
        if (!nr) { hp_ur_pushfail++; return; }
        uint32_t n = 0;
        for (uint32_t k = hp_ur_rethead; k != hp_ur_rettail;
             k = (k + 1) & (hp_ur_retcap - 1))
            nr[n++] = hp_ur_retry[k];
        free(hp_ur_retry);
        hp_ur_retry = nr;
        hp_ur_rethead = 0;
        hp_ur_rettail = n;
        hp_ur_retcap = ncap;
    }
    if (ncap > hp_ur_closecap) {
        uint32_t *nc = (uint32_t *)realloc(hp_ur_closing,
                                           (size_t)ncap * sizeof *nc);
        if (nc) { hp_ur_closing = nc; hp_ur_closecap = ncap; }
    }
}

/* An fd whose SQE push hit a full ring parks here and is re-armed after the
 * next CQE drain, when slots have freed up. At the multi-10k-connection
 * scale each idle connection parks one recv SQE while the kernel caps the
 * SQ ring at 32768 entries, so per-cycle full-table scans would otherwise
 * burn measurable CPU; this stays O(pending). */
static void hp_ur_retfd(hp_sock fd) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (st && st->rtagged) { if (hp_ur_dbg) fprintf(stderr, "[ur] park-already fd=%d\n", (int)fd); return; }
    if (st) st->rtagged = 1;
    if (!hp_ur_retcap) hp_ur_trackgrow(4096);
    if (!hp_ur_retcap) { hp_ur_pushfail++; if (hp_ur_dbg) fprintf(stderr, "[ur] park-nocap fd=%d\n", (int)fd); return; }
    uint32_t nt = (hp_ur_rettail + 1) & (hp_ur_retcap - 1);
    if (nt == hp_ur_rethead) {
        if (!hp_ur_trackgrow(hp_ur_retcap + 1)) {
            hp_ur_pushfail++;
            if (hp_ur_dbg) fprintf(stderr, "[ur] park-full fd=%d\n", (int)fd);
            return;
        }
        nt = (hp_ur_rettail + 1) & (hp_ur_retcap - 1);
    }
    hp_ur_retry[hp_ur_rettail] = (uint32_t)fd;
    hp_ur_rettail = nt;
    if (hp_ur_dbg) fprintf(stderr, "[ur] park fd=%d\n", (int)fd);
}

/* A multishot receive that exhausts its provided buffers must not be re-armed
 * with every other connection at once: that recreates the same ENOBUFS burst.
 * Keep each stopped connection once in a FIFO and drain it with bounded,
 * nonblocking buffer-select reads before returning to multishot receive. */
static bool hp_ur_fifo_push(struct hp_ur_readq_ent **q, uint32_t *cap,
                            uint32_t *head, uint32_t *tail,
                            struct hp_ur_readq_ent ent, uint64_t *moved,
                            uint32_t *max_depth) {
    if (!*cap) {
        uint32_t ncap = 1024;
        struct hp_ur_readq_ent *n = (struct hp_ur_readq_ent *)malloc(
            (size_t)ncap * sizeof *n);
        if (!n) return false;
        *q = n;
        *cap = ncap;
        *head = *tail = 0;
    }
    uint32_t count = *tail - *head;
    if (hp_ur_circular_queues) {
        if (count == *cap) {
            if (*cap >= (1U << 22)) return false;
            uint32_t ncap = *cap << 1;
            struct hp_ur_readq_ent *n = (struct hp_ur_readq_ent *)malloc(
                (size_t)ncap * sizeof *n);
            if (!n) return false;
            for (uint32_t i = 0; i < count; i++)
                n[i] = (*q)[(*head + i) & (*cap - 1)];
            free(*q);
            *q = n;
            *cap = ncap;
            *head = 0;
            *tail = count;
        }
        (*q)[*tail & (*cap - 1)] = ent;
        (*tail)++;
    } else {
        if (*tail == *cap) {
            if (*head) {
                count = *tail - *head;
                memmove(*q, *q + *head, (size_t)count * sizeof **q);
                *moved += count;
                *head = 0;
                *tail = count;
            }
            if (*tail == *cap) {
                if (*cap >= (1U << 22)) return false;
                uint32_t ncap = *cap << 1;
                struct hp_ur_readq_ent *n = (struct hp_ur_readq_ent *)realloc(
                    *q, (size_t)ncap * sizeof *n);
                if (!n) return false;
                *q = n;
                *cap = ncap;
            }
        }
        (*q)[(*tail)++] = ent;
    }
    count++;
    if (count > *max_depth) *max_depth = count;
    return true;
}

static bool hp_ur_fifo_pop(struct hp_ur_readq_ent *q, uint32_t cap,
                           uint32_t *head, uint32_t *tail,
                           struct hp_ur_readq_ent *out) {
    if (*head == *tail) {
        if (!hp_ur_circular_queues) *head = *tail = 0;
        return false;
    }
    if (hp_ur_circular_queues)
        *out = q[(*head) & (cap - 1)];
    else
        *out = q[*head];
    (*head)++;
    if (!hp_ur_circular_queues && *head == *tail) *head = *tail = 0;
    return true;
}

static bool hp_ur_rq_push(struct hp_uring_ring *rg, struct hp_ur_readq_ent ent) {
    uint32_t count = rg->rq_tail - rg->rq_head;
    if (count == rg->rq_cap) {
        uint32_t ncap = rg->rq_cap ? rg->rq_cap << 1 : 1024;
        if (ncap > (1U << 22) || ncap < rg->rq_cap) return false;
        struct hp_ur_readq_ent *n = (struct hp_ur_readq_ent *)malloc((size_t)ncap * sizeof *n);
        if (!n) return false;
        for (uint32_t i = 0; i < count; i++)
            n[i] = rg->rq[(rg->rq_head + i) & (rg->rq_cap - 1)];
        free(rg->rq);
        rg->rq = n;
        rg->rq_cap = ncap;
        rg->rq_head = 0;
        rg->rq_tail = count;
    }
    rg->rq[rg->rq_tail & (rg->rq_cap - 1)] = ent;
    rg->rq_tail++;
    hp_ur_readq_total++;
    if (hp_ur_readq_total > hp_ur_readq_max) hp_ur_readq_max = hp_ur_readq_total;
    return true;
}
static bool hp_ur_rq_pop(struct hp_uring_ring *rg, struct hp_ur_readq_ent *out) {
    if (rg->rq_head == rg->rq_tail) return false;
    *out = rg->rq[rg->rq_head & (rg->rq_cap - 1)];
    rg->rq_head++;
    hp_ur_readq_total--;
    return true;
}

static bool hp_ur_readq_push(hp_sock fd, struct hp_urfds *st, int reason) {
    if (!st || st->rreadq) return true;
    hp_ur_recovery_mode = 1;
    int rr = hp_ur_ring_of(fd);
    if (rr < 0 || rr >= hp_ur_nrings) rr = 0;   /* not a direct handle: dropped by the drain */
    if (!hp_ur_rq_push(&hp_ur_rings[rr],
                       (struct hp_ur_readq_ent){ (uint32_t)fd, st->epoch }))
        return false;
    st->rreadq = 1;
    hp_ur_readq_enqueues++;
    if (hp_ur_dbg && !hp_ur_readq_debugged) {
        hp_ur_readq_debugged = 1;
        fprintf(stderr, "[ur] bounded read recovery activated reason=%d armed=%d reading=%d\n",
                reason, st->rarmed, st->rreading);
    }
    return true;
}


static bool hp_ur_answerq_push(hp_sock fd, struct hp_urfds *st) {
    if (!st || st->ranswerq) return true;
    if (!hp_ur_fifo_push(&hp_ur_answerq, &hp_ur_answerq_cap,
                         &hp_ur_answerq_head, &hp_ur_answerq_tail,
                         (struct hp_ur_readq_ent){ (uint32_t)fd, st->epoch },
                         &hp_ur_answerq_moved, &hp_ur_answerq_max))
        return false;
    st->ranswerq = 1;
    if (hp_diag_enabled()) {
        int r = hp_ur_ring_of(fd);
        if (r >= 0 && hp_ur_rings[r].diag_answerq_ns) {
            uint32_t slot = (uint32_t)(fd - (hp_sock)HP_UR_HANDLE_BASE) %
                            HP_UR_SLOTS_PER_RING;
            hp_ur_rings[r].diag_answerq_ns[slot] = hp_diag_now_ns();
        }
    }
    return true;
}

static bool hp_ur_answerq_pop(struct hp_ur_readq_ent *out) {
    return hp_ur_fifo_pop(hp_ur_answerq, hp_ur_answerq_cap,
                          &hp_ur_answerq_head, &hp_ur_answerq_tail, out);
}

/* Bound how many ready sockets reach the application in one loop pass. The
 * same FIFO serves normal receive completions and burst recovery. */
static void hp_ur_recovery_ready(hp_sock fd, struct hp_urfds *st, HpFdList *in) {
    if (!hp_fdlist_has(in) || !st) return;
    /* One PHP read_fd() drains all bytes already buffered for this socket.
     * Multiple CQEs for the same connection in one drain must not consume
     * several answer slots or add duplicate handles to the app's ready list. */
    if (hp_ur_ready_dedupe) {
        if (st->rdispatch_valid && st->rdispatch == hp_ur_ready_bit) return;
        st->rdispatch = hp_ur_ready_bit;
        st->rdispatch_valid = 1;
    }
    if (hp_ur_answer_budget) {
        hp_fdlist_push(in, fd);
        hp_ur_answer_budget--;
    } else if (!hp_ur_answerq_push(fd, st)) {
        /* Preserve readiness if the bounded FIFO cannot grow. */
        hp_fdlist_push(in, fd);
    }
}

static uint32_t hp_ur_dispatch_answerq(HpFdList *in) {
    uint32_t dispatched = 0;
    struct hp_ur_readq_ent ent;
    while (hp_fdlist_has(in) && hp_ur_answer_budget && hp_ur_answerq_pop(&ent)) {
        hp_sock fd = (hp_sock)ent.fd;
        struct hp_urfds *st = hp_ur_st_of(fd);
        if (!st || st->epoch != ent.epoch || !st->ranswerq) continue;
        st->ranswerq = 0;
        if (hp_diag_enabled()) {
            int r = hp_ur_ring_of(fd);
            if (r >= 0 && hp_ur_rings[r].diag_answerq_ns) {
                struct hp_uring_ring *rg = &hp_ur_rings[r];
                uint32_t slot = (uint32_t)(fd - (hp_sock)HP_UR_HANDLE_BASE) %
                                HP_UR_SLOTS_PER_RING;
                uint64_t t0 = rg->diag_answerq_ns[slot];
                uint64_t now = hp_diag_now_ns();
                rg->diag_answerq_ns[slot] = 0;
                if (t0 && now >= t0) {
                    uint64_t age = now - t0;
                    rg->diag_answerq_wait_ns += age;
                    rg->diag_answerq_wait_count++;
                    if (age > rg->diag_answerq_wait_max_ns)
                        rg->diag_answerq_wait_max_ns = age;
                }
            }
        }
        if (!st->rready || st->closeafter) continue;
        if (hp_ur_ready_dedupe) {
            st->rdispatch = hp_ur_ready_bit;
            st->rdispatch_valid = 1;
        }
        hp_fdlist_push(in, fd);
        hp_ur_answer_budget--;
        dispatched++;
    }
    return dispatched;
}

static void hp_ur_drop_send_chain(struct hp_urfds *st) {
    struct hp_urchunk *c = st->shead;
    while (c) { struct hp_urchunk *n = c->next; hp_ur_chunk_release(c); c = n; }
    st->shead = st->stail = NULL;
    st->squeued = 0;
}

static long hp_ur_enter(long to_submit, long min_complete, int wait_us);
static bool hp_ur_sq_push(struct hp_uring_sqe *fill);
static bool hp_ur_arm_send_head(hp_sock fd);
static bool hp_uring_arm_recv(hp_sock fd, size_t want);
static bool hp_uring_arm_accept(hp_sock fd, int ring);
static void hp_ur_arm_all_accepts(void);
static void hp_ur_free_fd(hp_sock fd);
static void hp_ur_ep_del(hp_sock fd);
static void hp_ur_detach(struct hp_urfds *st);
static uint64_t hp_ur_ud(struct hp_urfds *st, hp_sock fd, int kind);
static hp_sock hp_ur_ud_handle(uint64_t ud);

/* Submit one IORING_OP_CLOSE for a registered file slot. The uapi encodes
 * the slot as file_index = idx + 1 and rejects IOSQE_FIXED_FILE on CLOSE. */
/* 1 = asynchronous close queued, 0 = synchronous table removal succeeded,
 * -1 = both close paths failed. */
static int hp_ur_close_slot(uint32_t idx, uint64_t ud) {
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_CLOSE;
    s.fd = 0;
    s.file_index = idx + 1;
    s.user_data = ud;
    if (hp_ur_sq_push(&s)) return 1;
    hp_st[HP_ST_ARM_FAIL]++;
    long pend = (long)(hp_ur_sqtail -
        __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE));
    hp_ur_enter(pend, 0, 0);
    if (hp_ur_sq_push(&s)) return 1;
    /* Last resort: clear the registered slot synchronously. */
    int nfd = -1;
    struct hp_uring_files_update up;
    up.offset = idx;
    up.resv = 0;
    up.fds = (uint64_t)(uintptr_t)&nfd;
    if (syscall(SYS_io_uring_register, (long)hp_ur_fd,
                HP_UR_REGISTER_FILES_UPDATE, (long)&up, 1L) == 1)
        return 0;
    return -1;
}

/* The poll-read mode mirrors the low-kernel-memory idle strategy used by the
 * leading io_uring servers: one multishot poll per idle socket, then a single
 * buffer-selected recv only after readiness. It is opt-in for A/B testing. */
static int hp_ur_poll_read_on(void) {
    if (hp_ur_recv_poll < 0) {
        const char *v = getenv("HPHP_RECV_POLL");
        hp_ur_recv_poll = v && v[0] == '1';
    }
    return hp_ur_recv_poll;
}

static int hp_ur_sq_reserve(int need) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t head = __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE);
        uint32_t pending = hp_ur_sqtail - head;
        if (pending + (uint32_t)need <= hp_ur_sqmask + 1) return 1;
        if (pending == 0 || hp_ur_enter((long)pending, 0, 0) < 0) return 0;
    }
    return 0;
}

/* A multishot poll keeps a fixed-file reference until cancelled. Order its
 * cancellation, socket shutdown, and direct-slot close as one hard-linked
 * chain so closing idle peers cannot leave kernel poll requests behind. */
static int hp_ur_close_poll_slot(hp_sock fd, struct hp_urfds *st) {
    if (!st || !st->direct || !st->rpoll) return -2;
    if (!hp_ur_sq_reserve(3)) return -1;
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_ASYNC_CANCEL;
    s.fd = -1;
    s.addr = hp_ur_ud(st, fd, HP_UR_KIND_POLL);
    s.flags = HP_UR_IOSQE_IO_HARDLINK | HP_UR_IOSQE_CQE_SKIP_SUCCESS;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_CANCEL);
    if (!hp_ur_sq_push(&s)) return -1;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_SHUTDOWN;
    s.fd = st->diridx;
    s.len = SHUT_RDWR;
    s.flags = HP_UR_IOSQE_FIXED_FILE | HP_UR_IOSQE_IO_HARDLINK |
              HP_UR_IOSQE_CQE_SKIP_SUCCESS;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_CANCEL);
    if (!hp_ur_sq_push(&s)) return -1;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_CLOSE;
    s.fd = 0;
    s.file_index = (uint32_t)st->diridx + 1;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_CLOSE);
    if (!hp_ur_sq_push(&s)) return -1;
    return 1;
}

/* The app asked to close an fd while a SEND is still queued in the ring.
 * The bytes have not reached the kernel yet, so closing now would cancel the
 * response (exactly what the shed path does: write_response closes the fd
 * right after stream_send2_nb reports "sent"). Leave the SQE queued -- the
 * next enter submits it with everything else -- and keep the state, and with
 * it the buffers the kernel reads from, alive until the chain drains;
 * hp_uring_wait finishes the close. */
static void hp_ur_finish_close(hp_sock fd) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st) return;
    st->ctagged = 0;
    st->closeafter = 0;
    st->sarmed = 0;                /* nothing may defer the close again */
    if (st->direct) {
        if (!st->listener) {
            int cr = hp_ur_close_poll_slot(fd, st);
            if (cr == -2)
                cr = hp_ur_close_slot((uint32_t)st->diridx,
                                      hp_ur_ud(st, fd, HP_UR_KIND_CLOSE));
            else if (cr < 0)
                cr = hp_ur_close_slot((uint32_t)st->diridx,
                                      hp_ur_ud(st, fd, HP_UR_KIND_CLOSE));
            if (cr == 0) hp_ur_slot_freed(fd);
            else if (cr < 0 && hp_ur_dbg)
                fprintf(stderr, "[ur] direct close failed fd=%d slot=%u errno=%d\n",
                        (int)fd, st->diridx, errno);
        }
        sock_forget((intptr_t)fd);
    } else {
        sock_forget((intptr_t)fd);
        close(fd);                 /* cancels any in-flight op first */
    }
    if (st->epolled) hp_ur_ep_del(fd);
    hp_ur_detach(st);
}

/* Detach a handle from the ring without closing its descriptor. Real fd
 * owners close after epoll_del + stream_close; direct descriptors are closed
 * by hp_ur_finish_close() before this runs. */
static void hp_ur_free_fd(hp_sock fd) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st) return;
    if (!st->listener && (st->sarmed || st->shead)) {
        if (!st->sarmed) hp_ur_arm_send_head(fd);
        /* The send SQE is already queued; the next wait cycle submits it
         * together with everything else. Only the close has to wait. */
        st->closeafter = 1;
        st->cdeadline = (int64_t)time(NULL) + 15;
        if (!st->ctagged) {
            if (hp_ur_closecnt >= hp_ur_closecap)
                hp_ur_trackgrow(hp_ur_closecnt + 1);
            if (hp_ur_closecnt < hp_ur_closecap) {
                st->ctagged = 1;
                hp_ur_closing[hp_ur_closecnt++] = (uint32_t)fd;
                return;
            }
            /* no tracking room: close now (bounded, never leaks) */
            st->closeafter = 0;
        } else {
            return;
        }
    }
    /* A direct listener can be one member of a SO_REUSEPORT group. Cancel
     * every accept for this fd, subtract only its in-flight count, and keep
     * the remaining listener queues accepting. */
    if (st->listener && hp_ur_direct == 1) {
        int li = hp_ur_listener_find(fd);
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            uint32_t pending = li >= 0 ? rg->accepts_by_listener[li] : 0;
            if (!pending) continue;
            hp_ur_cur = &hp_ur_rings[r];
            struct hp_uring_sqe s;
            memset(&s, 0, sizeof s);
            s.opcode = HP_UR_OP_ASYNC_CANCEL;
            s.fd = -1;
            s.addr = hp_ur_ud(st, fd, HP_UR_KIND_ACCEPT);
            s.cancel_flags = HP_UR_ASYNC_CANCEL_USERDATA |
                             HP_UR_ASYNC_CANCEL_ALL;
            hp_ur_sq_push(&s);
            if (hp_ur_accept_depth)
                rg->accept_armed = rg->accept_armed >= pending
                    ? rg->accept_armed - pending : 0;
            else
                rg->accept_armed = 0;
            if (li >= 0) rg->accepts_by_listener[li] = 0;
        }
        hp_ur_cur = &hp_ur_rings[0];
        if (li >= 0) {
            hp_ur_listen_fds[li] = 0;
            if (hp_ur_listen_count) hp_ur_listen_count--;
        }
        hp_ur_listen_fd = -1;
        for (int i = 0; i < HP_UR_LISTENERS_MAX; i++) {
            if (hp_ur_listen_fds[i] > 0) {
                hp_ur_listen_fd = hp_ur_listen_fds[i];
                break;
            }
        }
        hp_ur_listen_on = hp_ur_listen_count != 0;
        if (hp_ur_listen_on) hp_ur_arm_all_accepts();
    }
    /* A direct descriptor is owned by the ring, not by the process fd
     * table: after a deferred close it must be released explicitly. */
    if (st->direct && !st->listener) {
        hp_ur_close_slot((uint32_t)st->diridx,
                         hp_ur_ud(st, fd, HP_UR_KIND_CLOSE));
        sock_forget((intptr_t)fd);
    }
    if (st->epolled) hp_ur_ep_del(fd);
    hp_ur_detach(st);
}

static long hp_ur_enter_impl(long to_submit, long min_complete, int wait_us) {
    hp_st[HP_ST_ENTER]++;
    hp_st[HP_ST_SQE_SUB] += (uint64_t)(to_submit > 0 ? to_submit : 0);
    unsigned enter_flags = HP_UR_ENTER_GET;
    if (hp_ur_cur && hp_ur_cur->sqpoll && hp_ur_cur->sq_flags &&
        (__atomic_load_n(hp_ur_cur->sq_flags, __ATOMIC_ACQUIRE) & HP_UR_SQ_NEED_WAKEUP))
        enter_flags |= HP_UR_ENTER_SQ_WAKEUP;
    if (wait_us < 0) return syscall(SYS_io_uring_enter, (long)hp_ur_fd,
                                    to_submit, min_complete,
                                    enter_flags, 0L, 0L);
    struct timespec ts;
    ts.tv_sec = wait_us / 1000000;
    ts.tv_nsec = (long)(wait_us % 1000000) * 1000L;
    /* IORING_ENTER_EXT_ARG expects a struct io_uring_getevents_arg, not a
     * bare timespec: sigmask, sigmask_sz, pad, ts-pointer. Passing the
     * timespec itself made the kernel reject the call with EINVAL, so the
     * fallback below blocked in an enter with no timeout at all -- a wait
     * that never returned whenever no completion was pending. */
    struct {
        uint64_t sigmask;
        uint32_t sigmask_sz;
        uint32_t pad;
        uint64_t ts;
    } ga;
    memset(&ga, 0, sizeof ga);
    ga.ts = (uint64_t)(uintptr_t)&ts;
    long r = syscall(SYS_io_uring_enter, (long)hp_ur_fd, to_submit,
                     min_complete, enter_flags | HP_UR_ENTER_EXT,
                     (long)&ga, (long)sizeof ga);
    if (r >= 0 || (errno != EINVAL && errno != ENOSYS)) return r;
    /* Old kernel without the ext-arg timeout: poll the ring fd for the
     * timeout, then submit without waiting. Never fall back to an unbounded
     * enter -- that can sleep forever when nothing is armed. */
    if (wait_us > 0) {
        struct pollfd pf;
        pf.fd = hp_ur_fd;
        pf.events = POLLIN;
        pf.revents = 0;
        (void)poll(&pf, 1, (wait_us + 999) / 1000);
    }
    return syscall(SYS_io_uring_enter, (long)hp_ur_fd, to_submit, 0,
                   enter_flags, 0L, 0L);
}

static long hp_ur_enter(long to_submit, long min_complete, int wait_us) {
    bool diag = hp_diag_enabled();
    uint64_t t0 = diag ? hp_diag_now_ns() : 0;
    long rc = hp_ur_enter_impl(to_submit, min_complete, wait_us);
    if (diag && hp_ur_cur) {
        uint64_t t1 = hp_diag_now_ns();
        uint64_t dt = t1 >= t0 ? t1 - t0 : 0;
        hp_ur_cur->diag_enters++;
        hp_ur_cur->diag_enter_ns += dt;
        if (dt > hp_ur_cur->diag_enter_max_ns)
            hp_ur_cur->diag_enter_max_ns = dt;
    }
    return rc;
}

/* Secondary ordinary-taskrun rings only need their queued SQEs published.
 * Their CQEs are posted while ring 0 waits, so GETEVENTS would be a needless
 * syscall on every busy ring each turn. */
static long hp_ur_submit_only(long to_submit) {
    hp_st[HP_ST_ENTER]++;
    hp_st[HP_ST_SQE_SUB] += (uint64_t)(to_submit > 0 ? to_submit : 0);
    bool diag = hp_diag_enabled();
    uint64_t t0 = diag ? hp_diag_now_ns() : 0;
    long rc = 0;
    uint32_t sq_flags = hp_ur_cur && hp_ur_cur->sq_flags ?
        __atomic_load_n(hp_ur_cur->sq_flags, __ATOMIC_ACQUIRE) : 0;
    bool task_work = (sq_flags & (HP_UR_SQ_CQ_OVERFLOW | HP_UR_SQ_TASKRUN)) != 0;
    if (!hp_ur_cur || !hp_ur_cur->sqpoll || !hp_ur_cur->sq_flags ||
        (sq_flags & HP_UR_SQ_NEED_WAKEUP) || task_work) {
        unsigned flags = hp_ur_cur && hp_ur_cur->sqpoll
            ? HP_UR_ENTER_SQ_WAKEUP : 0;
        if (hp_ur_force_enter || task_work) flags |= HP_UR_ENTER_GET;
        rc = syscall(SYS_io_uring_enter, (long)hp_ur_fd, to_submit, 0,
                     flags, 0L, 0L);
    }
    if (diag && hp_ur_cur) {
        uint64_t t1 = hp_diag_now_ns();
        uint64_t dt = t1 >= t0 ? t1 - t0 : 0;
        hp_ur_cur->diag_submit_only++;
        hp_ur_cur->diag_submit_ns += dt;
    }
    return rc;
}

/* Create and map one io_uring instance. */
static bool hp_ur_ring_setup(struct hp_uring_ring *r, int defer_taskrun) {
    struct hp_uring_setup_params p;
    uint32_t sq_entries = HP_UR_SQ_ENTRIES;
    int fd = -1;
    int sqpoll = 0;
    const char *sq_env = getenv("HPHP_SQ_ENTRIES");
    if (sq_env && *sq_env) {
        long v = atol(sq_env);
        if (v >= 1024 && v <= HP_UR_CQ_ENTRIES &&
            (((uint32_t)v & ((uint32_t)v - 1)) == 0))
            sq_entries = (uint32_t)v;
    }
    memset(&p, 0, sizeof p);
    /* Keep the SQ small as in Rust; submitted multishot operations do not
     * occupy SQ slots. The deep CQ absorbs bursts across the direct rings. */
    p.sq_entries = sq_entries;
    p.cq_entries = HP_UR_CQ_ENTRIES;
    uint32_t base = HP_UR_SETUP_CQSIZE | HP_UR_SETUP_CLAMP |
                    HP_UR_SETUP_SUBMIT_ALL;
    /* SQPOLL and DEFER_TASKRUN are incompatible. In this opt-in experiment
     * the kernel thread consumes the shared SQ; ordinary taskrun handles
     * completions. If setup is denied, retain the normal ring setup below. */
    if (hp_ur_sqpoll) {
        p.flags = base | HP_UR_SETUP_SQPOLL | HP_UR_SETUP_SINGLE_ISSUER;
        p.sq_thread_idle = 1000;
        fd = (int)syscall(SYS_io_uring_setup, (unsigned long)p.sq_entries, &p);
        if (fd >= 0) sqpoll = 1;
    }
    /* DEFER_TASKRUN keeps completion work inside io_uring_enter instead of
     * running it as task_work on every syscall exit; off-switchable via
     * HPHP_TASKRUN=0 if the box kernel treats it badly. */
    if (fd < 0 && defer_taskrun && hp_ur_taskrun) {
        memset(&p, 0, sizeof p);
        p.sq_entries = sq_entries;
        p.cq_entries = HP_UR_CQ_ENTRIES;
        p.flags = base | HP_UR_SETUP_COOP_TASKRUN | HP_UR_SETUP_TASKRUN_FLAG |
                  HP_UR_SETUP_SINGLE_ISSUER | HP_UR_SETUP_DEFER_TASKRUN;
        fd = (int)syscall(SYS_io_uring_setup, (unsigned long)p.sq_entries, &p);
    } else if (fd < 0) {
        memset(&p, 0, sizeof p);
        p.sq_entries = sq_entries;
        p.cq_entries = HP_UR_CQ_ENTRIES;
        p.flags = defer_taskrun ? base : base | HP_UR_SETUP_SINGLE_ISSUER;
        fd = (int)syscall(SYS_io_uring_setup, (unsigned long)p.sq_entries, &p);
    }
    if (fd < 0 && defer_taskrun && hp_ur_taskrun) {
        /* older kernel: drop DEFER_TASKRUN/SINGLE_ISSUER, keep COOP_TASKRUN */
        p.flags = base | HP_UR_SETUP_COOP_TASKRUN | HP_UR_SETUP_TASKRUN_FLAG;
        fd = (int)syscall(SYS_io_uring_setup, (unsigned long)p.sq_entries, &p);
    }
    if (fd < 0) {
        p.flags = base;
        fd = (int)syscall(SYS_io_uring_setup, (unsigned long)p.sq_entries, &p);
    }
    if (fd < 0) {
        memset(&p, 0, sizeof p);
        /* A kernel may reject the requested deep CQ. Keep the Rust-sized SQ
         * and use its default CQ instead of allocating an oversized SQ. */
        memset(&p, 0, sizeof p);
        fd = (int)syscall(SYS_io_uring_setup,
                          (unsigned long)sq_entries, &p);
        if (fd < 0) return false;
    }
    r->defer_taskrun = (p.flags & HP_UR_SETUP_DEFER_TASKRUN) != 0;
    r->sqpoll = sqpoll;
    /* A registered eventfd makes CQ readiness pollable even with
     * DEFER_TASKRUN (the same mechanism the top C submission uses). */
    r->efd = -1;
    /* HPHP_RING_EVENTFD=0 skips the registration: the kernel then signals no
     * eventfd on CQ posts (a locked wakeup per flush), and a multi-ring loop
     * relies on ring 0's bounded wait, as the multi-ring Rust loop does. */
    /* HPHP_RING_EVENTFD=async registers with IORING_REGISTER_EVENTFD_ASYNC:
     * completions posted inline while this thread is inside io_uring_enter
     * (most sends) signal nothing, since the loop drains the CQ right after;
     * completions posted later (receives, deferred sends) still wake it. */
    const char *efd_env = getenv("HPHP_RING_EVENTFD");
    if (!(efd_env && strcmp(efd_env, "0") == 0)) {
        long reg_op = (efd_env && strcmp(efd_env, "async") == 0)
            ? HP_UR_REGISTER_EVENTFD_ASYNC : HP_UR_REGISTER_EVENTFD;
        int evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evfd >= 0) {
            if (syscall(SYS_io_uring_register, (long)fd,
                        reg_op, (long)&evfd, 1L) == 0)
                r->efd = evfd;
            else
                close(evfd);
        }
    }
    size_t sq_sz = p.sq_off.array + (size_t)p.sq_entries * sizeof(uint32_t);
    size_t cq_sz = p.cq_off.cqes + (size_t)p.cq_entries * sizeof(struct hp_uring_cqe);
    size_t sqe_sz = (size_t)p.sq_entries * sizeof(struct hp_uring_sqe);
    void *sqring = mmap(0, sq_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, 0);
    void *cqring = mmap(0, cq_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, (off_t)HP_UR_OFF_CQ);
    void *sqes = mmap(0, sqe_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, (off_t)HP_UR_OFF_SQES);
    if (sqring == MAP_FAILED || cqring == MAP_FAILED || sqes == MAP_FAILED) {
        if (sqring != MAP_FAILED) munmap(sqring, sq_sz);
        if (cqring != MAP_FAILED) munmap(cqring, cq_sz);
        if (sqes != MAP_FAILED) munmap(sqes, sqe_sz);
        if (r->efd >= 0) close(r->efd);
        close(fd);
        return false;
    }
    /* ring sizes/masks must come from the kernel-reported BYTE offsets:
     * with IORING_FEAT_SINGLE_MMAP both rings share one mmap and their
     * header fields interleave (sq mask sits at sq_off.ring_mask = +16,
     * NOT at byte 8 like a plain struct overlay would read). Verified
     * against the raw mmap dump on kernel 6.18. */
    r->fd = fd;
    r->sqmask = *(volatile uint32_t *)((char *)sqring + p.sq_off.ring_mask);
    r->cqmask = *(volatile uint32_t *)((char *)cqring + p.cq_off.ring_mask);
    r->sq_head = (volatile uint32_t *)((char *)sqring + p.sq_off.head);
    r->sq_tail = (volatile uint32_t *)((char *)sqring + p.sq_off.tail);
    r->sq_flags = (volatile uint32_t *)((char *)sqring + p.sq_off.flags);
    r->cq_head = (volatile uint32_t *)((char *)cqring + p.cq_off.head);
    r->cq_tail = (volatile uint32_t *)((char *)cqring + p.cq_off.tail);
    r->sqarr = (uint32_t *)((char *)sqring + p.sq_off.array);
    r->sqes = (unsigned char *)sqes;
    r->cqes = (struct hp_uring_cqe *)((char *)cqring + p.cq_off.cqes);
    r->sqtail = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE);
    r->cqhead = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
    return true;
}

static bool hp_uring_setup(void) {
    const char *env = getenv("HPHP_URING");
    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N')) return false;
    hp_ur_dbg = getenv("HPHP_URING_DEBUG") != NULL;
    if ((env = getenv("HPHP_SQPOLL")) && env[0] == '1')
        hp_ur_sqpoll = 1;
    if ((env = getenv("HPHP_READY_DEDUPE")) && env[0] == '1')
        hp_ur_ready_dedupe = 1;
    if ((env = getenv("HPHP_RING0_WAIT")) && env[0] == '0')
        hp_ur_ring0_wait = 0;
    if ((env = getenv("HPHP_RING0_BUSY")) && env[0] == '0')
        hp_ur_ring0_busy_aware = 0;
    if ((env = getenv("HPHP_DRAIN_RR")) && env[0] == '1')
        hp_ur_drain_rr = 1;
    if ((env = getenv("HPHP_ACCEPT_DEPTH")) && *env) {
        long depth = atol(env);
        /* Keep at least half of the SQ available for recv/send/close work;
         * deeper accept pipelines are useful during large connection ramps. */
        if (depth >= 0 && (uint64_t)depth <= HP_UR_ACCEPT_DEPTH_MAX)
            hp_ur_accept_depth = (uint32_t)depth;
    }
    if ((env = getenv("HPHP_ACCEPT_HANDOVER")) && env[0] == '1') {
        hp_ur_accept_handover = 1;
        /* Ring handover requires bounded one-shot accepts; multishot can
         * install more slots after the active lane has yielded. */
        if (!hp_ur_accept_depth) hp_ur_accept_depth = HP_UR_ACCEPT_DEPTH;
        hp_ur_accept_lane = -1;
        hp_ur_accept_last_lane = -1;
    }
    if ((env = getenv("HPHP_ACCEPT_PARALLEL")) && env[0] == '1')
        hp_ur_accept_parallel = 1;
    if ((env = getenv("HPHP_ACCEPT_DIAG")) && env[0] == '1')
        hp_ur_accept_diag = 1;
    if ((env = getenv("HPHP_BATCH_N"))) hp_ur_batch = atol(env);
    if (hp_ur_batch < 1) hp_ur_batch = 1;
    if ((env = getenv("HPHP_BATCH_US"))) hp_ur_batch_us = atol(env);
    if (hp_ur_batch_us < 0) hp_ur_batch_us = 0;
    if ((env = getenv("HPHP_SQ_FLUSH_GETEVENTS")) && env[0] == '0')
        hp_ur_sq_flush_getevents = 0;
    if ((env = getenv("HPHP_ANSWER_BUDGET")) && *env) {
        long v = atol(env);
        if (v >= 1 && v <= 262144) hp_ur_answer_budget_limit = (uint32_t)v;
    }
    /* Circular FIFOs by default: the linear variant compacts with memmove
     * whenever the tail reaches capacity, which under a sustained answer
     * backlog moves the whole queue again and again. HPHP_CIRCULAR_QUEUES=0
     * restores it for comparison. */
    hp_ur_circular_queues = 1;
    if ((env = getenv("HPHP_CIRCULAR_QUEUES")) && *env == '0')
        hp_ur_circular_queues = 0;
    if ((env = getenv("HPHP_TASKRUN")) && (env[0] == '0' || env[0] == 'n' ||
                                           env[0] == 'N'))
        hp_ur_taskrun = 0;
    if ((env = getenv("HPHP_DEFER_ALL")) &&
        (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'))
        hp_ur_defer_all = 1;
    if ((env = getenv("HPHP_FORCE_ENTER")) && env[0] == '1')
        hp_ur_force_enter = 1;
    if ((env = getenv("HPHP_EPOLL_BATCH_US")) && *env) {
        long v = atol(env);
        if (v >= 0 && v <= 10000) hp_ur_epoll_batch_us = (int)v;
    }
    if ((env = getenv("HPHP_CQ_BUDGET")) && *env) {
        long v = atol(env);
        if (v >= 0 && v <= 65536) hp_ur_cq_budget = (uint32_t)v;
    }
    /* Multiple rings multiply the direct-descriptor space: the kernel caps
     * each registration at RLIMIT_NOFILE (65535 on the box), not the sum.
     * HPHP_RINGS is bounded by HP_UR_RINGS_MAX so handle encoding stays
     * inside the app's int tables. */
    int want = 1;
    if ((env = getenv("HPHP_RINGS")) && *env) {
        long v = atol(env);
        if (v >= 1 && v <= HP_UR_RINGS_MAX) want = (int)v;
        else fprintf(stderr,
                     "[ur] HPHP_RINGS=%ld outside supported range 1..%d; using 1 ring\n",
                     v, HP_UR_RINGS_MAX);
    }
    hp_ur_nrings = 0;
    for (int i = 0; i < want; i++) {
        hp_ur_cur = &hp_ur_rings[i];
        int defer_taskrun = hp_ur_taskrun &&
                            (i == 0 || hp_ur_defer_all);
        if (!hp_ur_ring_setup(&hp_ur_rings[i], defer_taskrun)) {
            int err = errno;
            if (i == 0) {
                fprintf(stderr, "[ur] ring 0 setup failed errno=%d (%s)\n",
                        err, strerror(err));
                return false;
            }
            fprintf(stderr,
                    "[ur] ring %d setup failed errno=%d (%s); keeping %d ring(s)\n",
                    i, err, strerror(err), i);
            break;
        }
        hp_ur_nrings = i + 1;
    }
    hp_ur_cur = &hp_ur_rings[0];
    if (hp_ur_dbg)
        fprintf(stderr, "[ur] rings=%d answer-budget=%u defer-all=%d\n",
                hp_ur_nrings, hp_ur_answer_budget_limit, hp_ur_defer_all);
    return true;
}

static char *hp_ur_pbuf_data(uint32_t bid) {
    if (!hp_ur_pbuf_mem || bid >= hp_ur_pbuf_n) return NULL;
    return hp_ur_pbuf_mem + (size_t)bid * hp_ur_pbuf_stride + offsetof(hstr, data);
}

static hstr *hp_ur_pbuf_header(uint32_t bid) {
    if (!hp_ur_pbuf_mem || bid >= hp_ur_pbuf_n) return NULL;
    return (hstr *)(void *)(hp_ur_pbuf_mem + (size_t)bid * hp_ur_pbuf_stride);
}

/* Add one buffer to the registered provided-buffer ring. */
static void hp_ur_buf_add(void *addr, uint32_t len, uint16_t bid) {
    struct hp_uring_buf *b =
        (struct hp_uring_buf *)(void *)hp_ur_pbuf +
        (hp_ur_pbuf_tail & hp_ur_pbuf_mask);
    b->addr = (uint64_t)(uintptr_t)addr;
    b->len = len;
    b->bid = bid;
    b->resv = 0;
    __atomic_store_n(&hp_ur_pbuf->tail, (uint16_t)(hp_ur_pbuf_tail + 1),
                     __ATOMIC_RELEASE);
    hp_ur_pbuf_tail++;
}
static void hp_ur_pbuf_return(uint32_t bid) {
    if (!hp_ur_pbuf || bid >= hp_ur_pbuf_n) return;
    hp_ur_buf_add(hp_ur_pbuf_data(bid),
                  hp_ur_pbuf_blen, (uint16_t)bid);
}

static bool hp_uring_pbuf_setup(void) {
    uint32_t n = HP_UR_PBUF_N_DEF;
    const char *e = getenv("HPHP_PBUF_N");
    if (e && *e) {
        long v = atol(e);
        if (v >= 256 && v <= (1L << 20)) n = (uint32_t)v;
    }
    uint32_t p = 1;
    while (p < n) p <<= 1;
    n = p;
    size_t ring_sz = (size_t)n * sizeof(struct hp_uring_buf);
    ring_sz = (ring_sz + 4095) & ~(size_t)4095;
    void *ring = mmap(0, ring_sz, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (ring == MAP_FAILED) return false;
    size_t blen = hp_ur_rlen();
    size_t stride = (offsetof(hstr, data) + blen + 1 + 15) & ~(size_t)15;
    char *mem = (char *)malloc((size_t)n * stride);
    if (!mem) { munmap(ring, ring_sz); return false; }
    struct hp_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr = (uint64_t)(uintptr_t)ring;
    reg.ring_entries = n;
    reg.bgid = 0;
    long registered = syscall(SYS_io_uring_register, (long)hp_ur_fd,
                              HP_UR_REGISTER_PBUF_RING, (long)&reg, 1L);
    /* Some Ubuntu 6.8 backports invert the reserved-word validation. Try
     * the standard zeroed ABI first; only EINVAL permits this single retry.
     * Correct kernels never reach the compatibility path on valid input. */
    const char *compat = getenv("HPHP_PBUF_COMPAT");
    if (registered < 0 && errno == EINVAL && !(compat && compat[0] == '0')) {
        reg.resv[0] = 1;
        registered = syscall(SYS_io_uring_register, (long)hp_ur_fd,
                             HP_UR_REGISTER_PBUF_RING, (long)&reg, 1L);
        if (registered == 0)
            fprintf(stderr, "[ur] provided-buffer compatibility retry succeeded\n");
    }
    if (registered < 0) {
        fprintf(stderr, "[ur] provided-buffer registration failed: errno=%d\n", errno);
        munmap(ring, ring_sz);
        free(mem);
        return false;
    }
    hp_ur_pbuf = (struct hp_uring_buf_ring *)ring;
    hp_ur_pbuf_mapsz = ring_sz;
    hp_ur_pbuf_mem = mem;
    hp_ur_pbuf_n = n;
    hp_ur_pbuf_mask = n - 1;
    hp_ur_pbuf_tail = 0;
    hp_ur_pbuf_blen = (uint32_t)blen;
    hp_ur_pbuf_stride = stride;
    for (uint32_t i = 0; i < n; i++)
        hp_ur_buf_add(hp_ur_pbuf_data(i), (uint32_t)blen, (uint16_t)i);
    return true;
}

/* HPHP_DIRECT=1: register a sparse file table so accept(ALLOC) can hand out
 * direct descriptors above the RLIMIT_NOFILE ceiling, plus a provided-buffer
 * ring for multishot recv. If the table cannot be registered the proven
 * real-fd path stays; if only the buffer ring fails, direct descriptors stay
 * and recv falls back to one fixed-file recv per connection. */
static bool hp_uring_direct_setup(void) {
    const char *e = getenv("HPHP_DIRECT");
    if (!e || !(e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) {
        hp_ur_direct = 0;
        hp_ur_nrings = 1;        /* unused rings would only idle */
        return false;
    }
    uint32_t n = HP_UR_DIRECT_FILES_DEF;
    if ((e = getenv("HPHP_DIRECT_FILES")) && *e) {
        long v = atol(e);
        if (v >= 1024 && v <= (1L << 20)) n = (uint32_t)v;
    }
    /* io_uring_register(FILES2) refuses a table larger than RLIMIT_NOFILE.
     * Lift the soft limit to the hard one first (generic, no CAP needed). */
    struct rlimit rl = { 0, 0 };
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        if (rl.rlim_cur < rl.rlim_max) {
            struct rlimit up = { rl.rlim_max, rl.rlim_max };
            setrlimit(RLIMIT_NOFILE, &up);
            getrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    if (rl.rlim_cur != 0 &&
        rl.rlim_cur != RLIM_INFINITY && (uint64_t)rl.rlim_cur < n)
        n = (uint32_t)rl.rlim_cur;
    /* A fully sparse table per ring, so ALLOC can fill any slot. This is
     * FILES2 with IORING_RSRC_REGISTER_SPARSE: plain REGISTER_FILES(NULL)
     * is EFAULT. The kernel caps each registration at RLIMIT_NOFILE, but
     * the sum over rings is not capped. */
    e = getenv("HPHP_PBUF");
    int nopbuf = (e && e[0] == '0');
    int okrings = 0;
    for (int ri = 0; ri < hp_ur_nrings; ri++) {
        hp_ur_cur = &hp_ur_rings[ri];
        struct hp_uring_rsrc_register rr;
        memset(&rr, 0, sizeof rr);
        rr.nr = n;
        rr.flags = HP_UR_RSRC_REGISTER_SPARSE;
        if (syscall(SYS_io_uring_register, (long)hp_ur_fd,
                    HP_UR_REGISTER_FILES2, (long)&rr, (long)sizeof rr) < 0) {
            fprintf(stderr,
                    "[ur] direct: ring %d files register failed errno=%d (%s)\n",
                    ri, errno, strerror(errno));
            break;
        }
        hp_ur_dirs = (struct hp_urfds *)calloc(n, sizeof *hp_ur_dirs);
        if (!hp_ur_dirs) break;
        hp_ur_dircap = n;
        if (hp_diag_enabled()) {
            hp_ur_cur->diag_recv_ready_ns =
                (uint64_t *)calloc(n, sizeof(uint64_t));
            if (!hp_ur_cur->diag_recv_ready_ns)
                fprintf(stderr,
                        "[ur] diag: ring %d receive-age timestamps unavailable\n",
                        ri);
            hp_ur_cur->diag_answerq_ns =
                (uint64_t *)calloc(n, sizeof(uint64_t));
            if (!hp_ur_cur->diag_answerq_ns)
                fprintf(stderr,
                        "[ur] diag: ring %d answer-queue timestamps unavailable\n",
                        ri);
        }
        hp_ur_cur->live = 0;
        hp_ur_cur->accept_armed = 0;
        hp_ur_cur->accepting = 0;
        hp_ur_cur->accept_full = 0;
        hp_ur_cur->accept_resume = 0;
        if (hp_ur_accept_handover) {
            /* Match the leader's 1/8 table watermark, capped at 1,024 slots,
             * while preserving room for every queued single-shot accept. */
            hp_ur_cur->accept_margin = n / 8;
            if (hp_ur_cur->accept_margin > 1024)
                hp_ur_cur->accept_margin = 1024;
        } else {
            hp_ur_cur->accept_margin = n / 4;
            if (hp_ur_cur->accept_margin > 512)
                hp_ur_cur->accept_margin = 512;
        }
        uint32_t accept_min = hp_ur_accept_handover
            ? hp_ur_accept_depth + 1 : HP_UR_ACCEPT_DEPTH + 1;
        if (hp_ur_cur->accept_margin < accept_min)
            hp_ur_cur->accept_margin = accept_min;
        /* Provided buffers are the fast path, not a requirement. If the
         * kernel refuses the registration (memlock/accounting), keep direct
         * descriptors and use one fixed-file recv per connection. */
        if (nopbuf) {
            if (hp_ur_dbg)
                fprintf(stderr, "[ur] direct: provided buffers disabled\n");
        } else if (!hp_uring_pbuf_setup()) {
            fprintf(stderr,
                    "[ur] direct: ring %d provided-buffer setup failed, "
                    "using one-shot recv\n", ri);
        }
        okrings = ri + 1;
    }
    hp_ur_cur = &hp_ur_rings[0];
    if (okrings == 0) {
        hp_ur_direct = 0;
        hp_ur_nrings = 1;
        return false;
    }
    hp_ur_nrings = okrings;      /* rings without a table are unusable */
    hp_ur_direct = 1;
    /* Match the multi-ring leader's wake path: each secondary ring signals
     * its registered eventfd, and a multishot POLL_ADD on ring 0 turns that
     * into a ring-0 CQE. One epoll registration then wakes the whole loop. */
    hp_ur_ring_wake = okrings > 1 && hp_ur_rings[0].efd >= 0;
    if (hp_ur_ring_wake) {
        hp_ur_cur = &hp_ur_rings[0];
        for (int ri = 1; ri < okrings; ri++) {
            if (hp_ur_rings[ri].efd < 0) { hp_ur_ring_wake = 0; break; }
            struct hp_uring_sqe s;
            memset(&s, 0, sizeof s);
            s.opcode = HP_UR_OP_POLL_ADD;
            s.fd = hp_ur_rings[ri].efd;
            s.len = HP_UR_POLL_ADD_MULTI;
            s.poll32_events = POLLIN;
            s.user_data = HP_UR_WAKE_UD(ri);
            if (!hp_ur_sq_push(&s)) { hp_ur_ring_wake = 0; break; }
        }
    }
    hp_ur_cur = &hp_ur_rings[0];
    int sqpoll_rings = 0;
    for (int ri = 0; ri < okrings; ri++)
        sqpoll_rings += hp_ur_rings[ri].sqpoll != 0;
    fprintf(stderr,
            "[ur] direct mode: %d rings, %u slots/ring, %u pbufs, "
            "sqpoll=%d/%d, accept_depth=%u, answer_budget=%u, accept_parallel=%s, ready_dedupe=%s, batch=%ld/%ldus\n",
            okrings, n, hp_ur_rings[0].pbuf_n, sqpoll_rings, okrings,
            hp_ur_accept_depth,
            hp_ur_answer_budget_limit,
            hp_ur_accept_parallel ? "yes" : "no",
            hp_ur_ready_dedupe ? "yes" : "no",
            hp_ur_batch, hp_ur_batch_us);
    fprintf(stderr, "[ur] ring0 wake-poll: %s\n",
            hp_ur_ring_wake ? "enabled" : "disabled");
    return true;
}

static int hp_uring_maybe_setup(void) {
    if (hp_ur_st == 0) {
        hp_ur_st = hp_uring_setup() ? 1 : -1;
        if (hp_ur_st == 1 && hp_ur_direct < 0) hp_uring_direct_setup();
    }
    return hp_ur_st == 1;
}

/* Queue one SQE. As in Rust's push helper, flush a full SQ before giving up;
 * only a kernel that still cannot consume it parks the fd for a later retry. */
static bool hp_ur_sq_push(struct hp_uring_sqe *fill) {
    uint32_t tail = hp_ur_sqtail;
    uint32_t head = __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE);
    if (tail - head >= (uint32_t)(hp_ur_sqmask + 1)) {
        for (int tries = 0; tries < 1000 &&
             tail - head >= (uint32_t)(hp_ur_sqmask + 1); tries++) {
            long pending = (long)(tail - head);
            if (pending > 0) {
                long rc = hp_ur_sq_flush_getevents
                    ? hp_ur_enter(pending, 0, -1)
                    : hp_ur_submit_only(pending);
                if (rc < 0 && errno != EINTR && errno != EBUSY &&
                    errno != EAGAIN)
                    (void)hp_ur_enter(0, 0, -1);
            }
            tail = hp_ur_sqtail;
            head = __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE);
        }
    }
    if (tail - head >= (uint32_t)(hp_ur_sqmask + 1)) {
        hp_st[HP_ST_SQE_PARK]++;
        return false;   /* no slot: the fd is parked in the retry ring */
    }
    struct hp_uring_sqe *slot = (struct hp_uring_sqe *)
        (hp_ur_sqes + (size_t)(tail & hp_ur_sqmask) * sizeof(struct hp_uring_sqe));
    *slot = *fill;
    hp_ur_sqarr[tail & hp_ur_sqmask] = tail & hp_ur_sqmask;
    __atomic_store_n(hp_ur_sq_tail, tail + 1, __ATOMIC_RELEASE);
    hp_ur_sqtail = tail + 1;
    hp_st[HP_ST_SQE_PUSH]++;
    return true;
}

static uint64_t hp_ur_ud(struct hp_urfds *st, hp_sock fd, int kind) {
    return ((uint64_t)st->epoch << 40) |
           (((uint64_t)fd & 0xffffffffULL) << 8) | (uint64_t)kind;
}
static hp_sock hp_ur_ud_handle(uint64_t ud) {
    return (hp_sock)((ud >> 8) & 0xffffffffULL);
}

/* ---- overflow readiness (epoll) -------------------------------------------
 * If the ring cannot arm another multishot receive after flushing its SQ,
 * that connection uses ordinary epoll readiness until a ring slot frees. */
static int hp_ur_ep_setup(void) {
    if (hp_ur_epfd >= 0) return hp_ur_epfd;
    int efd = epoll_create1(0);
    if (efd < 0) return -1;
    int nr = hp_ur_ring_wake ? 1 : hp_ur_nrings;
    for (int r = 0; r < nr; r++) {
        int ifd = hp_ur_rings[r].efd >= 0 ? hp_ur_rings[r].efd
                                          : hp_ur_rings[r].fd;
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        ev.data.fd = ifd;                 /* completions pending */
        if (epoll_ctl(efd, EPOLL_CTL_ADD, ifd, &ev) != 0) {
            close(efd);
            return -1;
        }
    }
    hp_ur_epfd = efd;
    return efd;
}

static bool hp_ur_ep_add(hp_sock fd) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st) return false;
    if (hp_ur_ep_setup() < 0) return false;
    if (!st->rh) {
        hstr *h = (hstr *)hp_alloc(sizeof(hstr) + hp_ur_rlen() + 1);
        if (!h) return false;
        h->len = 0;
        h->cap = hp_ur_rlen() + 1;
        h->hash = 0;
        h->rc = 1;
        h->data[0] = 0;
        st->rh = h;
        st->rcap = hp_ur_rlen();
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = (int)fd;
    if (epoll_ctl(hp_ur_epfd, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
    st->epolled = 1;
    hp_ur_ep_conns++;
    if (hp_ur_dbg)
        fprintf(stderr, "[ur] epoll-add fd=%d ep_conns=%d ring_conns=%d\n",
                (int)fd, hp_ur_ep_conns, hp_ur_ring_conns);
    return true;
}

static void hp_ur_ep_del(hp_sock fd) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || !st->epolled) return;
    epoll_ctl(hp_ur_epfd, EPOLL_CTL_DEL, fd, NULL);
    st->epolled = 0;
    if (hp_ur_ep_conns > 0) hp_ur_ep_conns--;
}

/* Track (never double-track) one recv per fd. The kernel fills this
 * connection's own hstr block (allocated once, flag scoped so the app's
 * persist/concopy paths copy and free skips it); on a data completion the
 * exact same hstr is handed out, so a request costs zero recv copies and
 * zero recv string allocations. */
static bool hp_uring_arm_recv(hp_sock fd, size_t want) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || st->rarmed || st->reof || st->listener) return false;
    /* overflow connections are watched by epoll, not by an armed SQE */
    if (st->epolled) return true;
    if (st->direct) {
        if (hp_ur_poll_read_on() && hp_ur_pbuf) {
            struct hp_uring_sqe s;
            memset(&s, 0, sizeof s);
            s.opcode = HP_UR_OP_POLL_ADD;
            s.flags = HP_UR_IOSQE_FIXED_FILE;
            s.fd = st->diridx;
            s.len = HP_UR_POLL_ADD_MULTI;
            s.poll32_events = POLLIN | HP_UR_POLLRDHUP;
            s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_POLL);
            hp_st[HP_ST_ARM_POLL]++;
            if (!hp_ur_sq_push(&s)) {
                hp_st[HP_ST_ARM_FAIL]++;
                hp_ur_retfd(fd);
                return false;
            }
            st->rpoll = 1;
            st->rmshot = 1;
            st->rarmed = 1;
            return true;
        }
        /* Preferred: multishot recv + provided buffers. One SQE stays in
         * flight for the life of the connection, so holding a very large
         * number of connections never needs that many re-arms. Data is
         * copied into rh and the buffer id is returned in the CQE drain. */
        /* Provided-buffer multishot receives do not write into rh; acquire an
         * aggregation buffer only when a CQE actually carries request bytes. */
        if (!st->rh && (!hp_ur_pbuf || !hp_ur_recvpool_on())) {
            size_t want2 = hp_ur_rlen();
            hstr *h = (hstr *)hp_alloc(sizeof(hstr) + want2 + 1);
            if (!h) return false;
            h->len = 0;
            h->cap = want2 + 1;
            h->hash = 0;
            h->rc = 1;
            h->data[0] = 0;
            st->rh = h;
            st->rcap = want2;
        }
        struct hp_uring_sqe s;
        memset(&s, 0, sizeof s);
        s.opcode = HP_UR_OP_RECV;
        s.flags = HP_UR_IOSQE_FIXED_FILE;
        s.fd = st->diridx;
        s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_RECV);
        if (hp_ur_pbuf) {
            s.flags |= HP_UR_IOSQE_BUFFER_SELECT;
            s.ioprio = HP_UR_RECV_MULTISHOT;
            s.buf_group = 0;
            st->rmshot = 1;
        } else {
            /* no provided-buffer ring: one fixed-file recv into rh */
            if (want > st->rcap) {
                hstr *nb = (hstr *)realloc(st->rh, sizeof(hstr) + want + 1);
                if (!nb) return false;
                nb->cap = want + 1;
                st->rh = nb;
                st->rcap = want;
            }
            s.addr = (uint64_t)(uintptr_t)st->rh->data;
            s.len = (uint32_t)st->rcap;
            st->rmshot = 0;
        }
        hp_st[HP_ST_ARM_RECV]++;
        if (!hp_ur_sq_push(&s)) {
            hp_st[HP_ST_ARM_FAIL]++;
            hp_ur_retfd(fd);
            return false;
        }
        st->rarmed = 1;
        return true;
    }
    if (!st->rh) {
        if (want < hp_ur_rlen()) want = hp_ur_rlen();
        /* hstr layout with inline data; one extra byte so data[want] can
         * hold the NUL after a full 4096-byte read */
        hstr *h = (hstr *)hp_alloc(sizeof(hstr) + want + 1);
        if (!h) return false;
        h->len = 0;
        h->cap = want + 1;
        h->hash = 0;
        h->rc = 1;
        h->data[0] = 0;
        st->rh = h;
        st->rcap = want;
    }
    if (want > st->rcap) {
        hstr *nb = (hstr *)realloc(st->rh, sizeof(hstr) + want + 1);
        if (!nb) return false;
        nb->cap = want + 1;
        st->rh = nb;
        st->rcap = want;
    }
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_RECV;
    hp_st[HP_ST_ARM_RECV]++;
    s.fd = (int)fd;
    s.addr = (uint64_t)(uintptr_t)st->rh->data;
    s.len = (uint32_t)st->rcap;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_RECV);
    if (!hp_ur_sq_push(&s)) { hp_st[HP_ST_ARM_FAIL]++; hp_ur_retfd(fd); return false; }
    st->rarmed = 1;
    return true;
}

/* Drain one queued direct socket without waiting for data. BUFFER_SELECT
 * keeps the receive allocation per ring; MSG_DONTWAIT makes the CQE arrive
 * during this pass, including EAGAIN when the socket has been drained. */
static bool hp_uring_arm_read(hp_sock fd) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || !st->direct || !st->rmshot || !hp_ur_pbuf || st->reof ||
        st->closeafter || (st->rarmed && !st->rpoll) || st->rreading) return false;
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_RECV;
    s.flags = HP_UR_IOSQE_FIXED_FILE | HP_UR_IOSQE_BUFFER_SELECT;
    s.fd = st->diridx;
    s.rw_flags = MSG_DONTWAIT;
    s.buf_group = 0;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_READ);
    if (!hp_ur_sq_push(&s)) return false;
    st->rreading = 1;
    hp_ur_cur->read_inflight++;
    return true;
}

static bool hp_uring_arm_poll(hp_sock fd) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || st->pollarmed) return false;
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_POLL;
    hp_st[HP_ST_ARM_POLL]++;
    s.fd = (int)fd;
    s.poll_events = POLLIN;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_POLL);
    if (!hp_ur_sq_push(&s)) { hp_st[HP_ST_ARM_FAIL]++; hp_ur_retfd(fd); return false; }
    st->pollarmed = 1;
    return true;
}

/* Accept into a direct descriptor slot. The Rust-compatible path keeps a
 * bounded depth of one-shot accepts so it can stop before a table fills and
 * spill to the next ring without losing connections. */
static bool hp_uring_arm_accept(hp_sock fd, int ring) {
    if (ring < 0 || ring >= hp_ur_nrings) return false;
    struct hp_uring_ring *rg = &hp_ur_rings[ring];
    int li = hp_ur_listener_find(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || !st->listener || li < 0) return false;
    if (hp_ur_direct != 1) return false;
    if (hp_ur_accept_depth) {
        uint32_t ceiling = rg->dircap > rg->accept_margin
            ? rg->dircap - rg->accept_margin : 0;
        if (rg->accept_full || !rg->accepting) return true;
        if (rg->live + rg->accept_armed >= ceiling) {
            rg->accept_full = 1;
            rg->accepting = 0;
            return true;
        }
        if (rg->accept_armed >= hp_ur_accept_depth) return true;
    } else if (rg->accept_armed) {
        return false;
    }
    hp_ur_cur = rg;
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_ACCEPT;
    s.fd = (int)fd;
    /* Null sockaddr pointers match Rust's accept entries and avoid sharing
     * mutable address storage across the bounded one-shot accepts. */
    if (!hp_ur_accept_depth) {
        s.addr = (uint64_t)(uintptr_t)&rg->accept_addr;
        s.addr2 = (uint64_t)(uintptr_t)&rg->accept_len;
        s.ioprio = HP_UR_ACCEPT_MULTISHOT;
    }
    s.file_index = HP_UR_FILE_INDEX_ALLOC;
    /* A direct-slot accept must NOT carry SOCK_CLOEXEC: the kernel rejects
     * that combination with EINVAL (io_accept_prep). */
    s.accept_flags = 0;
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_ACCEPT);
    if (!hp_ur_sq_push(&s)) {
        hp_st[HP_ST_ARM_FAIL]++;
        return false;
    }
    if (hp_ur_accept_depth) {
        rg->accept_armed++;
        rg->accepts_by_listener[li]++;
    } else {
        rg->accept_armed = 1;
        rg->accepts_by_listener[li] = 1;
    }
    return true;
}

/* Rust fills ring 0 first, then adds acceptance on the next table only when
 * the current ring reaches its safety margin. A ring reopened by closes can
 * accept again alongside the active spill ring, as in Rust's close path. */
static void hp_ur_fill_accept_quota(int r) {
    struct hp_uring_ring *rg = &hp_ur_rings[r];
    if (!rg->accepting || rg->accept_full || !hp_ur_listen_count) return;

    /* Keep the same per-listener accept floor as the reference runtime.
     * SO_REUSEPORT hashes connections to a listener before accept(); a healthy
     * global accept count can therefore hide an empty queue on one listener. */
    uint32_t quota = hp_ur_accept_depth / hp_ur_listen_count;
    if (!quota) quota = 1;
    for (int li = 0; li < HP_UR_LISTENERS_MAX; li++) {
        hp_sock fd = hp_ur_listen_fds[li];
        if (fd <= 0) continue;
        while (rg->accepts_by_listener[li] < quota &&
               rg->accept_armed < hp_ur_accept_depth && !rg->accept_full) {
            uint32_t before = rg->accept_armed;
            if (!hp_uring_arm_accept(fd, r) || rg->accept_armed == before)
                return;
        }
    }

    /* Distribute a non-divisible remainder so the configured total depth is
     * retained when users choose a listener count that does not divide it. */
    while (rg->accept_armed < hp_ur_accept_depth && !rg->accept_full) {
        uint32_t before = rg->accept_armed;
        hp_sock fd = hp_ur_listener_next();
        if (fd < 0 || !hp_uring_arm_accept(fd, r) ||
            rg->accept_armed == before)
            return;
    }
}

static void hp_ur_arm_all_accepts(void) {
    if (!hp_ur_listen_on || hp_ur_listen_count == 0 || hp_ur_direct != 1) return;
    if (hp_ur_accept_depth) {
        if (hp_ur_accept_parallel) {
            /* Each ring owns an independent direct table. Keep a bounded
             * accept window on every non-full table at once, so admission
             * does not wait for one table to approach its spill watermark. */
            for (int r = 0; r < hp_ur_nrings; r++) {
                struct hp_uring_ring *rg = &hp_ur_rings[r];
                if (rg->accept_full) continue;
                uint64_t used = (uint64_t)rg->live + rg->accept_armed +
                                rg->accept_margin;
                if (used >= rg->dircap) {
                    rg->accepting = 0;
                    rg->accept_full = 1;
                    continue;
                }
                rg->accepting = 1;
                rg->accept_resume = 0;
                hp_ur_cur = rg;
                hp_ur_fill_accept_quota(r);
            }
            hp_ur_cur = &hp_ur_rings[0];
            return;
        }
        if (hp_ur_accept_handover) {
            int lane = hp_ur_accept_lane;
            if (lane >= 0 && lane < hp_ur_nrings) {
                struct hp_uring_ring *rg = &hp_ur_rings[lane];
                if (!rg->accepting || rg->accept_full ||
                    rg->live + rg->accept_armed + rg->accept_margin >= rg->dircap) {
                    rg->accepting = 0;
                    if (rg->live + rg->accept_armed + rg->accept_margin >= rg->dircap)
                        rg->accept_full = 1;
                    hp_ur_accept_last_lane = lane;
                    hp_ur_accept_lane = -1;
                    lane = -1;
                }
            } else {
                lane = -1;
            }
            if (lane < 0) {
                for (int step = 1; step <= hp_ur_nrings; step++) {
                    int r = (hp_ur_accept_last_lane + step + hp_ur_nrings) %
                            hp_ur_nrings;
                    struct hp_uring_ring *rg = &hp_ur_rings[r];
                    uint64_t used = (uint64_t)rg->live + rg->accept_armed +
                                    rg->accept_margin;
                    if (rg->accept_full || used >= rg->dircap) {
                        if (used >= rg->dircap) {
                            rg->accepting = 0;
                            rg->accept_full = 1;
                        }
                        continue;
                    }
                    rg->accept_full = 0;
                    rg->accept_resume = 0;
                    rg->accepting = 1;
                    hp_ur_accept_lane = r;
                    hp_ur_accept_last_lane = r;
                    lane = r;
                    if (hp_ur_accept_diag)
                        fprintf(stderr,
                                "[ur] accept-handover ring=%d live=%u armed=%u margin=%u slots=%u\n",
                                r, rg->live, rg->accept_armed,
                                rg->accept_margin, rg->dircap);
                    break;
                }
            }
            if (lane >= 0 && lane < hp_ur_nrings) {
                hp_ur_cur = &hp_ur_rings[lane];
                hp_ur_fill_accept_quota(lane);
            }
            hp_ur_cur = &hp_ur_rings[0];
            return;
        }
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            if (rg->accept_resume && !rg->accept_full) {
                rg->accept_resume = 0;
                rg->accepting = 1;
            }
        }
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            if (!rg->accepting || rg->accept_full) continue;
            hp_ur_cur = rg;
            hp_ur_fill_accept_quota(r);
        }
        int accepting = 0;
        for (int r = 0; r < hp_ur_nrings; r++)
            if (hp_ur_rings[r].accepting && !hp_ur_rings[r].accept_full)
                accepting = 1;
        if (!accepting) {
            for (int r = 0; r < hp_ur_nrings; r++) {
                struct hp_uring_ring *rg = &hp_ur_rings[r];
                if (rg->accept_full) continue;
                rg->accepting = 1;
                if (hp_ur_accept_diag && rg->accept_armed == 0)
                    fprintf(stderr, "[ur] accept-start ring=%d live=%u margin=%u slots=%u\n",
                            r, rg->live, rg->accept_margin, rg->dircap);
                hp_ur_cur = rg;
                hp_ur_fill_accept_quota(r);
                break;
            }
        }
        hp_ur_cur = &hp_ur_rings[0];
        return;
    }
    for (int r = 0; r < hp_ur_nrings; r++) {
        if (hp_ur_rings[r].accept_armed || hp_ur_rings[r].accept_full) continue;
        hp_ur_cur = &hp_ur_rings[r];
        hp_sock lfd = hp_ur_listener_next();
        if (lfd < 0 || !hp_uring_arm_accept(lfd, r)) {
            /* a full SQ is retried on the next cycle */
            if (hp_ur_rings[r].accept_armed) continue;
        }
    }
    hp_ur_cur = &hp_ur_rings[0];
}

static bool hp_ur_arm_send_head(hp_sock fd) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || st->sarmed || !st->shead) return false;
    struct hp_urchunk *c = st->shead;
    struct hp_uring_sqe s;
    memset(&s, 0, sizeof s);
    s.opcode = HP_UR_OP_SEND;
    hp_st[HP_ST_ARM_SEND]++;
    if (st->direct) {
        s.flags = HP_UR_IOSQE_FIXED_FILE;
        s.fd = st->diridx;
    } else {
        s.fd = (int)fd;
    }
    s.addr = (uint64_t)(uintptr_t)(c->data + c->off);
    s.len = (uint32_t)(c->len - c->off);
    s.user_data = hp_ur_ud(st, fd, HP_UR_KIND_SEND);
    if (!hp_ur_sq_push(&s)) { hp_st[HP_ST_ARM_FAIL]++; hp_ur_retfd(fd); return false; }
    st->sarmed = 1;
    return true;
}

/* Track a fresh connection: arm a ring recv. When the SQ ring is full the fd
 * parks in the retry ring and is re-armed after the next drain freed slots --
 * the regime the 145k box run succeeded with. HPHP_OVERFLOW=1 opts into
 * watching overflow connections in an epoll set instead (each request there
 * pays a recv syscall plus epoll churn; measured at 2500 conns pinned to one
 * core it costs 57 us/req vs 31 us/req for arm-retry, and its first box run
 * scored at 76 us/req where the park-and-retry version scored 60 us/req). */
static int hp_ur_overflow_mode = -1;
static bool hp_uring_add_conn(hp_sock fd) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st) return false;
    if (st->direct) {
        /* Direct descriptors do not consume a real fd, so the fd-limit
         * overflow regime does not apply; the multishot recv SQE is armed
         * once and stays in flight. */
        st->ringconn = 1;
        hp_ur_ring_conns++;
        return hp_uring_arm_recv(fd, hp_ur_rlen());
    }
    if (hp_ur_ring_max < 0) {
        const char *e = getenv("HPHP_RING_MAX");
        hp_ur_ring_max = (e && *e) ? atoi(e) : HP_UR_RING_MAX_DEFAULT;
        if (hp_ur_ring_max < 1) hp_ur_ring_max = 1;
    }
    if (hp_ur_overflow_mode < 0) {
        const char *e = getenv("HPHP_OVERFLOW");
        hp_ur_overflow_mode = (e && (e[0] == '1' || e[0] == 'y' ||
                                     e[0] == 'Y')) ? 1 : 0;
    }
    if (hp_ur_overflow_mode == 1 && hp_ur_ring_conns >= hp_ur_ring_max)
        return hp_ur_ep_add(fd);
    st->ringconn = 1;
    hp_ur_ring_conns++;
    return hp_uring_arm_recv(fd, hp_ur_rlen());
}

/* Queue a response whose buffer the runtime already owns (stream_send_sb):
 * like hp_uring_send2, but there is nothing to copy -- the malloc'd builder
 * buffer becomes the chunk. -2 on a fatal path, buffer freed. */
static int64_t hp_uring_send_owned(hp_sock fd, char *buf, size_t off, size_t len, size_t cap) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || st->reof) { hp_resp_buf_release(buf, cap); return -2; }
    if (st->squeued >= 64) {                       /* a stalled peer */
        st->reof = 1;
        hp_resp_buf_release(buf, cap);
        return -2;
    }
    struct hp_urchunk *c = hp_ur_chunk_alloc();
    if (!c) { hp_resp_buf_release(buf, cap); st->reof = 1; hp_ur_drop_send_chain(st); return -2; }
    c->data = buf; c->off = off; c->len = len; c->cap = cap; c->next = NULL;
    uint64_t *queued_ns = hp_ur_chunk_diag_ns(c);
    if (queued_ns) *queued_ns = hp_diag_now_ns();
    if (st->stail) st->stail->next = c; else st->shead = c;
    st->stail = c;
    st->squeued++;
    if (!st->sarmed) hp_ur_arm_send_head(fd);
    return (int64_t)(len - off);
}

/* Queue a response for the ring: the bytes are copied into the chunk so the
 * caller's hstr can be freed while the send is still in flight. The full
 * length is claimed as sent (so write_response's bookkeeping matches the
 * sync contract) and the flush happens on the next poll cycle's enter.
 * -2 on a fatal path (no ring slot, OOM, stalled peer) so the caller's close
 * path cleans up, exactly like the synchronous contract. */
static int64_t hp_uring_send2(hp_sock fd, hstr *a, hstr *b) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st || st->reof) return -2;
    if (st->squeued >= 64) {                       /* a stalled peer */
        st->reof = 1;
        /* keep the chain: the head may still be in flight, so freeing it
         * here would let the kernel read freed memory. The completion (or
         * the deferred close) drops it. */
        return -2;
    }
    size_t n = a->len + b->len;
    size_t cap = 0;
    size_t need = n ? n : 1;
    char *buf = hp_resp_buf_acquire_min(need, &cap);
    if (!buf) buf = hp_resp_buf_acquire(&cap);
    if (cap < need) {
        size_t next_cap = cap ? cap : 256;
        while (next_cap < need && next_cap <= HP_RESP_BUF_POOL_CAP_MAX / 2)
            next_cap *= 2;
        if (next_cap < need) next_cap = need;
        char *next = (char *)realloc(buf, next_cap);
        if (!next) {
            hp_resp_buf_release(buf, cap);
            st->reof = 1;
            hp_ur_drop_send_chain(st);
            return -2;
        }
        buf = next;
        cap = next_cap;
    }
    if (a->len) memcpy(buf, a->data, a->len);
    if (b->len) memcpy(buf + a->len, b->data, b->len);
    struct hp_urchunk *c = hp_ur_chunk_alloc();
    if (!c) { hp_resp_buf_release(buf, cap); st->reof = 1; hp_ur_drop_send_chain(st); return -2; }
    c->data = buf; c->off = 0; c->len = n; c->cap = cap; c->next = NULL;
    uint64_t *queued_ns = hp_ur_chunk_diag_ns(c);
    if (queued_ns) *queued_ns = hp_diag_now_ns();
    hp_st[HP_ST_SEND_CHUNK]++;
    if (st->stail) st->stail->next = c; else st->shead = c;
    st->stail = c;
    st->squeued++;
    if (!st->sarmed) hp_ur_arm_send_head(fd);
    return (int64_t)n;
}

/* Return the fresh bytes for fd. The kernel wrote them into this
 * connection's own hstr block, so handout is a pointer, not a copy -- a
 * read costs zero syscalls, zero malloc and zero memcpy. The block is
 * handed out exactly once (the rready gate) and the scene: with the ring
 * re-armed the next packet overwrites it, which is safe because the app
 * is done with the previous buffer by the time it consumed the request
 * (parse is synchronous; anything it retains goes through persist/concat,
 * which copy because the block carries HP_F_SCOPED). The hash is reset
 * each handout since the bytes mutate. */
static hstr *hp_uring_take_recv(hp_sock fd, size_t maxall) {
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st) return hp_str_lit("");
    if (st->epolled) {
        /* overflow connection: epoll reported readable, read it now */
        if (!st->rh) return hp_str_lit("");
        size_t cap = st->rcap ? st->rcap : hp_ur_rlen();
        if (maxall && maxall < cap) cap = maxall;
        ssize_t r = recv(fd, st->rh->data, cap, 0);
        if (r > 0) {
            hp_st[HP_ST_REQ]++;
            st->rh->len = (size_t)r;
            st->rh->hash = 0;
            st->rh->data[r] = 0;
            return st->rh;
        }
        if (r == 0) {
            st->reof = 1;
            hp_sockrec *sr = sockrec((intptr_t)fd, false);
            if (sr) sr->eof = true;
        }
        return hp_str_lit("");
    }
    if (!st->rready) return hp_str_lit("");
    hp_ur_diag_recv_taken(fd);
    size_t avail = st->rlen < maxall ? st->rlen : maxall;
    /* Bytes copied out of a provided buffer no longer sit in the socket, so
     * the kernel cannot signal them again: hand the app the whole queue. */
    if (st->direct && st->rlen > avail) avail = st->rlen;
    if (avail < st->rlen) {
        /* caller asks for fewer bytes than arrived (rare; the challenge app
         * always reads the recv length): hand out a copy of the head and
         * keep the rest queued, like the classic path. */
        hstr *out = hp_str_new(st->rh->data, avail);
        st->rh->hash = 0;
        return out;
    }
    st->rh->len = avail;
    st->rh->hash = 0;
    st->rh->data[avail] = 0;
    if (avail) {
        hp_st[HP_ST_REQ]++;
        st->rready = 0;
        st->rlen = 0;
        if (hp_ur_dbg)
            fprintf(stderr, "[ur] handout fd=%d len=%zu\n", (int)fd, avail);
        /* One-shot recv must be re-armed after the app is done with the
         * buffer. A multishot op that is still in flight keeps receiving;
         * only a connection whose op stopped is parked for a re-arm. */
        if ((!st->rmshot || !st->rarmed) && !st->rreadq && !st->rreading) {
            st->rarmed = 0;
            hp_ur_retfd(fd);
        }
    }
    return st->rh;
}

/* Attach a single provided buffer directly when the app can consume it now.
 * If a request spans buffers, keep appending while the borrowed buffer fits;
 * otherwise promote the accumulated bytes to the normal per-connection hstr. */
static bool hp_ur_recv_append(struct hp_urfds *st, const char *p, size_t n,
                              uint32_t bid, bool allow_borrow) {
    if (!n) return false;
    /* Borrowed provided-buffer strings need the core's HP_F_BORROWED
     * lifetime flag; without it every receive is copied out of the ring
     * buffer (HPHP_RECV_ZEROCOPY stays a no-op here). */
    (void)allow_borrow;
    size_t base = st->rready ? st->rlen : 0;
    size_t need = base + n;
    if (!st->rh && st->direct && st->rmshot && hp_ur_cur->pbuf &&
        hp_ur_recvpool_on()) {
        st->rh = hp_ur_recvbuf_take();
        st->rcap = st->rh->cap - 1;
    }
    if (!st->rh || need + 1 > st->rcap) {
        size_t cap = st->rcap ? st->rcap : hp_ur_rlen();
        while (cap < need + 1) cap *= 2;
        hstr *h = (hstr *)hp_alloc(sizeof(hstr) + cap + 1);
        if (!h) { st->reof = 1; return false; }
        h->len = 0;
        h->cap = cap + 1;
        h->hash = 0;
        h->rc = 1;
        if (st->rh && base) memcpy(h->data, st->rh->data, base);
        if (st->rborrowed) {
            hp_ur_pbuf_return(st->rborrow_bid);
            st->rborrowed = 0;
            st->rborrow_bid = 0;
        } else if (st->direct && st->rmshot && hp_ur_cur->pbuf &&
                   hp_ur_recvpool_on()) {
            hp_ur_recvbuf_put(st->rh);
        } else {
            hp_free(st->rh);
        }
        st->rh = h;
        st->rcap = cap;
    }
    memcpy(st->rh->data + base, p, n);
    st->rh->len = need;
    st->rh->hash = 0;
    st->rh->data[need] = 0;
    st->rlen = need;
    st->rready = 1;
    return false;
}

/* Accepted direct handles wait here until the app calls
 * stream_socket_accept2(). The listener fd is pushed to the app's ready
 * list when a completion queues one, so the accept loop drains this before
 * the next wait. */
static bool hp_ur_acceptq_push(hp_sock h) {
    if (!hp_ur_acceptq) {
        hp_ur_acceptq_cap = 1024;
        hp_ur_acceptq = (uint32_t *)malloc(
            (size_t)hp_ur_acceptq_cap * sizeof *hp_ur_acceptq);
        if (!hp_ur_acceptq) return false;
        hp_ur_acceptq_len = hp_ur_acceptq_pos = 0;
    }
    if (hp_ur_acceptq_len == hp_ur_acceptq_cap) {
        if (hp_ur_acceptq_pos) {
            memmove(hp_ur_acceptq, hp_ur_acceptq + hp_ur_acceptq_pos,
                    (size_t)(hp_ur_acceptq_len - hp_ur_acceptq_pos) *
                        sizeof *hp_ur_acceptq);
            hp_ur_acceptq_len -= hp_ur_acceptq_pos;
            hp_ur_acceptq_pos = 0;
        }
        if (hp_ur_acceptq_len == hp_ur_acceptq_cap) {
            uint32_t ncap = hp_ur_acceptq_cap * 2;
            uint32_t *n = (uint32_t *)realloc(
                hp_ur_acceptq, (size_t)ncap * sizeof *n);
            if (!n) return false;
            hp_ur_acceptq = n;
            hp_ur_acceptq_cap = ncap;
        }
    }
    hp_ur_acceptq[hp_ur_acceptq_len++] = (uint32_t)h;
    return true;
}
static hp_sock hp_ur_acceptq_pop(void) {
    if (!hp_ur_acceptq || hp_ur_acceptq_pos >= hp_ur_acceptq_len) {
        hp_ur_acceptq_pos = hp_ur_acceptq_len = 0;
        return HP_SOCK_BAD;
    }
    return (hp_sock)hp_ur_acceptq[hp_ur_acceptq_pos++];
}

/* Initialize the per-connection state for a slot the kernel just installed
 * with accept(ALLOC). The epoch keeps counting across slot reuse so a late
 * completion from the previous owner can never touch this connection. */
static hp_sock hp_ur_accept_one(uint32_t ring, uint32_t idx) {
    if (ring >= (uint32_t)hp_ur_nrings) return HP_SOCK_BAD;
    struct hp_uring_ring *rg = &hp_ur_rings[ring];
    if (!rg->dirs || idx >= rg->dircap) return HP_SOCK_BAD;
    struct hp_urfds *st = &rg->dirs[idx];
    uint32_t e = st->epoch + 1;
    memset(st, 0, sizeof *st);
    st->epoch = e;
    st->direct = 1;
    st->diridx = (int)idx;
    /* Track installed direct slots for both bounded one-shot accepts and
     * multishot accepts. The live count also drives ENFILE recovery after a
     * client closes a slot that had paused this ring. */
    st->slot_live = 1;
    rg->live++;
    if (hp_ur_accept_depth) {
        if (!rg->accept_full &&
            rg->live + rg->accept_armed + rg->accept_margin >= rg->dircap) {
            rg->accept_full = 1;
            rg->accepting = 0;
            if (hp_ur_accept_diag)
                fprintf(stderr, "[ur] accept-full ring=%u live=%u armed=%u margin=%u slots=%u\n",
                        ring, rg->live, rg->accept_armed,
                        rg->accept_margin, rg->dircap);
        }
    }
    hp_sock h = (hp_sock)(HP_UR_HANDLE_BASE +
                          (hp_sock)ring * (hp_sock)HP_UR_SLOTS_PER_RING +
                          (hp_sock)idx);
    hp_st[HP_ST_ACCEPT]++;
    hp_sockrec *sr = sockrec((intptr_t)h, true);
    if (sr) { sr->eof = false; sr->listener = false; }
    return h;
}

/* Detach runtime state after the descriptor is gone. */
static void hp_ur_detach(struct hp_urfds *st) {
    hp_ur_drop_send_chain(st);
    if (st->ringconn) {
        st->ringconn = 0;
        if (hp_ur_ring_conns > 0) hp_ur_ring_conns--;
    }
    hstr *rh = st->rh;
    int borrowed = st->rborrowed;
    uint32_t bid = st->rborrow_bid;
    int pooled = st->direct && st->rmshot && hp_ur_cur->pbuf;
    uint32_t e = st->epoch + 1;
    memset(st, 0, sizeof *st);
    st->epoch = e;
    if (borrowed) hp_ur_pbuf_return(bid);
    else if (pooled && hp_ur_recvpool_on()) hp_ur_recvbuf_put(rh);
    else hp_free(rh);
}

/* Re-arm one handle after a push failure freed ring slots. Shared by the
 * real-fd and direct-descriptor full scans. */
static void hp_ur_rescan_one(hp_sock fd) {
    hp_ur_use_ring(fd);
    struct hp_urfds *st = hp_ur_st_of(fd);
    if (!st) return;
    st->rtagged = 0;
    if (st->reof) return;
    if (st->closeafter) {
        if (st->shead && !st->sarmed && !hp_ur_arm_send_head(fd))
            hp_ur_retfd(fd);
        return;
    }
    if (st->listener) {
        if (hp_ur_direct == 1) hp_ur_arm_all_accepts();
        else if (!hp_uring_arm_poll(fd)) hp_ur_retfd(fd);
        return;
    }
    if (st->shead && !st->sarmed && !hp_ur_arm_send_head(fd))
        hp_ur_retfd(fd);
    if (st->rh && !st->rready && !st->rarmed &&
        !st->rreadq && !st->rreading &&
        !hp_uring_arm_recv(fd, hp_ur_rlen()))
        hp_ur_retfd(fd);
}

/* Drain one ring's completion queue. hp_ur_cur selects the ring; helper
 * calls may switch it, so the loop restores it every iteration. */
static int hp_ur_drain_ring(HpFdList *in) {
    struct hp_uring_ring *self = hp_ur_cur;
    bool diag = hp_diag_enabled();
    uint64_t diag_t0 = diag ? hp_diag_now_ns() : 0;
    uint32_t head = hp_ur_cqhead;
    uint32_t start_head = head;
    int drained = 0;
    /* One listener wakeup per drain is enough: accept_all() empties the
     * whole accept queue. Pushing the listener for every one of tens of
     * thousands of accept CQEs would bloat the app's ready array. */
    int accept_woken = 0;
    for (;;) {
        /* Leave the remainder published in CQ for the next pass. Refreshing
         * the producer tail without a quantum can monopolize the issuer
         * while other rings and application-ready sockets wait. */
        if (hp_ur_cq_budget && head - start_head >= hp_ur_cq_budget) break;
        hp_ur_cur = self;
        uint32_t tail = __atomic_load_n(hp_ur_cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail) break;
        struct hp_uring_cqe *c = hp_ur_cqes + (head & hp_ur_cqmask);
        head++;
        drained = 1;
        hp_st[HP_ST_CQE]++;
        uint64_t ud = c->user_data;
        int kind = (int)(ud & HP_UR_KIND);
        if (kind == HP_UR_KIND_WAKE) {
            uint32_t ri = (uint32_t)((ud >> 8) & 0xffU);
            if (ri > 0 && ri < (uint32_t)hp_ur_nrings) {
                hp_ur_rings[ri].wake_ready = 1;
                if (!(c->flags & HP_UR_CQE_F_MORE)) {
                    struct hp_uring_sqe s;
                    memset(&s, 0, sizeof s);
                    s.opcode = HP_UR_OP_POLL_ADD;
                    s.fd = hp_ur_rings[ri].efd;
                    s.len = HP_UR_POLL_ADD_MULTI;
                    s.poll32_events = POLLIN;
                    s.user_data = HP_UR_WAKE_UD(ri);
                    hp_ur_sq_push(&s);
                }
            }
            continue;
        }
        if (kind != HP_UR_KIND_RECV && kind != HP_UR_KIND_READ &&
            kind != HP_UR_KIND_SEND &&
            kind != HP_UR_KIND_POLL && kind != HP_UR_KIND_ACCEPT &&
            kind != HP_UR_KIND_CLOSE) continue;
        hp_sock fd = hp_ur_ud_handle(ud);
        if (kind == HP_UR_KIND_CLOSE) {
            if (c->res == 0 || c->res == -EBADF)
                hp_ur_slot_close_done(fd);
            else if (hp_ur_dbg)
                fprintf(stderr, "[ur] direct close CQE failed fd=%d res=%d\n",
                        (int)fd, c->res);
            continue;
        }
        uint32_t epoch = (uint32_t)(ud >> 40);
        if (kind == HP_UR_KIND_READ && self->read_inflight > 0)
            self->read_inflight--;
        if (kind == HP_UR_KIND_READ) hp_ur_readq_completions++;
        if (hp_ur_dbg)
            fprintf(stderr, "[ur] cqe kind=%d fd=%d ep=%u res=%d\n",
                    kind, (int)fd, epoch, c->res);
        struct hp_urfds *st = hp_ur_st_of(fd);
        if (!st || st->epoch != epoch) {
            /* A recv that was in flight when the connection closed still owns
             * a provided buffer; return it or the ring drains permanently. */
            if ((kind == HP_UR_KIND_RECV || kind == HP_UR_KIND_READ) &&
                (c->flags & HP_UR_CQE_F_BUFFER))
                hp_ur_pbuf_return(c->flags >> HP_UR_CQE_BUFFER_SHIFT);
            /* An accept that completed after the listener was paused still
             * installed a file; close that slot instead of leaking it. */
            if (kind == HP_UR_KIND_ACCEPT && c->res >= 0)
                hp_ur_close_slot((uint32_t)c->res, 0);
            continue;   /* closed + reused fd */
        }
        if (kind == HP_UR_KIND_ACCEPT) {
            if (diag) self->diag_accept_cqes++;
            if (!st->listener) {
                /* a reused real fd with a stale accept completion */
                if (c->res >= 0) hp_ur_close_slot((uint32_t)c->res, 0);
                continue;
            }
            int rr = (int)(hp_ur_cur - hp_ur_rings);
            int li = hp_ur_listener_find(fd);
            if (hp_ur_accept_depth) {
                if (hp_ur_rings[rr].accept_armed)
                    hp_ur_rings[rr].accept_armed--;
                if (li >= 0 && hp_ur_rings[rr].accepts_by_listener[li])
                    hp_ur_rings[rr].accepts_by_listener[li]--;
            } else {
                hp_ur_rings[rr].accept_armed =
                    (c->flags & HP_UR_CQE_F_MORE) ? 1 : 0;
                if (li >= 0)
                    hp_ur_rings[rr].accepts_by_listener[li] =
                        (c->flags & HP_UR_CQE_F_MORE) ? 1 : 0;
            }
            if (c->res >= 0) {
                uint32_t idx = (uint32_t)c->res;
                hp_sock nh = hp_ur_accept_one((uint32_t)rr, idx);
                if (nh == HP_SOCK_BAD || !hp_ur_acceptq_push(nh)) {
                    /* no queue room: release the fresh slot right away */
                    if (nh != HP_SOCK_BAD) {
                        struct hp_urfds *ns = hp_ur_st_of(nh);
                        int cr = hp_ur_close_slot(idx,
                            ns ? hp_ur_ud(ns, nh, HP_UR_KIND_CLOSE) : 0);
                        if (cr == 0) {
                            hp_ur_slot_freed(nh);
                            if (ns) hp_ur_detach(ns);
                        } else if (cr > 0) {
                            if (ns) hp_ur_detach(ns);
                        } else if (hp_ur_dbg) {
                            fprintf(stderr, "[ur] accepted slot close failed fd=%d slot=%u\n",
                                    (int)nh, idx);
                        }
                    } else {
                        if (hp_ur_accept_depth && hp_ur_rings[rr].live)
                            hp_ur_rings[rr].live--;
                        hp_ur_close_slot(idx, 0);
                    }
                    g_accept_errno = EMFILE;
                } else {
                    g_accept_errno = 0;
                }
                if (in && !accept_woken) {
                    hp_fdlist_push(in, fd);  /* wake accept_all */
                    accept_woken = 1;
                }
            } else {
                if (hp_ur_accept_diag)
                    fprintf(stderr, "[ur] accept-error ring=%d res=%d live=%u armed=%u full=%d\n",
                            rr, c->res, hp_ur_rings[rr].live,
                            hp_ur_rings[rr].accept_armed,
                            hp_ur_rings[rr].accept_full);
                /* -EAGAIN is normal churn. A full direct table belongs to
                 * ONE ring: disable just that ring instead of pausing the
                 * listener, so the other rings keep accepting. */
                if (c->res == -ECANCELED || c->res == -EBADF ||
                    c->res == -EAGAIN || c->res == -EINTR) {
                    g_accept_errno = 0;
                } else if (hp_ur_direct == 1 &&
                           (c->res == -ENFILE || c->res == -EMFILE ||
                            c->res == -ENOMEM)) {
                    hp_ur_rings[rr].accept_full = 1;
                    hp_ur_rings[rr].accepting = 0;
                    if (!hp_ur_accept_depth) {
                        hp_ur_rings[rr].accept_armed = 0;
                        if (li >= 0)
                            hp_ur_rings[rr].accepts_by_listener[li] = 0;
                    }
                    g_accept_errno = 0;
                } else {
                    g_accept_errno = (int)(-c->res);
                    if (in && !accept_woken) {
                        hp_fdlist_push(in, fd);
                        accept_woken = 1;
                    }
                }
            }
            /* Keep at least one accept pending on the listener that completed.
             * SO_REUSEPORT hashes each new flow to one listener; refilling only
             * the global ring depth can leave that listener with no accept SQE
             * while its queue has connections, and the ring then sleeps on the
             * other listeners. */
            if (st->listener && hp_ur_direct == 1 &&
                (c->res >= 0 || c->res == -EAGAIN || c->res == -EINTR) &&
                (!hp_ur_accept_depth ||
                 hp_ur_rings[rr].accept_armed < hp_ur_accept_depth))
                hp_uring_arm_accept(fd, rr);
            continue;
        }
        if (kind == HP_UR_KIND_READ) {
            if (diag) self->diag_recv_cqes++;
            hp_st[HP_ST_CQE_RECV]++;
            hp_ur_recovery_mode = 1;
            st->rreading = 0;
            uint32_t bid = c->flags >> HP_UR_CQE_BUFFER_SHIFT;
            int have_buf = (c->flags & HP_UR_CQE_F_BUFFER) &&
                           bid < hp_ur_pbuf_n;
            if (have_buf && c->res > 0)
                hp_ur_recv_append(st,
                    hp_ur_pbuf_data(bid),
                    (size_t)c->res, bid, false);
            if (have_buf) hp_ur_pbuf_return(bid);
            if (st->closeafter) continue;
            if (c->res > 0 && have_buf) {
                hp_ur_diag_recv_ready(fd);
                hp_ur_recovery_ready(fd, st, in);
                if ((uint32_t)c->res == hp_ur_pbuf_blen) {
                    if (!hp_ur_readq_push(fd, st, 1)) hp_ur_retfd(fd);
                } else if (!(st->rpoll && st->rarmed) &&
                           !hp_uring_arm_recv(fd, hp_ur_rlen())) {
                    hp_ur_retfd(fd);
                }
            } else if (c->res == -ENOBUFS) {
                if (!hp_ur_readq_push(fd, st, 2)) hp_ur_retfd(fd);
            } else if (c->res == -EAGAIN || c->res == -EINTR) {
                if (!(st->rpoll && st->rarmed) &&
                    !hp_uring_arm_recv(fd, hp_ur_rlen())) hp_ur_retfd(fd);
            } else {
                st->reof = 1;
                hp_sockrec *sr = sockrec((intptr_t)fd, false);
                if (sr) sr->eof = true;
                hp_fdlist_push(in, fd);
            }
            continue;
        }
        if (kind == HP_UR_KIND_RECV) {
            if (diag) self->diag_recv_cqes++;
            hp_st[HP_ST_CQE_RECV]++;
            int more = (c->flags & HP_UR_CQE_F_MORE) ? 1 : 0;
            if (!st->direct || !more) st->rarmed = 0;
            if (hp_ur_dbg)
                fprintf(stderr, "[ur] recv-cqe fd=%d res=%d\n", (int)fd, c->res);
            if (st->direct && (c->flags & HP_UR_CQE_F_BUFFER)) {
                uint32_t bid = c->flags >> HP_UR_CQE_BUFFER_SHIFT;
                if (bid < hp_ur_pbuf_n) {
                    bool borrowed = false;
                    if (c->res > 0)
                        borrowed = hp_ur_recv_append(st,
                            hp_ur_pbuf_data(bid), (size_t)c->res, bid, true);
                    if (!borrowed) hp_ur_pbuf_return(bid);
                } else if (c->res > 0) {
                    st->reof = 1;      /* kernel/our ring disagree: drop */
                }
            }
            if (st->closeafter) continue;   /* app already closed: ignore */
            if (st->direct && st->rmshot && !more)
                hp_ur_recovery_mode = 1;
            if (c->res > 0) {
                hp_ur_diag_recv_ready(fd);
                if (!st->direct || !st->rmshot) {
                    /* one-shot recv wrote straight into rh */
                    st->rlen = (size_t)c->res;
                    st->rready = 1;
                    if (st->rh) {
                        st->rh->len = (size_t)c->res;
                        st->rh->hash = 0;
                        st->rh->data[c->res] = 0;
                    }
                }
                hp_ur_recovery_ready(fd, st, in);
                /* The multishot stopped after this buffer (normally because
                 * the buffer ring was empty). Drain any remaining socket data
                 * with bounded reads before re-arming it. */
                if (st->direct && st->rmshot && !more &&
                    !hp_ur_readq_push(fd, st, 3)) hp_ur_retfd(fd);
            } else if (c->res == -ENOBUFS) {
                /* Don't re-arm a whole burst of stopped multishots together. */
                if (!more) st->rarmed = 0;
                if (!more && st->direct && st->rmshot && hp_ur_pbuf) {
                    if (!hp_ur_readq_push(fd, st, 4)) hp_ur_retfd(fd);
                } else if (!more) {
                    hp_ur_retfd(fd);
                }
            } else if (c->res == -EAGAIN || c->res == -EINTR) {
                hp_uring_arm_recv(fd, hp_ur_rlen());
            } else {
                /* 0 = orderly close, negative = hard error. Mirror it into
                 * the app-side socket record: stream_eof() consults that,
                 * and without this the app never closes an EOF'd connection
                 * (leaking the fd and the per-connection recv block). */
                st->reof = 1;
                hp_sockrec *sr = sockrec((intptr_t)fd, false);
                if (sr) sr->eof = true;
                hp_ur_recovery_ready(fd, st, in);
            }
        } else if (kind == HP_UR_KIND_POLL) {
            if (diag) self->diag_poll_cqes++;
            hp_st[HP_ST_CQE_POLL]++;
            if (st->rpoll) {
                int more = (c->flags & HP_UR_CQE_F_MORE) ? 1 : 0;
                if (!more) st->rarmed = 0;
                if (st->closeafter) continue;
                if (c->res > 0 && (c->res & POLLIN)) {
                    if (!st->rready && !st->rreadq && !st->rreading &&
                        !hp_uring_arm_read(fd)) hp_ur_retfd(fd);
                } else if (c->res > 0 &&
                           (c->res & (HP_UR_POLLRDHUP | POLLHUP | POLLERR))) {
                    st->reof = 1;
                    hp_sockrec *sr = sockrec((intptr_t)fd, false);
                    if (sr) sr->eof = true;
                    hp_fdlist_push(in, fd);
                } else if (c->res < 0 && c->res != -ECANCELED && c->res != -EBADF) {
                    if (!more && !st->rreading && !st->rready && !st->rreadq &&
                        !hp_uring_arm_recv(fd, hp_ur_rlen())) hp_ur_retfd(fd);
                }
                continue;
            }
            st->pollarmed = 0;
            if (st->closeafter) continue;   /* app already closed: ignore */
            if (c->res < 0 && (c->res == -ECANCELED || c->res == -EBADF))
                continue;
            /* the completion res is the event mask (positive) only when the
             * poll succeeded; a negative res must not arm-ready the fd */
            if (c->res > 0 && (c->res & POLLIN)) {
                hp_fdlist_push(in, fd);
                hp_uring_arm_poll(fd);             /* keep watching */
            } else if (c->res <= 0) {
                /* the poll died: surface the fd so the owner closes it */
                st->reof = 1;
                hp_fdlist_push(in, fd);
            }
        } else {
            if (diag) self->diag_send_cqes++;
            hp_st[HP_ST_CQE_SEND]++;
            st->sarmed = 0;
            struct hp_urchunk *c2 = st->shead;
            if (c2) {
                if (c->res >= 0) {
                    c2->off += (size_t)c->res;
                    if (c2->off >= c2->len) {
                        uint64_t *queued_ns = hp_ur_chunk_diag_ns(c2);
                        if (diag && queued_ns && *queued_ns) {
                            uint64_t now = hp_diag_now_ns();
                            uint64_t age = now >= *queued_ns ?
                                           now - *queued_ns : 0;
                            self->diag_send_complete_ns += age;
                            self->diag_send_complete_count++;
                            if (age > self->diag_send_complete_max_ns)
                                self->diag_send_complete_max_ns = age;
                        }
                        st->shead = c2->next;
                        if (!st->shead) st->stail = NULL;
                        st->squeued--;
                        hp_ur_chunk_release(c2);
                    }
                } else if (c->res != -ECANCELED && c->res != -EBADF &&
                           c->res != -EAGAIN && c->res != -EINTR) {
                    /* EPIPE / ECONNRESET: the peer died mid-response */
                    st->reof = 1;
                    hp_fdlist_push(in, fd);
                    hp_ur_drop_send_chain(st);
                }
                if (!st->reof) hp_ur_arm_send_head(fd);
            }
            if (st->closeafter) {
                if (!st->shead) hp_ur_finish_close(fd);
                continue;
            }
        }
        /* keep this fd's pipeline flowing for the next cycle */
        if (!st->reof && !st->closeafter && !st->listener &&
            !st->rready && !st->rarmed && !st->rreadq && !st->rreading) {
            if (hp_ur_dbg)
                fprintf(stderr, "[ur] rearm-after-cqe fd=%d\n", (int)fd);
            if (!hp_uring_arm_recv(fd, hp_ur_rlen()) && hp_ur_dbg)
                fprintf(stderr, "[ur] rearm-failed fd=%d\n", (int)fd);
        }
    }
    hp_ur_cur = self;
    __atomic_store_n(hp_ur_cq_head, head, __ATOMIC_RELEASE);
    hp_ur_cqhead = head;
    if (diag) {
        uint64_t t1 = hp_diag_now_ns();
        self->diag_drain_cqes += (uint64_t)(head - start_head);
        self->diag_drain_ns += t1 >= diag_t0 ? t1 - diag_t0 : 0;
    }
    return drained;
}

/* Drain a burst in bounded, FIFO rounds. Each direct ring has its own buffer
 * pool; limiting fallback reads to that pool ensures every read can return a
 * selected buffer instead of creating another ENOBUFS wave. */
static int hp_ur_run_readq(HpFdList *in) {
    uint32_t budget = 16384;
    int any_drained = 0;
    while (budget && hp_ur_readq_total) {
        uint32_t issued = 0;
        bool sq_full = false;
        /* Per ring, issue only as many reads as it has free provided
         * buffers; a saturated ring keeps its FIFO untouched until its
         * reads complete (no rotation of waiting entries). */
        for (int rr = 0; rr < hp_ur_nrings && issued < budget && !sq_full; rr++) {
            struct hp_uring_ring *rg = &hp_ur_rings[rr];
            hp_ur_cur = rg;
            /* Without a provided-buffer pool the entries are not eligible;
             * pop them so the checks below drop them. */
            while (issued < budget && rg->rq_head != rg->rq_tail &&
                   (!hp_ur_pbuf || rg->read_inflight < hp_ur_pbuf_n)) {
                struct hp_ur_readq_ent ent;
                if (!hp_ur_rq_pop(rg, &ent)) break;
                hp_sock fd = (hp_sock)ent.fd;
                if (hp_ur_ring_of(fd) != rr) continue;
                struct hp_urfds *st = hp_ur_st_of(fd);
                if (!st || st->epoch != ent.epoch || !st->rreadq) continue;
                if (hp_ur_dbg && !hp_ur_readq_diag) {
                    hp_ur_readq_diag = 1;
                    fprintf(stderr, "[ur] recovery state fd=%d direct=%d mshot=%d pbuf=%d eof=%d closing=%d armed=%d reading=%d inflight=%u/%u\n",
                            (int)fd, st->direct, st->rmshot, hp_ur_pbuf != 0,
                            st->reof, st->closeafter, st->rarmed, st->rreading,
                            rg->read_inflight, hp_ur_pbuf_n);
                }
                if (!st->direct || !st->rmshot || !hp_ur_pbuf || st->reof ||
                    st->closeafter || st->rarmed || st->rreading) {
                    st->rreadq = 0;
                    continue;
                }
                st->rreadq = 0;
                if (!hp_uring_arm_read(fd)) {
                    if (!hp_ur_readq_push(fd, st, 6)) hp_ur_retfd(fd);
                    /* The SQ is full: let the next normal wait reap it first. */
                    sq_full = true;
                    break;
                }
                issued++;
            }
        }
        if (!issued) break;
        budget -= issued;

        /* MSG_DONTWAIT reads should finish within this enter. Draining after
         * every round returns buffers before more queued sockets are issued. */
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            if (!hp_ur_ring_needs_enter(rg)) continue;
            hp_ur_cur = rg;
            long to_submit = (long)(rg->sqtail -
                __atomic_load_n(rg->sq_head, __ATOMIC_ACQUIRE));
            hp_ur_enter(to_submit > 0 ? to_submit : 0, 0, -1);
        }
        for (int r = 0; r < hp_ur_nrings; r++) {
            hp_ur_cur = &hp_ur_rings[r];
            any_drained |= hp_ur_drain_ring(in);
        }
    }
    hp_ur_cur = &hp_ur_rings[0];
    return any_drained;
}

/* Submit everything queued since the last cycle, wait until at least one
 * ring has progress (or the timeout expires), then drain every ring. */
static void hp_uring_wait(HpFdList *in, HpFdList *out, int timeout_ms) {
    unsigned char cq_ready[HP_UR_RINGS_MAX] = {0};
    if (hp_ur_dbg) fprintf(stderr, "[ur] wait to=%d\n", timeout_ms);
    hp_ur_ready_bit ^= 1U;
    hp_ur_answer_budget = hp_ur_answer_budget_limit;
    hp_ur_recovery_mode =
        (hp_ur_answerq_head != hp_ur_answerq_tail ||
         hp_ur_readq_total != 0);
    uint32_t recovery_dispatched = hp_ur_dispatch_answerq(in);
    hp_ur_arm_all_accepts();

    int drained = 0;
    uint32_t have = 0;
    unsigned int nrings = (unsigned int)hp_ur_nrings;
    unsigned int drain_start = (hp_ur_drain_rr && nrings)
        ? hp_ur_drain_start % nrings : 0;
    for (unsigned int i = 0; i < nrings; i++) {
        int r = (int)((drain_start + i) % nrings);
        hp_ur_cur = &hp_ur_rings[r];
        if (hp_ur_rings[r].wake_ready) {
            hp_ur_rings[r].wake_ready = 0;
            cq_ready[r] = 1;
        }
        uint32_t t = __atomic_load_n(hp_ur_cq_tail, __ATOMIC_ACQUIRE);
        have += t - hp_ur_cqhead;
        if (t != hp_ur_cqhead) cq_ready[r] = 1;
    }
    if (recovery_dispatched) have = 1; /* queued answers must not wait for I/O */

    bool ring0_wait = hp_ur_ring0_wait && hp_ur_direct == 1 &&
                      hp_ur_nrings > 1 && hp_ur_ep_conns == 0;
    bool ring0_work_pending = have > 0 || recovery_dispatched > 0 ||
        hp_ur_readq_total != 0 ||
        hp_ur_answerq_head != hp_ur_answerq_tail;
    bool ring0_do_wait = ring0_wait &&
        (!hp_ur_ring0_busy_aware || !ring0_work_pending);
    if (ring0_do_wait) {
        /* Match the multi-ring Rust loop: flush all nonzero rings first, then
         * let ring 0 own the bounded wait for the whole pass. Completions on
         * the other rings are drained immediately after that wait. */
        uint32_t posted = 0;
        for (int r = hp_ur_nrings - 1; r > 0; r--) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            if (!hp_ur_ring_needs_enter(rg)) continue;
            hp_ur_cur = rg;
            long to_submit = (long)(rg->sqtail -
                __atomic_load_n(rg->sq_head, __ATOMIC_ACQUIRE));
            if (rg->defer_taskrun)
                hp_ur_enter(to_submit > 0 ? to_submit : 0, 0, -1);
            else
                hp_ur_submit_only(to_submit > 0 ? to_submit : 0);
            /* A deferred ring posts its completions only inside that enter,
             * so nothing will wake ring 0's wait for them: count them now. */
            posted += __atomic_load_n(hp_ur_cq_tail, __ATOMIC_ACQUIRE) - hp_ur_cqhead;
        }
        hp_ur_cur = &hp_ur_rings[0];
        long to_submit = (long)(hp_ur_sqtail -
            __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE));
        /* With no ready work, ring 0 owns the bounded wait for the pass --
         * unless the flush above already produced completions to drain. */
        long wait_us = hp_ur_batch_us > 0 ? hp_ur_batch_us : 1000;
        long min_complete = hp_ur_batch > 0 ? hp_ur_batch : 1;
        if (posted) { wait_us = 0; min_complete = 0; }
        hp_ur_enter(to_submit, min_complete, (int)wait_us);
    } else if (ring0_wait) {
        /* The Rust loop does not block when a request/read queue or CQ work
         * already exists. Flush deferred task work and submissions, then
         * drain the ready CQEs below without a completion-count wait. */
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            if (!hp_ur_ring_needs_enter(rg)) continue;
            hp_ur_cur = rg;
            long to_submit = (long)(rg->sqtail -
                __atomic_load_n(rg->sq_head, __ATOMIC_ACQUIRE));
            if (rg->defer_taskrun)
                hp_ur_enter(to_submit > 0 ? to_submit : 0, 0, 0);
            else
                hp_ur_submit_only(to_submit > 0 ? to_submit : 0);
        }
    } else {
    int epfd = -1;
    if (hp_ur_nrings > 1 || hp_ur_ep_conns > 0) epfd = hp_ur_ep_setup();
    if (epfd >= 0) {
        /* Submit on every ring without blocking (this also flushes
         * DEFER_TASKRUN work), then block on the epoll set that watches all
         * ring fds and any overflow connections. */
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            if (!hp_ur_ring_needs_enter(rg)) continue;
            hp_ur_cur = rg;
            long to_submit = (long)(hp_ur_sqtail -
                __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE));
            if (rg->defer_taskrun)
                hp_ur_enter(to_submit > 0 ? to_submit : 0, 0, 0);
            else
                hp_ur_submit_only(to_submit > 0 ? to_submit : 0);
        }
        int ep_to = timeout_ms;
        if (have > 0) ep_to = 0;
        else if (hp_ur_busy) {
            ep_to = (int)(hp_ur_batch_us / 1000);
            if (ep_to < 1) ep_to = 1;
        }
        struct epoll_event evs[256];
        int n = epoll_wait(epfd, evs, 256, ep_to);
        for (int i = 0; i < n; i++) {
            int efd = evs[i].data.fd;
            int ring_event = 0;
            for (int r = 0; r < hp_ur_nrings; r++) {
                if (efd == hp_ur_rings[r].fd) {
                    cq_ready[r] = 1;
                    ring_event = 1;
                    break;
                }
                if (hp_ur_rings[r].efd >= 0 && efd == hp_ur_rings[r].efd) {
                    /* level-triggered: clear the counter we just consumed */
                    uint64_t v;
                    while (read(efd, &v, sizeof v) == (ssize_t)sizeof v)
                        ;
                    cq_ready[r] = 1;
                    ring_event = 1;
                    break;
                }
            }
            if (!ring_event) hp_fdlist_push(in, (hp_sock)efd);
        }
        /* A readiness wake often represents only one completion. Collect a
         * bounded burst before returning to the application, unless queued
         * application/recovery work already needs service. Deferred rings
         * must then publish task work that arrived during the collection. */
        if (hp_ur_epoll_batch_us && (n > 0 || have > 0) &&
            !recovery_dispatched &&
            hp_ur_readq_total == 0 &&
            hp_ur_answerq_head == hp_ur_answerq_tail) {
            for (int pass = 0; pass < 2; pass++) {
                for (int r = 0; r < hp_ur_nrings; r++) {
                    struct hp_uring_ring *rg = &hp_ur_rings[r];
                    if (!hp_ur_ring_needs_enter(rg)) continue;
                    hp_ur_cur = rg;
                    long pending = (long)(rg->sqtail -
                        __atomic_load_n(rg->sq_head, __ATOMIC_ACQUIRE));
                    hp_ur_enter(pending > 0 ? pending : 0, 0, -1);
                }
                if (pass == 0) {
                    uint32_t ready = 0;
                    for (int r = 0; r < hp_ur_nrings; r++)
                        ready += __atomic_load_n(hp_ur_rings[r].cq_tail,
                                   __ATOMIC_ACQUIRE) - hp_ur_rings[r].cqhead;
                    if (ready >= (uint32_t)(hp_ur_batch > 1 ? hp_ur_batch : 64))
                        break;
                    struct timespec pause = {0, (long)hp_ur_epoll_batch_us * 1000L};
                    nanosleep(&pause, NULL);
                }
            }
        }
    } else {
        /* One ring, no epoll: one enter submits and waits, as before. */
        hp_ur_cur = &hp_ur_rings[0];
        long to_submit = (long)(hp_ur_sqtail -
            __atomic_load_n(hp_ur_sq_head, __ATOMIC_ACQUIRE));
        int min = 1;
        long wait_us = (long)timeout_ms * 1000;
        if (have > 0) { min = 0; wait_us = 0; }
        else if (hp_ur_busy) { min = (int)hp_ur_batch; wait_us = hp_ur_batch_us; }
        hp_ur_enter(to_submit, min, wait_us);
    }
    }

    for (unsigned int i = 0; i < nrings; i++) {
        int r = (int)((drain_start + i) % nrings);
        hp_ur_cur = &hp_ur_rings[r];
        if (!cq_ready[r] &&
            __atomic_load_n(hp_ur_cq_tail, __ATOMIC_ACQUIRE) != hp_ur_cqhead)
            cq_ready[r] = 1;
        if (cq_ready[r]) drained |= hp_ur_drain_ring(in);
        /* The ring-0 multishot POLL_ADD fires on every eventfd signal, so the
         * counter only needs resetting for rings that posted CQEs this pass,
         * and one non-semaphore read returns and clears the whole counter.
         * Draining every ring until EAGAIN cost up to two syscalls per ring
         * per pass (mostly EAGAIN) without changing any wakeup. */
        if ((ring0_wait || (hp_ur_ring_wake && r > 0)) &&
            cq_ready[r] && hp_ur_rings[r].efd >= 0) {
            uint64_t v;
            if (read(hp_ur_rings[r].efd, &v, sizeof v) < 0) { /* EAGAIN: already clear */ }
        }
    }
    if (hp_ur_drain_rr && nrings > 1)
        hp_ur_drain_start = (drain_start + 1) % nrings;
    hp_ur_arm_all_accepts();
    hp_ur_cur = &hp_ur_rings[0];
    hp_ur_busy = drained;
    /* an fd whose SQE push hit a full ring is parked in the retry ring and
     * re-armed here, after the CQE drain freed slots up. In the common case
     * the ring is empty and this costs nothing; the rare overflow flag
     * falls back to one full fd-table scan. */
    /* Only try as many parked fds as the SQ ring can take right now. With a
     * full ring every pop would just re-park its entry, which turns this
     * into an O(parked) loop on every cycle -- and the kernel caps the SQ at
     * 32768 entries while the box holds up to ~61k connections. Entries we
     * do not reach stay queued for the next cycle. */
    /* Bound the work per cycle; each push checks its own ring's SQ space,
     * and a failed push simply re-parks the handle for the next cycle. */
    uint32_t budget = 4096;
    while (hp_ur_rethead != hp_ur_rettail && budget > 0) {
        budget--;
        uint32_t i = hp_ur_retry[hp_ur_rethead];
        hp_ur_rethead = (hp_ur_rethead + 1) & (hp_ur_retcap - 1);
        hp_sock h = (hp_sock)i;
        int rr = hp_ur_ring_of(h);
        hp_ur_cur = &hp_ur_rings[rr < 0 ? 0 : rr];
        struct hp_urfds *st = hp_ur_st_of(h);
        if (!st) continue;
        if (!st->rtagged) continue;              /* already handled */
        st->rtagged = 0;
        if (st->reof) continue;
        if (st->closeafter) {
            if (st->shead && !st->sarmed && !hp_ur_arm_send_head(h))
                hp_ur_retfd(h);
            continue;
        }
        if (st->listener) {
            bool ok = (hp_ur_direct == 1)
                ? hp_uring_arm_accept(h, rr < 0 ? 0 : rr)
                : hp_uring_arm_poll(h);
            if (!ok) hp_ur_retfd(h);
            continue;
        }
        if (st->shead && !st->sarmed && !hp_ur_arm_send_head(h))
            hp_ur_retfd(h);
        if (budget == 0) { hp_ur_retfd(h); continue; }   /* recv next cycle */
        if (st->rh && !st->rready && !st->rarmed &&
            !st->rreadq && !st->rreading) {
            if (!hp_uring_arm_recv(h, hp_ur_rlen())) {
                /* leave it parked for the next cycle instead of re-parking
                 * and popping it again in this same loop */
                hp_ur_retfd(h);
                break;
            }
        }
    }
    int read_drained = hp_ur_run_readq(in);
    hp_ur_busy |= read_drained;
    if (hp_ur_answerq_head != hp_ur_answerq_tail) hp_ur_busy = 1;
    /* deferred closes: the send chain drained (or the peer is gone for
     * good) -> close the fd for real. Entries live until then, bounded by
     * the deadline so a stalled peer cannot pin descriptors forever. */
    if (hp_ur_closecnt) {
        uint32_t n = 0;
        int64_t now = (int64_t)time(NULL);
        for (uint32_t k = 0; k < hp_ur_closecnt; k++) {
            uint32_t i = hp_ur_closing[k];
            struct hp_urfds *st = hp_ur_st_of((hp_sock)i);
            if (!st || !st->ctagged) continue;
            if (st->closeafter && !st->shead) {
                hp_ur_finish_close((hp_sock)i);
                continue;
            }
            if (!st->closeafter) { st->ctagged = 0; continue; }
            if (now >= st->cdeadline) {
                hp_ur_finish_close((hp_sock)i);
                continue;
            }
            hp_ur_closing[n++] = i;
        }
        hp_ur_closecnt = n;
    }
    if (hp_ur_pushfail) {
        hp_ur_pushfail = 0;
        hp_ur_cur = &hp_ur_rings[0];
        for (uint32_t i = 0; i < hp_ur_fdscap; i++)
            hp_ur_rescan_one((hp_sock)i);
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            for (uint32_t i = 0; i < rg->dircap; i++) {
                if (!rg->dirs[i].direct) continue;
                hp_ur_rescan_one((hp_sock)(HP_UR_HANDLE_BASE +
                    (hp_sock)r * (hp_sock)HP_UR_SLOTS_PER_RING + (hp_sock)i));
            }
        }
        hp_ur_cur = &hp_ur_rings[0];
        hp_ur_rethead = hp_ur_rettail = 0;
    }
    (void)out;
}
#endif /* HP_UR_CAN */



#ifdef HP_UR_CAN
static int hp_uring_active(void);
static void hp_ur_free_fd(hp_sock fd);
static hstr *hp_uring_take_recv(hp_sock fd, size_t want);
static int64_t hp_uring_send2(hp_sock fd, hstr *a, hstr *b);
static int64_t hp_uring_send_owned(hp_sock fd, char *buf, size_t off, size_t len, size_t cap);
#endif

hval hpbi_epoll_add(hval s) {
    hp_net_init();
#if !defined(_WIN32)
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_bool(false);
#ifdef HP_UR_CAN
    if (hp_uring_maybe_setup()) {
        /* direct handles are indexed in hp_ur_dirs, not in the real-fd table */
        if (!hp_ur_is_direct(fd)) hp_ur_fdgrow(fd);
        struct hp_urfds *st = hp_ur_st_of(fd);
        if (st) {
            /* The listener flag is recorded when the socket is created; only
             * an fd the runtime does not track pays the getsockopt probe
             * (one syscall per accepted connection would otherwise). */
            hp_sockrec *sr = sockrec((intptr_t)fd, false);
            int lt = 0;
            if (sr) {
                lt = sr->listener ? 1 : 0;
            } else {
                socklen_t accsl = sizeof lt;
                getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &lt, &accsl);
            }
            st->listener = (lt == 1);
            if (hp_ur_dbg)
                fprintf(stderr, "[ur] add fd=%d listener=%d\n", (int)fd, st->listener);
            if (st->listener) {
                /* Register every member of a reuseport listener group first.
                 * The event loop arms the bounded accept queues after startup,
                 * so the first ring can distribute them across the full group. */
                if (hp_ur_direct == 1) {
                    if (!hp_ur_listener_add(fd))
                        return hp_of_bool(false);
                } else {
                    hp_uring_arm_poll(fd);
                }
            } else {
                hp_uring_add_conn(fd);
            }
        }
        return hp_of_bool(true);
    }
#endif
    if (g_epfd < 0) g_epfd = epoll_create1(0);
    if (g_epfd < 0) return hp_of_bool(false);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) != 0 && errno != EEXIST)
        return hp_of_bool(false);
#endif
    return hp_of_bool(true);
}

hval hpbi_epoll_del(hval s) {
#if !defined(_WIN32)
    hp_sock fd = sock_of(s);
#ifdef HP_UR_CAN
    if (hp_uring_active()) {
        /* A direct descriptor is released by stream_close: epoll_del runs
         * first in the app's close path, so closing here would race the
         * queued response. */
        if (!hp_ur_is_direct(fd)) hp_ur_free_fd(fd);
        return hp_of_bool(true);
    }
#endif
    if (fd != HP_SOCK_BAD && g_epfd >= 0)
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL);
#endif
    return hp_of_bool(true);
}

/* epoll_mod($fd, $mask): 1 = readable, 2 = writable (or both). */
hval hpbi_epoll_mod(hval s, hval mask) {
    hp_net_init();
#if !defined(_WIN32)
    hp_sock fd = sock_of(s);
#ifdef HP_UR_CAN
    if (hp_uring_active()) return hp_of_bool(true);
#endif
    if (fd == HP_SOCK_BAD || g_epfd < 0) return hp_of_bool(false);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    int m = (int)hp_val_to_int(mask);
    if (m & 1) ev.events |= EPOLLIN;
    if (m & 2) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &ev) != 0) return hp_of_bool(false);
#endif
    return hp_of_bool(true);
}

static void hp_epoll_wait_fdlist(HpFdList *in, HpFdList *out, int to) {
    hp_net_init();
#if !defined(_WIN32)
#ifdef HP_UR_CAN
    if (hp_uring_maybe_setup()) {
        hp_uring_wait(in, out, to);
        return;
    }
#endif
    if (g_epfd < 0) return;
    static _Thread_local struct epoll_event evs[1024];
    int n = epoll_wait(g_epfd, evs, 1024, to);
    for (int i = 0; i < n; i++) {
        unsigned int e = evs[i].events;
        if (e & (EPOLLIN | EPOLLERR | EPOLLHUP)) hp_fdlist_push(in, (hp_sock)evs[i].data.fd);
        if (e & EPOLLOUT) hp_fdlist_push(out, (hp_sock)evs[i].data.fd);
    }
#endif
}

/* epoll_wait2($readable, $writable, $timeoutMs): fills both lists. */
hval hpbi_epoll_wait2(hval inv, hval outv, hval timeout_ms) {
    harr *in = (inv.tag == HV_ARR) ? inv.u.a : NULL;
    harr *out = (outv.tag == HV_ARR) ? outv.u.a : NULL;
    if (in) { in->len = 0; in->next_index = 0; in->is_map = false; }
    if (out) { out->len = 0; out->next_index = 0; out->is_map = false; }
    int to = (int)hp_val_to_int(timeout_ms);
    if (to < 0) to = 0;
    HpFdList ri = { .values = in, .ints = NULL };
    HpFdList wi = { .values = out, .ints = NULL };
    hp_epoll_wait_fdlist(&ri, &wi, to);
    return hp_of_bool(true);
}

static bool hp_epoll_wait_iarr2_impl(int64_t inv, int64_t outv, int to) {
    hp_iarr_t *in = (hp_iarr_t *)(intptr_t)inv;
    hp_iarr_t *out = (hp_iarr_t *)(intptr_t)outv;
    if (in && in->leased) in->len = 0;
    if (out && out->leased) out->len = 0;
    if (to < 0) to = 0;
    HpFdList ri = { .values = NULL, .ints = in };
    HpFdList wi = { .values = NULL, .ints = out };
    hp_epoll_wait_fdlist(&ri, &wi, to);
    return true;
}
hval hpbi_epoll_wait_iarr2(hval inv, hval outv, hval timeout_ms) {
    return hp_of_bool(hp_epoll_wait_iarr2_impl(
        hp_val_to_int(inv), hp_val_to_int(outv), (int)hp_val_to_int(timeout_ms)));
}
hval hpbi_epoll_wait_iarr2_safe(hval inv, hval outv, hval timeout_ms) {
    if (inv.tag != HV_INT || outv.tag != HV_INT || inv.u.i == 0 || outv.u.i == 0)
        return hp_of_bool(false);
    return hpbi_epoll_wait_iarr2(inv, outv, timeout_ms);
}
bool hp_epoll_wait_iarr2_t(int64_t in, int64_t out, int64_t timeout_ms) {
    return hp_epoll_wait_iarr2_impl(in, out, (int)timeout_ms);
}

/* stream_send_nb($fd, $data): one send(2). Returns the bytes written, -1 if
 * the socket would block, -2 on error. The caller buffers the remainder and
 * asks for EPOLLOUT; the loop must never sleep on a slow peer. */
hval hpbi_stream_send_nb(hval s, hval data) {
    hp_net_init();
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_int(-2);
    hstr *d = hp_val_to_str(data);
    if (d->len == 0) return hp_of_int(0);
#ifdef HP_UR_CAN
    if (hp_uring_active() && hp_ur_is_direct(fd)) {
        /* direct descriptors cannot be written with send(2); queue the bytes
         * like stream_send2_nb does */
        char *buf = (char *)malloc(d->len);
        if (!buf) return hp_of_int(-2);
        memcpy(buf, d->data, d->len);
        return hp_of_int(hp_uring_send_owned(fd, buf, 0, d->len, d->len));
    }
#endif
    int r = (int)send(fd, d->data, (int)d->len, 0);
    if (r >= 0) return hp_of_int(r);
    if (hp_sock_wouldblock()) return hp_of_int(-1);
    return hp_of_int(-2);
}

/* epoll_wait_into($out, $timeoutMs): clear $out, fill it with ready fds. */
hval hpbi_epoll_wait_into(hval outv, hval timeout_ms) {
    hp_net_init();
    harr *out = (outv.tag == HV_ARR) ? outv.u.a : NULL;
    if (!out) return hp_of_arr(hp_arr_new());
    out->len = 0;
    out->next_index = 0;
    out->is_map = false;
#if !defined(_WIN32)
    if (g_epfd < 0) return hp_of_arr(out);
    static _Thread_local struct epoll_event evs[1024];
    int to = (int)hp_val_to_int(timeout_ms);
    if (to < 0) to = 0;
    int n = epoll_wait(g_epfd, evs, 1024, to);
    for (int i = 0; i < n; i++) {
        if (evs[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP))
            hp_arr_push(out, hp_of_int(evs[i].data.fd));
    }
#endif
    return hp_of_arr(out);
}









/* PHP-style callable dispatch: any hval may hold a closure. */


/* stream_send_sb(fd, sb): hand the finished builder buffer straight to the
 * send path without copying it -- the ring send chunk adopts the malloc'd
 * buffer, so a response costs zero payload copies. Contract:
 *   n >= 0 : bytes accepted, builder consumed and its buffer now owned by
 *            the runtime (freed after the send) / in fallback mode fully
 *            drained.
 *   -2     : fatal (peer gone, no room, OOM); buffer freed; close the fd. */
static int64_t hp_send_sb_impl(hp_sock fd, HpSb *sb) {
    if (!sb) return -2;
    char *buf = sb->b.data;
    size_t len = sb->b.len;
    size_t cap = sb->b.cap;
    size_t head = buf ? sb->head : 0;
    sb->b.data = NULL;
    sb->b.len = sb->b.cap = 0;
    sb->head = 0;
    hp_free(sb);         /* the builder handle is consumed: only the buffer
                            travels on (it was leaked before: ~55 B/request) */
    if (!buf) {                                   /* empty builder: send "" */
        buf = (char *)malloc(1);
        if (!buf) return -2;
        cap = 0;
    }
#ifdef HP_UR_CAN
    if (hp_uring_active()) return hp_uring_send_owned(fd, buf, head, len, cap);
#endif
    size_t off = head;
    int waited_ms = 0;
    while (off < len) {
        ssize_t r = send(fd, buf + off, (int)(len - off), 0);
        if (r > 0) { off += (size_t)r; continue; }
        if (r < 0 && hp_sock_wouldblock() && waited_ms < 2000) {
            sock_sleep_ms(1);                     /* mirrors stream_send */
            waited_ms++;
            continue;
        }
        if (r < 0) sockrec((intptr_t)fd, true)->eof = true;
        hp_resp_buf_release(buf, cap);
        return -2;
    }
    hp_resp_buf_release(buf, cap);
    return (int64_t)(len - head);
}
hval hpbi_stream_send_sb(hval s, hval h) {
    hp_net_init();
    return hp_of_int(hp_send_sb_impl(sock_of(s), sb_h(h)));
}
int64_t hp_stream_send_nb_t(int64_t fd, hstr *d) {
    if (d->len == 0) return 0;
    int r = (int)send((hp_sock)fd, d->data, (int)d->len, 0);
    if (r >= 0) return r;
    if (hp_sock_wouldblock()) return -1;
    return -2;
}
int64_t hp_stream_send2_nb_t(int64_t fd, hstr *x, hstr *y) {
#ifdef HP_UR_CAN
    if (hp_uring_active()) return hp_uring_send2((hp_sock)fd, x, y);
#endif
#if defined(_WIN32)
    size_t n = x->len + y->len;
    char *tmp = malloc(n ? n : 1);
    if (!tmp) return -2;
    memcpy(tmp, x->data, x->len);
    memcpy(tmp + x->len, y->data, y->len);
    int r = (int)send((hp_sock)fd, tmp, (int)n, 0);
    free(tmp);
    if (r >= 0) return r;
    return hp_sock_wouldblock() ? -1 : -2;
#else
    struct iovec iov[2];
    iov[0].iov_base = x->data;
    iov[0].iov_len = x->len;
    iov[1].iov_base = y->data;
    iov[1].iov_len = y->len;
    ssize_t r = writev((hp_sock)fd, iov, 2);
    if (r >= 0) return (int64_t)r;
    return hp_sock_wouldblock() ? -1 : -2;
#endif
}
int64_t hp_stream_send_sb_t(int64_t fd, int64_t hb) {
    return hp_send_sb_impl((hp_sock)fd, (HpSb *)(intptr_t)hb);
}
bool hp_epoll_mod_t(int64_t fd, int64_t mask) {
#if !defined(_WIN32)
    if (fd < 0 || g_epfd < 0) return false;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    if (mask & 1) ev.events |= EPOLLIN;
    if (mask & 2) ev.events |= EPOLLOUT;
    ev.data.fd = (int)fd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, (int)fd, &ev) != 0) return false;
#endif
    return true;
}
int64_t hp_nofile_limit_t(void) {
#if defined(_WIN32)
    return 0;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 0;
    return (int64_t)rl.rlim_cur;
#endif
}
/* Whether the io_uring backend actually came up. Generic: a program can size
 * its own connection table to the backend's real capacity. */
bool hp_uring_active_t(void) {
#ifdef HP_UR_CAN
    return hp_uring_active();
#else
    return false;
#endif
}
hval hpbi_uring_active(void) { return hp_of_bool(hp_uring_active_t()); }

/* Whether direct descriptors (ring-owned file table) are in use. Generic: a
 * program can tell whether the real-fd limit still applies to it. */
bool hp_uring_direct_t(void) {
#ifdef HP_UR_CAN
    return hp_ur_direct_on() != 0;
#else
    return false;
#endif
}
hval hpbi_uring_direct(void) { return hp_of_bool(hp_uring_direct_t()); }

/* Number of direct-descriptor slots actually registered (0 when off). */
int64_t hp_uring_direct_slots_t(void) {
#ifdef HP_UR_CAN
    if (!hp_ur_direct_on()) return 0;
    int64_t total = 0;
    for (int r = 0; r < hp_ur_nrings; r++)
        total += (int64_t)hp_ur_rings[r].dircap;
    return total;
#else
    return 0;
#endif
}
hval hpbi_uring_direct_slots(void) {
    return hp_of_int(hp_uring_direct_slots_t());
}

/* ---- shutdown diagnostics ------------------------------------------------
 * One stderr line at exit (normal return or SIGTERM/SIGINT) with the counters
 * that explain a benchmark run: requests, io_uring enters (so enter/request
 * shows whether batching engaged), parked SQEs and failed arms. The line is
 * written with write(2) and a manual integer formatter, so it is safe from a
 * signal handler and costs nothing before shutdown. HPHP_STATS=0 disables it.
 * Programs that never served a request print nothing, which keeps normal CLI
 * stderr clean. */
static void hp_stats_put(const char *s, size_t n) {
#if !defined(_WIN32)
    ssize_t ignored = write(2, s, n);
    (void)ignored;
#else
    (void)s; (void)n;
#endif
}
static void hp_stats_num(uint64_t v) {
    char b[24];
    size_t i = sizeof b;
    if (v == 0) { hp_stats_put("0", 1); return; }
    while (v && i) { b[--i] = (char)('0' + (int)(v % 10)); v /= 10; }
    hp_stats_put(b + i, sizeof b - i);
}
static void hp_stats_emit(void) {
    if (!hp_st[HP_ST_REQ] && !hp_st[HP_ST_ACCEPT] && !hp_st[HP_ST_ENTER])
        return;                    /* no server traffic: stay quiet */
    static const struct { const char *name; int idx; } f[] = {
        {"reqs", HP_ST_REQ}, {"enter", HP_ST_ENTER},
        {"sqe", HP_ST_SQE_SUB}, {"cqe", HP_ST_CQE},
        {"cqe_recv", HP_ST_CQE_RECV}, {"cqe_send", HP_ST_CQE_SEND},
        {"arm_fail", HP_ST_ARM_FAIL}, {"parked", HP_ST_SQE_PARK},
        {"close", HP_ST_CLOSE}, {"accept", HP_ST_ACCEPT},
    };
    hp_stats_put("[hphp]", 6);
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        hp_stats_put(" ", 1);
        hp_stats_put(f[i].name, strlen(f[i].name));
        hp_stats_put("=", 1);
        hp_stats_num(hp_st[f[i].idx]);
    }
    hp_stats_put(" readq=", 7);
    {
        uint64_t enq = 0, done = 0;
#ifdef HP_UR_CAN
        enq = hp_ur_readq_enqueues;
        done = hp_ur_readq_completions;
#endif
        hp_stats_num(enq);
        hp_stats_put(" bounded_recv=", 14);
        hp_stats_num(done);
    }
    hp_stats_put("\n", 1);
}
#if !defined(_WIN32)
static void hp_stats_sig(int sig) {
    hp_stats_emit();
    signal(sig, SIG_DFL);
#if defined(HPHP_PGO_FLUSH)
    /* GCC's profile counters are normally flushed by atexit. During an
     * instrumented benchmark, preserve the signal-derived exit code while
     * allowing that flush to run; ordinary builds keep the original signal
     * termination semantics. */
    const char *flush = getenv("HPHP_PGO_FLUSH");
    if (flush && flush[0] == '1' && flush[1] == '\0') exit(128 + sig);
#endif
    raise(sig);                    /* keep the normal exit status/semantics */
}
#endif
static void hp_stats_atexit(void) { hp_stats_emit(); }
#if defined(__GNUC__)
__attribute__((constructor))
#endif
static void hp_stats_install(void) {
    const char *e = getenv("HPHP_STATS");
    if (e && e[0] == '0') return;
#if !defined(_WIN32)
    signal(SIGTERM, hp_stats_sig);
    signal(SIGINT, hp_stats_sig);
#endif
    atexit(hp_stats_atexit);
}

/* runtime_stats(): JSON snapshot of the process counters (see hphp_rt.h).
 * Generic introspection, like nofile_limit(): a program (or the operator) can
 * see syscalls/allocations per request. Key order == HP_ST index order. */
static const char *const hp_stat_names[HP_ST_N] = {
    "reqs",
    "enter", "sqe_submitted", "sqe_pushed", "sqe_parked",
    "cqe", "cqe_recv", "cqe_send", "cqe_poll",
    "arm_recv", "arm_send", "arm_poll", "arm_fail",
    "close", "accept", "send_chunk", "str_new", "scope_end",
};
hval hpbi_runtime_stats(void) {
    return hp_of_str(hp_runtime_stats_t());
}
hstr *hp_runtime_stats_t(void) {
    char tmp[1024];
    Buf b;
    hbuf_init(&b);
    hbuf_write(&b, "{\"live_allocs\":", 15);
    snprintf(tmp, sizeof tmp, "%lld", (long long)hp_live_allocs());
    hbuf_write(&b, tmp, strlen(tmp));
    for (int i = 0; i < HP_ST_N; i++) {
        snprintf(tmp, sizeof tmp, "%s\"%s\":%llu", ",",
                 hp_stat_names[i], (unsigned long long)hp_st[i]);
        hbuf_write(&b, tmp, strlen(tmp));
    }
#ifdef HP_UR_CAN
    snprintf(tmp, sizeof tmp,
             ",\"recv_recovery\":{\"enqueued\":%llu,\"completed\":%llu}",
             (unsigned long long)hp_ur_readq_enqueues,
             (unsigned long long)hp_ur_readq_completions);
    hbuf_write(&b, tmp, strlen(tmp));
        snprintf(tmp, sizeof tmp,
             ",\"queues\":{\"circular\":%s,\"ring0_busy_aware\":%s,\"read_depth\":%u,\"read_max\":%u,\"read_moved\":%llu,\"answer_depth\":%u,\"answer_max\":%u,\"answer_moved\":%llu}",
             hp_ur_circular_queues ? "true" : "false",
             hp_ur_ring0_busy_aware ? "true" : "false",
             hp_ur_readq_total, hp_ur_readq_max,
             (unsigned long long)hp_ur_readq_moved,
             hp_ur_answerq_tail - hp_ur_answerq_head, hp_ur_answerq_max,
             (unsigned long long)hp_ur_answerq_moved);
    hbuf_write(&b, tmp, strlen(tmp));
    if (hp_ur_direct == 1) {
        hbuf_write(&b, ",\"rings\":[", 10);
        for (int r = 0; r < hp_ur_nrings; r++) {
            struct hp_uring_ring *rg = &hp_ur_rings[r];
            snprintf(tmp, sizeof tmp,
                     "%s{\"id\":%d,\"live\":%u,\"accepts\":%u,\"full\":%s,\"slots\":%u"
                     ",\"enter_calls\":%llu,\"enter_ns\":%llu,\"enter_max_ns\":%llu"
                     ",\"submit_only\":%llu,\"submit_ns\":%llu"
                     ",\"drain_cqes\":%llu,\"drain_ns\":%llu"
                     ",\"accept_cqes\":%llu,\"recv_cqes\":%llu,\"send_cqes\":%llu,\"poll_cqes\":%llu"
                     ",\"recv_to_app_ns\":%llu,\"recv_to_app_n\":%llu,\"recv_to_app_max_ns\":%llu"
                     ",\"answerq_wait_ns\":%llu,\"answerq_wait_n\":%llu,\"answerq_wait_max_ns\":%llu"
                     ",\"send_complete_ns\":%llu,\"send_complete_n\":%llu,\"send_complete_max_ns\":%llu}",
                     r ? "," : "", r, rg->live, rg->accept_armed,
                     rg->accept_full ? "true" : "false", rg->dircap,
                     (unsigned long long)rg->diag_enters,
                     (unsigned long long)rg->diag_enter_ns,
                     (unsigned long long)rg->diag_enter_max_ns,
                     (unsigned long long)rg->diag_submit_only,
                     (unsigned long long)rg->diag_submit_ns,
                     (unsigned long long)rg->diag_drain_cqes,
                     (unsigned long long)rg->diag_drain_ns,
                     (unsigned long long)rg->diag_accept_cqes,
                     (unsigned long long)rg->diag_recv_cqes,
                     (unsigned long long)rg->diag_send_cqes,
                     (unsigned long long)rg->diag_poll_cqes,
                     (unsigned long long)rg->diag_recv_to_app_ns,
                     (unsigned long long)rg->diag_recv_to_app_count,
                     (unsigned long long)rg->diag_recv_to_app_max_ns,
                     (unsigned long long)rg->diag_answerq_wait_ns,
                     (unsigned long long)rg->diag_answerq_wait_count,
                     (unsigned long long)rg->diag_answerq_wait_max_ns,
                     (unsigned long long)rg->diag_send_complete_ns,
                     (unsigned long long)rg->diag_send_complete_count,
                     (unsigned long long)rg->diag_send_complete_max_ns);
            hbuf_write(&b, tmp, strlen(tmp));
        }
        hbuf_write(&b, "]", 1);
        if (hp_diag_enabled()) {
            uint64_t now = hp_diag_now_ns();
            hbuf_write(&b, ",\"outstanding\":[", 16);
            for (int r = 0; r < hp_ur_nrings; r++) {
                struct hp_uring_ring *rg = &hp_ur_rings[r];
                uint32_t sends = 0, unarmed_sends = 0, reads = 0, missing_recv = 0;
                uint64_t send_age = 0, read_age = 0;
                for (uint32_t i = 0; rg->dirs && i < rg->dircap; i++) {
                    struct hp_urfds *st = &rg->dirs[i];
                    if (!st->direct || !st->slot_live) continue;
                    if (st->shead) {
                        sends++;
                        if (!st->sarmed) unarmed_sends++;
                        uint64_t *stamp = hp_ur_chunk_diag_ns(st->shead);
                        if (stamp && *stamp && now >= *stamp && now - *stamp > send_age)
                            send_age = now - *stamp;
                    }
                    if (st->rready) {
                        reads++;
                        uint64_t stamp = rg->diag_recv_ready_ns ? rg->diag_recv_ready_ns[i] : 0;
                        if (stamp && now >= stamp && now - stamp > read_age)
                            read_age = now - stamp;
                    }
                    if (!st->reof && !st->closeafter && !st->rarmed &&
                        !st->rreading && !st->rreadq && !st->rtagged && !st->rready)
                        missing_recv++;
                }
                snprintf(tmp, sizeof tmp,
                         "%s{\"id\":%d,\"sends\":%u,\"unarmed_sends\":%u,\"send_max_age_ns\":%llu,\"buffered_reads\":%u,\"read_max_age_ns\":%llu,\"missing_recv\":%u,\"sq_flags\":%u,\"sq_pending\":%u,\"cq_pending\":%u,\"defer_taskrun\":%d}",
                         r ? "," : "", r, sends, unarmed_sends,
                         (unsigned long long)send_age, reads,
                         (unsigned long long)read_age, missing_recv,
                         rg->sq_flags ? __atomic_load_n(rg->sq_flags, __ATOMIC_ACQUIRE) : 0,
                         rg->sqtail - __atomic_load_n(rg->sq_head, __ATOMIC_ACQUIRE),
                         __atomic_load_n(rg->cq_tail, __ATOMIC_ACQUIRE) - rg->cqhead,
                         rg->defer_taskrun);
                hbuf_write(&b, tmp, strlen(tmp));
            }
            hbuf_write(&b, "]", 1);
        }
    }
#endif
    hbuf_write(&b, "}", 1);
    char *p = hbuf_take(&b);
    hstr *r = hp_str_new(p, strlen(p));
    free(p);
    return r;
}

hval hpbi_runtime_diag_write(hval message) {
    if (!hp_diag_enabled()) return hp_of_bool(false);
    hstr *s = hp_val_to_str(message);
    if (!s) return hp_of_bool(false);
    size_t n = s->len;
    if (n > 8192) n = 8192;
    size_t wrote = fwrite(s->data, 1, n, stderr);
    if (n == 0 || s->data[n - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
    return hp_of_bool(wrote == n);
}

/* ---- typed fast paths for the request/parse and SQLite calls ---- */

int64_t hp_strlen_t(hstr *s) { return (int64_t)s->len; }
hstr *hp_strtolower_t(hstr *s) { return hp_str_tolower(s); }
hstr *hp_trim_t(hstr *s) { return hp_str_trim(s); }
hstr *hp_substr_t(hstr *x, int64_t st, int64_t l) {
    st = st < 0 ? st + (int64_t)x->len : st;
    if (st < 0) st = 0;
    if ((size_t)st > x->len) return hp_str_lit("");
    if (l < 0) l = (int64_t)x->len - st + l;
    if (l < 0) l = 0;
    return hp_str_slice(x, st, st + l);
}
hstr *hp_stream_recv_t(int64_t fd, int64_t want) {
    hp_net_init();
    if ((hp_sock)fd == HP_SOCK_BAD) return hp_str_lit("");
#ifdef HP_UR_CAN
    if (hp_uring_active()) {
        /* The ring owns and reuses its receive buffer. The language result
         * must own stable bytes after the next receive/re-arm. */
        return hp_str_copy(hp_uring_take_recv((hp_sock)fd,
                                               (size_t)(want > 0 ? want : 4096)));
    }
#endif
    if (want <= 0) want = 4096;
    if (want > (1 << 20)) want = (1 << 20);
    static _Thread_local char *g_rbuf = NULL;
    static _Thread_local size_t g_rbuf_cap = 0;
    if ((size_t)want > g_rbuf_cap) {
        char *nb = (char *)realloc(g_rbuf, (size_t)want);
        if (!nb) return hp_null_str();
        g_rbuf = nb;
        g_rbuf_cap = (size_t)want;
    }
    int r = recv((hp_sock)fd, g_rbuf, (int)want, 0);
    bool blocked = (r < 0) && hp_sock_wouldblock();
    if (r <= 0 && !blocked) sockrec((intptr_t)fd, true)->eof = true;
    if (r > 0) {
        hp_st[HP_ST_REQ]++;
        return hp_str_new(g_rbuf, (size_t)r);
    }
    return hp_str_lit("");
}
int64_t hp_stream_recv_release_t(int64_t fd) {
#ifdef HP_UR_CAN
    if (!hp_uring_active()) return 0;
    hp_ur_use_ring((hp_sock)fd);
    struct hp_urfds *st = hp_ur_st_of((hp_sock)fd);
    if (!st || !st->direct || !st->rmshot || !hp_ur_cur->pbuf ||
        st->rready || st->rlen != 0 || !st->rh)
        return 0;
    if (st->rborrowed) {
        uint32_t bid = st->rborrow_bid;
        st->rh = NULL;
        st->rcap = 0;
        st->rborrowed = 0;
        st->rborrow_bid = 0;
        hp_ur_pbuf_return(bid);
        return 1;
    }
    if (!hp_ur_recvpool_on()) return 0;
    hstr *h = st->rh;
    st->rh = NULL;
    st->rcap = 0;
    hp_ur_recvbuf_put(h);
    return 1;
#else
    (void)fd;
    return 0;
#endif
}
hval hpbi_stream_socket_server(hval addr) {
    return hp_stream_socket_server_impl(addr, 0);
}



hval hpbi_stream_socket_client(hval addr) {
    hp_net_init();
    char host[256], port[32];
    if (!sock_split_addr(hp_val_to_str(addr), host, sizeof host, port, sizeof port))
        return hp_of_bool(false);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host[0] ? host : "127.0.0.1", port, &hints, &res) != 0 || !res)
        return hp_of_bool(false);
    hp_sock s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == HP_SOCK_BAD) { freeaddrinfo(res); return hp_of_bool(false); }
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) { hp_sock_close(s); return hp_of_bool(false); }
    sock_nonblocking(s, true);
    sockrec((intptr_t)s, true);
    return hp_of_int((int64_t)(intptr_t)s);
}
hval hpbi_stream_socket_accept(hval srv) {
    hp_net_init();
    hp_sock s = sock_of(srv);
    if (s == HP_SOCK_BAD) return hp_of_bool(false);
    /* block up to 50 ms at a time so ctrl-c / ui ticks still get through */
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
#if defined(__linux__) && defined(SYS_accept4)
        /* one syscall: the accepted fd is already non-blocking (plain accept
         * plus two fcntl round trips would cost two more per connection) */
        hp_sock c = (hp_sock)syscall(SYS_accept4, s,
                                     (struct sockaddr *)&ss, &sl,
                                     (long)O_NONBLOCK);
#else
        hp_sock c = accept(s, (struct sockaddr *)&ss, &sl);
#endif
        if (c != HP_SOCK_BAD) {
#if !defined(__linux__) || !defined(SYS_accept4)
            sock_nonblocking(c, true);
#endif
            sockrec((intptr_t)c, true);
            g_accept_errno = 0;
            hp_st[HP_ST_ACCEPT]++;
            return hp_of_int((int64_t)(intptr_t)c);
        }
#if defined(_WIN32)
        g_accept_errno = WSAGetLastError();
#else
        g_accept_errno = errno;
#endif
        if (!hp_sock_wouldblock()) return hp_of_bool(false);
        sock_sleep_ms(5);
    }
}

#if defined(__linux__) && defined(SYS_io_uring_setup)
/* Defined with the io_uring backend further down; accept2 runs before it. */
static int hp_uring_active(void);
static int hp_ur_direct_on(void);
static hp_sock hp_ur_acceptq_pop(void);
static bool hp_ur_is_direct(hp_sock h);
static hstr *hp_uring_take_recv(hp_sock fd, size_t want);
static int64_t hp_uring_send2(hp_sock fd, hstr *a, hstr *b);
#endif

/* stream_socket_accept2(srv, timeoutMs): poll-then-accept, PHP-style.
 * Returns the client handle, or false when nothing arrived within the
 * timeout. Never blocks longer than timeoutMs (in 5 ms slices). */
hval hpbi_stream_socket_accept2(hval srv, hval timeout_ms) {
    hp_net_init();
    hp_sock s = sock_of(srv);
    if (s == HP_SOCK_BAD) return hp_of_bool(false);
#if defined(__linux__) && defined(SYS_io_uring_setup)
    if (hp_uring_active() && hp_ur_direct_on()) {
        /* multishot accept already installed the connections in the ring's
         * file table; hand the queued handles to the app */
        hp_sock h = hp_ur_acceptq_pop();
        if (h != HP_SOCK_BAD) {
            g_accept_errno = 0;
            return hp_of_int((int64_t)h);
        }
        return hp_of_bool(false);
    }
#endif
    int64_t waited = 0;
    int64_t limit = hp_val_to_int(timeout_ms);
    if (limit < 0) limit = 0;
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
#if defined(__linux__) && defined(SYS_accept4)
        hp_sock c = (hp_sock)syscall(SYS_accept4, s,
                                     (struct sockaddr *)&ss, &sl,
                                     (long)O_NONBLOCK);
#else
        hp_sock c = accept(s, (struct sockaddr *)&ss, &sl);
#endif
        if (c != HP_SOCK_BAD) {
#if !defined(__linux__) || !defined(SYS_accept4)
            sock_nonblocking(c, true);
#endif
            sockrec((intptr_t)c, true);
            g_accept_errno = 0;
            hp_st[HP_ST_ACCEPT]++;
            return hp_of_int((int64_t)(intptr_t)c);
        }
#if defined(_WIN32)
        g_accept_errno = WSAGetLastError();
#else
        g_accept_errno = errno;
#endif
        if (!hp_sock_wouldblock()) return hp_of_bool(false);
        if (waited >= limit) return hp_of_bool(false);   /* poll timed out */
        sock_sleep_ms(5);
        waited += 5;
    }
}

hval hpbi_stream_set_blocking(hval s, hval flag) {
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_bool(false);
    sock_nonblocking(fd, !hp_val_to_bool(flag));
    return hp_of_bool(true);
}

/* Disable Nagle on a connected socket: responses are small and the peer
 * waits for each one, so coalescing only adds latency. */
hval hpbi_stream_recv(hval s, hval maxn) {
    hp_net_init();
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_str(hp_str_lit(""));
    int64_t want = hp_val_to_int(maxn);
    if (want <= 0) want = 4096;
    if (want > (1 << 20)) want = (1 << 20);
    /* one reusable buffer per thread instead of a malloc/free per recv */
    static _Thread_local char *g_rbuf = NULL;
    static _Thread_local size_t g_rbuf_cap = 0;
    if ((size_t)want > g_rbuf_cap) {
        char *nb = (char *)realloc(g_rbuf, (size_t)want);
        if (!nb) return hp_of_str(hp_null_str());
        g_rbuf = nb;
        g_rbuf_cap = (size_t)want;
    }
#if defined(__linux__) && defined(SYS_io_uring_setup)
    if (hp_uring_active())
        return hp_of_str(hp_str_copy(hp_uring_take_recv(fd, (size_t)want)));
#endif
    char *buf = g_rbuf;
    int r = recv(fd, buf, (int)want, 0);
    bool blocked = (r < 0) && hp_sock_wouldblock();
    if (r <= 0 && !blocked)
        sockrec((intptr_t)fd, true)->eof = true;
    if (r > 0) {
        hp_st[HP_ST_REQ]++;
        hstr *out = hp_str_new(buf, (size_t)r);
        return hp_of_str(out);
    }
    if (r == 0) {                    /* orderly shutdown by the peer */
        sockrec((intptr_t)fd, true)->eof = true;
        return hp_of_str(hp_str_lit(""));
    }
    if (!blocked)                    /* hard error: treat as closed */
        sockrec((intptr_t)fd, true)->eof = true;
    return hp_of_str(hp_str_lit(""));
}
hval hpbi_stream_send(hval s, hval data) {
    hp_net_init();
    hp_sock fd = sock_of(s);
    if (fd == HP_SOCK_BAD) return hp_of_int(0);
    hstr *d = hp_val_to_str(data);
    size_t sent = 0;
    int waited_ms = 0;
    while (sent < d->len) {
        int r = send(fd, d->data + sent, (int)(d->len - sent), 0);
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && hp_sock_wouldblock() && waited_ms < 2000) {
            sock_sleep_ms(1);        /* let the peer drain; frames stay whole */
            waited_ms++;
            continue;
        }
        if (r < 0) sockrec((intptr_t)fd, true)->eof = true;
        break;
    }
    return hp_of_int((int64_t)sent);
}
hval hpbi_stream_ready(hval handles, hval timeout_ms) {
    hp_net_init();
    harr *out = hp_arr_new();
    if (handles.tag != HV_ARR || !handles.u.a) return hp_of_arr(out);
    int64_t n = hp_arr_len(handles.u.a);
    if (n <= 0) return hp_of_arr(out);
#ifdef _WIN32
    WSAPOLLFD *fds = (WSAPOLLFD *)calloc((size_t)n, sizeof(WSAPOLLFD));
#else
    struct pollfd *fds = (struct pollfd *)calloc((size_t)n, sizeof(struct pollfd));
#endif
    if (!fds) return hp_of_arr(out);
    int64_t nvalid = 0;
    for (int64_t i = 0; i < n; i++) {
        hval item = hp_arr_get(handles.u.a, hp_of_int(i));
        hp_sock fd = sock_of(item);
        if (fd == HP_SOCK_BAD) continue;
        fds[nvalid].fd = fd;
        fds[nvalid].events = POLLIN;
        nvalid++;
    }
    int to = (int)hp_val_to_int(timeout_ms);
    if (to < 0) to = 0;
    int r = 0;
    if (nvalid > 0) {
#ifdef _WIN32
        r = WSAPoll(fds, (ULONG)nvalid, to);
#else
        r = poll(fds, (nfds_t)nvalid, to);
#endif
    }
    if (r > 0) {
        int64_t k = 0;
        for (int64_t i = 0; i < n; i++) {
            hval item = hp_arr_get(handles.u.a, hp_of_int(i));
            hp_sock fd = sock_of(item);
            if (fd == HP_SOCK_BAD) continue;
            short rev = (short)fds[k++].revents;
            if (!rev) continue;
            /* NOTE: POLLHUP regularly arrives together with POLLIN when the
             * peer writes and closes immediately; the unread data is still
             * in the socket buffer. Do NOT mark the sticky eof flag here —
             * stream_recv stays the single authority for EOF (recv()==0). */
            if (rev & (POLLIN | POLLERR | POLLHUP))
                hp_arr_push(out, item);
        }
    }
    free(fds);
    return hp_of_arr(out);
}

hval hpbi_stream_eof(hval s) {
    hp_sockrec *rec = sockrec((intptr_t)sock_of(s), false);
    return hp_of_bool(rec ? rec->eof : true);
}

hval hpbi_stream_peer(hval s) {
    hp_sock fd = sock_of(s);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (fd == HP_SOCK_BAD || getpeername(fd, (struct sockaddr *)&ss, &sl) != 0)
        return hp_of_str(hp_str_lit(""));
    char host[128] = "", port[16] = "";
    if (getnameinfo((struct sockaddr *)&ss, sl, host, sizeof host, port, sizeof port,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return hp_of_str(hp_str_lit(""));
    char addr[160];
    snprintf(addr, sizeof addr, "%s:%s", host, port);
    return hp_of_str(hp_str_cstr(addr));
}

hval hpbi_stream_close(hval s) {
    hp_sock fd = sock_of(s);
    if (fd != HP_SOCK_BAD) hp_st[HP_ST_CLOSE]++;
#ifdef HP_UR_CAN
    if (hp_uring_active()) {
        struct hp_urfds *st = hp_ur_st_of(fd);
        /* a send is still in flight: hp_ur_free_fd defers the real close
         * until the kernel has taken the bytes (see hp_ur_finish_close) */
        if (st && st->closeafter) return hp_of_bool(true);
        hp_ur_free_fd(fd);
        if (st && st->closeafter) return hp_of_bool(true);
        if (hp_ur_is_direct(fd)) {
            /* the descriptor lives in the ring's table, not the fd table */
            sock_forget((intptr_t)fd);
            return hp_of_bool(true);
        }
    }
#endif
    if (fd == HP_SOCK_BAD) return hp_of_bool(false);
    hp_sock_graceful_close(fd);
    sock_forget((intptr_t)fd);
    return hp_of_bool(true);
}
static void hp_sock_graceful_close(hp_sock s) {
#ifdef _WIN32
    shutdown(s, SD_SEND);
    for (int i = 0; i < 8; i++) {           /* drain until nothing pends */
        fd_set rf;
        struct timeval tv = { 0, 2000 };    /* 2ms per round (Win rounds up) */
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        if (select(0, &rf, NULL, NULL, &tv) <= 0)
            break;                          /* nothing in flight: done */
        char tmp[4096];
        int r = recv(s, tmp, sizeof tmp, 0);
        if (r <= 0) break;                  /* FIN or error: done */
    }
    closesocket(s);
#else
    close(s);
#endif
}
