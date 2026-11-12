#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "link.h"
#include "packet_gso.h"
#ifdef TWELVE_AF_XDP
#include "link_xdp.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    DEFAULT_TX_FRAMES = 512, DEFAULT_RX_FRAMES = 512, BLOCK_FRAMES = 16,
    MAX_RX_BYTES = 32 * 1024 * 1024, MAX_TX_BYTES = 64 * 1024 * 1024
};

struct BypassLink {
    int fd;
#ifdef TWELVE_AF_XDP
    XdpLink *xdp;
#endif
    LinkConfig cfg;
    unsigned ifindex, frame_size, tx_frame_size, rx_slots, tx_slots, rx, tx, held, kicked;
    size_t rx_bytes, mapping_bytes;
    unsigned char *mapping;
    uint64_t rx_frames, tx_frames, partial_checksums, rejected, oversized;
    uint64_t kernel_packets, kernel_drops, tx_busy, tx_deferred, tx_errors;
    PacketGso pending;
    uint64_t gso_groups, gso_merged;
};

static int errorf(char *out, size_t size, const char *fmt, ...) {
    if (out && size) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(out, size, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int socket_error(char *out, size_t size, const char *what) {
    return errorf(out, size, "%s: %s", what, strerror(errno));
}

/* A classic per-socket filter rejects unrelated traffic before a ring frame
 * is allocated. X is the IPv4 header length while checking the TCP port. */
static int filter_socket(int fd, const LinkConfig *cfg) {
    enum { BAD = 255, ARP = 251 };
    struct sock_filter program[] = {
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_ARP, ARP, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IP, 0, BAD),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, get32(cfg->mac), 0, BAD),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, get16(cfg->mac + 4), 0, BAD),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 30),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(cfg->ipv4_be), 0, BAD),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 23),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_TCP, 0, BAD),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 20),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x3fff, BAD, 0),
        BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 14),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 16),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, cfg->port, 0, BAD),
        BPF_STMT(BPF_RET | BPF_K, UINT32_MAX),
        /* ARP is either addressed to this interface or broadcast. */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, get32(cfg->mac), 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, UINT32_MAX, 0, BAD),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffff, 2, BAD),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, get16(cfg->mac + 4), 0, BAD),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 14),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x00010800, 0, BAD),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 18),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0604, 0, BAD),
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 20),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARPOP_REQUEST, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARPOP_REPLY, 0, BAD),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 38),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ntohl(cfg->ipv4_be), 0, BAD),
        BPF_STMT(BPF_RET | BPF_K, UINT32_MAX),
        BPF_STMT(BPF_RET | BPF_K, 0)
    };
    unsigned count = sizeof(program) / sizeof(program[0]);
    for (unsigned i = 0; i < count; ++i) {
        if (program[i].jt == ARP) program[i].jt = 17 - i - 1;
        if (program[i].jt == BAD) program[i].jt = count - i - 2;
        if (program[i].jf == BAD) program[i].jf = count - i - 2;
    }
    struct sock_fprog filter = {.len = (unsigned short)count, .filter = program};
    return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &filter, sizeof(filter));
}

/* SIOCGIFCONF uses the network namespace of the supplied socket. Inspect
 * every IPv4 address, including secondary aliases: Linux would answer or
 * reset traffic for an address that it also owns on another local device. */
