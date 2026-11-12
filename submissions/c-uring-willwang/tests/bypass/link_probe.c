/* Link-level fixture for an owned, disposable veth. A normal client's SYN
 * receives the RST/ACK for a closed port. The HTTP load tests use lwIP. */
#define _GNU_SOURCE
#include "../../bypass/link.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/capability.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping, reporting;
static unsigned arp_requests, arp_replies, syns, invalid, pseudo_only;

static void signal_handler(int sig) {
    if (sig == SIGUSR1) reporting = 1;
    else stopping = 1;
}

static unsigned read16(const unsigned char *p) {
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)read16(p) << 16 | read16(p + 2);
}

static void write16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void write32(unsigned char *p, uint32_t value) {
    write16(p, value >> 16);
    write16(p + 2, value);
}

static unsigned add_words(unsigned sum, const unsigned char *p, unsigned n) {
    while (n >= 2) {
        sum += read16(p);
        p += 2;
        n -= 2;
    }
    return n ? sum + ((unsigned)*p << 8) : sum;
}

static unsigned fold(unsigned sum) {
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return sum;
}

static int send_frame(BypassLink *link, const unsigned char *frame, unsigned bytes) {
    for (unsigned attempt = 0; attempt < 5000 && !stopping; ++attempt) {
        int result = link_send(link, frame, bytes);
        if (result) return result > 0 ? 0 : -1;
        if (link_flush(link) < 0) return -1;
        struct timespec delay = {.tv_nsec = 100000};
        nanosleep(&delay, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int ask_neighbor(BypassLink *link, const LinkConfig *cfg) {
    unsigned char frame[42] = {0};
    memset(frame, 255, 6);
    memcpy(frame + 6, cfg->mac, 6);
    write16(frame + 12, 0x0806);
    write16(frame + 14, 1);
    write16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    write16(frame + 20, 1);
    memcpy(frame + 22, cfg->mac, 6);
    memcpy(frame + 28, &cfg->ipv4_be, 4);
    if (inet_pton(AF_INET, "10.241.1.254", frame + 38) != 1) return -1;
    return send_frame(link, frame, sizeof(frame));
}

static int receive_frame(BypassLink *link, const LinkConfig *cfg, LinkFrame in) {
    unsigned char reply[54] = {0};
    const unsigned char *p = in.data;
    if (in.len < 42) return -1;
    memcpy(reply, p + 6, 6);
    memcpy(reply + 6, cfg->mac, 6);
    if (read16(p + 12) == 0x0806) {
        if (read16(p + 20) == 2) {
            ++arp_replies;
            return 0;
        }
        if (read16(p + 20) != 1) return -1;
        ++arp_requests;
        memcpy(reply + 12, p + 12, 8);
        write16(reply + 20, 2);
        memcpy(reply + 22, cfg->mac, 6);
        memcpy(reply + 28, &cfg->ipv4_be, 4);
        memcpy(reply + 32, p + 22, 6);
        memcpy(reply + 38, p + 28, 4);
        return send_frame(link, reply, 42);
    }
    unsigned ihl = (p[14] & 15u) * 4, total = read16(p + 16);
    if (read16(p + 12) != 0x0800 || ihl < 20 || total < ihl + 20 || 14 + total > in.len) return -1;
    const unsigned char *tcp = p + 14 + ihl;
    if ((tcp[13] & 0x17) != 2) return 0;
    unsigned size = total - ihl;
    unsigned seed = fold(add_words(size + 6, p + 26, 8));
    unsigned expected = 65535 - fold(add_words(add_words(seed, tcp, 16), tcp + 18, size - 18));
    unsigned received = read16(tcp + 16);
    if (expected != received) {
        if (++invalid <= 4)
            fprintf(stderr, "syn_checksum=%04x expected=%04x ipv4_pseudoheader_seed=%04x\n",
                    received, expected, seed);
        if (received != seed) return -1;
        ++pseudo_only;
    }
    if (cfg->shared_ip && syns == 0 && ask_neighbor(link, cfg) < 0) return -1;
    ++syns;
    write16(reply + 12, 0x0800);
    reply[14] = 0x45;
    write16(reply + 16, 40);
    reply[22] = 64;
    reply[23] = 6;
    memcpy(reply + 26, &cfg->ipv4_be, 4);
    memcpy(reply + 30, p + 26, 4);
    write16(reply + 24, 65535 - fold(add_words(0, reply + 14, 20)));
    write16(reply + 34, cfg->port);
    write16(reply + 36, read16(tcp));
    write32(reply + 42, read32(tcp + 4) + 1);
    reply[46] = 0x50;
    reply[47] = 0x14;
    write16(reply + 50, 65535 - fold(add_words(add_words(26, reply + 26, 8), reply + 34, 20)));
    return send_frame(link, reply, sizeof(reply));
}

static unsigned long number(const char *s) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || errno) exit(2);
    return n;
}

int main(int argc, char **argv) {
    /* Numeric settings and addresses come directly from link_check.py. */
    if (argc != 10) return 2;
    struct stat ns;
    if (stat("/proc/self/ns/net", &ns) || (unsigned long)ns.st_ino == number(argv[1])) return 2;
    LinkConfig cfg = {.ifname = "bench0", .packet_fd = -1};
    if (!strcmp(argv[2], "packet")) cfg.mode = LINK_PACKET;
    else if (!strcmp(argv[2], "xdp")) cfg.mode = LINK_XDP;
    else if (!strcmp(argv[2], "xdp-skb")) cfg.mode = LINK_XDP_GENERIC;
    else if (!strcmp(argv[2], "xdp-zc")) cfg.mode = LINK_XDP_ZEROCOPY;
    else return 2;
    if (inet_pton(AF_INET, argv[3], &cfg.ipv4_be) != 1 || (ntohl(cfg.ipv4_be) >> 8) != 0x0af101) return 2;
    cfg.port = (uint16_t)number(argv[4]);
    if (cfg.port != 3000 && cfg.port != 3100) return 2;
    cfg.shared_ip = (int)number(argv[5]);
    cfg.probe_rx_cpus = (unsigned)number(argv[6]);
    if (cfg.mode == LINK_PACKET) cfg.packet_rx_frames = (unsigned)number(argv[7]);
    else cfg.xdp_rx_frames = (unsigned)number(argv[7]);
    cfg.xdp_tx_frames = (unsigned)number(argv[8]);
    cfg.packet_gso = (int)number(argv[9]);
    struct ifreq req = {0};
    strcpy(req.ifr_name, cfg.ifname);
    int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctl < 0 || ioctl(ctl, SIOCGIFHWADDR, &req) < 0) return 2;
    memcpy(cfg.mac, req.ifr_hwaddr.sa_data, 6);
    close(ctl);
    char error[320] = {0};
    BypassLink *link = link_open(&cfg, error, sizeof(error));
    if (!link) { fprintf(stderr, "open failed: %s\n", error); return 1; }
    struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[_LINUX_CAPABILITY_U32S_3] = {0};
    int result = 1;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) || syscall(SYS_capset, &header, caps) ||
        syscall(SYS_capget, &header, caps)) goto out;
    for (unsigned i = 0; i < _LINUX_CAPABILITY_U32S_3; ++i)
        if (caps[i].effective || caps[i].permitted || caps[i].inheritable) goto out;
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGUSR1, &action, NULL)) goto out;
    fprintf(stderr, "ready mode=%s caps=0 no_new_privs=1\n", link_mode(link));
    while (!stopping) {
        LinkFrame frames[64];
        if (reporting) { reporting = 0; link_report(link); }
        int n = link_receive(link, frames, 64);
        if (n < 0) goto out;
        for (int i = 0; i < n; ++i) if (receive_frame(link, &cfg, frames[i]) < 0) goto out;
        if (n) link_release(link);
        if (link_flush(link) < 0 || (!n && link_wait(link, 100) < 0)) goto out;
    }
    result = 0;
out:
    fprintf(stderr, "probe_syn=%u arp_requests=%u arp_replies=%u invalid_checksums=%u pseudo_only=%u\n",
            syns, arp_requests, arp_replies, invalid, pseudo_only);
    link_close(link);
    return result;
}
