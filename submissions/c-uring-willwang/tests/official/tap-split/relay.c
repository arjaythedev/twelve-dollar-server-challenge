/* Source-owned FIFO forwarding between private, single-queue Linux TAPs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <linux/virtio_net.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>
#include <poll.h>
#include <linux/capability.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>

enum {
    MAX_PORTS = 5,
    MAX_WORKERS = 5,
    DEPTH = 2048,
    FRAME = 131072,
    HEADER = sizeof(struct virtio_net_hdr),
    READ_BURST = 16,
    SEND_BURST = 32
};
_Static_assert((DEPTH & (DEPTH - 1)) == 0, "power-of-two FIFO");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal-safe stop");
static int ports[MAX_PORTS], count;
static uint64_t delay;
static _Atomic int stop;
static void finish(int signo) {
    (void)signo;
    atomic_store_explicit(&stop, 1, memory_order_relaxed);
}
static void fail(const char *what) {
    perror(what);
    exit(2);
}
static void check_thread(int rc, const char *what) {
    if (rc) {
        errno = rc;
        fail(what);
    }
}
static uint64_t nstime(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t))
        fail("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t now(void) {
    return nstime(CLOCK_MONOTONIC);
}
static int nsfd(const char *path, ino_t *ino) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    struct statfs fs;
    if (fd < 0 || fstat(fd, &st) || fstatfs(fd, &fs) || fs.f_type != NSFS_MAGIC ||
        ioctl(fd, NS_GET_NSTYPE) != CLONE_NEWNET)
        fail("private netns");
    *ino = st.st_ino;
    return fd;
}
static int tap(int napi) {
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    struct ifreq req = {.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR | IFF_TUN_EXCL};
    if (napi)
        req.ifr_flags |= IFF_NAPI;
    strcpy(req.ifr_name, "labtap");
    if (fd < 0 || ioctl(fd, TUNSETIFF, &req))
        fail("create private TAP");
    int bytes = HEADER;
    if (ioctl(fd, TUNSETVNETHDRSZ, &bytes) ||
        ioctl(fd, TUNSETOFFLOAD, TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6))
        fail("TAP offloads");
    return fd;
}
static unsigned targets(const unsigned char *frame, unsigned sender) {
    const unsigned char prefix[] = {0x02, 0x12, 0x0d, 0x00, 0x00};
    unsigned mask = (1u << count) - 1;
    if (!memcmp(frame, prefix, sizeof prefix) && frame[5] >= 1 && frame[5] <= count)
        mask = 1u << (frame[5] - 1);
    return mask & ~(1u << sender);
}
/* Each source has one writer for its local FIFO, including removal. Different
 * sources can inject complete Ethernet frames into the same TAP fd. A given
 * direction of a TCP flow always belongs to just one source. */
struct packet {
    uint64_t due;
    unsigned length, dest;
};
struct fifo {
    struct packet packets[DEPTH];
    unsigned sequence[DEPTH], available[DEPTH];
    unsigned head, size, allocated, returned;
    uint64_t retry_at;
    unsigned char *arena;
};
struct port_stats {
    uint64_t read, wire_bytes, written, write_drop, forwarded, queue_depth, queue_peak;
    uint64_t read_burst_full, send_burst_full, full_polls, write_retries, retry_expired;
    uint64_t service_bursts, service_frames, service_burst_max;
};
/* One server reader publishes slot IDs to each destination's SPSC queue. The
 * destination advances its head only after the actual TAP write. Only the
 * producer recycles buffers, after all destinations have returned their ID. */
enum { EGRESS_POOL = 8192, EGRESS_BURST = 64, PUBLISH_GROUP = 8, HIST = 6 };
struct lane_stats {
    uint64_t enqueued, delivered, written, dropped, queue_peak, full_polls, write_retries;
    uint64_t late50, late250, late1000, max_late, producer_wait_hist[HIST],
        delivery_late_hist[HIST];
};
struct lane {
    _Alignas(64) _Atomic unsigned published;
    _Alignas(64) _Atomic unsigned consumed;
    _Alignas(64) unsigned tail, reclaimed; /* producer-owned */
    _Alignas(64) unsigned head;            /* consumer-owned */
    uint64_t retry_at;
    _Alignas(64) unsigned ids[DEPTH];
};
struct shared_frame {
    uint64_t due;
    unsigned length, refs;
};
static struct lane lanes[MAX_PORTS];
static struct {
    struct shared_frame frames[EGRESS_POOL];
    unsigned free[EGRESS_POOL], returned, allocated, blocked, blocked_id, blocked_mask, dirty;
    unsigned char *arena;
} outbound;
static unsigned egress_owner[MAX_PORTS];

