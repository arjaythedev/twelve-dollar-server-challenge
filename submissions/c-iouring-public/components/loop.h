/* io/loop.h (sha256 07b420a42635), developed separately for this entry. */
/*
 * loop.h - batch-oriented event loop for the cursed-http server.
 *
 * One wakeup -> one batch. The loop gathers every connection that received bytes
 * since the last wakeup and hands the application an array of (conn, bytes). The
 * application parses as many complete requests as it can from each entry (pipelining),
 * appends responses with loop_write()/loop_wbuf(), says how many bytes it consumed,
 * and returns. Unconsumed bytes (partial requests) are kept and re-presented,
 * prefixed to whatever arrives next.
 *
 * Group commit: responses written during a batch are *staged*. If the callback returns
 * LOOP_COMMITTED they are released at once; if it returns LOOP_DEFERRED they stay
 * staged until the application calls loop_commit(seq) with that batch's sequence
 * number (loop_batch_seq() inside the callback). Commits release batches in order.
 * After the callback the loop queues every released send and submits them all with
 * one io_uring_enter per ring (one kevent/write round on macOS).
 *
 * Backends: io_uring (Linux >= 6.1, raw syscalls, no liburing), epoll (Linux fallback when io_uring is
 * unavailable or forced) and kqueue (macOS dev).
 * Single-threaded: every function must be called from the loop's thread.
 */
#ifndef CURSED_LOOP_H
#define CURSED_LOOP_H

#include <stddef.h>
#include <stdint.h>

typedef struct loop loop_t;
typedef uint32_t loop_conn;              /* opaque handle; stable while the connection lives */

typedef struct {
    loop_conn conn;
    uint32_t len;                        /* bytes available at data */
    const char *data;                    /* valid only during the callback; followed by >= LOOP_SLACK readable
                                            bytes (unspecified contents) so SIMD parsers may over-read */
    uint32_t consumed;                   /* set by the app: bytes fully handled (0..len) */
    uint32_t qdelay_us;                  /* how long the oldest new bytes waited since the kernel received
                                            them (rx_timestamps) or since the loop reaped them (otherwise) */
} loop_in;

enum { LOOP_COMMITTED = 0, LOOP_DEFERRED = 1 };
enum { LOOP_RECV_AUTO = 0, LOOP_RECV_MULTISHOT = 1, LOOP_RECV_POLL = 2 };
#define LOOP_SLACK 64

typedef int  (*loop_batch_fn)(loop_t *, loop_in *in, int n, void *ud);
typedef void (*loop_tick_fn)(loop_t *, void *ud);   /* once per loop iteration, after the batch */