static int kernel_owns_ip(int fd, uint32_t ip, const char *ifname) {
    struct ifconf list = {0};
    if (ioctl(fd, SIOCGIFCONF, &list) < 0) return -1;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        int room = list.ifc_len + 16 * (int)sizeof(struct ifreq);
        if (room <= 0 || room > 1024 * 1024) { errno = EOVERFLOW; return -1; }
        struct ifreq *items = malloc((size_t)room);
        if (!items) return -1;
        list.ifc_len = room;
        list.ifc_req = items;
        int rc = ioctl(fd, SIOCGIFCONF, &list);
        int local = 0;
        if (!rc) {
            for (int i = 0; i < list.ifc_len / (int)sizeof(*items); ++i) {
                struct sockaddr_in *a = (void *)&items[i].ifr_addr;
                if (a->sin_family == AF_INET && a->sin_addr.s_addr == ip) {
                    size_t n = strnlen(items[i].ifr_name, IFNAMSIZ);
                    const char *alias = memchr(items[i].ifr_name, ':', n);
                    if (alias) n = (size_t)(alias - items[i].ifr_name);
                    if (strlen(ifname) != n || memcmp(items[i].ifr_name, ifname, n)) {
                        free(items);
                        return 2;
                    }
                    local = 1;
                }
            }
        }
        free(items);
        if (rc < 0) return -1;
        if (list.ifc_len < room - (int)sizeof(*items)) return local;
    }
    errno = EAGAIN;
    return -1;
}

static int inspect_interface(BypassLink *link, int fd, char *error, size_t size) {
    const LinkConfig *cfg = &link->cfg;
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", cfg->ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return socket_error(error, size, "private interface index");
    link->ifindex = (unsigned)ifr.ifr_ifindex;
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) return socket_error(error, size, "private interface MAC");
    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER || memcmp(ifr.ifr_hwaddr.sa_data, cfg->mac, ETH_ALEN))
        return errorf(error, size, "configured MAC does not match dedicated Ethernet interface");
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) return socket_error(error, size, "private interface flags");
    if (!(ifr.ifr_flags & IFF_UP) || (ifr.ifr_flags & IFF_LOOPBACK))
        return errorf(error, size, "dedicated Ethernet interface is down or loopback");
    int owned = kernel_owns_ip(fd, cfg->ipv4_be, cfg->ifname);
    if (owned < 0) return socket_error(error, size, "list IPv4 addresses in interface namespace");
    if (cfg->shared_ip && owned != 1) {
        errno = EADDRNOTAVAIL;
        return errorf(error, size, "shared service IPv4 must be assigned only to the selected interface");
    }
    if (!cfg->shared_ip && owned) {
        errno = EADDRINUSE;
        return errorf(error, size, "service IPv4 is already assigned to Linux in the interface namespace");
    }
    if (ioctl(fd, SIOCGIFMTU, &ifr) < 0) return socket_error(error, size, "private interface MTU");
    if (ifr.ifr_mtu < 576 || ifr.ifr_mtu > 9000 || (cfg->mtu && cfg->mtu != (unsigned)ifr.ifr_mtu))
        return errorf(error, size, "interface MTU %d differs from requested %u or is outside 576..9000",
                      ifr.ifr_mtu, cfg->mtu);
    link->cfg.mtu = (unsigned)ifr.ifr_mtu;
    return 0;
}

