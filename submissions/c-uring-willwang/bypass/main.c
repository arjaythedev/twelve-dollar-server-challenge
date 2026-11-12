#include "challenge.h"
#include "link.h"
#include "local.h"
#include "stack.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/capability.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t stopping, stats_requested;

static void signal_handler(int signo) {
    if (signo == SIGUSR1) stats_requested = 1;
    else stopping = 1;
}

static void usage(FILE *stream) {
    fputs("Usage: start.sh --bypass --interface IFNAME --address IPV4/PREFIX\n"
          "       [--gateway IPV4] [--mode auto|packet|xdp|xdp-skb|xdp-zc]\n"
          "       [--shared-ip] [--packet-fd FD] [--packet-rx-frames N] [--packet-tx-frames N]\n"
          "       [--packet-gso] [--queue N]\n"
          "       [--xdp-rx-frames N] [--xdp-tx-frames N] [--max-connections N] [--rx-cpu-stats]\n"
          "Set SQLITE_PATH, JWT_SECRET, HOST and PORT as for the standard server.\n"
          "Use a dedicated service IPv4 address, or select --shared-ip with an explicit XDP mode.\n",
          stream);
}

static int number(const char *s, unsigned limit, unsigned *out) {
    char *end;
    if (!s || *s < '0' || *s > '9') return -1;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > limit) return -1;
    *out = (unsigned)n;
    return 0;
}

static int unicast(const char *s, uint32_t *addr) {
    if (inet_pton(AF_INET, s, addr) != 1) return -1;
    unsigned first = ntohl(*addr) >> 24;
    return first != 0 && first != 127 && first < 224 ? 0 : -1;
}

static int subnet(const char *s, struct stack_config *stack) {
    const char *slash = strchr(s, '/');
    char ip[INET_ADDRSTRLEN];
    unsigned prefix;
    if (!slash || (size_t)(slash - s) >= sizeof(ip) || number(slash + 1, 32, &prefix))
        return -1;
    memcpy(ip, s, (size_t)(slash - s));
    ip[slash - s] = '\0';
    if (unicast(ip, &stack->address_be)) return -1;
    stack->netmask_be = htonl(prefix ? UINT32_MAX << (32 - prefix) : 0);
    uint32_t host = stack->address_be & ~stack->netmask_be;
    return prefix < 31 && (!host || host == ~stack->netmask_be) ? -1 : 0;
}

