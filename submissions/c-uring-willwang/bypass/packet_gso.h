#ifndef TWELVE_PACKET_GSO_H
#define TWELVE_PACKET_GSO_H

#include <linux/if_ether.h>
#include <linux/virtio_net.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

/* The packet backend accepts unaligned Ethernet headers. */
static inline uint16_t get16(const unsigned char *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint32_t add_words(uint32_t sum, const unsigned char *p, unsigned n) {
    while (n >= 2) { sum += get16(p); p += 2; n -= 2; }
    if (n) sum += (unsigned)p[0] << 8;
    return sum;
}

enum { TCP4_HEADER = ETH_HLEN + 20 + 20 };
typedef struct {
    unsigned length, segments;
    uint32_t next_sequence;
    uint16_t segment_size, last_ip_id;
} PacketGso;

/* lwIP emits IPv4 data with DF clear. Keep DF-set packets intact as well:
 * some TSO devices may choose the IP ID for an atomic IPv4 datagram.
 * Options, fragments, control flags and urgent data keep their own frame. */
static inline int gso_data_frame(const unsigned char *p, unsigned n) {
    if (n <= TCP4_HEADER || get16(p + 12) != ETH_P_IP) return 0;
    const unsigned char *ip = p + ETH_HLEN, *tcp = ip + 20;
    return ip[0] == 0x45 && get16(ip + 2) == n - ETH_HLEN &&
           get16(ip + 6) == 0 && ip[9] == IPPROTO_TCP &&
           tcp[12] == 0x50 && (tcp[13] == 0x10 || tcp[13] == 0x18) && !get16(tcp + 18);
}

static inline int gso_begin(PacketGso *pending, const unsigned char *p, unsigned n) {
    if (!gso_data_frame(p, n) || (p[ETH_HLEN + 20 + 13] & 8)) return 0;
    unsigned bytes = n - TCP4_HEADER;
    *pending = (PacketGso){.length = n, .segments = 1, .segment_size = (uint16_t)bytes,
        .next_sequence = get32(p + ETH_HLEN + 20 + 4) + bytes, .last_ip_id = get16(p + ETH_HLEN + 4)};
    return 1;
}

enum { GSO_SEPARATE, GSO_CONTINUE, GSO_FINAL };

static inline int gso_append(PacketGso *pending, unsigned char *first, unsigned capacity,
                             const unsigned char *next, unsigned n) {
    if (!gso_data_frame(next, n)) return GSO_SEPARATE;
    unsigned bytes = n - TCP4_HEADER;
    const unsigned char *ip = next + ETH_HLEN, *tcp = ip + 20;
    const unsigned char *first_ip = first + ETH_HLEN, *first_tcp = first_ip + 20;
    if (bytes > pending->segment_size || pending->length + bytes > capacity ||
        get32(tcp + 4) != pending->next_sequence || get16(ip + 4) != (uint16_t)(pending->last_ip_id + 1))
        return GSO_SEPARATE;

    /* Keep Ethernet, IPv4 addresses/TOS/TTL/flags and TCP ports/ACK/window.
     * Linux regenerates both lengths and checksums, advances seq and IP ID,
     * and preserves PSH on the last segment. */
    if (memcmp(first, next, ETH_HLEN + 2) || memcmp(first_ip + 6, ip + 6, 4) ||
        memcmp(first_ip + 12, ip + 12, 8) || memcmp(first_tcp, tcp, 4) ||
        memcmp(first_tcp + 8, tcp + 8, 5) || memcmp(first_tcp + 14, tcp + 14, 2))
        return GSO_SEPARATE;

    memcpy(first + pending->length, next + TCP4_HEADER, bytes);
    first[ETH_HLEN + 20 + 13] = tcp[13];
    pending->length += bytes;
    pending->next_sequence += bytes;
    ++pending->last_ip_id;
    ++pending->segments;
    return (tcp[13] & 8) || bytes < pending->segment_size ? GSO_FINAL : GSO_CONTINUE;
}

static inline void gso_put16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static inline unsigned gso_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return sum;
}

static inline void gso_header(const PacketGso *pending, struct virtio_net_hdr *header) {
    unsigned char *ip = (unsigned char *)(header + 1) + ETH_HLEN, *tcp = ip + 20;
    unsigned ip_len = pending->length - ETH_HLEN;
    gso_put16(ip + 2, ip_len);
    ip[10] = ip[11] = 0;
    gso_put16(ip + 10, ~gso_fold(add_words(0, ip, 20)));
    /* CHECKSUM_PARTIAL begins with the positive TCP pseudoheader sum. */
    gso_put16(tcp + 16, gso_fold(add_words(ip_len - 20 + IPPROTO_TCP, ip + 12, 8)));
    *header = (struct virtio_net_hdr){.flags = VIRTIO_NET_HDR_F_NEEDS_CSUM,
        .gso_type = VIRTIO_NET_HDR_GSO_TCPV4, .gso_size = pending->segment_size,
        .hdr_len = TCP4_HEADER, .csum_start = ETH_HLEN + 20, .csum_offset = 16};
}

#endif