static int map_packet_rings(BypassLink *link, unsigned rx_slots, char *error, size_t size) {
    int fd = link->fd;
    unsigned frame = 2048;
    while (frame < link->cfg.mtu + ETH_HLEN + 128) frame *= 2;
    unsigned max_slots = MAX_RX_BYTES / frame;
    if (rx_slots && (rx_slots < BLOCK_FRAMES || (rx_slots & (rx_slots - 1)) || rx_slots > max_slots)) {
        errno = EINVAL;
        return errorf(error, size, "RX ring needs a power of two in %u..%u for interface MTU %u",
                      BLOCK_FRAMES, max_slots, link->cfg.mtu);
    }
    unsigned tx_frame = frame;
    if (link->cfg.packet_gso) {
        /* Ordinary queued transmit lets Linux segment in software when the
         * device lacks TSO. A delegated socket may have different defaults. */
        int one = 1, zero = 0;
        if (setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &zero, sizeof(zero)) ||
            setsockopt(fd, SOL_PACKET, PACKET_VNET_HDR, &one, sizeof(one)))
            return socket_error(error, size, "queued AF_PACKET GSO setup");
        if (tx_frame < 8192) tx_frame = 8192;
    }
    unsigned tx_slots = link->cfg.packet_tx_frames ? link->cfg.packet_tx_frames : DEFAULT_TX_FRAMES;
    if (tx_slots < BLOCK_FRAMES || (tx_slots & (tx_slots - 1)) || tx_slots > MAX_TX_BYTES / tx_frame) {
        errno = EINVAL;
        return errorf(error, size, "TX ring needs a power of two in %u..%u for %u-byte slots",
                      BLOCK_FRAMES, MAX_TX_BYTES / tx_frame, tx_frame);
    }
    if (rx_slots) {
        struct tpacket_req rx = {.tp_block_size = frame * BLOCK_FRAMES, .tp_frame_size = frame,
                                 .tp_block_nr = rx_slots / BLOCK_FRAMES, .tp_frame_nr = rx_slots};
        if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &rx, sizeof(rx)))
            return socket_error(error, size, "packet RX ring");
    }
    struct tpacket_req tx = {.tp_block_size = tx_frame * BLOCK_FRAMES, .tp_frame_size = tx_frame,
                             .tp_block_nr = tx_slots / BLOCK_FRAMES, .tp_frame_nr = tx_slots};
    if (setsockopt(fd, SOL_PACKET, PACKET_TX_RING, &tx, sizeof(tx)))
        return socket_error(error, size, "packet TX ring");
    link->rx_slots = rx_slots;
    link->tx_slots = tx_slots;
    link->frame_size = frame;
    link->tx_frame_size = tx_frame;
    link->rx_bytes = (size_t)frame * rx_slots;
    link->mapping_bytes = link->rx_bytes + (size_t)tx_frame * tx_slots;
    void *p = mmap(NULL, link->mapping_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return socket_error(error, size, "packet rings mmap");
    link->mapping = p;
    return 0;
}

static int open_packet(BypassLink *link, char *error, size_t size) {
    const LinkConfig *cfg = &link->cfg;
    /* Never open an unbound ETH_P_ALL listener. Protocol zero keeps a new
     * socket dormant until the private device and filter are both selected. */
    int fd = cfg->packet_fd >= 0 ? fcntl(cfg->packet_fd, F_DUPFD_CLOEXEC, 3) :
                                  socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return socket_error(error, size, "dedicated packet socket");
    link->fd = fd;
    if (inspect_interface(link, fd, error, size) < 0) return -1;
    if (cfg->packet_fd >= 0) {
        struct sockaddr_ll local = {0};
        socklen_t n = sizeof(local);
        int type = 0;
        socklen_t tn = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tn) || getsockname(fd, (void *)&local, &n))
            return socket_error(error, size, "delegated packet socket");
        if (type != SOCK_RAW || local.sll_family != AF_PACKET || local.sll_ifindex != (int)link->ifindex ||
            local.sll_protocol != htons(ETH_P_ALL))
            return errorf(error, size, "delegated fd is not a bound ETH_P_ALL raw socket on %s", cfg->ifname);
    }
    int one = 1, version = TPACKET_V2;
    if (filter_socket(fd, cfg) < 0 ||
        setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof(one)) ||
        setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)))
        return socket_error(error, size, "private transport filter or TPACKET_V2 setup");
    if (cfg->packet_fd < 0) {
        struct sockaddr_ll local = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                                    .sll_ifindex = (int)link->ifindex};
        if (bind(fd, (void *)&local, sizeof(local)) < 0) return socket_error(error, size, "packet bind");
    }
    unsigned slots = cfg->packet_rx_frames ? cfg->packet_rx_frames : DEFAULT_RX_FRAMES;
    return map_packet_rings(link, slots, error, size);
}

#ifdef TWELVE_AF_XDP
/* Protocol zero binds only the egress device and registers no packet receive
 * hook. The TX-ring payload carries its Ethernet protocol in the actual frame. */
static int open_packet_sender(BypassLink *link, char *error, size_t size) {
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return socket_error(error, size, "hybrid packet TX socket");
    link->fd = fd;
    int version = TPACKET_V2;
    struct sockaddr_ll local = {.sll_family = AF_PACKET, .sll_ifindex = (int)link->ifindex};
    if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) ||
        bind(fd, (void *)&local, sizeof(local)))
        return socket_error(error, size, "bind TX-only private packet ring");
    return map_packet_rings(link, 0, error, size);
}
#endif