struct metrics {
    struct lane_stats lane[MAX_PORTS];
    uint64_t service_admitted, producer_late50, producer_late250, producer_late1000, pool_full,
        pool_peak;

    struct port_stats port[MAX_PORTS];
    uint64_t loops, idle_loops, busy_ns, idle_ns, thread_cpu_ns, timestamp;
    uint64_t queue_overflow, bad, gso, partial, forwarded, late50, late250, late1000, max_late;
};
struct worker {
    int cpu, tid;
    unsigned sources[MAX_PORTS], nsrc, destinations[MAX_PORTS], ndest;
    struct metrics local, published;
    pthread_t thread;
    pthread_mutex_t publish_lock;
};
static struct fifo fifos[MAX_PORTS];
static struct worker workers[MAX_WORKERS];
static unsigned nworkers;
static pthread_barrier_t prepared;
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static int go;
static void peak(uint64_t *val, uint64_t v) {
    if (v > *val)
        *val = v;
}
static void publish(struct worker *w, uint64_t stamp) {
    w->local.thread_cpu_ns = nstime(CLOCK_THREAD_CPUTIME_ID);
    w->local.timestamp = stamp;
    check_thread(pthread_mutex_lock(&w->publish_lock), "lock stats");
    w->published = w->local;
    check_thread(pthread_mutex_unlock(&w->publish_lock), "unlock stats");
}
static void init_fifo(struct fifo *q) {
    /* The LIFO credit is only committed on a valid frame. Usually just tens
     * of these maximum-sized slots acquire pages. read() can go straight
     * into the final buffer, including a full virtio GSO frame. */
    size_t bytes = (size_t)FRAME * DEPTH;
    q->arena = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (q->arena == MAP_FAILED)
        fail("relay arena");
    if (madvise(q->arena, bytes, MADV_NOHUGEPAGE))
        fail("relay arena huge-page policy");
}
static int read_source(struct worker *w, unsigned port) {
    struct fifo *q = &fifos[port];
    struct metrics *m = &w->local;
    struct port_stats *s = &m->port[port];
    unsigned frames = 0;
    for (; frames < READ_BURST; ++frames) {
        if (q->size == DEPTH) {
            s->full_polls++;
            break;
        }
        unsigned id = q->returned ? q->available[q->returned - 1] : q->allocated;
        unsigned char *bytes = q->arena + (size_t)FRAME * id;
        ssize_t len = read(ports[port], bytes, FRAME);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            fail("TAP read");
        }
        if (len < HEADER + 14) {
            ++m->bad;
            continue;
        }
        s->read++;
        s->wire_bytes += (uint64_t)len - HEADER;
        struct virtio_net_hdr hdr;
        memcpy(&hdr, bytes, HEADER);
        if (hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM)
            ++m->partial;
        if (hdr.gso_type != VIRTIO_NET_HDR_GSO_NONE)
            ++m->gso;
        if (((hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) &&
             (size_t)hdr.csum_start + hdr.csum_offset + 2 > (size_t)len - HEADER) ||
            ((size_t)len > HEADER + 1518 &&
             (hdr.gso_type == VIRTIO_NET_HDR_GSO_NONE || !hdr.gso_size))) {
            ++m->bad;
            continue;
        }
        unsigned dest = targets(bytes + HEADER, port);
        if (!dest)
            continue;
        if (q->returned)
            --q->returned;
        else
            ++q->allocated;
        struct packet *p = &q->packets[id];
        p->dest = dest;
        p->length = (unsigned)len;
        p->due = now() + delay;
        q->sequence[(q->head + q->size) & (DEPTH - 1)] = id;
        ++q->size;
        s->queue_depth = q->size;
        peak(&s->queue_peak, q->size);
    }
    if (frames == READ_BURST)
        ++s->read_burst_full;
    return frames != 0;
}
static int send_due(struct worker *w, unsigned port) {
    struct fifo *q = &fifos[port];
    struct metrics *m = &w->local;
    struct port_stats *s = &m->port[port];
    unsigned handled = 0, service = 0;
    while (q->size && handled < SEND_BURST) {
        unsigned id = q->sequence[q->head];
        struct packet *p = &q->packets[id];
        uint64_t stamp = now();
        if (stamp < p->due || stamp < q->retry_at)
            break;
        unsigned dest = p->dest;
        int blocked = 0;
        while (dest) {
            unsigned out = (unsigned)__builtin_ctz(dest), bit = 1u << out;
            dest &= ~bit;
            ssize_t rc = write(ports[out], q->arena + (size_t)FRAME * id, p->length);
            if (rc == (ssize_t)p->length) {
                ++m->port[out].written;
                if (out == 0)
                    ++service;
                p->dest &= ~bit;
                continue;
            }
            if (rc < 0 && errno == EINTR) {
                blocked = 1;
                continue;
            }
            if (rc < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ENOMEM)) {
                ++s->write_retries;
                if (stamp - p->due < 50000000) {
                    q->retry_at = stamp + 5000;
                    blocked = 1;
                    continue;
                }
                ++s->retry_expired;
            } else if (!(rc < 0 && (errno == ENETDOWN || errno == EIO))) {
                fail("TAP write");
            }
            ++m->port[out].write_drop;
            p->dest &= ~bit;
        }
        if (blocked)
            break;
        /* All destinations consumed this oldest source frame. Measure the
         * true completion, including a retry or a delayed write syscall. */
        uint64_t late = now() - p->due;
        m->late50 += late > 50000;
        m->late250 += late > 250000;
        m->late1000 += late > 1000000;
        peak(&m->max_late, late);
        ++m->forwarded;
        ++s->forwarded;
        ++handled;
        q->head = (q->head + 1) & (DEPTH - 1);
        --q->size;
        q->available[q->returned++] = id;
        s->queue_depth = q->size;
        q->retry_at = 0;
    }
    if (handled == SEND_BURST && q->size)
        ++s->send_burst_full;
    if (service) {
        ++s->service_bursts;
        s->service_frames += service;
        peak(&s->service_burst_max, service);
    }
    return handled != 0 || service != 0;
}
static void init_outbound(void) {
    size_t bytes = (size_t)FRAME * EGRESS_POOL;
    outbound.arena = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (outbound.arena == MAP_FAILED || madvise(outbound.arena, bytes, MADV_NOHUGEPAGE))
        fail("service frame arena");
}
static unsigned hist_bucket(uint64_t ns) {
    if (ns <= 10000)
        return 0;
    if (ns <= 50000)
        return 1;
    if (ns <= 100000)
        return 2;
    if (ns <= 250000)
        return 3;
    return ns <= 1000000 ? 4 : 5;
}
static void publish_service(void) {
    unsigned mask = outbound.dirty;
    outbound.dirty = 0;
    while (mask) {
        unsigned port = (unsigned)__builtin_ctz(mask);
        mask &= mask - 1;
        struct lane *l = &lanes[port];
        atomic_store_explicit(&l->published, l->tail, memory_order_release);
    }
}
static unsigned reclaim_service(void) {
    unsigned reclaimed = 0;
    for (unsigned port = 1; port < (unsigned)count; port++) {
        struct lane *l = &lanes[port];
        unsigned end = atomic_load_explicit(&l->consumed, memory_order_acquire);
        while (l->reclaimed != end) {
            unsigned id = l->ids[l->reclaimed++ & (DEPTH - 1)];
            if (!--outbound.frames[id].refs)
                outbound.free[outbound.returned++] = id;
            ++reclaimed;
        }
    }
    return reclaimed;
}
static int handoff(struct worker *w, unsigned id, unsigned mask) {
    struct metrics *m = &w->local;
    for (unsigned dst = mask; dst; dst &= dst - 1) {
        unsigned port = (unsigned)__builtin_ctz(dst);
        struct lane *l = &lanes[port];
        if (l->tail - l->reclaimed == DEPTH) {
            ++m->lane[port].full_polls;
            return 0;
        }
    }
    struct shared_frame *p = &outbound.frames[id];
    p->refs = (unsigned)__builtin_popcount(mask);
    uint64_t stamp = now(), wait = stamp - (p->due - delay),
             late = stamp > p->due ? stamp - p->due : 0;
    unsigned bucket = hist_bucket(wait);
    for (unsigned dst = mask; dst; dst &= dst - 1) {
        unsigned port = (unsigned)__builtin_ctz(dst);
        struct lane *l = &lanes[port];
        l->ids[l->tail++ & (DEPTH - 1)] = id;
        struct lane_stats *s = &m->lane[port];
        ++s->enqueued;
        ++s->producer_wait_hist[bucket];
        peak(&s->queue_peak, l->tail - l->reclaimed);
    }
    outbound.dirty |= mask;
    ++m->service_admitted;
    ++m->forwarded;
    ++m->port[0].forwarded;
    m->producer_late50 += late > 50000;
    m->producer_late250 += late > 250000;
    m->producer_late1000 += late > 1000000;
    m->late50 += late > 50000;
    m->late250 += late > 250000;
    m->late1000 += late > 1000000;
    peak(&m->max_late, late);
    return 1;
}
static int read_service(struct worker *w) {
    struct metrics *m = &w->local;
    struct port_stats *s = &m->port[0];
    unsigned released = reclaim_service(), frames = 0;
    if (outbound.blocked) {
        if (!handoff(w, outbound.blocked_id, outbound.blocked_mask)) {
            ++s->full_polls;
            return released != 0;
        }
        outbound.blocked = 0;
    }
    for (; frames < EGRESS_BURST; ++frames) {
        if (!outbound.returned && outbound.allocated == EGRESS_POOL) {
            ++m->pool_full;
            ++s->full_polls;
            break;
        }
        unsigned id = outbound.returned ? outbound.free[outbound.returned - 1] : outbound.allocated;
        unsigned char *bytes = outbound.arena + (size_t)FRAME * id;
        ssize_t len = read(ports[0], bytes, FRAME);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            fail("server TAP read");
        }
        if (len < HEADER + 14) {
            ++m->bad;
            continue;
        }
        ++s->read;
        s->wire_bytes += (uint64_t)len - HEADER;
        struct virtio_net_hdr hdr;
        memcpy(&hdr, bytes, HEADER);
        if (hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM)
            ++m->partial;
        if (hdr.gso_type != VIRTIO_NET_HDR_GSO_NONE)
            ++m->gso;
        if (((hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) &&
             (size_t)hdr.csum_start + hdr.csum_offset + 2 > (size_t)len - HEADER) ||
            ((size_t)len > HEADER + 1518 &&
             (hdr.gso_type == VIRTIO_NET_HDR_GSO_NONE || !hdr.gso_size))) {
            ++m->bad;
            continue;
        }
        unsigned mask = targets(bytes + HEADER, 0);
        if (!mask)
            continue;
        if (outbound.returned)
            --outbound.returned;
        else
            ++outbound.allocated;
        struct shared_frame *p = &outbound.frames[id];
        p->length = (unsigned)len;
        p->due = now() + delay;
        if (!handoff(w, id, mask)) {
            outbound.blocked = 1;
            outbound.blocked_id = id;
            outbound.blocked_mask = mask;
            ++s->full_polls;
            break;
        }
        if ((frames & (PUBLISH_GROUP - 1)) == PUBLISH_GROUP - 1)
            publish_service();
    }
    publish_service();
    s->queue_depth = outbound.allocated - outbound.returned;
    peak(&s->queue_peak, s->queue_depth);
    peak(&m->pool_peak, s->queue_depth);
    if (frames == EGRESS_BURST)
        ++s->read_burst_full;
    return frames != 0 || released != 0 || outbound.blocked;
}
static int deliver_service(struct worker *w, unsigned port) {
    struct lane *l = &lanes[port];
    struct metrics *m = &w->local;
    struct lane_stats *s = &m->lane[port];
    unsigned available = atomic_load_explicit(&l->published, memory_order_acquire);
    unsigned n = 0;
    while (l->head != available && n < SEND_BURST) {
        unsigned id = l->ids[l->head & (DEPTH - 1)];
        struct shared_frame *p = &outbound.frames[id];
        uint64_t stamp = now();
        if (stamp < p->due || stamp < l->retry_at)
            break;
        ssize_t rc = write(ports[port], outbound.arena + (size_t)FRAME * id, p->length);
        if (rc == (ssize_t)p->length) {
            ++m->port[port].written;
            ++s->written;
        } else {
            if (rc < 0 && errno == EINTR)
                break;
            if (rc < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ENOMEM)) {
                ++s->write_retries;
                if (stamp - p->due < 50000000) {
                    l->retry_at = stamp + 5000;
                    break;
                }
            } else if (!(rc < 0 && (errno == ENETDOWN || errno == EIO)))
                fail("client TAP injection");
            ++s->dropped;
            ++m->port[port].write_drop;
        }
        uint64_t late = now() - p->due;
        ++s->delivery_late_hist[hist_bucket(late)];
        s->late50 += late > 50000;
        s->late250 += late > 250000;
        s->late1000 += late > 1000000;
        peak(&s->max_late, late);
        ++s->delivered;
        m->late50 += late > 50000;
        m->late250 += late > 250000;
        m->late1000 += late > 1000000;
        peak(&m->max_late, late);
        ++m->forwarded;
        ++l->head;
        ++n;
        l->retry_at = 0;
    }
    if (n)
        atomic_store_explicit(&l->consumed, l->head, memory_order_release);
    return n != 0;
}

