#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "link_xdp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/bpf.h>
#include <linux/ethtool.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/magic.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

enum {
    DEFAULT_TX_DEPTH = 512,
    DEFAULT_RX_DEPTH = 512,
    SMALL_CHUNK = 2048,
    LARGE_CHUNK = 4096,
    MAX_UMEM_BYTES = 32 * 1024 * 1024,
    HEADROOM = 256,
    ARP_BURST = 4,
    ARP_BYTES = 42
};
typedef struct {
    void *map, *descriptors;
    size_t bytes;
    uint32_t *producer, *consumer, *flags;
} Ring;

enum { MAX_PROBE_CPUS = 8192 };
typedef struct {
    int fd;
    unsigned count, ids[MAX_PROBE_CPUS];
    uint64_t values[MAX_PROBE_CPUS];
} RxCpuProbe;

struct XdpLink {
    RxCpuProbe *cpu_probe;
    int fd, mapfd, progfd, attachment, driver, zero_copy;
    int arp_fd, port_guard, local_listener, local_epoll_fd, tx_waiting, more_tx, tx_stalled;
    unsigned stalled_consumer, stalled_completion;
    uint64_t pressure_kicks, pressure_rescued;
    uint64_t tx_busy, tx_deferred, tx_kick_busy, tx_errors;
    unsigned held, held_arp, arp_batches, free_count;
    unsigned char arp_frames[ARP_BURST][ARP_BYTES];
    unsigned rx_depth, tx_depth, chunk;
    size_t rx_bytes, umem_bytes;
    uint64_t *tx_free;
    void *memory;
    Ring rx, tx, fill, completion;
};