static int options(int argc, char **argv, LinkConfig *link, struct stack_config *stack) {
    static const struct option opts[] = {
        {"interface", required_argument, NULL, 'i'}, {"address", required_argument, NULL, 'a'},
        {"gateway", required_argument, NULL, 'g'}, {"mode", required_argument, NULL, 'm'},
        {"packet-fd", required_argument, NULL, 'f'}, {"queue", required_argument, NULL, 'q'},
        {"max-connections", required_argument, NULL, 'n'}, {"shared-ip", no_argument, NULL, 's'},
        {"packet-rx-frames", required_argument, NULL, 'r'}, {"packet-tx-frames", required_argument, NULL, 'T'},
        {"xdp-rx-frames", required_argument, NULL, 'x'},
        {"xdp-tx-frames", required_argument, NULL, 't'},
        {"rx-cpu-stats", no_argument, NULL, 'c'}, {"packet-gso", no_argument, NULL, 'G'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        unsigned value;
        switch (opt) {
        case 'i': link->ifname = optarg; break;
        case 'a': if (subnet(optarg, stack)) return -1; break;
        case 'g': if (unicast(optarg, &stack->gateway_be)) return -1; break;
        case 'm':
            if (!strcmp(optarg, "auto")) link->mode = LINK_AUTO;
            else if (!strcmp(optarg, "packet")) link->mode = LINK_PACKET;
            else if (!strcmp(optarg, "xdp")) link->mode = LINK_XDP;
            else if (!strcmp(optarg, "xdp-skb")) link->mode = LINK_XDP_GENERIC;
            else if (!strcmp(optarg, "xdp-zc")) link->mode = LINK_XDP_ZEROCOPY;
            else return -1;
            break;
        case 'f':
            if (number(optarg, INT_MAX, &value)) return -1;
            link->packet_fd = (int)value;
            break;
        case 'q': if (number(optarg, UINT_MAX, &link->queue_id)) return -1; break;
        case 's': link->shared_ip = 1; break;
        case 'G': link->packet_gso = 1; break;
        case 'c': link->probe_rx_cpus = 1; break;
        case 'r': case 'T': case 'x': case 't':
            if (number(optarg, 16384, &value) || value < (opt == 'r' || opt == 'T' ? 16u : 64u) ||
                (value & (value - 1))) return -1;
            if (opt == 'r') link->packet_rx_frames = value;
            else if (opt == 'T') link->packet_tx_frames = value;
            else if (opt == 'x') link->xdp_rx_frames = value;
            else link->xdp_tx_frames = value;
            break;
        case 'n': if (number(optarg, UINT_MAX, &stack->max_connections) || !stack->max_connections) return -1; break;
        case 'h': usage(stdout); exit(0);
        default: return -1;
        }
    }
    if (optind != argc || !link->ifname || !*link->ifname || strlen(link->ifname) >= IFNAMSIZ ||
        !stack->address_be) return -1;
    if (stack->gateway_be && (stack->gateway_be == stack->address_be ||
        ((stack->gateway_be ^ stack->address_be) & stack->netmask_be))) return -1;
    if (link->packet_fd >= 0) {
        if (link->mode != LINK_AUTO && link->mode != LINK_PACKET) return -1;
        link->mode = LINK_PACKET;
    }
    if (link->shared_ip && !link_is_xdp(link->mode)) return -1;
    if (link->probe_rx_cpus && !link_is_xdp(link->mode)) return -1;
    if (link->packet_rx_frames && link->mode != LINK_PACKET) return -1;
    if (link->packet_tx_frames && link->mode != LINK_PACKET && !link->packet_gso) return -1;
    if (link->packet_gso && link->mode != LINK_PACKET && !link_is_xdp(link->mode)) return -1;
    if (link->packet_gso && link->xdp_tx_frames) return -1;
    if ((link->xdp_rx_frames || link->xdp_tx_frames) && !link_is_xdp(link->mode)) return -1;
    return 0;
}

/* An inherited packet fd can belong to a different network namespace. Query
 * that fd for its device's real MAC and MTU, rather than consulting /sys here. */
static int interface_info(LinkConfig *link, struct stack_config *stack) {
    int fd = link->packet_fd;
    if (fd < 0) fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq device = {0};
    memcpy(device.ifr_name, link->ifname, strlen(link->ifname) + 1);
    int rc = ioctl(fd, SIOCGIFHWADDR, &device);
    if (!rc && device.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        errno = EPROTONOSUPPORT;
        rc = -1;
    }
    if (!rc) {
        memcpy(link->mac, device.ifr_hwaddr.sa_data, sizeof(link->mac));
        memcpy(stack->mac, link->mac, sizeof(stack->mac));
        rc = ioctl(fd, SIOCGIFMTU, &device);
    }
    if (!rc && (device.ifr_mtu < 576 || device.ifr_mtu > UINT16_MAX)) {
        errno = EINVAL;
        rc = -1;
    }
    if (!rc) {
        link->mtu = (unsigned)device.ifr_mtu;
        stack->mtu = (uint16_t)device.ifr_mtu;
    }
    if (link->packet_fd < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return rc;
}

typedef struct {
    BypassLink *link;
    int tx_error;
} Transport;

static int send_frame(void *opaque, const unsigned char *frame, size_t length) {
    Transport *transport = opaque;
    if (length > UINT_MAX) return -1;
    int sent = link_send(transport->link, frame, (unsigned)length);
    if (sent < 0) transport->tx_error = errno ? errno : EIO;
    return sent == 1 ? 0 : -1;
}

/* Packet rings and an attached XDP link remain usable through their open
 * descriptors. HTTP, the TCP stack, and SQLite need no Linux capabilities. */
static int release_capabilities(void) {
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
    struct __user_cap_data_struct empty[_LINUX_CAPABILITY_U32S_3] = {0};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) < 0) return -1;
    return (int)syscall(SYS_capset, &header, empty);
}

static void report_stats(void) {
    struct stack_stats stats;
    stack_get_stats(&stats);
    fprintf(stderr, "bypass stats accepted=%" PRIu64 " live=%" PRIu64 " peak=%" PRIu64
            " rx=%" PRIu64 " rx_dropped=%" PRIu64 " tx=%" PRIu64 " tx_busy=%" PRIu64
            " requests=%" PRIu64 " commit_failures=%" PRIu64
            " buffered=%" PRIu64 " peak_buffered=%" PRIu64
            " protocol_bytes=%" PRIu64 " peak_protocol_bytes=%" PRIu64 "\n",
            stats.accepted, stats.live, stats.peak_live, stats.input_frames, stats.input_dropped,
            stats.output_frames, stats.output_busy, stats.requests, stats.commit_failures,
            stats.buffered_bytes, stats.peak_buffered_bytes,
            stats.protocol_bytes, stats.peak_protocol_bytes);
    stack_tail_report(stderr);
}