static void *run_worker(void *arg) {
    struct worker *w = arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(w->cpu, &set);
    check_thread(pthread_setaffinity_np(pthread_self(), sizeof(set), &set), "pin relay worker");
    w->tid = (int)syscall(SYS_gettid);
    for (unsigned n = 0; n < w->nsrc; n++) {
        if (w->sources[n])
            init_fifo(&fifos[w->sources[n]]);
        else
            init_outbound();
    }
    publish(w, now());
    int barrier = pthread_barrier_wait(&prepared);
    if (barrier != PTHREAD_BARRIER_SERIAL_THREAD)
        check_thread(barrier, "worker ready");
    check_thread(pthread_mutex_lock(&start_lock), "start lock");
    while (!go)
        check_thread(pthread_cond_wait(&start_cond, &start_lock), "start wait");
    check_thread(pthread_mutex_unlock(&start_lock), "start unlock");
    unsigned rotate = 0, ds = 0;
    uint64_t prev = now(), next = prev + 50000000;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        int progress = 0;
        for (unsigned i = 0; i < w->ndest; i++) {
            unsigned pos = ds + i;
            if (pos >= w->ndest)
                pos -= w->ndest;
            progress |= deliver_service(w, w->destinations[pos]);
        }
        for (unsigned i = 0; i < w->nsrc; i++) {
            unsigned pos = rotate + i;
            if (pos >= w->nsrc)
                pos -= w->nsrc;
            unsigned port = w->sources[pos];
            if (!port) {
                progress |= read_service(w);
                continue;
            }
            progress |= send_due(w, port);
            progress |= read_source(w, port);
            progress |= send_due(w, port);
        }
        if (++rotate == w->nsrc)
            rotate = 0;
        if (++ds >= w->ndest)
            ds = 0;
        ++w->local.loops;
        uint64_t current = now();
        if (progress)
            w->local.busy_ns += current - prev;
        else {
            ++w->local.idle_loops;
            w->local.idle_ns += current - prev;
        }
        prev = current;
        if (current >= next) {
            publish(w, current);
            next = current + 50000000;
        }
    }
    publish(w, now());
    return NULL;
}
static void add_port(struct port_stats *sum, const struct port_stats *p) {
#define ADD_PORT(x) sum->x += p->x
    ADD_PORT(read);
    ADD_PORT(wire_bytes);
    ADD_PORT(written);
    ADD_PORT(write_drop);
    ADD_PORT(forwarded);
    ADD_PORT(queue_depth);
    peak(&sum->queue_peak, p->queue_peak);
    ADD_PORT(read_burst_full);
    ADD_PORT(send_burst_full);
    ADD_PORT(full_polls);
    ADD_PORT(write_retries);
    ADD_PORT(retry_expired);
    ADD_PORT(service_bursts);
    ADD_PORT(service_frames);
    peak(&sum->service_burst_max, p->service_burst_max);
#undef ADD_PORT
}
static void add_lane(struct lane_stats *, const struct lane_stats *);
static void add_metrics(struct metrics *sum, const struct metrics *m) {
#define ADD_METRIC(x) sum->x += m->x
    ADD_METRIC(loops);
    ADD_METRIC(idle_loops);
    ADD_METRIC(busy_ns);
    ADD_METRIC(idle_ns);
    ADD_METRIC(thread_cpu_ns);
    ADD_METRIC(queue_overflow);
    ADD_METRIC(bad);
    ADD_METRIC(gso);
    ADD_METRIC(partial);
    ADD_METRIC(forwarded);
    ADD_METRIC(late50);
    ADD_METRIC(late250);
    ADD_METRIC(late1000);
    peak(&sum->max_late, m->max_late);
    peak(&sum->timestamp, m->timestamp);
    ADD_METRIC(service_admitted);
    ADD_METRIC(producer_late50);
    ADD_METRIC(producer_late250);
    ADD_METRIC(producer_late1000);
    ADD_METRIC(pool_full);
    peak(&sum->pool_peak, m->pool_peak);
    for (int p = 0; p < count; p++) {
        add_port(&sum->port[p], &m->port[p]);
        add_lane(&sum->lane[p], &m->lane[p]);
    }
#undef ADD_METRIC
}
static void field(FILE *f, const char *k, uint64_t n) {
    fprintf(f, ",\"%s\":%" PRIu64, k, n);
}
static void add_lane(struct lane_stats *a, const struct lane_stats *v) {
    a->enqueued += v->enqueued;
    a->delivered += v->delivered;
    a->written += v->written;
    a->dropped += v->dropped;
    peak(&a->queue_peak, v->queue_peak);
    a->full_polls += v->full_polls;
    a->write_retries += v->write_retries;
    a->late50 += v->late50;
    a->late250 += v->late250;
    a->late1000 += v->late1000;
    peak(&a->max_late, v->max_late);
    for (unsigned i = 0; i < HIST; i++) {
        a->producer_wait_hist[i] += v->producer_wait_hist[i];
        a->delivery_late_hist[i] += v->delivery_late_hist[i];
    }
}
static void histogram(FILE *f, const char *key, const uint64_t *values) {
    fprintf(f, ",\"%s\":[", key);
    for (unsigned i = 0; i < HIST; i++)
        fprintf(f, "%s%" PRIu64, i ? "," : "", values[i]);
    fputc(']', f);
}
static void report_handoffs(FILE *f, const struct metrics *m) {
    fputs(",\"handoffs\":[", f);
    for (unsigned port = 1; port < (unsigned)count; port++) {
        const struct lane_stats *s = &m->lane[port];
        struct lane *l = &lanes[port];
        unsigned before, after, tail;
        do {
            before = atomic_load_explicit(&l->consumed, memory_order_acquire);
            tail = atomic_load_explicit(&l->published, memory_order_acquire);
            after = atomic_load_explicit(&l->consumed, memory_order_acquire);
        } while (before != after || tail - before > DEPTH);
        fprintf(f, "%s{\"source\":0,\"destination\":%u,\"producer_cpu\":%d,\"consumer_cpu\":%d",
                port == 1 ? "" : ",", port, workers[0].cpu, workers[egress_owner[port]].cpu);
        field(f, "enqueued", s->enqueued);
        field(f, "delivered", s->delivered);
        field(f, "written", s->written);
        field(f, "dropped", s->dropped);
        field(f, "queue_depth", tail - before);
        field(f, "queue_peak", s->queue_peak);
        field(f, "full_polls", s->full_polls);
        field(f, "write_retries", s->write_retries);
        field(f, "late_over_50us", s->late50);
        field(f, "late_over_250us", s->late250);
        field(f, "late_over_1000us", s->late1000);
        field(f, "max_late_ns", s->max_late);
        histogram(f, "producer_wait_hist", s->producer_wait_hist);
        histogram(f, "delivery_late_hist", s->delivery_late_hist);
        fputc('}', f);
    }
    fputc(']', f);
}

