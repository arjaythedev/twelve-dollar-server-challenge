/* Drive the production listener and upstream TCP timers without wall-clock
 * waits. Only the IPv4 netif output and unused HTTP/SQLite calls are replaced. */
#include "stack.h"
#include "../challenge.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "keepalive test failed at line %d: %s\n", __LINE__, #test); \
    exit(1); \
} } while (0)

static ip4_addr_t local, remote;
static unsigned elapsed_ms;
static unsigned fin_available_ms, fin_attempts;
enum { LIVE, CLOSED_PEER, VANISHED, PEER_COUNT, FIRST_PORT = 41000 };
static struct peer {
    uint32_t next_seq, server_seq;
    unsigned first_probe_ms, last_probe_ms, probes, resets, fins;
    int syn_ack, pending_probe, closing;
} peers[PEER_COUNT];

static void send_segment(unsigned which, unsigned flags) {
    struct peer *peer = &peers[which];
    const u16_t length = IP_HLEN + TCP_HLEN;
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
    tcp->src = lwip_htons(FIRST_PORT + which);
    tcp->dest = lwip_htons(8080);
    tcp->seqno = lwip_htonl(peer->next_seq);
    tcp->ackno = flags & TCP_ACK ? lwip_htonl(peer->server_seq) : 0;
    tcp->wnd = lwip_htons(30000);
    TCPH_HDRLEN_FLAGS_SET(tcp, 5, flags);
    if (flags & (TCP_SYN | TCP_FIN)) ++peer->next_seq;
    CHECK(!pbuf_remove_header(packet, IP_HLEN));
    tcp->chksum = ip_chksum_pseudo(packet, IP_PROTO_TCP, TCP_HLEN, &remote, &local);
    CHECK(!pbuf_add_header(packet, IP_HLEN));
    CHECK(ip4_input(packet, netif_default) == ERR_OK);
}

static err_t capture(struct netif *netif, struct pbuf *packet, const ip4_addr_t *dst) {
    (void)netif;
    CHECK(ip4_addr_cmp(dst, &remote));
    struct ip_hdr ip;
    struct tcp_hdr tcp;
    CHECK(pbuf_copy_partial(packet, &ip, sizeof(ip), 0) == sizeof(ip));
    CHECK(IPH_PROTO(&ip) == IP_PROTO_TCP);
    CHECK(pbuf_copy_partial(packet, &tcp, sizeof(tcp), IPH_HL_BYTES(&ip)) == sizeof(tcp));
    unsigned port = lwip_ntohs(tcp.dest);
    CHECK(port >= FIRST_PORT && port < FIRST_PORT + PEER_COUNT);
    struct peer *peer = &peers[port - FIRST_PORT];
    unsigned flags = TCPH_FLAGS(&tcp);
    uint32_t seq = lwip_ntohl(tcp.seqno);
    if (flags == (TCP_SYN | TCP_ACK)) {
        CHECK(!peer->syn_ack && lwip_ntohl(tcp.ackno) == peer->next_seq);
        peer->syn_ack = 1;
        peer->server_seq = seq + 1;
    } else if (flags & TCP_RST) {
        CHECK(port == FIRST_PORT + VANISHED && peer->resets == 0);
        ++peer->resets;
    } else if (peer->closing) {
        CHECK((flags & TCP_ACK) && seq == peer->server_seq);
        if (flags & TCP_FIN) {
            ++fin_attempts;
            if (elapsed_ms < fin_available_ms) return ERR_MEM;
            ++peer->server_seq;
            ++peer->fins;
        }
    } else {
        CHECK(flags == TCP_ACK && lwip_ntohl(tcp.ackno) == peer->next_seq);
        CHECK(seq == peer->server_seq - 1 && packet->tot_len == IPH_HL_BYTES(&ip) + TCPH_HDRLEN_BYTES(&tcp));
        if (!peer->probes) peer->first_probe_ms = elapsed_ms;
        if (port == FIRST_PORT + VANISHED && peer->probes)
            CHECK(elapsed_ms - peer->last_probe_ms == 10000);
        if (port == FIRST_PORT + LIVE && peer->probes >= 2)
            CHECK(elapsed_ms - peer->last_probe_ms == 75500);
        peer->last_probe_ms = elapsed_ms;
        ++peer->probes;
        peer->pending_probe = 1;
    }
    return ERR_OK;
}

static int ignore_startup_arp(void *opaque, const unsigned char *frame, size_t length) {
    (void)opaque;
    CHECK(length >= 14 && frame[12] == 8 && frame[13] == 6);
    return 0;
}

/* This test sends only TCP control packets, so no HTTP or database operation
 * should occur. A FIN can wake the normal dispatcher for an empty pass. */