typedef struct {
    const char *host;                    /* default "0.0.0.0" */
    int port;                            /* default 80 */
    int listen_fd;                       /* >=0: use this listening socket instead */
    int backlog;                         /* default 4096 (somaxconn) */

    /* io_uring */
    unsigned rings;                      /* max rings, 0 = enough for max_conns (each <= RLIMIT_NOFILE); created on demand */
    unsigned max_conns;                  /* default 1<<20 (capped by rings * table size) */
    unsigned sq_entries;                 /* default 1024 (CQ = 4x) */
    unsigned bufs;                       /* provided recv buffers per ring, default 512 */
    unsigned buf_size;                   /* default 2048 */
    unsigned batch;                      /* wait-for-N target, default 128 */
    unsigned wait_us;                    /* max extra wait for N under load (us), default 2000 */
    int force_epoll;                     /* 1: use the epoll backend even if io_uring works (also env CURSED_EPOLL=1);
                                            io_uring setup failures (ENOSYS, EPERM, ...) fall back to epoll anyway */
    int recv_mode;                       /* LOOP_RECV_AUTO (0, = multishot), LOOP_RECV_MULTISHOT (1), LOOP_RECV_POLL (2):
                                            idle conns hold a multishot POLL_ADD, data via one-shot recv. Poll saves
                                            ~190 B/conn on 6.8 (~608 B on >= 6.10) but holds every FIN skb until user
                                            space reaps it: a mass close of 300k conns spiked +630 MB (massclose.sh).
                                            rx_timestamps implies multishot */
    int coop_taskrun;                    /* 1: COOP_TASKRUN instead of SINGLE_ISSUER|DEFER_TASKRUN */
    int accept_multishot;                /* 1: multishot accept while a ring has headroom (default: single-shot only) */
    int ring_wait_evfd;                  /* 1: rings 1..N-1 wake ring 0 via registered eventfds (default: ring 0 waits
                                            <= poll_wait_us and every ring is reaped each pass; eventfd wakeups are
                                            reported late on 6.8 with DEFER_TASKRUN) */
    unsigned poll_wait_us;               /* default 1000 */
    int legacy_bufs;                     /* 1: IORING_OP_PROVIDE_BUFFERS instead of a buffer ring (auto if PBUF_RING fails) */
    unsigned max_cqes;                   /* CQEs handled per ring per pass, default 2048 */
    unsigned accept_singles;             /* single-shot accepts kept in flight, default 256 */
    int send_zc;                         /* use SEND_ZC for responses >= send_zc bytes (0 = off) */
    int nodelay;                         /* TCP_NODELAY on the listener (inherited): 0 default on, -1 off */
    int defer_accept;                    /* TCP_DEFER_ACCEPT seconds, default 0 */
    int rcvbuf, sndbuf;                  /* SO_RCVBUF/SO_SNDBUF on the listener (inherited), 0 = kernel default */
    int rx_timestamps;                   /* 1: multishot RECVMSG + SO_TIMESTAMPNS, so qdelay_us includes socket queueing */
    int hugepages;                       /* 1: MADV_HUGEPAGE on the connection table and recv buffers */

    /* policy */
    unsigned max_in;                     /* max buffered unconsumed input per conn, default 64 KiB */
    unsigned out_hiwat;                  /* pause reading above this much unsent output, default 256 KiB */
    unsigned idle_timeout_s;             /* close idle keep-alives after this (0 = never), default 0 */
    unsigned stall_timeout_s;            /* close partial-request / stuck-send conns, default 30 */
    /* memory pressure. "available" = min(MemAvailable, cgroup headroom incl. inactive file pages);
     * TCP memory above the kernel's tcp_mem pressure threshold also counts as pressure. */
    /* Connection-cap policy (explore/conn-policy/POLICY.md): cap = (available - reserve) / conn_bytes at
     * startup, shrunk each second while available < reserve and recovered by 1%/s. Close hint engages at
     * 0.85 cap, disengages below 0.75 cap, f = (open - 0.75 cap) / (0.25 cap); f is frozen while the loop
     * is > 85% busy. At the cap, accepts stop (parking). */
    int conn_policy;                     /* 1: enable the cap policy */
    unsigned conn_cap;                   /* fixed cap instead of the memory-derived one (0 = derive) */
    unsigned conn_bytes;                 /* bytes per connection for the cap, default 4432 (4.4 KB + record) */
    unsigned reserve_mb;                 /* memory kept for everything else, default 250 */
    unsigned cap_ceiling;                /* never above this many open conns, default 380,000: the Xeon OOMs at ~450k
                                            sockets and the page cache drains from ~200k (ideas-lab exp42) */
    float policy_engage, policy_release; /* band as fractions of cap: engage at >= engage, release below release;
                                            f = (open - release*cap) / ((1 - release)*cap). Default 0.85 / 0.75 */
    int busy_source;                     /* CPU-guard signal: 0 auto (1 CPU online -> /proc/stat box; one-CPU affinity ->
                                            its cpuN line; cgroup quota <= 1.5 CPU -> cpu.stat/quota; else rusage),
                                            1 getrusage (process only), 2 /proc/stat whole box, 3 cgroup quota,
                                            4 the pinned CPU's line */
    int shed_headroom;                   /* 1: f is a static function of memory headroom, not of open/cap (conn-policy
                                            hband): f = 0 while headroom >= shed_hi_mb, rising linearly to shed_fmax at
                                            shed_lo_mb; no integrator, no CPU input, no cap ceiling or band parking */
    unsigned shed_hi_mb, shed_lo_mb;     /* defaults 400 / 200 MB (the 100 / 60 MB floors sit below) */
    float shed_fmax;                     /* default 0.5 */
    unsigned sock503_mbps, sock503_avail_mb; /* socket-memory pressure flag (loop_sock_pressure): rise per second / memory gate,
                                            default 50 / 450 MB; the app answers queued requests with 503 while it is set */
    unsigned press_pending, press_age_ms;    /* drain_first: also flag pressure (same memory gate) once the loop's own backlog
                                            (loop_backlog) exceeds this many requests or its oldest is older than this.
                                            Default 20000 / 300 ms */
    int drain_first;                     /* 1: under load, keep request bytes out of the kernel: copy recv data into compact
                                            per-conn buffers right away (provided buffers return at once), and hand the
                                            app at most drain_chunk entries per callback, entering every ring between
                                            chunks so pending recvs complete and sends go out. A request queued in a
                                            socket costs a ~4 KB skb; copied out it costs ~0.5 KB (arena 425k OOM) */
    unsigned drain_chunk;                /* default 256 */
    unsigned drain_budget;               /* entries per pass before draining stops (the pass must end), default 8192 */
    float guard_freeze, guard_relax;     /* CPU guard: freeze f above guard_freeze busy, relax below guard_relax.
                                            Default 0.92 / 0.85 */
    int backlog_parked_dynamic;          /* 1: while parked at the cap, listen() with a backlog of clamp(closes/s x
                                            50 ms, 256, backlog). Default 0 = keep `backlog` (4096) always: exp41 on
                                            the Xeon showed a permanent 4096 beats any shrink (p99.9 99 vs 406 ms) */
    int linger_close;                    /* Connection: close / loop_close: 0 (default) auto = linger only if input arrived
                                            after the close decision or is unconsumed (nginx `lingering_close on`),
                                            else plain server-first close; 1 always; -1 never. Lingering = SHUT_WR after
                                            the last send, keep the socket (discarding input) until the peer's EOF or
                                            linger_s, then close: no orphan, and pipelined requests aren't answered
                                            with RST. io_uring: FIXED_FD_INSTALL + shutdown() inline (6.8+, ~+1 us per
                                            close); older kernels fall back to IORING_OP_SHUTDOWN on io-wq (~+20 us,
                                            1 worker per ring). -1: plain close */
    unsigned linger_s;                   /* default 5 */
    unsigned mem_accept_floor_mb;        /* stop accepting below this (parking: SYNs then drop, 0 = off) */
    unsigned mem_shed_floor_mb;          /* RST the longest-idle keep-alives below this (0 = off) */
    unsigned shed_min_idle_s;            /* only shed conns idle at least this long, default 20 */
    int shed_fin;                        /* 1: shed with FIN instead of RST (SO_LINGER 0) */

    loop_batch_fn on_batch;
    loop_tick_fn on_tick;
    void *ud;
} loop_cfg;