static void fifo_fields(FILE *f, const struct port_stats *p) {
    field(f, "queue_depth", p->queue_depth);
    field(f, "queue_peak", p->queue_peak);
    field(f, "full_polls", p->full_polls);
    field(f, "read_burst_full", p->read_burst_full);
    field(f, "send_burst_full", p->send_burst_full);
    field(f, "write_retries", p->write_retries);
    field(f, "retry_expired", p->retry_expired);
    field(f, "forwarded", p->forwarded);
    field(f, "service_bursts", p->service_bursts);
    field(f, "service_frames", p->service_frames);
    field(f, "service_burst_max", p->service_burst_max);
}
static void common_fields(FILE *f, const struct metrics *m) {
    struct port_stats all = {0};
    for (int p = 0; p < count; p++)
        add_port(&all, &m->port[p]);
    field(f, "loops", m->loops);
    field(f, "idle_loops", m->idle_loops);
    field(f, "busy_ns", m->busy_ns);
    field(f, "idle_ns", m->idle_ns);
    field(f, "thread_cpu_ns", m->thread_cpu_ns);
    field(f, "snapshot_monotonic_ns", m->timestamp);
    field(f, "queue_overflow", m->queue_overflow);
    field(f, "bad_frame", m->bad);
    field(f, "gso", m->gso);
    field(f, "partial_checksum", m->partial);
    field(f, "forwarded_events", m->forwarded);
    field(f, "late_over_50us", m->late50);
    field(f, "late_over_250us", m->late250);
    field(f, "late_over_1000us", m->late1000);
    field(f, "max_late_ns", m->max_late);
    fifo_fields(f, &all);
    field(f, "service_admitted", m->service_admitted);
    field(f, "service_pool_full", m->pool_full);
    field(f, "service_pool_peak", m->pool_peak);
}
static void report(const char *path) {
    struct metrics snapshots[MAX_WORKERS], totals = {0};
    for (unsigned i = 0; i < nworkers; i++) {
        struct worker *w = &workers[i];
        check_thread(pthread_mutex_lock(&w->publish_lock), "read stats");
        snapshots[i] = w->published;
        check_thread(pthread_mutex_unlock(&w->publish_lock), "finish reading stats");
        add_metrics(&totals, &snapshots[i]);
    }
    char tmp[1024];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        fail("stats path");
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        fail("stats file");
    FILE *f = fdopen(fd, "w");
    if (!f)
        fail("stats stream");
    fprintf(f,
            "{\"pid\":%d,\"cpu\":%d,\"reporter_cpu\":%d,\"uid\":%d,\"worker_count\":%u,"
            "\"queue_peak_kind\":\"largest_source_fifo\",\"fifo_capacity_per_source\":%d",
            getpid(), workers[0].cpu, sched_getcpu(), geteuid(), nworkers, DEPTH);
    /* Producer handoffs are a separately observable stage of each delivery. */
    totals.forwarded -= totals.service_admitted;
    totals.late50 -= totals.producer_late50;
    totals.late250 -= totals.producer_late250;
    totals.late1000 -= totals.producer_late1000;
    common_fields(f, &totals);
    report_handoffs(f, &totals);
    fputs(",\"ports\":[", f);
    for (int p = 0; p < count; p++) {
        const struct port_stats *s = &totals.port[p];
        fprintf(f, "%s{\"read\":%" PRIu64, p ? "," : "", s->read);
        field(f, "wire_bytes", s->wire_bytes);
        field(f, "written", s->written);
        field(f, "write_drop", s->write_drop);
        fifo_fields(f, s);
        fputc('}', f);
    }
    fputs("],\"workers\":[", f);
    for (unsigned i = 0; i < nworkers; i++) {
        struct worker *w = &workers[i];
        const char *role = nworkers == 1 ? "combined" : i == 0 ? "server-egress" : "client-ingress";
        fprintf(f, "%s{\"role\":\"%s\",\"cpu\":%d,\"tid\":%d,\"sources\":[", i ? "," : "", role,
                w->cpu, w->tid);
        for (unsigned j = 0; j < w->nsrc; j++)
            fprintf(f, "%s%u", j ? "," : "", w->sources[j]);
        fputs("],\"destinations\":[", f);
        for (unsigned j = 0; j < w->ndest; j++)
            fprintf(f, "%s%u", j ? "," : "", w->destinations[j]);
        fprintf(f, "],\"event_kind\":\"%s\"",
                nworkers == 1 ? "handoff_and_delivery"
                : i == 0      ? "handoff_enqueues"
                              : "deliveries");
        common_fields(f, &snapshots[i]);
        fputs(",\"fifos\":[", f);
        for (unsigned j = 0; j < w->nsrc; j++) {
            unsigned p = w->sources[j];
            fprintf(f, "%s{\"source\":%u", j ? "," : "", p);
            fifo_fields(f, &snapshots[i].port[p]);
            fputc('}', f);
        }
        fputs("]}", f);
    }
    fputs("]}\n", f);
    if (fclose(f) || rename(tmp, path))
        fail("commit relay stats");
}
static unsigned number(const char *s, unsigned maximum) {
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n > maximum || *s == '-') {
        errno = EINVAL;
        fail("invalid relay argument");
    }
    return (unsigned)n;
}
static void configure_workers(const char *list) {
    cpu_set_t allowed, set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(allowed), &allowed))
        fail("read permitted relay cores");
    int cpus[MAX_WORKERS];
    unsigned n = 0;
    char *copy = strdup(list);
    if (!copy)
        fail("CPUs");
    char *p = copy, *part;
    while ((part = strsep(&p, ",")) != NULL) {
        unsigned cpu = number(part, CPU_SETSIZE - 1);
        if (n == MAX_WORKERS || !CPU_ISSET(cpu, &allowed) || CPU_ISSET(cpu, &set)) {
            errno = EINVAL;
            fail("relay CPU list must be distinct permitted cores");
        }
        cpus[n++] = (int)cpu;
        CPU_SET(cpu, &set);
    }
    free(copy);
    if (!n) {
        errno = EINVAL;
        fail("relay CPU list");
    }
    if (sched_setaffinity(0, sizeof(set), &set))
        fail("pin reporter to external cores");
    nworkers = n < (unsigned)count ? n : (unsigned)count;
    workers[0].cpu = cpus[n - 1];
    for (unsigned i = 1; i < nworkers; i++)
        workers[i].cpu = cpus[i - 1];
    for (unsigned src = 0; src < (unsigned)count; src++) {
        unsigned id = !src || nworkers == 1 ? 0 : 1 + (src - 1) % (nworkers - 1);
        struct worker *w = &workers[id];
        w->sources[w->nsrc++] = src;
        if (src) {
            /* For four CPUs, move the fourth response lane off the worker
             * already reading two request lanes. */
            unsigned owner = id;
            if (nworkers > 2 && src > nworkers - 1)
                owner = 1 + id % (nworkers - 1);
            egress_owner[src] = owner;
            w = &workers[owner];
            w->destinations[w->ndest++] = src;
        }
    }
}
static void drop_privileges(uid_t uid, gid_t gid) {
    if (setgroups(0, NULL) || setresgid(gid, gid, gid) || setresuid(uid, uid, uid))
        fail("drop relay identity");
    struct __user_cap_header_struct ch = {.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    struct __user_cap_data_struct caps[2] = {{0}, {0}};
    if (syscall(SYS_capset, &ch, caps) || syscall(SYS_capget, &ch, caps) || caps[0].effective ||
        caps[1].effective || caps[0].permitted || caps[1].permitted || caps[0].inheritable ||
        caps[1].inheritable || getgroups(0, NULL) != 0 || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        fail("drop relay capabilities");
}
int main(int argc, char **argv) {
    /* uid gid delay-us stats-file cpu-list server-netns client-netns... */
    if (argc < 8 || argc > 11) {
        fprintf(stderr, "usage: relay UID GID DELAY_US STATS CPU_LIST SERVER_NS CLIENT_NS...\n");
        return 2;
    }
    uid_t uid = (uid_t)number(argv[1], UINT_MAX);
    gid_t gid = (gid_t)number(argv[2], UINT_MAX);
    if (!uid || !gid || uid == (uid_t)-1 || gid == (gid_t)-1)
        return 2;
    delay = (uint64_t)number(argv[3], 1000000) * 1000;
    count = argc - 6;
    configure_workers(argv[5]);
    ino_t inodes[MAX_PORTS], host;
    int original = nsfd("/proc/self/ns/net", &host);
    for (int p = 0; p < count; p++) {
        int ns = nsfd(argv[p + 6], &inodes[p]);
        if (inodes[p] == host)
            return 2;
        for (int j = 0; j < p; j++)
            if (inodes[j] == inodes[p])
                return 2;
        if (setns(ns, CLONE_NEWNET))
            fail("enter private netns");
        close(ns);
        ports[p] = tap(p == 0);
    }
    if (setns(original, CLONE_NEWNET))
        fail("restore control netns");
    close(original);
    drop_privileges(uid, gid);
    struct sigaction sa = {.sa_handler = finish};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL))
        fail("signals");
    sigset_t block, previous;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    check_thread(pthread_sigmask(SIG_BLOCK, &block, &previous), "protect worker shutdown");
    check_thread(pthread_barrier_init(&prepared, NULL, nworkers + 1), "ready barrier");
    for (unsigned i = 0; i < nworkers; i++) {
        struct worker *w = &workers[i];
        check_thread(pthread_mutex_init(&w->publish_lock, NULL), "stats mutex");
        check_thread(pthread_create(&w->thread, NULL, run_worker, w), "start relay worker");
    }
    int b = pthread_barrier_wait(&prepared);
    if (b != PTHREAD_BARRIER_SERIAL_THREAD)
        check_thread(b, "all workers ready");
    check_thread(pthread_sigmask(SIG_SETMASK, &previous, NULL), "receive controller signal");
    report(argv[4]);
    printf("READY %d\n", getpid());
    fflush(stdout);
    char token;
    if (read(STDIN_FILENO, &token, 1) != 1 || token != 'G')
        return 2;
    check_thread(pthread_mutex_lock(&start_lock), "release start");
    go = 1;
    check_thread(pthread_cond_broadcast(&start_cond), "start workers");
    check_thread(pthread_mutex_unlock(&start_lock), "release started workers");
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        /* Only the Python launcher owns the write end of the start pipe. A
         * sudo monitor may outlive it, so the relay follows the pipe lifetime. */
        struct pollfd controller = {.fd = STDIN_FILENO};
        int ready = poll(&controller, 1, 1000);
        if (ready < 0 && errno != EINTR)
            fail("watch controller pipe");
        if (ready > 0)
            atomic_store_explicit(&stop, 1, memory_order_relaxed);
        if (ready == 0 && !atomic_load_explicit(&stop, memory_order_relaxed))
            report(argv[4]);
    }
    for (unsigned i = 0; i < nworkers; i++)
        check_thread(pthread_join(workers[i].thread, NULL), "join relay worker");
    report(argv[4]);
    return 0;
}