BypassLink *link_open(const LinkConfig *cfg, char *error, size_t size) {
    if (error && size) *error = '\0';
    if (!cfg || !cfg->ifname || !*cfg->ifname || strlen(cfg->ifname) >= IFNAMSIZ ||
        !cfg->port || !cfg->ipv4_be || (cfg->mac[0] & 1)) {
        errorf(error, size, "invalid dedicated interface, address, MAC or port");
        return NULL;
    }
    if (cfg->shared_ip && (cfg->packet_fd >= 0 ||
        !link_is_xdp(cfg->mode))) {
        errorf(error, size, "sharing Linux IPv4 requires explicit XDP mode in the interface namespace");
        return NULL;
    }
    if (cfg->local_listener && (!cfg->shared_ip ||
        (cfg->local_bind_be && cfg->local_bind_be != cfg->ipv4_be))) {
        errorf(error, size, "local listener requires shared XDP, bound to HOST or wildcard");
        return NULL;
    }
    if (cfg->packet_gso && cfg->mode != LINK_PACKET &&
        (!link_is_xdp(cfg->mode) || cfg->xdp_tx_frames)) {
        errorf(error, size, "packet GSO requires explicit packet mode or an XDP receiver without XDP TX");
        return NULL;
    }
    if (cfg->packet_rx_frames && cfg->mode != LINK_PACKET) {
        errorf(error, size, "setting the PACKET_MMAP RX ring size requires explicit packet mode");
        return NULL;
    }
    if (cfg->packet_tx_frames && cfg->mode != LINK_PACKET && !cfg->packet_gso) {
        errorf(error, size, "setting the PACKET_MMAP TX ring size requires a packet sender");
        return NULL;
    }
    if ((cfg->xdp_rx_frames || cfg->xdp_tx_frames) && !link_is_xdp(cfg->mode)) {
        errorf(error, size, "setting XDP ring sizes requires explicit XDP mode");
        return NULL;
    }
    if (cfg->probe_rx_cpus && !link_is_xdp(cfg->mode)) {
        errorf(error, size, "XDP RX CPU measurement requires an explicit XDP mode");
        return NULL;
    }
    BypassLink *link = calloc(1, sizeof(*link));
    if (!link) { socket_error(error, size, "allocate link"); return NULL; }
    link->cfg = *cfg;
    link->fd = -1;
    if (cfg->mode < LINK_AUTO || cfg->mode > LINK_XDP_GENERIC) {
        errorf(error, size, "unknown link mode"); free(link); return NULL;
    }
    if (link_is_xdp(cfg->mode) ||
        (cfg->mode == LINK_AUTO && cfg->packet_fd < 0)) {
#ifdef TWELVE_AF_XDP
        if (cfg->packet_fd >= 0) {
            errorf(error, size, "AF_XDP setup must run in the dedicated interface namespace");
            free(link); return NULL;
        }
        int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (ctl < 0 || inspect_interface(link, ctl, error, size) < 0) {
            if (ctl >= 0) close(ctl); else socket_error(error, size, "interface control socket");
            free(link); return NULL;
        }
        close(ctl);
        if (cfg->packet_gso && open_packet_sender(link, error, size) < 0) {
            link_close(link);
            return NULL;
        }
        link->xdp = xdp_link_open(&link->cfg, link->ifindex, error, size);
        if (link->xdp) return link;
        int saved = errno;
        if (error && size && !*error) socket_error(error, size, "AF_XDP setup");
        if (cfg->mode != LINK_AUTO || saved == EBUSY) {link_close(link);return NULL;}
        if (error && size) *error = '\0';
#else
        if (cfg->mode != LINK_AUTO) {
            errorf(error, size, "AF_XDP was not compiled; request the explicit AF_XDP build");
            free(link); return NULL;
        }
#endif
    }
    if (open_packet(link, error, size) < 0) {
        link_close(link);
        return NULL;
    }
    return link;
}