static unsigned acquire(const uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void publish(uint32_t *p, unsigned v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static int bpf_call(enum bpf_cmd cmd, union bpf_attr *attr) {
    return (int)syscall(SYS_bpf, cmd, attr, sizeof(*attr));
}

/* Build the restricted XDP program from the operator's exact interface
 * identity. It passes every unrelated frame to Linux. A missing XSK map
 * entry also falls through to XDP_PASS. No object is pinned in bpffs. */
enum Label { L_ARP, L_BROADCAST, L_FIELDS, L_REDIRECT, L_PASS, L_DECISION, L_IHL5, L_COUNT = L_IHL5 + 11 };
typedef struct {
    struct bpf_insn insn[256];
    unsigned count, label[L_COUNT], jcount;
    int overflow;
    struct {
        unsigned index;
        enum Label label;
    } jumps[160];
} Program;
static void emit(Program *p, unsigned code, unsigned dst, unsigned src, int off, int imm) {
    if (p->count == sizeof(p->insn) / sizeof(p->insn[0])) {
        p->overflow = 1;
        return;
    }
    p->insn[p->count++] =
        (struct bpf_insn){.code = code, .dst_reg = dst, .src_reg = src, .off = off, .imm = imm};
}
static void jump(Program *p, unsigned code, unsigned dst, unsigned src, int value, enum Label label) {
    if (p->jcount == sizeof(p->jumps) / sizeof(p->jumps[0])) {
        p->overflow = 1;
        return;
    }
    p->jumps[p->jcount].index = p->count;
    p->jumps[p->jcount++].label = label;
    emit(p, BPF_JMP | code, dst, src, 0, value);
}
static void load(Program *p, unsigned size, unsigned dst, unsigned base, unsigned offset) {
    emit(p, BPF_LDX | BPF_MEM | size, dst, base, (int)offset, 0);
}
static void eq_bytes(Program *p, unsigned base, unsigned off, const uint8_t *bytes, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        load(p, BPF_B, 5, base, off + i);
        jump(p, BPF_JNE | BPF_K, 5, 0, bytes[i], L_PASS);
    }
}
static void bounds(Program *p, unsigned base, int amount) {
    emit(p, BPF_ALU64 | BPF_MOV | BPF_X, 9, base, 0, 0);
    emit(p, BPF_ALU64 | BPF_ADD | BPF_K, 9, 0, 0, amount);
    jump(p, BPF_JGT | BPF_X, 9, 8, 0, L_PASS);
}
static int load_filter(const LinkConfig *cfg, int mapfd, int cpu_fd, char *error, size_t size) {
    Program p = {0};
    const uint8_t tcp[] = {6}, arp_type[] = {0, 1, 8, 0, 6, 4, 0};
    const uint8_t port[] = {(uint8_t)(cfg->port >> 8), (uint8_t)cfg->port};
    const uint8_t broadcast[] = {255, 255, 255, 255, 255, 255};
    const uint8_t *ip = (const void *)&cfg->ipv4_be;
    emit(&p, BPF_ALU64 | BPF_MOV | BPF_X, 6, 1, 0, 0);
    load(&p, BPF_W, 7, 6, offsetof(struct xdp_md, data));
    load(&p, BPF_W, 8, 6, offsetof(struct xdp_md, data_end));
    bounds(&p, 7, 42);
    eq_bytes(&p, 7, 12, (const uint8_t[]){8}, 1);
    load(&p, BPF_B, 5, 7, 13);
    if (!cfg->shared_ip)
        jump(&p, BPF_JEQ | BPF_K, 5, 0, 6, L_ARP);
    jump(&p, BPF_JNE | BPF_K, 5, 0, 0, L_PASS);
    eq_bytes(&p, 7, 0, cfg->mac, 6);
    eq_bytes(&p, 7, 30, ip, 4);
    eq_bytes(&p, 7, 23, tcp, 1);
    load(&p, BPF_B, 5, 7, 20);
    jump(&p, BPF_JSET | BPF_K, 5, 0, 0x3f, L_PASS);
    eq_bytes(&p, 7, 21, (const uint8_t[]){0}, 1);
    load(&p, BPF_B, 3, 7, 14);
    /* The ordinary 20-byte IPv4 header gets the first branch. Every legal
     * option length uses a constant packet offset with its own end check. */
    for (unsigned words = 5; words <= 15; ++words)
        jump(&p, BPF_JEQ | BPF_K, 3, 0, 0x40 + words, (enum Label)(L_IHL5 + words - 5));
    jump(&p, BPF_JA, 0, 0, 0, L_PASS);
    for (unsigned words = 5; words <= 15; ++words) {
        unsigned port_offset = 14 + 4 * words + 2;
        p.label[L_IHL5 + words - 5] = p.count;
        bounds(&p, 7, port_offset + 2);
        eq_bytes(&p, 7, port_offset, port, 2);
        jump(&p, BPF_JA, 0, 0, 0, L_REDIRECT);
    }

    if (!cfg->shared_ip) {
        p.label[L_ARP] = p.count;
        load(&p, BPF_B, 5, 7, 0);
        jump(&p, BPF_JEQ | BPF_K, 5, 0, 255, L_BROADCAST);
        eq_bytes(&p, 7, 0, cfg->mac, 6);
        jump(&p, BPF_JA, 0, 0, 0, L_FIELDS);
        p.label[L_BROADCAST] = p.count;
        eq_bytes(&p, 7, 0, broadcast, 6);
        p.label[L_FIELDS] = p.count;
        eq_bytes(&p, 7, 14, arp_type, sizeof(arp_type));
        load(&p, BPF_B, 5, 7, 21);
        jump(&p, BPF_JLT | BPF_K, 5, 0, 1, L_PASS);
        jump(&p, BPF_JGT | BPF_K, 5, 0, 2, L_PASS);
        eq_bytes(&p, 7, 38, ip, 4);
    }
    p.label[L_REDIRECT] = p.count;
    emit(&p, BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, mapfd);
    emit(&p, 0, 0, 0, 0, 0);
    load(&p, BPF_W, 2, 6, offsetof(struct xdp_md, rx_queue_index));
    emit(&p, BPF_ALU64 | BPF_MOV | BPF_K, 3, 0, 0, XDP_PASS);
    emit(&p, BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_redirect_map);
    if (cpu_fd >= 0) {
        /* Optional measurement only. Preserve the helper's actual verdict and
         * count successful redirect decisions on their executing kernel CPU. */
        emit(&p, BPF_ALU64 | BPF_MOV | BPF_X, 6, 0, 0, 0);
        jump(&p, BPF_JNE | BPF_K, 6, 0, XDP_REDIRECT, L_DECISION);
        emit(&p, BPF_ST | BPF_MEM | BPF_W, 10, 0, -4, 0);
        emit(&p, BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, cpu_fd);
        emit(&p, 0, 0, 0, 0, 0);
        emit(&p, BPF_ALU64 | BPF_MOV | BPF_X, 2, 10, 0, 0);
        emit(&p, BPF_ALU64 | BPF_ADD | BPF_K, 2, 0, 0, -4);
        emit(&p, BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem);
        jump(&p, BPF_JEQ | BPF_K, 0, 0, 0, L_DECISION);
        emit(&p, BPF_ALU64 | BPF_MOV | BPF_K, 1, 0, 0, 1);
        emit(&p, BPF_STX | BPF_ATOMIC | BPF_DW, 0, 1, 0, BPF_ADD);
        p.label[L_DECISION] = p.count;
        emit(&p, BPF_ALU64 | BPF_MOV | BPF_X, 0, 6, 0, 0);
    }
    emit(&p, BPF_JMP | BPF_EXIT, 0, 0, 0, 0);
    p.label[L_PASS] = p.count;
    emit(&p, BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, XDP_PASS);
    emit(&p, BPF_JMP | BPF_EXIT, 0, 0, 0, 0);
    if (p.overflow) {
        errno = E2BIG;
        return -1;
    }
    for (unsigned i = 0; i < p.jcount; ++i) {
        unsigned at = p.jumps[i].index;
        p.insn[at].off = (int)(p.label[p.jumps[i].label] - at - 1);
    }
    char log[65536] = {0};
    union bpf_attr a = {0};
    a.prog_type = BPF_PROG_TYPE_XDP;
    a.insn_cnt = p.count;
    a.insns = (uintptr_t)p.insn;
    a.license = (uintptr_t) "GPL";
    a.log_buf = (uintptr_t)log;
    a.log_size = sizeof(log);
    a.log_level = 1;
    memcpy(a.prog_name, "twelve_tcp", 11);
    int fd = bpf_call(BPF_PROG_LOAD, &a);
    if (fd < 0 && error && size) {
        int saved = errno;
        size_t used = strnlen(log, sizeof(log));
        if (used)
            fprintf(stderr, "XDP verifier: %s", log + (used > 1400 ? used - 1400 : 0));
        snprintf(error, size, "XDP program load: %s", strerror(saved));
        errno = saved;
    }
    return fd;
}

static int inspect_device(unsigned ifindex, unsigned *allocated_queues) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct {
        struct nlmsghdr h;
        struct ifinfomsg i;
    } q = {.h = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                 .nlmsg_type = RTM_GETLINK,
                 .nlmsg_flags = NLM_F_REQUEST,
                 .nlmsg_seq = 1},
           .i = {.ifi_family = AF_UNSPEC, .ifi_index = (int)ifindex}};
    struct sockaddr_nl nl = {.nl_family = AF_NETLINK};
    unsigned char reply[8192];
    int n = sendto(fd, &q, q.h.nlmsg_len, 0, (void *)&nl, sizeof(nl)) < 0
                ? -1
                : (int)recv(fd, reply, sizeof(reply), 0);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n < 0)
        return -1;
    for (struct nlmsghdr *h = (void *)reply; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
        if (h->nlmsg_type != RTM_NEWLINK)
            continue;
        struct ifinfomsg *i = NLMSG_DATA(h);
        int length = IFLA_PAYLOAD(h);
        int attached = 0;
        for (struct rtattr *r = IFLA_RTA(i); RTA_OK(r, length); r = RTA_NEXT(r, length)) {
            unsigned type = r->rta_type & NLA_TYPE_MASK;
            if (type == IFLA_NUM_RX_QUEUES && RTA_PAYLOAD(r) == sizeof(uint32_t))
                memcpy(allocated_queues, RTA_DATA(r), sizeof(uint32_t));
            if (type != IFLA_XDP)
                continue;
            int nested = RTA_PAYLOAD(r);
            for (struct rtattr *a = RTA_DATA(r); RTA_OK(a, nested); a = RTA_NEXT(a, nested))
                if ((a->rta_type & NLA_TYPE_MASK) == IFLA_XDP_ATTACHED && RTA_PAYLOAD(a) >= 1)
                    attached = *(uint8_t *)RTA_DATA(a) != XDP_ATTACHED_NONE;
        }
        return attached;
    }
    errno = EPROTO;
    return -1;
}