typedef struct {
    uint64_t accepted, closed, batches, batch_items, recvs, sends, send_bytes;
    uint64_t enobufs, rearms, partial_sends, shed, refused_pause, enters, wakeups;
    uint64_t drains;                     /* drain_first: between-chunk ring drains */
    uint64_t file_ops;                   /* loop_file_submit ops completed */
    uint64_t rst, hint_closes, stranded;   /* stranded: recvs re-armed by the watchdog (should stay 0) */
    uint32_t open;
    uint32_t avail_mb;                   /* last memory reading */
    uint32_t tcp_mem_pages, tcp_mem_pressure;   /* /proc/net/sockstat TCP mem and tcp_mem[1] */
    uint32_t tcp_orphans;                /* /proc/net/sockstat TCP orphan (box-wide); limit tcp_max_orphans */
    float close_frac;                    /* current close-hint fraction */
    uint32_t cap;                        /* current connection cap (0 = policy off) */
    float busy;                          /* fraction of the last second spent on CPU (source: busy_src) */
    int busy_src;                        /* -1 whole box (/proc/stat), N pinned CPU N, -3 cgroup quota, -2 rusage */
    uint64_t listen_overflows, listen_drops;   /* TcpExt ListenOverflows / ListenDrops (box-wide, conn_policy only) */
    uint32_t sock_pressure;              /* 1 while socket-memory pressure is flagged */
    uint64_t sock_pressure_events;
    uint32_t backlog_now;                /* backlog currently applied to the listener */
    int accepting;
} loop_stats;

