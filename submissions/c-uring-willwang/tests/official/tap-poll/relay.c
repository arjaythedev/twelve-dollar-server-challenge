/* A private Ethernet switch between Linux TAPs. The dataplane has no capabilities.
 * Its dedicated polling CPU never registers a waiter on the service's TX queue. */
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

enum { MAX_PORTS = 5, QUEUE = 8192, FRAME = 131072, HEADER = sizeof(struct virtio_net_hdr) };
struct slot {
    unsigned char *bytes;
    size_t capacity;
    unsigned length, dest;
    uint64_t due;
};
static struct slot slots[QUEUE];
static int ports[MAX_PORTS], count;
static uint64_t nread[MAX_PORTS], nwrite[MAX_PORTS], wrdrop[MAX_PORTS], onwire[MAX_PORTS];
static uint64_t queue_overflow, peak, idle, loops, gso, partial, bad, late_50us, late_250us;
static uint64_t head, tail, delay, max_late;
static volatile sig_atomic_t stop;
static void finish(int signo) {
    (void)signo;
    stop = 1;
}
static void fail(const char *what) {
    perror(what);
    exit(2);
}
static uint64_t now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
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
static int get_frames(unsigned port, unsigned char *buf) {
    int progress = 0;
    for (int burst = 0; burst < 16; burst++) {
        ssize_t nr = read(ports[port], buf, FRAME);
        if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (nr < 0 && errno == EINTR)
            continue;
        if (nr < HEADER + 14) {
            if (nr < 0)
                fail("TAP read");
            bad++;
            continue;
        }
        nread[port]++;
        onwire[port] += (uint64_t)nr - HEADER;
        progress = 1;
        struct virtio_net_hdr hdr;
        memcpy(&hdr, buf, HEADER);
        if (hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM)
            partial++;
        if (hdr.gso_type != VIRTIO_NET_HDR_GSO_NONE)
            gso++;
        if ((hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) &&
            (size_t)hdr.csum_start + hdr.csum_offset + 2 > (size_t)nr - HEADER) {
            bad++;
            continue;
        }
        if ((size_t)nr > HEADER + 1518 &&
            (hdr.gso_type == VIRTIO_NET_HDR_GSO_NONE || !hdr.gso_size)) {
            bad++;
            continue;
        }
        unsigned dest = targets(buf + HEADER, port);
        if (!dest)
            continue;
        if (tail - head >= QUEUE) {
            queue_overflow++;
            continue;
        }
        struct slot *s = &slots[tail++ % QUEUE];
        if (s->capacity < (size_t)nr) {
            size_t cap = ((size_t)nr + 255) & ~255UL;
            void *p = realloc(s->bytes, cap);
            if (!p)
                fail("relay allocation");
            s->bytes = p;
            s->capacity = cap;
        }
        memcpy(s->bytes, buf, nr);
        s->length = nr;
        s->dest = dest;
        s->due = now() + delay;
        if (tail - head > peak)
            peak = tail - head;
    }
    return progress;
}
static int send_due(void) {
    int progress = 0;
    while (head < tail) {
        struct slot *s = &slots[head % QUEUE];
        uint64_t ts = now();
        if (s->due > ts)
            break;
        uint64_t late = ts - s->due;
        if (late > max_late)
            max_late = late;
        late_50us += late > 50000;
        late_250us += late > 250000;
        for (int i = 0; i < count; i++)
            if (s->dest & (1u << i)) {
                ssize_t nw;
                do
                    nw = write(ports[i], s->bytes, s->length);
                while (nw < 0 && errno == EINTR && !stop);
                if (nw == s->length)
                    nwrite[i]++;
                else if (nw < 0 && (errno == EAGAIN || errno == ENOBUFS || errno == ENETDOWN ||
                                    errno == EIO || errno == ENOMEM))
                    wrdrop[i]++;
                else if (!stop)
                    fail("TAP write");
            }
        head++;
        progress = 1;
    }
    return progress;
}
static void report(const char *path) {
    char tmp[1024];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        fail("stats path");
    FILE *f = fopen(tmp, "w");
    if (!f)
        fail("relay stats");
    fprintf(f,
            "{\"pid\":%d,\"cpu\":%d,\"uid\":%d,\"loops\":%" PRIu64 ",\"idle_loops\":%" PRIu64
            ",\"queue_peak\":%" PRIu64 ",\"queue_overflow\":%" PRIu64 ",\"bad_frame\":%" PRIu64
            ",\"gso\":%" PRIu64 ",\"partial_checksum\":%" PRIu64 ",\"forwarded_events\":%" PRIu64
            ",\"late_over_50us\":%" PRIu64 ",\"late_over_250us\":%" PRIu64
            ",\"max_late_ns\":%" PRIu64 ",\"ports\":[",
            getpid(), sched_getcpu(), geteuid(), loops, idle, peak, queue_overflow, bad, gso,
            partial, head, late_50us, late_250us, max_late);
    for (int i = 0; i < count; i++)
        fprintf(f,
                "%s{\"read\":%" PRIu64 ",\"wire_bytes\":%" PRIu64 ",\"written\":%" PRIu64
                ",\"write_drop\":%" PRIu64 "}",
                i ? "," : "", nread[i], onwire[i], nwrite[i], wrdrop[i]);
    fputs("]}\n", f);
    if (fclose(f) || rename(tmp, path))
        fail("commit relay stats");
}
int main(int argc, char **argv) {
    /* uid gid delay-us stats-file /run/netns/server /run/netns/client... */
    if (argc < 7 || argc > 10)
        return 2;
    uid_t uid = (uid_t)strtoul(argv[1], NULL, 10);
    gid_t gid = (gid_t)strtoul(argv[2], NULL, 10);
    if (!uid || !gid)
        return 2;
    delay = strtoul(argv[3], NULL, 10) * 1000ULL;
    count = argc - 5;
    ino_t inodes[MAX_PORTS], host;
    int original = nsfd("/proc/self/ns/net", &host);
    for (int i = 0; i < count; i++) {
        int ns = nsfd(argv[i + 5], &inodes[i]);
        if (inodes[i] == host)
            return 2;
        for (int j = 0; j < i; j++)
            if (inodes[i] == inodes[j])
                return 2;
        if (setns(ns, CLONE_NEWNET))
            fail("enter private netns");
        close(ns);
        ports[i] = tap(i == 0);
    }
    if (setns(original, CLONE_NEWNET))
        fail("restore control netns");
    close(original);
    if (setgroups(0, NULL) || setresgid(gid, gid, gid) || setresuid(uid, uid, uid) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        fail("drop privileges");
    struct sigaction sa = {.sa_handler = finish};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    printf("READY %d\n", getpid());
    fflush(stdout);
    char start;
    if (read(STDIN_FILENO, &start, 1) != 1 || start != 'G')
        return 2;
    unsigned char *scratch = malloc(FRAME);
    if (!scratch)
        fail("relay buffer");
    uint64_t next = now() + 1000000000;
    while (!stop) {
        uint64_t ts = now();
        int progress = send_due();
        for (int i = 0; i < count; i++)
            progress |= get_frames(i, scratch);
        progress |= send_due();
        loops++;
        if (!progress)
            idle++;
        if (ts >= next) {
            report(argv[4]);
            next = ts + 1000000000;
        }
    }
    report(argv[4]);
    free(scratch);
    return 0;
}