enum { FRAME_BATCH = 64, LOCAL_INTERVAL = 8 };

static int network_pass(Transport *transport, LinkFrame frames[FRAME_BATCH]) {
    if (stats_requested) {
        stats_requested = 0;
        report_stats();
        link_report(transport->link);
    }
    int count = link_receive(transport->link, frames, FRAME_BATCH);
    if (count < 0) return -1;
    for (int i = 0; i < count; ++i) stack_input(frames[i].data, frames[i].len);
    if (count) link_release(transport->link);
    if (stack_dispatch() < 0 || link_flush(transport->link) < 0) return -1;
    if (transport->tx_error) { errno = transport->tx_error; return -1; }
    return count;
}

static int wait_network(Transport *transport, unsigned delay) {
    if (delay > 1000) delay = 1000;
    return delay && link_wait(transport->link, (int)delay) < 0 && errno != EINTR ? -1 : 0;
}

static int serve(Transport *transport) {
    LinkFrame frames[FRAME_BATCH];
    while (!stopping) {
        int count = network_pass(transport, frames);
        if (count < 0 || (!count && wait_network(transport, stack_timeout_ms()) < 0)) return -1;
    }
    return 0;
}

/* Local kernel TCP and external lwIP share the SQLite issuer. Each bounded
 * pass closes its transaction before another transport can use the database. */
static int serve_shared(Transport *transport) {
    LinkFrame frames[FRAME_BATCH];
    unsigned batches = 0, local_delay = UINT_MAX;
    while (!stopping) {
        int count = network_pass(transport, frames);
        if (count < 0) return -1;
        if (!count || !local_delay || ++batches >= LOCAL_INTERVAL) {
            if (local_step() < 0) return -1;
            local_delay = local_timeout_ms();
            batches = 0;
        }
        if (!count) {
            unsigned delay = stack_timeout_ms();
            if (wait_network(transport, delay < local_delay ? delay : local_delay) < 0) return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    LinkConfig link = {.packet_fd = -1};
    struct stack_config stack = {.max_connections = 524288};
    const char *host = getenv("HOST"), *port = getenv("PORT");
    const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
    struct in_addr bind_address;
    unsigned port_number;
    if (options(argc, argv, &link, &stack) || !host || !path || !secret ||
        inet_pton(AF_INET, host, &bind_address) != 1 ||
        (bind_address.s_addr && bind_address.s_addr != stack.address_be) ||
        number(port, UINT16_MAX, &port_number) || !port_number) {
        usage(stderr);
        return 2;
    }
    link.ipv4_be = stack.address_be;
    link.port = stack.port = (uint16_t)port_number;
    link.local_listener = link.shared_ip;
    link.local_bind_be = bind_address.s_addr;
    if (interface_info(&link, &stack)) { perror("bypass Ethernet interface"); return 1; }
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
        sigaction(SIGUSR1, &action, NULL)) { perror("bypass signal handler"); return 1; }
    char error[512] = {0};
    Transport transport = {.link = link_open(&link, error, sizeof(error))};
    if (!transport.link) {
        fprintf(stderr, "bypass link: %s\n", error);
        return 1;
    }
    if (release_capabilities() < 0) {
        perror("bypass drop startup capabilities");
        link_close(transport.link);
        return 1;
    }
    if (app_init(secret) || db_open(path)) { link_close(transport.link); return 1; }
    if (link.shared_ip) {
        int epoll_fd = local_open(link_local_listener(transport.link));
        if (epoll_fd < 0) {
            perror("shared-address Linux listener");
            link_close(transport.link);
            db_close();
            return 1;
        }
        link_set_local_epoll(transport.link, epoll_fd);
    }
    int rc = 1;
    if (stack_init(&stack, send_frame, &transport)) {
        fputs("bypass TCP stack initialization failed\n", stderr);
    } else {
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &stack.address_be, address, sizeof(address));
        fprintf(stderr, "bypass ready mode=%s interface=%s address=%s port=%u mtu=%u\n",
                link_mode(transport.link), link.ifname, address, link.port, link.mtu);
        if (link.shared_ip) fprintf(stderr, "local HTTP listener on %s:%u\n", host, link.port);
        rc = (link.shared_ip ? serve_shared(&transport) : serve(&transport)) ? 1 : 0;
        if (rc) perror("bypass event loop");
        report_stats();
        stack_shutdown();
    }
    if (link.shared_ip) {
        link_set_local_epoll(transport.link, -1);
        local_close();
    }
    link_close(transport.link);
    db_close();
    return rc;
}