void db_begin_pass(void) {}
int db_writes_deferred(void) { return 0; }
int db_commit_pass(void) { return 0; }
int app_request(const char *input, size_t n, size_t *used, Buf *out, int *close_after) {
    (void)input; (void)n; (void)used; (void)out; (void)close_after;
    abort();
}
void app_error_500(Buf *out) { (void)out; abort(); }

int main(void) {
    IP4_ADDR(&local, 10, 0, 0, 1);
    IP4_ADDR(&remote, 10, 0, 0, 2);
    ip4_addr_t mask;
    IP4_ADDR(&mask, 255, 255, 255, 0);
    struct stack_config config = {
        .address_be = local.addr, .netmask_be = mask.addr, .mac = {2, 0, 0, 0, 0, 1},
        .port = 8080, .mtu = 1500, .max_connections = PEER_COUNT
    };
    CHECK(stack_init(&config, ignore_startup_arp, NULL) == 0);
    netif_default->output = capture;
    for (unsigned i = 0; i < PEER_COUNT; ++i) {
        peers[i].next_seq = 1000 + 1000 * i;
        send_segment(i, TCP_SYN);
        CHECK(peers[i].syn_ack);
        send_segment(i, TCP_ACK);
    }
    struct stack_stats stats;
    stack_get_stats(&stats);
    CHECK(stats.accepted == PEER_COUNT && stats.live == PEER_COUNT);
    for (elapsed_ms = TCP_SLOW_INTERVAL; elapsed_ms <= 600000; elapsed_ms += TCP_SLOW_INTERVAL) {
        tcp_slowtmr();
        if (peers[LIVE].pending_probe && peers[LIVE].probes > 1)
            send_segment(LIVE, TCP_ACK); /* drop one probe, then behave like a quiet host */
        if (peers[CLOSED_PEER].pending_probe)
            send_segment(CLOSED_PEER, TCP_RST); /* lost final FIN; its port is now closed */
        for (unsigned i = 0; i < PEER_COUNT; ++i) peers[i].pending_probe = 0;
        stack_get_stats(&stats);
        if (elapsed_ms <= 75000) {
            CHECK(stats.live == 3 && !peers[LIVE].probes && !peers[VANISHED].probes);
        } else if (elapsed_ms <= 135000) {
            CHECK(stats.live == 2 && !peers[VANISHED].resets);
        } else {
            CHECK(stats.live == 1 && peers[VANISHED].resets == 1);
        }
    }
    CHECK(peers[LIVE].first_probe_ms == 75500 && peers[LIVE].probes >= 8);
    CHECK(peers[CLOSED_PEER].first_probe_ms == 75500 && peers[CLOSED_PEER].probes == 1);
    CHECK(peers[VANISHED].first_probe_ms == 75500 && peers[VANISHED].probes == 6);
    peers[LIVE].closing = 1;
    unsigned fin_start = elapsed_ms;
    fin_available_ms = fin_start + 9300;
    send_segment(LIVE, TCP_FIN | TCP_ACK);
    CHECK(stack_dispatch() == 0 && !peers[LIVE].fins);
    stack_get_stats(&stats);
    CHECK(stats.live == 0 && tcp_active_pcbs && tcp_active_pcbs->state == LAST_ACK);
    CHECK(tcp_active_pcbs->unsent && !tcp_active_pcbs->unacked);
    while (!peers[LIVE].fins && elapsed_ms <= fin_available_ms + 1000) {
        elapsed_ms += TCP_SLOW_INTERVAL;
        tcp_slowtmr();
    }
    CHECK(peers[LIVE].fins == 1 && fin_attempts > 1 && elapsed_ms <= fin_available_ms + 1000);
    CHECK(!tcp_active_pcbs->unsent && tcp_active_pcbs->unacked);
    /* Withhold its final ACK for a second. The segment must remain owned
     * by TCP even though the HTTP connection was already released. */
    tcp_slowtmr();
    tcp_slowtmr();
    CHECK(tcp_active_pcbs && tcp_active_pcbs->unacked);
    stack_get_stats(&stats);
    uint64_t before_ack = stats.protocol_bytes;
    send_segment(LIVE, TCP_ACK);
    stack_get_stats(&stats);
    CHECK(stats.live == 0 && stats.requests == 0 && stats.buffered_bytes == 0);
    CHECK(tcp_active_pcbs == NULL && tcp_tw_pcbs == NULL);
    CHECK(before_ack - stats.protocol_bytes >= sizeof(struct tcp_pcb) + sizeof(struct tcp_seg));
    stack_shutdown();
    puts("Three real TCP handshakes, ten virtual minutes: live ACK survived a lost probe; "
         "closed port reset at 75.5s; silent peer expired after six probes at 135.5s.");
    printf("After 9.3s of FIN backpressure, FIN was sent at %.1fs; its delayed ACK freed "
           "the final PCB and all TCP segments.\n", (double)(elapsed_ms - fin_start) / 1000);
    return 0;
}