/* Local virtual links can deliver a checksum-partial skb to an AF_PACKET
 * receiver. Complete only those packets the kernel explicitly marks; a bad
 * checksum on a real received segment remains visible to the TCP stack. */
static int complete_tcp_checksum(unsigned char *frame, unsigned len) {
    if (len < 54 || get16(frame + 12) != ETH_P_IP || (frame[14] >> 4) != 4 ||
        frame[23] != IPPROTO_TCP) return -1;
    unsigned char *ip = frame + ETH_HLEN;
    unsigned hlen = (ip[0] & 15u) * 4, total = get16(ip + 2);
    if (hlen < 20 || total < hlen + 20 || total > len - ETH_HLEN) return -1;
    unsigned char *tcp = ip + hlen;
    unsigned tcp_len = total - hlen;
    tcp[16] = tcp[17] = 0;
    uint32_t sum = add_words(tcp_len + IPPROTO_TCP, ip + 12, 8);
    sum = add_words(sum, tcp, tcp_len);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    unsigned checksum = (~sum) & 65535;
    tcp[16] = (unsigned char)(checksum >> 8);
    tcp[17] = (unsigned char)checksum;
    return 0;
}

static int own_input(const LinkConfig *cfg, const unsigned char *p, unsigned n) {
    static const unsigned char broadcast[6] = {255, 255, 255, 255, 255, 255};
    if (n < ETH_HLEN || (p[6] & 1)) return 0;
    uint16_t proto = get16(p + 12);
    if (proto == ETH_P_ARP)
        return n >= 42 && (!memcmp(p, cfg->mac, 6) || !memcmp(p, broadcast, 6)) &&
               get32(p + 14) == 0x00010800 && get16(p + 18) == 0x0604 &&
               (get16(p + 20) == ARPOP_REQUEST || get16(p + 20) == ARPOP_REPLY) &&
               (!cfg->shared_ip || (get16(p + 20) == ARPOP_REPLY &&
                 !memcmp(p, cfg->mac, 6) && !memcmp(p + 32, cfg->mac, 6))) &&
               !memcmp(p + 6, p + 22, 6) && !memcmp(p + 38, &cfg->ipv4_be, 4);
    if (proto != ETH_P_IP || n < 54 || memcmp(p, cfg->mac, 6) || (p[14] >> 4) != 4 ||
        p[23] != IPPROTO_TCP || (get16(p + 20) & 0x3fff) ||
        memcmp(p + 30, &cfg->ipv4_be, 4)) return 0;
    unsigned hlen = (p[14] & 15u) * 4, total = get16(p + 16);
    return hlen >= 20 && total >= hlen + 20 && total <= n - ETH_HLEN &&
           get16(p + ETH_HLEN + hlen + 2) == cfg->port;
}

static struct tpacket2_hdr *rx_header(BypassLink *link, unsigned seq) {
    return (void *)(link->mapping + (size_t)(seq & (link->rx_slots - 1)) * link->frame_size);
}

int link_receive(BypassLink *link, LinkFrame *frames, unsigned capacity) {
#ifdef TWELVE_AF_XDP
    if (link->xdp) {
        int count = xdp_link_receive(link->xdp, frames, capacity);
        if (count <= 0) return count;
        unsigned kept = 0;
        for (int i = 0; i < count; ++i) {
            if (own_input(&link->cfg, frames[i].data, frames[i].len)) frames[kept++] = frames[i];
            else ++link->rejected;
        }
        link->rx_frames += kept;
        if (!kept) xdp_link_release(link->xdp);
        return (int)kept;
    }
#endif
    if (link->held) { errno = EBUSY; return -1; }
    unsigned n = 0;
    while (n < capacity && link->held < link->rx_slots) {
        struct tpacket2_hdr *h = rx_header(link, link->rx + link->held);
        unsigned status = __atomic_load_n(&h->tp_status, __ATOMIC_ACQUIRE);
        if (!(status & TP_STATUS_USER)) break;
        ++link->held;
        if (h->tp_mac > link->frame_size || h->tp_snaplen > link->frame_size - h->tp_mac ||
            h->tp_snaplen != h->tp_len || h->tp_len > link->cfg.mtu + ETH_HLEN) {
            ++link->oversized;
            continue;
        }
        unsigned char *p = (unsigned char *)h + h->tp_mac;
        if (!own_input(&link->cfg, p, h->tp_len)) { ++link->rejected; continue; }
        if (status & TP_STATUS_CSUMNOTREADY) {
            if (complete_tcp_checksum(p, h->tp_len) < 0) { ++link->rejected; continue; }
            ++link->partial_checksums;
        }
        frames[n++] = (LinkFrame){.data = p, .len = h->tp_len};
        ++link->rx_frames;
    }
    if (!n) link_release(link);
    return (int)n;
}