/* This backend owns queue zero. Other active RX queues could receive the same
 * TCP service but have no XSK entry. Refuse that configuration before attach. */
static int single_rx_queue(const char *name, unsigned allocated_queues) {
    if (allocated_queues == 1)
        return 1;
    struct ethtool_channels channels = {.cmd = ETHTOOL_GCHANNELS};
    struct ifreq request = {0};
    memcpy(request.ifr_name, name, strlen(name) + 1);
    request.ifr_data = (void *)&channels;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int rc = ioctl(fd, SIOCETHTOOL, &request);
    int saved = errno;
    close(fd);
    errno = saved;
    if (rc < 0)
        return -1;
    return (uint64_t)channels.rx_count + channels.combined_count == 1;
}

static int map_ring(int fd, Ring *r, const struct xdp_ring_offset *off, uint64_t where, unsigned depth,
                    size_t descriptor_size) {
    r->bytes = off->desc + depth * descriptor_size;
    void *base = mmap(NULL, r->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)where);
    if (base == MAP_FAILED)
        return -1;
    r->map = base;
    unsigned char *b = base;
    r->producer = (void *)(b + off->producer);
    r->consumer = (void *)(b + off->consumer);
    r->flags = (void *)(b + off->flags);
    r->descriptors = b + off->desc;
    return 0;
}