loop_t *loop_new(const loop_cfg *cfg);   /* NULL + errno/message on failure */
int  loop_run(loop_t *);                 /* runs until loop_stop(); returns 0 or -errno */
void loop_stop(loop_t *);
void loop_free(loop_t *);
const char *loop_backend(loop_t *);

/* --- inside the batch callback --- */
uint64_t loop_batch_seq(loop_t *);
int   loop_write(loop_t *, loop_conn, const void *p, size_t n);
char *loop_wbuf(loop_t *, loop_conn, size_t need);         /* writable space for >= need bytes */
void  loop_wcommit(loop_t *, loop_conn, size_t n);          /* n bytes of the last loop_wbuf are a response */
void  loop_close(loop_t *, loop_conn);                     /* close after released output is flushed (server-first FIN) */
int   loop_close_hint(loop_t *, loop_conn);                /* 1: this response should carry "Connection: close"
                                                              (then call loop_close). Ask only for a complete
                                                              response with no pipelined bytes after it. */
int   loop_sock_pressure(loop_t *);                       /* 1: socket buffers are filling with queued data; answer cheaply */
uint64_t loop_now_ns(loop_t *);                            /* CLOCK_MONOTONIC_COARSE at this pass */

/* --- file I/O on the loop's ring (e.g. a checkpoint driven as io_uring batches) ---
 * Ops on regular fds (never socket slots) go to io_uring ring 0 and complete asynchronously; `cb` runs on the
 * loop thread while completions are processed, with res = bytes or -errno, and may submit more ops or call
 * loop_commit. An op with link = 1 chains to the next op in the same array: the next one starts only if this
 * one succeeds (otherwise it completes with -ECANCELED), e.g. {write, write, fsync}. The whole array is
 * submitted in one go (a chain is never split across submissions), so n must be <= sq_entries.
 * On io_uring every file op carries IOSQE_ASYNC, so it runs on io-wq (capped at 1 bounded worker per ring),
 * never inline on the loop thread (linked follow-ups included). epoll/kqueue
 * fallbacks perform the ops synchronously and deliver the callbacks after the current batch. */
enum { LOOP_FOP_READ = 0, LOOP_FOP_WRITE = 1, LOOP_FOP_FSYNC = 2, LOOP_FOP_FDATASYNC = 3 };
typedef void (*loop_file_cb)(loop_t *, void *ud, int res);
typedef struct {
    int fd;
    uint8_t op;                          /* LOOP_FOP_* */
    uint8_t link;                        /* 1: the next op in the array runs only if this one succeeds */
    void *buf;                           /* READ/WRITE: must stay valid until the callback */
    uint32_t len;
    uint64_t off;
    loop_file_cb cb;                     /* may be NULL */
    void *ud;
} loop_fop;
int loop_file_submit(loop_t *, const loop_fop *ops, int n);   /* 0, or -errno (nothing submitted) */
/* Shutdown with file ops in flight: completions are only reaped while loop_run() runs, so drain first,
 * e.g. from on_tick: if (stopping && nothing_in_flight) loop_stop(L). */

/* Backlog of this pass (meaningful inside the batch callback; with drain_first, chunks drained later in the pass
 * accumulate here instead of in kernel socket buffers): requests reaped but not yet handed to the app after the
 * current chunk, and the age of the oldest of them (us, from its receive time). Both 0 when empty. */
void loop_backlog(loop_t *, uint32_t *pending, uint32_t *oldest_age_us);

/* --- any time on the loop thread --- */
void  loop_commit(loop_t *, uint64_t seq);                  /* release all staged output of batches <= seq */
void  loop_represent(loop_t *, loop_conn);                 /* re-present unconsumed input next pass, no new bytes needed */
unsigned loop_shed_idle(loop_t *, unsigned n);              /* close up to n idle keep-alive conns */
const loop_stats *loop_get_stats(loop_t *);

#endif
