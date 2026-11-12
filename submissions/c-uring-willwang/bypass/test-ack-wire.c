/* Real TCP state, production dispatcher, one synthetic IPv4 device and a controlled clock. */
#include "stack.h"
#include "challenge.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "short ACK wire test failed at line %d: %s\n", __LINE__, #test); \
    exit(1); \
} } while (0)

static ip4_addr_t local, remote;
static u32_t seq = 1000, ack;
static unsigned now, refusals, wire_acks, payloads, fins, refused;
static u32_t acked;

int __wrap_clock_gettime(clockid_t clock, struct timespec *ts) {
    CHECK(clock == CLOCK_MONOTONIC);
    *ts = (struct timespec){.tv_sec = now / 1000, .tv_nsec = (now % 1000) * 1000000L};
    return 0;
}

static void send_segment(uint32_t input_seq, unsigned flags, const char *bytes) {
    unsigned count = bytes ? strlen(bytes) : 0;
    const u16_t length = IP_HLEN + TCP_HLEN + count;
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, length, PBUF_RAM);
    CHECK(packet && packet->len == length);
    memset(packet->payload, 0, length);
    struct ip_hdr *ip = packet->payload;
    struct tcp_hdr *tcp = (struct tcp_hdr *)((unsigned char *)ip + IP_HLEN);
    IPH_VHL_SET(ip, 4, 5);
    IPH_LEN_SET(ip, lwip_htons(length));
    IPH_PROTO_SET(ip, IP_PROTO_TCP);
    IPH_TTL_SET(ip, 64);
    ip->src.addr = remote.addr;
    ip->dest.addr = local.addr;
    IPH_CHKSUM_SET(ip, inet_chksum(ip, IP_HLEN));
    tcp->src = lwip_htons(41234);
    tcp->dest = lwip_htons(8080);
    tcp->seqno = lwip_htonl(input_seq);
    tcp->ackno = flags & TCP_ACK ? lwip_htonl(ack) : 0;
    tcp->wnd = lwip_htons(30000);
    TCPH_HDRLEN_FLAGS_SET(tcp, 5, flags);
    if (count) memcpy((char *)tcp + TCP_HLEN, bytes, count);
    CHECK(!pbuf_remove_header(packet, IP_HLEN));
    tcp->chksum = ip_chksum_pseudo(packet, IP_PROTO_TCP, TCP_HLEN + count, &remote, &local);
    CHECK(!pbuf_add_header(packet, IP_HLEN));
    CHECK(ip4_input(packet, netif_default) == ERR_OK);
}

static void send_in_order(unsigned flags, const char *bytes) {
    u32_t first = seq;
    seq += (bytes ? strlen(bytes) : 0) + ((flags & (TCP_SYN | TCP_FIN)) != 0);
    send_segment(first, flags, bytes);
}

static err_t capture(struct netif *netif, struct pbuf *packet, const ip4_addr_t *dst) {
    (void)netif;
    CHECK(ip4_addr_cmp(dst, &remote));
    struct ip_hdr ip;
    struct tcp_hdr tcp;
    CHECK(pbuf_copy_partial(packet, &ip, sizeof(ip), 0) == sizeof(ip));
    CHECK(IPH_PROTO(&ip) == IP_PROTO_TCP);
    CHECK(pbuf_copy_partial(packet, &tcp, sizeof(tcp), IPH_HL_BYTES(&ip)) == sizeof(tcp));
    unsigned flags = TCPH_FLAGS(&tcp);
    unsigned length = packet->tot_len - IPH_HL_BYTES(&ip) - TCPH_HDRLEN_BYTES(&tcp);
    CHECK(lwip_ntohs(tcp.dest) == 41234);
    if (refusals) {
        --refusals;
        ++refused;
        return ERR_MEM;
    }
    if (flags & TCP_SYN) {
        ack = lwip_ntohl(tcp.seqno) + 1;
        return ERR_OK;
    }
    if (flags & TCP_RST) return ERR_OK;
    CHECK(flags & TCP_ACK);
    acked = lwip_ntohl(tcp.ackno);
    if (length) {
        char body[5];
        CHECK(length == sizeof(body));
        CHECK(pbuf_copy_partial(packet, body, length, packet->tot_len - length) == length);
        CHECK(!memcmp(body, "ready", length));
        ++payloads;
        ack += length;
    } else {
        ++wire_acks;
    }
    if (flags & TCP_FIN) {
        ++fins;
        ++ack;
    }
    return ERR_OK;
}

static int ignore_startup_arp(void *opaque, const unsigned char *frame, size_t length) {
    (void)opaque;
    CHECK(length >= 14 && frame[12] == 8 && frame[13] == 6);
    return 0;
}