static int configure_xsk(XdpLink *x, unsigned ifindex, unsigned queue, unsigned mode, char *error,
                         size_t size) {
    x->fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (x->fd < 0)
        return -1;
    size_t bytes = x->umem_bytes;
    x->memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (x->memory == MAP_FAILED) {
        x->memory = NULL;
        return -1;
    }
    struct xdp_umem_reg u = {.addr = (uintptr_t)x->memory, .len = bytes, .chunk_size = x->chunk};
    unsigned rx = x->rx_depth, tx = x->tx_depth;
    /* Linux requires a completion queue for every independent UMEM, even
     * when an explicit AF_PACKET sender owns TX and no XDP TX ring exists. */
    unsigned completion = tx ? tx : 1;
    if (setsockopt(x->fd, SOL_XDP, XDP_UMEM_REG, &u, sizeof(u)) < 0) {
        int saved = errno;
        if (error && size) {
            struct rlimit limit;
            if (saved == ENOBUFS && getrlimit(RLIMIT_MEMLOCK, &limit) == 0) {
                snprintf(error, size,
                         "AF_XDP registration of %zu UMEM bytes failed: %s "
                         "(process memlock limit=%" PRIu64 ", pinned pages are charged to UID %lu)",
                         bytes, strerror(saved), (uint64_t)limit.rlim_cur, (unsigned long)getuid());
            } else {
                snprintf(error, size, "AF_XDP registration of %zu UMEM bytes failed: %s", bytes,
                         strerror(saved));
            }
        }
        errno = saved;
        return -1;
    }
    if (setsockopt(x->fd, SOL_XDP, XDP_UMEM_FILL_RING, &rx, sizeof(rx)) ||
        setsockopt(x->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &completion, sizeof(completion)) ||
        setsockopt(x->fd, SOL_XDP, XDP_RX_RING, &rx, sizeof(rx)) ||
        (tx && setsockopt(x->fd, SOL_XDP, XDP_TX_RING, &tx, sizeof(tx))))
        return -1;
    struct xdp_mmap_offsets offsets;
    socklen_t len = sizeof(offsets);
    if (getsockopt(x->fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &len) || len < sizeof(offsets) ||
        map_ring(x->fd, &x->fill, &offsets.fr, XDP_UMEM_PGOFF_FILL_RING, rx, sizeof(uint64_t)) ||
        (tx && map_ring(x->fd, &x->completion, &offsets.cr, XDP_UMEM_PGOFF_COMPLETION_RING, tx,
                        sizeof(uint64_t))) ||
        map_ring(x->fd, &x->rx, &offsets.rx, XDP_PGOFF_RX_RING, rx, sizeof(struct xdp_desc)) ||
        (tx && map_ring(x->fd, &x->tx, &offsets.tx, XDP_PGOFF_TX_RING, tx, sizeof(struct xdp_desc))))
        return -1;
    /* The first rx_depth chunks circulate exclusively from FILL to RX. A
     * separate fixed suffix circulates from TX to COMPLETION. An RX pointer
     * can never alias bytes being transmitted, independently of RX capacity. */
    uint64_t *fill = x->fill.descriptors;
    for (unsigned i = 0; i < rx; ++i)
        fill[i] = (uint64_t)i * x->chunk;
    for (unsigned i = 0; i < tx; ++i)
        x->tx_free[i] = x->rx_bytes + (uint64_t)i * x->chunk;
    publish(x->fill.producer, rx);
    x->free_count = tx;
    struct sockaddr_xdp addr = {.sxdp_family = AF_XDP,
                                .sxdp_ifindex = ifindex,
                                .sxdp_queue_id = queue,
                                .sxdp_flags = (uint16_t)(mode | XDP_USE_NEED_WAKEUP)};
    if (bind(x->fd, (void *)&addr, sizeof(addr)))
        return -1;
    struct xdp_options actual = {0};
    len = sizeof(actual);
    if (getsockopt(x->fd, SOL_XDP, XDP_OPTIONS, &actual, &len))
        return -1;
    x->zero_copy = !!(actual.flags & XDP_OPTIONS_ZEROCOPY);
    if (x->zero_copy != (mode == XDP_ZEROCOPY)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/* Reserve the service port before attaching any redirect. Host-local traffic
 * skips an ingress XDP program. The HTTP server can use the same reserved
 * socket as a nonblocking local listener, with its configured HOST binding. */
static int reserve_shared_port(const LinkConfig *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons(cfg->port),
                                  .sin_addr.s_addr =
                                      cfg->local_listener ? cfg->local_bind_be : cfg->ipv4_be};
    int reuse = 1;
    if ((cfg->local_listener && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) ||
        bind(fd, (void *)&address, sizeof(address)) < 0 ||
        (cfg->local_listener && listen(fd, SOMAXCONN) < 0)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Linux owns ARP for a shared address. Our TCP stack can issue its own
 * neighbor queries, and needs just the 42 bytes of unicast ARP replies
 * explicitly addressed to it. Install the filter before binding. */
static int open_arp_receiver(const LinkConfig *cfg, unsigned ifindex) {
    uint32_t mac_word;
    uint16_t mac_tail;
    memcpy(&mac_word, cfg->mac, sizeof(mac_word));
    memcpy(&mac_tail, cfg->mac + sizeof(mac_word), sizeof(mac_tail));
    enum { REJECT = 255 };
    struct sock_filter code[] = {BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(mac_word), 0, REJECT),
                                 BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohs(mac_tail), 0, REJECT),
                                 BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 14),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x00010800, 0, REJECT),
                                 BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 18),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x06040002, 0, REJECT),
                                 BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 32),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(mac_word), 0, REJECT),
                                 BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 36),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohs(mac_tail), 0, REJECT),
                                 BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 38),
                                 BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(cfg->ipv4_be), 0, REJECT),
                                 BPF_STMT(BPF_RET | BPF_K, ARP_BYTES),
                                 BPF_STMT(BPF_RET | BPF_K, 0)};
    unsigned count = sizeof(code) / sizeof(code[0]);
    for (unsigned i = 0; i < count; ++i)
        if (code[i].jf == REJECT)
            code[i].jf = (uint8_t)(count - i - 2);
    struct sock_fprog filter = {.len = (unsigned short)count, .filter = code};
    struct sockaddr_ll local = {
        .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ARP), .sll_ifindex = (int)ifindex};
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    int size = 65536, one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &filter, sizeof(filter)) < 0 ||
        setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof(one)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0 ||
        bind(fd, (void *)&local, sizeof(local)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* A per-CPU map lookup returns one u64 for each possible CPU in ascending
 * order, including offline CPUs. Affinity and the online CPU count differ. */
static int open_cpu_probe(XdpLink *x) {
    RxCpuProbe *probe = calloc(1, sizeof(*probe));
    if (!probe)
        return -1;
    x->cpu_probe = probe;
    probe->fd = -1;
    FILE *list = fopen("/sys/devices/system/cpu/possible", "re");
    if (!list)
        return -1;
    struct statfs fs;
    char cpus[16384];
    int valid = fstatfs(fileno(list), &fs) == 0 && fs.f_type == SYSFS_MAGIC &&
                fgets(cpus, sizeof(cpus), list) && strchr(cpus, '\n');
    fclose(list);
    if (!valid) {
        errno = EPROTO;
        return -1;
    }
    char *pos = cpus;
    while (*pos != '\n') {
        char *end;
        if (*pos < '0' || *pos > '9') {
            errno = EPROTO;
            return -1;
        }
        unsigned long first = strtoul(pos, &end, 10), last = first;
        if (*end == '-') {
            pos = end + 1;
            if (*pos < '0' || *pos > '9') {
                errno = EPROTO;
                return -1;
            }
            last = strtoul(pos, &end, 10);
        }
        if (first > last || last >= MAX_PROBE_CPUS ||
            (probe->count && first <= probe->ids[probe->count - 1])) {
            errno = EOVERFLOW;
            return -1;
        }
        for (unsigned long cpu = first; cpu <= last; ++cpu)
            probe->ids[probe->count++] = (unsigned)cpu;
        if (*end == '\n')
            break;
        if (*end != ',') {
            errno = EPROTO;
            return -1;
        }
        pos = end + 1;
    }
    if (!probe->count) {
        errno = EPROTO;
        return -1;
    }
    union bpf_attr map = {0};
    map.map_type = BPF_MAP_TYPE_PERCPU_ARRAY;
    map.key_size = sizeof(uint32_t);
    map.value_size = sizeof(uint64_t);
    map.max_entries = 1;
    memcpy(map.map_name, "twelve_rx_cpu", sizeof("twelve_rx_cpu"));
    probe->fd = bpf_call(BPF_MAP_CREATE, &map);
    return probe->fd < 0 ? -1 : 0;
}

static void report_cpu_probe(RxCpuProbe *probe) {
    uint32_t key = 0;
    union bpf_attr entry = {0};
    entry.map_fd = probe->fd;
    entry.key = (uintptr_t)&key;
    entry.value = (uintptr_t)probe->values;
    if (bpf_call(BPF_MAP_LOOKUP_ELEM, &entry) < 0) {
        fprintf(stderr, "XDP redirect CPU counters unavailable: %s\n", strerror(errno));
        return;
    }
    uint64_t total = 0;
    fprintf(stderr, "xdp redirect_cpus=");
    for (unsigned i = 0; i < probe->count; ++i) {
        uint64_t value = probe->values[i];
        if (value) {
            fprintf(stderr, "%s%u:%" PRIu64, total ? "," : "", probe->ids[i], value);
            total += value;
        }
    }
    fprintf(stderr, " total=%" PRIu64 "\n", total);
}

static int attach(XdpLink *x, const LinkConfig *cfg, unsigned ifindex, char *error, size_t size) {
    if (cfg->shared_ip) {
        x->local_listener = cfg->local_listener;
        x->port_guard = reserve_shared_port(cfg);
        if (x->port_guard < 0) {
            if (error && size)
                snprintf(error, size, "reserve shared TCP service port: %s", strerror(errno));
            return -1;
        }
        x->arp_fd = open_arp_receiver(cfg, ifindex);
        if (x->arp_fd < 0) {
            if (error && size)
                snprintf(error, size, "shared ARP reply socket: %s", strerror(errno));
            return -1;
        }
    }
    unsigned entries = cfg->queue_id + 1;
    union bpf_attr map = {0};
    map.map_type = BPF_MAP_TYPE_XSKMAP;
    map.key_size = 4;
    map.value_size = 4;
    map.max_entries = entries;
    memcpy(map.map_name, "twelve_xsk", 11);
    x->mapfd = bpf_call(BPF_MAP_CREATE, &map);
    if (x->mapfd < 0)
        return -1;
    if (cfg->probe_rx_cpus && open_cpu_probe(x) < 0) {
        if (error && size)
            snprintf(error, size, "XDP RX CPU probe: %s", strerror(errno));
        return -1;
    }
    x->progfd = load_filter(cfg, x->mapfd, x->cpu_probe ? x->cpu_probe->fd : -1, error, size);
    if (x->progfd < 0)
        return -1;
    uint32_t key = cfg->queue_id, value = (uint32_t)x->fd;
    union bpf_attr entry = {0};
    entry.map_fd = x->mapfd;
    entry.key = (uintptr_t)&key;
    entry.value = (uintptr_t)&value;
    if (bpf_call(BPF_MAP_UPDATE_ELEM, &entry))
        return -1;
    /* The kernel rejects replacing an existing program with a BPF link.
     * Closing this specific unpinned link fd atomically detaches only ours. */
    union bpf_attr link = {0};
    link.link_create.prog_fd = x->progfd;
    link.link_create.target_ifindex = ifindex;
    link.link_create.attach_type = BPF_XDP;
    if (cfg->mode == LINK_XDP_GENERIC) {
        link.link_create.flags = XDP_FLAGS_SKB_MODE;
        x->attachment = bpf_call(BPF_LINK_CREATE, &link);
        return x->attachment < 0 ? -1 : 0;
    }
    link.link_create.flags = XDP_FLAGS_DRV_MODE;
    x->attachment = bpf_call(BPF_LINK_CREATE, &link);
    if (x->attachment >= 0) {
        x->driver = 1;
        return 0;
    }
    if (x->zero_copy || (errno != EOPNOTSUPP && errno != EINVAL && errno != ENOTSUP))
        return -1;
    link.link_create.flags = XDP_FLAGS_SKB_MODE;
    x->attachment = bpf_call(BPF_LINK_CREATE, &link);
    return x->attachment < 0 ? -1 : 0;
}

XdpLink *xdp_link_open(const LinkConfig *cfg, unsigned ifindex, char *error, size_t size) {
    if (cfg->queue_id || cfg->mtu > LARGE_CHUNK - HEADROOM - ETH_HLEN) {
        errno = EINVAL;
        if (error && size)
            snprintf(error, size, "AF_XDP requires queue zero and an MTU at most %d",
                     LARGE_CHUNK - HEADROOM - ETH_HLEN);
        return NULL;
    }
    unsigned rx_depth = cfg->xdp_rx_frames ? cfg->xdp_rx_frames : DEFAULT_RX_DEPTH;
    unsigned tx_depth = cfg->packet_gso ? 0 : (cfg->xdp_tx_frames ? cfg->xdp_tx_frames : DEFAULT_TX_DEPTH);
    unsigned chunk = cfg->mtu <= SMALL_CHUNK - HEADROOM - ETH_HLEN ? SMALL_CHUNK : LARGE_CHUNK;
    uint64_t bytes = ((uint64_t)rx_depth + tx_depth) * chunk;
    if (rx_depth < 64 || (rx_depth & (rx_depth - 1)) ||
        (tx_depth && (tx_depth < 64 || (tx_depth & (tx_depth - 1)))) || bytes > MAX_UMEM_BYTES) {
        errno = EINVAL;
        if (error && size)
            snprintf(error, size,
                     "AF_XDP RX/TX slots each need a power of two >=64 within the %u MiB total UMEM limit",
                     MAX_UMEM_BYTES / (1024 * 1024));
        return NULL;
    }
    unsigned allocated_queues = 0;
    int existing = inspect_device(ifindex, &allocated_queues);
    if (existing) {
        if (existing > 0)
            errno = EBUSY;
        return NULL;
    }
    int single = single_rx_queue(cfg->ifname, allocated_queues);
    if (single <= 0) {
        if (single == 0)
            errno = EOPNOTSUPP;
        if (error && size)
            snprintf(error, size, "AF_XDP requires one verified active RX queue: %s", strerror(errno));
        return NULL;
    }
    unsigned first_mode = cfg->mode == LINK_XDP_GENERIC ? 1 : 0;
    for (unsigned attempt = first_mode; attempt < 2; ++attempt) {
        if (attempt && cfg->mode == LINK_XDP_ZEROCOPY)
            break;
        XdpLink *x = calloc(1, sizeof(*x));
        if (!x)
            return NULL;
        x->tx_free = tx_depth ? malloc((size_t)tx_depth * sizeof(*x->tx_free)) : NULL;
        if (tx_depth && !x->tx_free) {
            free(x);
            return NULL;
        }
        x->tx_depth = tx_depth;
        x->rx_depth = rx_depth;
        x->chunk = chunk;
        x->rx_bytes = (size_t)rx_depth * chunk;
        x->umem_bytes = (size_t)bytes;
        x->local_epoll_fd = -1;
        x->fd = x->mapfd = x->progfd = x->attachment = x->arp_fd = x->port_guard = -1;
        if (configure_xsk(x, ifindex, cfg->queue_id, attempt ? XDP_COPY : XDP_ZEROCOPY, error, size) == 0 &&
            attach(x, cfg, ifindex, error, size) == 0)
            return x;
        int saved = errno;
        xdp_link_close(x);
        errno = saved;
        if (saved == EBUSY || saved == EPERM || saved == EACCES || (error && size && *error))
            break;
    }
    return NULL;
}

static int receive_arp_replies(XdpLink *x, LinkFrame *frames, unsigned capacity) {
    unsigned count = 0;
    while (count < ARP_BURST && count < capacity) {
        ssize_t n = recv(x->arp_fd, x->arp_frames[count], ARP_BYTES, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            return -1;
        }
        if (n != ARP_BYTES) {
            errno = EPROTO;
            return -1;
        }
        frames[count] = (LinkFrame){.data = x->arp_frames[count], .len = ARP_BYTES};
        ++count;
    }
    x->held_arp = count;
    return (int)count;
}

int xdp_link_receive(XdpLink *x, LinkFrame *f, unsigned capacity) {
    if (x->held || x->held_arp) {
        errno = EBUSY;
        return -1;
    }
    unsigned consumer = acquire(x->rx.consumer), available = acquire(x->rx.producer) - consumer;
    if (available > x->rx_depth) {
        errno = EPROTO;
        return -1;
    }
    /* Idle polls wake immediately for ARP. Also sample on busy RX so a
     * continuously full TCP queue cannot starve an unresolved neighbor. */
    if (x->arp_fd >= 0 && (!available || ++x->arp_batches >= 32)) {
        x->arp_batches = 0;
        int received = receive_arp_replies(x, f, capacity);
        if (received < 0)
            return -1;
        f += received;
        capacity -= (unsigned)received;
    }
    if (available > capacity)
        available = capacity;
    struct xdp_desc *desc = x->rx.descriptors;
    for (unsigned i = 0; i < available; ++i) {
        struct xdp_desc d = desc[(consumer + i) & (x->rx_depth - 1)];
        uint64_t base = d.addr & ~(uint64_t)(x->chunk - 1);
        if (base >= x->rx_bytes || d.len > x->chunk - (d.addr - base) || d.options) {
            errno = EPROTO;
            return -1;
        }
        f[i] = (LinkFrame){.data = (unsigned char *)x->memory + d.addr, .len = d.len};
    }
    x->held = available;
    return (int)(available + x->held_arp);
}

void xdp_link_release(XdpLink *x) {
    x->held_arp = 0;
    if (!x->held)
        return;
    /* A borrowed packet pointer expires here. RX consumer publication ends
     * descriptor use before FILL makes its backing memory writable again. */
    unsigned producer = acquire(x->fill.producer), consumer = acquire(x->rx.consumer);
    uint64_t *fill = x->fill.descriptors;
    struct xdp_desc *desc = x->rx.descriptors;
    for (unsigned i = 0; i < x->held; ++i) {
        uint64_t addr = desc[(consumer + i) & (x->rx_depth - 1)].addr;
        fill[(producer + i) & (x->rx_depth - 1)] = addr & ~(uint64_t)(x->chunk - 1);
    }
    publish(x->rx.consumer, consumer + x->held);
    publish(x->fill.producer, producer + x->held);
    x->held = 0;
}

static int reclaim_tx(XdpLink *x) {
    unsigned c = acquire(x->completion.consumer), end = acquire(x->completion.producer);
    if (end - c > x->tx_depth - x->free_count) {
        errno = EPROTO;
        return -1;
    }
    uint64_t *done = x->completion.descriptors;
    while (c != end) {
        uint64_t addr = done[c++ & (x->tx_depth - 1)];
        if (addr < x->rx_bytes || addr >= x->umem_bytes || (addr & (x->chunk - 1))) {
            errno = EPROTO;
            return -1;
        }
        x->tx_free[x->free_count++] = addr;
    }
    publish(x->completion.consumer, c);
    return 0;
}

static int kick_tx(XdpLink *x) {
    if (sendto(x->fd, NULL, 0, MSG_DONTWAIT, NULL, 0) >= 0)
        return 0;
    if (errno == EAGAIN || errno == EBUSY || errno == ENOBUFS || errno == EINTR) {
        ++x->tx_deferred;
        if (errno == EBUSY)
            ++x->tx_kick_busy;
        return 0;
    }
    ++x->tx_errors;
    return -1;
}

int xdp_link_send(XdpLink *x, const void *frame, unsigned len) {
    if (!x->tx_depth) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (len > x->chunk) {
        errno = EMSGSIZE;
        return -1;
    }
    if (!x->free_count && reclaim_tx(x) < 0)
        return -1;
    unsigned p = acquire(x->tx.producer), consumer = acquire(x->tx.consumer);
    if (!x->free_count || p - consumer >= x->tx_depth) {
        unsigned completed = acquire(x->completion.producer);
        /* A stack dispatch can queue more frames than fit in one TX ring.
         * Kick once immediately. Suppress further identical syscalls until
         * kernel ownership changes or the application returns to link_flush. */
        if (!x->tx_stalled || consumer != x->stalled_consumer || completed != x->stalled_completion) {
            if (p != consumer && (acquire(x->tx.flags) & XDP_RING_NEED_WAKEUP)) {
                ++x->pressure_kicks;
                if (kick_tx(x) < 0)
                    return -1;
            }
            if (!x->free_count && reclaim_tx(x) < 0)
                return -1;
            consumer = acquire(x->tx.consumer);
        }
        if (!x->free_count || p - consumer >= x->tx_depth) {
            x->tx_stalled = 1;
            x->stalled_consumer = consumer;
            x->stalled_completion = acquire(x->completion.producer);
            x->tx_waiting = 1;
            ++x->tx_busy;
            return 0;
        }
        ++x->pressure_rescued;
    }
    x->tx_stalled = 0;
    x->tx_waiting = 0;
    uint64_t addr = x->tx_free[--x->free_count];
    memcpy((unsigned char *)x->memory + addr, frame, len);
    struct xdp_desc *desc = x->tx.descriptors;
    desc[p & (x->tx_depth - 1)] = (struct xdp_desc){.addr = addr, .len = len};
    publish(x->tx.producer, p + 1);
    return 1;
}

int xdp_link_flush(XdpLink *x) {
    if (!x->tx_depth)
        return 0;
    x->tx_stalled = 0;
    x->more_tx = 0;
    /* Linux 6.8 generic XSK TX sends at most 32 frames per kick. EAGAIN can
     * therefore mean a completed prefix. Service at most four prefixes before
     * yielding to inbound TCP and timers, and retry a remaining active tail. */
    for (unsigned kick = 0; kick < 4; ++kick) {
        unsigned before = acquire(x->tx.consumer);
        if (before == acquire(x->tx.producer) || !(acquire(x->tx.flags) & XDP_RING_NEED_WAKEUP))
            return 0;
        if (kick_tx(x) < 0)
            return -1;
        if (before == acquire(x->tx.consumer))
            return 0;
    }
    x->more_tx =
        acquire(x->tx.consumer) != acquire(x->tx.producer) && (acquire(x->tx.flags) & XDP_RING_NEED_WAKEUP);
    return 0;
}

int xdp_link_wait(XdpLink *x, int timeout_ms, int pending_packet_tx_fd) {
    if (xdp_link_flush(x) < 0)
        return -1;
    if (x->more_tx)
        return 0;
    /* If the device accepted no descriptors, avoid busy-spinning on a socket
     * that can take more descriptors but currently cannot transmit its head. */
    if (x->tx_depth && (timeout_ms < 0 || timeout_ms > 1) &&
        acquire(x->tx.consumer) != acquire(x->tx.producer) && (acquire(x->tx.flags) & XDP_RING_NEED_WAKEUP))
        timeout_ms = 1;
    struct pollfd fds[4] = {{.fd = x->fd, .events = (short)(POLLIN | (x->tx_waiting ? POLLOUT : 0))},
                            {.fd = x->arp_fd, .events = POLLIN},
                            {.fd = pending_packet_tx_fd, .events = POLLOUT},
                            {.fd = x->local_epoll_fd, .events = POLLIN}};
    unsigned count =
        x->local_epoll_fd >= 0 ? 4 : (pending_packet_tx_fd >= 0 ? 3 : (x->arp_fd >= 0 ? 2 : 1));
    int n = poll(fds, count, timeout_ms);
    if (n < 0 && errno == EINTR)
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
    }
    if (fds[0].revents & POLLOUT)
        x->tx_waiting = 0;
    return n;
}
int xdp_link_local_listener(const XdpLink *x) {
    return x->local_listener ? x->port_guard : -1;
}
void xdp_link_set_local_epoll(XdpLink *x, int epfd) {
    x->local_epoll_fd = epfd;
}