void link_release(BypassLink *link) {
#ifdef TWELVE_AF_XDP
    if (link->xdp) { xdp_link_release(link->xdp); return; }
#endif
    for (unsigned i = 0; i < link->held; ++i) {
        struct tpacket2_hdr *h = rx_header(link, link->rx++);
        __atomic_store_n(&h->tp_status, TP_STATUS_KERNEL, __ATOMIC_RELEASE);
    }
    link->held = 0;
}

static int own_output(const LinkConfig *cfg, const unsigned char *p, unsigned n) {
    if (n < ETH_HLEN || n > cfg->mtu + ETH_HLEN || memcmp(p + 6, cfg->mac, 6)) return 0;
    uint16_t proto = get16(p + 12);
    if (proto == ETH_P_ARP)
        return n >= 42 && get32(p + 14) == 0x00010800 && get16(p + 18) == 0x0604 &&
               (get16(p + 20) == ARPOP_REQUEST || get16(p + 20) == ARPOP_REPLY) &&
               (!cfg->shared_ip || get16(p + 20) == ARPOP_REQUEST) &&
               !memcmp(p + 22, cfg->mac, 6) && !memcmp(p + 28, &cfg->ipv4_be, 4);
    if (proto != ETH_P_IP || n < 54 || (p[14] >> 4) != 4 || p[23] != IPPROTO_TCP ||
        (p[0] & 1) || memcmp(p + 26, &cfg->ipv4_be, 4) || (get16(p + 20) & 0x3fff)) return 0;
    unsigned hlen = (p[14] & 15u) * 4, total = get16(p + 16);
    return hlen >= 20 && total >= hlen + 20 && total <= n - ETH_HLEN &&
           get16(p + ETH_HLEN + hlen) == cfg->port;
}

static struct tpacket2_hdr *packet_tx_slot(BypassLink *link, unsigned sequence) {
    unsigned slot = sequence & (link->tx_slots - 1);
    return (void *)(link->mapping + link->rx_bytes + (size_t)slot * link->tx_frame_size);
}

static void publish_packet(BypassLink *link, struct tpacket2_hdr *slot, unsigned n) {
    slot->tp_len = n;
    __atomic_store_n(&slot->tp_status, TP_STATUS_SEND_REQUEST, __ATOMIC_RELEASE);
    ++link->tx;
}

static void finish_packet_gso(BypassLink *link) {
    PacketGso *pending = &link->pending;
    if (!pending->length) return;
    struct tpacket2_hdr *slot = packet_tx_slot(link, link->tx);
    struct virtio_net_hdr *meta = (void *)((unsigned char *)slot + TPACKET_ALIGN(sizeof(*slot)));
    if (pending->segments > 1) {
        gso_header(pending, meta);
        ++link->gso_groups;
        link->gso_merged += pending->segments - 1;
    }
    publish_packet(link, slot, pending->length + sizeof(*meta));
    pending->length = 0;
}

/* An unpublished slot may grow only until the next unrelated frame or kick.
 * lwIP still owns its original data and independently tracks every segment. */