/* A final exclamation mark produces a five-byte application response. */
void db_begin_pass(void) {}
int db_writes_deferred(void) { return 0; }
int db_commit_pass(void) { return 0; }
int app_request(const char *data, size_t n, size_t *used, Buf *out, int *close) {
    (void)close;
    if (!n || data[n - 1] != '!') return 0;
    out->data = malloc(5);
    CHECK(out->data);
    memcpy(out->data, "ready", 5);
    out->cap = out->len = 5;
    *used = n;
    return 1;
}

void app_error_500(Buf *out) { (void)out; abort(); }

int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && (!strcmp(argv[1], "retained") || !strcmp(argv[1], "lost"))));
    IP4_ADDR(&local, 10, 0, 0, 1);
    IP4_ADDR(&remote, 10, 0, 0, 2);
    ip4_addr_t mask;
    IP4_ADDR(&mask, 255, 255, 255, 0);
    struct stack_config config = {
        .address_be = local.addr, .netmask_be = mask.addr, .mac = {2, 0, 0, 0, 0, 1},
        .port = 8080, .mtu = 1500, .max_connections = 1
    };
    CHECK(stack_init(&config, ignore_startup_arp, NULL) == 0);
    netif_default->output = capture;


    send_in_order(TCP_SYN, NULL);
    send_in_order(TCP_ACK, NULL);
    send_in_order(TCP_ACK, "a");
    CHECK(!stack_dispatch() && stack_timeout_ms() == 40);
    now = 39;
    CHECK(!stack_dispatch() && !wire_acks && stack_timeout_ms() == 1);
    now = 40;
    CHECK(!stack_dispatch() && wire_acks == 1 && acked == seq);
    now = 41;
    send_in_order(TCP_ACK, "b");
    CHECK(!stack_dispatch());
    send_in_order(TCP_ACK, "c");
    CHECK(wire_acks == 2 && acked == seq);
    CHECK(!stack_dispatch());
    send_in_order(TCP_ACK, "!");
    CHECK(stack_dispatch() == 1 && wire_acks == 2 && payloads == 1 && acked == seq);
    send_in_order(TCP_ACK, NULL);

    now = 50;
    send_segment(seq + 1, TCP_ACK, "z");
    CHECK(wire_acks == 3 && acked == seq);
    send_in_order(TCP_ACK, "x");
    ++seq; /* Filling the gap allowed the receiver to consume z too. */
    CHECK(!stack_dispatch());
    now = 92;
    CHECK(!stack_dispatch() && acked == seq);
    send_segment(seq - 1, TCP_ACK, "z");
    CHECK(acked == seq);

    unsigned previous = wire_acks;
    send_in_order(TCP_ACK, "x");
    CHECK(!stack_dispatch());
    now = 132;
    refusals = 1;
    CHECK(!stack_dispatch() && refused == 1 && wire_acks == previous && stack_timeout_ms() > 0);
    now = 133;
    CHECK(!stack_dispatch() && wire_acks == previous);
    now = 172;
    CHECK(!stack_dispatch() && wire_acks == previous + 1 && acked == seq);

    if (argc == 2) {
        /* The 250 ms timer precedes this stream's 272 ms deadline. Refuse
         * its first attempt. No peer transmission wakes the stream again. */
        now = 232;
        send_in_order(TCP_ACK, "x");
        CHECK(!stack_dispatch());
        previous = wire_acks;
        now = 250;
        refusals = 1;
        CHECK(!stack_dispatch() && refused == 2 && wire_acks == previous);
        CHECK(stack_timeout_ms() == 22);
        now = 271;
        CHECK(!stack_dispatch() && wire_acks == previous);
        now = 272;
        CHECK(!stack_dispatch());
        if (!strcmp(argv[1], "retained")) {
            CHECK(wire_acks == previous + 1 && acked == seq);
        } else {
            CHECK(wire_acks == previous && acked == seq - 1);
        }
        printf("native 250 ms ACK refused, 272 ms ACKs emitted=%u, expected=%s\n",
               wire_acks - previous, argv[1]);
    }
    send_in_order(TCP_ACK, "x");
    CHECK(!stack_dispatch());
    send_in_order(TCP_ACK | TCP_FIN, NULL);
    CHECK(!stack_dispatch() && fins == 1 && acked == seq);
    send_in_order(TCP_ACK, NULL);
    now += 48;
    CHECK(!stack_dispatch());
    struct stack_stats stats;
    stack_get_stats(&stats);
    CHECK(!stats.live);
    stack_shutdown();
    puts("real lwIP: 40ms first ACK, second segment immediate, response piggyback,"
         " out-of-order and duplicate, FIN, failed TX and retry");
}