const char *xdp_link_mode(const XdpLink *x) {
    if (!x->tx_depth) {
        if (x->zero_copy)
            return "AF_XDP/driver RX XDP_OPTIONS=zero-copy + AF_PACKET/GSO TX";
        if (x->driver)
            return "AF_XDP/driver RX XDP_OPTIONS=copy + AF_PACKET/GSO TX";
        return "AF_XDP/generic RX XDP_OPTIONS=copy + AF_PACKET/GSO TX";
    }
    if (x->zero_copy)
        return "AF_XDP/driver XDP_OPTIONS=zero-copy";
    return x->driver ? "AF_XDP/driver XDP_OPTIONS=copy" : "AF_XDP/generic XDP_OPTIONS=copy";
}
void xdp_link_report(const XdpLink *x) {
    if (x->cpu_probe)
        report_cpu_probe(x->cpu_probe);
    struct xdp_statistics stats = {0};
    socklen_t len = sizeof(stats);
    if (getsockopt(x->fd, SOL_XDP, XDP_STATISTICS, &stats, &len) < 0) {
        fprintf(stderr, "AF_XDP kernel statistics unavailable: %s\n", strerror(errno));
        return;
    }
    fprintf(stderr,
            "xdp kernel rx_dropped=%" PRIu64 " rx_invalid=%" PRIu64 " tx_invalid=%" PRIu64
            " rx_ring_full=%" PRIu64 " fill_empty=%" PRIu64 " tx_ring_empty=%" PRIu64 " tx_busy=%" PRIu64
            " tx_kick_deferred=%" PRIu64 " tx_kick_ebusy=%" PRIu64 " tx_errors=%" PRIu64
            " rx_slots=%u tx_slots=%u chunk=%u umem_bytes=%zu tx_pressure_kicks=%" PRIu64
            " tx_pressure_rescued=%" PRIu64 "\n",
            (uint64_t)stats.rx_dropped, (uint64_t)stats.rx_invalid_descs, (uint64_t)stats.tx_invalid_descs,
            (uint64_t)stats.rx_ring_full, (uint64_t)stats.rx_fill_ring_empty_descs,
            (uint64_t)stats.tx_ring_empty_descs, x->tx_busy, x->tx_deferred, x->tx_kick_busy, x->tx_errors,
            x->rx_depth, x->tx_depth, x->chunk, x->umem_bytes, x->pressure_kicks, x->pressure_rescued);
}

void xdp_link_close(XdpLink *x) {
    if (!x)
        return;
    if (x->attachment >= 0)
        close(x->attachment);
    if (x->arp_fd >= 0)
        close(x->arp_fd);
    if (x->port_guard >= 0)
        close(x->port_guard);
    if (x->progfd >= 0)
        close(x->progfd);
    if (x->mapfd >= 0)
        close(x->mapfd);
    if (x->cpu_probe) {
        if (x->cpu_probe->fd >= 0)
            close(x->cpu_probe->fd);
        free(x->cpu_probe);
    }
    Ring *rings[] = {&x->rx, &x->tx, &x->fill, &x->completion};
    for (unsigned i = 0; i < 4; ++i)
        if (rings[i]->map)
            munmap(rings[i]->map, rings[i]->bytes);
    if (x->fd >= 0)
        close(x->fd);
    if (x->memory)
        munmap(x->memory, x->umem_bytes);
    free(x->tx_free);
    free(x);
}
