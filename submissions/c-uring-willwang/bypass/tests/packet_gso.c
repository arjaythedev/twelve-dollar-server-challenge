/* cc -std=c11 -O2 -Wall -Wextra -Werror bypass/tests/packet_gso.c -o /tmp/test-packet-gso */
#include "../packet_gso.h"
#include <assert.h>
#include <stdio.h>

enum { MSS = 80, TRAILING = 39, CAPACITY = 1024 };

static void write16(unsigned char *p, uint16_t n) {
    p[0] = (unsigned char)(n >> 8); p[1] = (unsigned char)n;
}

static void write32(unsigned char *p, uint32_t n) {
    write16(p, (uint16_t)(n >> 16)); write16(p + 2, (uint16_t)n);
}

static unsigned checksum_sum(const unsigned char *p, unsigned n, unsigned sum) {
    for (unsigned i = 0; i < n; i += 2) {
        sum += (unsigned)p[i] << 8;
        if (i + 1 < n) sum += p[i + 1];
    }
    while (sum > 65535) sum = (sum & 65535) + (sum >> 16);
    return sum;
}

static void checksums(unsigned char *frame, unsigned length) {
    unsigned char *ip = frame + ETH_HLEN, *tcp = ip + 20;
    unsigned tcp_length = length - ETH_HLEN - 20;
    ip[10] = ip[11] = tcp[16] = tcp[17] = 0;
    write16(ip + 10, (uint16_t)~checksum_sum(ip, 20, 0));
    unsigned pseudo = checksum_sum(ip + 12, 8, tcp_length + IPPROTO_TCP);
    write16(tcp + 16, (uint16_t)~checksum_sum(tcp, tcp_length, pseudo));
}

static unsigned frame(unsigned char *p, unsigned data, unsigned offset, uint16_t id, int push) {
    memset(p, 0, TCP4_HEADER + data);
    memcpy(p, (unsigned char[]){2, 2, 3, 4, 5, 6, 2, 7, 8, 9, 10, 11, 8, 0}, ETH_HLEN);
    unsigned char *ip = p + ETH_HLEN, *tcp = ip + 20;
    ip[0] = 0x45; ip[8] = 64; ip[9] = IPPROTO_TCP;
    memcpy(ip + 12, (unsigned char[]){10, 1, 1, 7, 10, 1, 1, 8}, 8);
    write16(ip + 2, (uint16_t)(40 + data)); write16(ip + 4, id);
    write16(tcp, 9010); write16(tcp + 2, 48000);
    write32(tcp + 4, UINT32_MAX - 20 + offset);
    write32(tcp + 8, 0x675a3143); write16(tcp + 14, 10994);
    tcp[12] = 0x50; tcp[13] = push ? 0x18 : 0x10;
    for (unsigned i = 0; i < data; ++i) p[TCP4_HEADER + i] = (unsigned char)((offset + i) * 31 + 1);
    checksums(p, TCP4_HEADER + data);
    return TCP4_HEADER + data;
}

static void reject_changes(const unsigned char *first, unsigned first_size, const unsigned char *second,
                           unsigned second_size) {
    /* Relative offsets in an Ethernet/IPv4/TCP data frame. */
    const unsigned fields[] = {0, 15, 18, 20, 22, 26, 30, 34, 36, 39, 43, 46, 48, 52};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        unsigned char changed[CAPACITY], output[CAPACITY];
        memcpy(output, first, first_size); memcpy(changed, second, second_size);
        changed[fields[i]] ^= fields[i] == 20 ? 0x40 : 1;
        PacketGso pending;
        assert(gso_begin(&pending, first, first_size));
        assert(gso_append(&pending, output, CAPACITY, changed, second_size) == GSO_SEPARATE);
    }
    for (unsigned flag = 1; flag < 256; flag <<= 1) {
        if (flag == 8 || flag == 16) continue;
        unsigned char control[CAPACITY]; memcpy(control, first, first_size); control[47] |= flag;
        PacketGso pending;
        assert(!gso_begin(&pending, control, first_size));
    }
    unsigned char atomic[CAPACITY]; memcpy(atomic, first, first_size);
    atomic[20] = 0x40;
    PacketGso pending;
    assert(!gso_begin(&pending, atomic, first_size));
}

int main(void) {
    unsigned char originals[3][CAPACITY];
    unsigned lengths[] = {frame(originals[0], MSS, 0, 65534, 0),
                          frame(originals[1], MSS, MSS, 65535, 0),
                          frame(originals[2], TRAILING, MSS * 2, 0, 1)};
    struct { struct virtio_net_hdr header; unsigned char payload[CAPACITY]; } tx = {0};
    PacketGso pending;
    memcpy(tx.payload, originals[0], lengths[0]);
    assert(gso_begin(&pending, originals[0], lengths[0]));
    assert(gso_append(&pending, tx.payload, lengths[0], originals[1], lengths[1]) == GSO_SEPARATE);
    assert(gso_append(&pending, tx.payload, CAPACITY, originals[1], lengths[1]) == GSO_CONTINUE);
    assert(gso_append(&pending, tx.payload, CAPACITY, originals[2], lengths[2]) == GSO_FINAL);
    gso_header(&pending, &tx.header);
    assert(tx.header.flags == VIRTIO_NET_HDR_F_NEEDS_CSUM && tx.header.gso_type == VIRTIO_NET_HDR_GSO_TCPV4);
    assert(tx.header.gso_size == MSS && tx.header.hdr_len == TCP4_HEADER);
    assert(tx.header.csum_start == ETH_HLEN + 20 && tx.header.csum_offset == 16);
    assert(checksum_sum(tx.payload + ETH_HLEN, 20, 0) == 65535);
    unsigned tcp_len = pending.length - ETH_HLEN - 20;
    assert(get16(tx.payload + ETH_HLEN + 20 + 16) == checksum_sum(tx.payload + ETH_HLEN + 12, 8, tcp_len + IPPROTO_TCP));

    for (unsigned i = 0, offset = 0; i < 3; ++i) {
        unsigned char actual[CAPACITY];
        unsigned n = lengths[i], bytes = n - TCP4_HEADER;
        memcpy(actual, tx.payload, TCP4_HEADER);
        memcpy(actual + TCP4_HEADER, tx.payload + TCP4_HEADER + offset, bytes);
        unsigned char *ip = actual + ETH_HLEN, *tcp = ip + 20;
        write16(ip + 2, (uint16_t)(n - ETH_HLEN));
        write16(ip + 4, (uint16_t)(get16(ip + 4) + i));
        write32(tcp + 4, get32(tcp + 4) + offset);
        if (i < 2) tcp[13] &= (unsigned char)~8u;
        checksums(actual, n);
        assert(!memcmp(actual, originals[i], n));
        offset += bytes;
    }
    reject_changes(originals[0], lengths[0], originals[1], lengths[1]);
    puts("GSO reconstructed every original frame and preserved non-mergeable boundaries");
    return 0;
}