static int queue_packet(BypassLink *link, const unsigned char *data, unsigned n) {
    const unsigned offset = TPACKET_ALIGN(sizeof(struct tpacket2_hdr));
    if (link->pending.length) {
        struct tpacket2_hdr *slot = packet_tx_slot(link, link->tx);
        unsigned prefix = offset + sizeof(struct virtio_net_hdr);
        int merged = gso_append(&link->pending, (unsigned char *)slot + prefix,
                                link->tx_frame_size - prefix, data, n);
        if (merged != GSO_CONTINUE) finish_packet_gso(link);
        if (merged != GSO_SEPARATE) {
            ++link->tx_frames;
            return 1;
        }
    }
    struct tpacket2_hdr *h = packet_tx_slot(link, link->tx);
    unsigned status = __atomic_load_n(&h->tp_status, __ATOMIC_ACQUIRE);
    if (status & TP_STATUS_WRONG_FORMAT) { ++link->tx_errors; errno = EPROTO; return -1; }
    if (status & (TP_STATUS_SENDING | TP_STATUS_SEND_REQUEST)) { ++link->tx_busy; return 0; }
    unsigned char *payload = (unsigned char *)h + offset;
    ++link->tx_frames;
    if (link->cfg.packet_gso) {
        struct virtio_net_hdr *meta = (void *)payload;
        *meta = (struct virtio_net_hdr){0};
        memcpy(meta + 1, data, n);
        if (!gso_begin(&link->pending, data, n)) publish_packet(link, h, n + sizeof(*meta));
    } else {
        memcpy(payload, data, n);
        publish_packet(link, h, n);
    }
    return 1;
}

int link_send(BypassLink *link, const void *data, unsigned n) {
    if (!own_output(&link->cfg, data, n)) { ++link->rejected; errno = EINVAL; return -1; }
#ifdef TWELVE_AF_XDP
    if (link->xdp && !link->cfg.packet_gso) {
        int sent = xdp_link_send(link->xdp, data, n);
        if (sent > 0) ++link->tx_frames;
        return sent;
    }
#endif
    return queue_packet(link, data, n);
}

static int flush_packet(BypassLink *link) {
    finish_packet_gso(link);
    if (link->tx == link->kicked) return 0;
    ssize_t n = sendto(link->fd, NULL, 0, MSG_DONTWAIT | MSG_NOSIGNAL, NULL, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == ENOBUFS || errno == EINTR) {
            ++link->tx_deferred;
            return 0;
        }
        ++link->tx_errors;
        return -1;
    }
    /* A nonblocking kick may send a prefix of the ring. Keep the suffix
     * pending and retry even if the application produces no further packets. */
    while (link->kicked != link->tx) {
        struct tpacket2_hdr *h = packet_tx_slot(link, link->kicked);
        unsigned status = __atomic_load_n(&h->tp_status, __ATOMIC_ACQUIRE);
        if (status & TP_STATUS_WRONG_FORMAT) { ++link->tx_errors; errno = EPROTO; return -1; }
        if (status & TP_STATUS_SEND_REQUEST) break;
        ++link->kicked;
    }
    return 0;
}

int link_flush(BypassLink *link) {
#ifdef TWELVE_AF_XDP
    if (link->xdp && !link->cfg.packet_gso) return xdp_link_flush(link->xdp);
#endif
    return flush_packet(link);
}

int link_wait(BypassLink *link, int timeout_ms) {
#ifdef TWELVE_AF_XDP
    if (link->xdp) {
        int pending_fd = -1;
        if (link->cfg.packet_gso) {
            if (flush_packet(link) < 0) return -1;
            if (link->kicked != link->tx) pending_fd = link->fd;
        }
        return xdp_link_wait(link->xdp, timeout_ms, pending_fd);
    }
#endif
    if (link_flush(link) < 0) return -1;
    struct pollfd p = {.fd = link->fd, .events = POLLIN | (link->tx != link->kicked ? POLLOUT : 0)};
    int n = poll(&p, 1, timeout_ms);
    if (n < 0 && errno == EINTR) return 0;
    if (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) { errno = EIO; return -1; }
    return n;
}

int link_local_listener(const BypassLink *link) {
#ifdef TWELVE_AF_XDP
    if (link && link->xdp) return xdp_link_local_listener(link->xdp);
#endif
    (void)link;
    return -1;
}

void link_set_local_epoll(BypassLink *link, int epfd) {
#ifdef TWELVE_AF_XDP
    if (link && link->xdp) xdp_link_set_local_epoll(link->xdp, epfd);
#else
    (void)link;
    (void)epfd;
#endif
}

const char *link_mode(const BypassLink *link) {
#ifdef TWELVE_AF_XDP
    if (link->xdp) return xdp_link_mode(link->xdp);
#endif
    if (link->cfg.packet_gso) return "AF_PACKET/TPACKET_V2 RX+TX PACKET_MMAP with TCP segmentation";
    return "AF_PACKET/TPACKET_V2 RX+TX PACKET_MMAP (copy)";
}

void link_report(BypassLink *link) {
    if (!link) return;
#ifdef TWELVE_AF_XDP
    if (link->xdp) {
        if (link->cfg.packet_gso) {
            fprintf(stderr, "hybrid link xdp_rx=%" PRIu64 " packet_tx_frames=%" PRIu64
                    " rejected=%" PRIu64 " tx_busy=%" PRIu64 " tx_kick_deferred=%" PRIu64
                    " tx_errors=%" PRIu64 " gso_groups=%" PRIu64 " merged_frames=%" PRIu64
                    " packet_tx_slots=%u tx_slot_bytes=%u\n",
                    link->rx_frames, link->tx_frames, link->rejected, link->tx_busy, link->tx_deferred,
                    link->tx_errors, link->gso_groups, link->gso_merged, link->tx_slots, link->tx_frame_size);
        } else {
            fprintf(stderr, "XDP link rx=%" PRIu64 " tx=%" PRIu64 " rejected=%" PRIu64 "\n",
                    link->rx_frames, link->tx_frames, link->rejected);
        }
        xdp_link_report(link->xdp);
        return;
    }
#endif
    if (!link->mapping) return;
    /* PACKET_STATISTICS resets the kernel counters on each successful read.
     * Accumulate intervals so SIGUSR1 snapshots and the shutdown totals agree. */
    struct tpacket_stats stats = {0};
    socklen_t length = sizeof(stats);
    int stat_errno = 0;
    if (getsockopt(link->fd, SOL_PACKET, PACKET_STATISTICS, &stats, &length) < 0)
        stat_errno = errno;
    else {
        link->kernel_packets += stats.tp_packets;
        link->kernel_drops += stats.tp_drops;
    }
    fprintf(stderr, "packet link rx=%" PRIu64 " tx=%" PRIu64 " checksum_completed=%" PRIu64
            " rejected=%" PRIu64 " oversize=%" PRIu64 " kernel_packets=%" PRIu64
            " kernel_drops=%" PRIu64 " stat_errno=%d tx_busy=%" PRIu64
            " tx_kick_deferred=%" PRIu64 " tx_errors=%" PRIu64 " rx_slots=%u tx_slots=%u rx_bytes=%zu\n",
            link->rx_frames, link->tx_frames, link->partial_checksums, link->rejected, link->oversized,
            link->kernel_packets, link->kernel_drops, stat_errno, link->tx_busy, link->tx_deferred,
            link->tx_errors, link->rx_slots, link->tx_slots, link->rx_bytes);
    if (link->cfg.packet_gso)
        fprintf(stderr, "packet GSO groups=%" PRIu64 " merged_frames=%" PRIu64 " tx_slot_bytes=%u\n",
                link->gso_groups, link->gso_merged, link->tx_frame_size);
}

void link_close(BypassLink *link) {
    if (!link) return;
    link_report(link);
#ifdef TWELVE_AF_XDP
    if (link->xdp) xdp_link_close(link->xdp);
#endif
    if (link->mapping) munmap(link->mapping, link->mapping_bytes);
    if (link->fd >= 0) close(link->fd);
    free(link);
}
