/* Isolated stack probe: all packets stay in memory, with a dummy IPv4 netif. */
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"
#include "lwip/timeouts.h"
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static struct netif iface;
static ip_addr_t local;
static uint32_t seqrng = 0x12345001, datarng = 0x9ce17add;
static unsigned tx_count, txflags, accepted, received, eof_seen, errs, polls;
static uint32_t txseq, txack;
static uint16_t last_port;
static int refuse_next;
static struct tcp_pcb *accepted_pcb;
static size_t live;
static uint32_t step(uint32_t *x) {
    uint32_t v = *x;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return *x = v;
}
uint32_t stack_random(void) {
    return step(&seqrng);
}
static void account(void *p) {
    if (p) {
        live += malloc_usable_size(p);
    }
}
void *stack_protocol_malloc(size_t n) {
    void *p = malloc(n);
    account(p);
    return p;
}
void *stack_protocol_calloc(size_t n, size_t k) {
    void *p = calloc(n, k);
    account(p);
    return p;
}
void stack_protocol_free(void *p) {
    if (p)
        live -= malloc_usable_size(p);
    free(p);
}
static uint64_t ns(clockid_t clock) {
    struct timespec t;
    CHECK(clock_gettime(clock, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
u32_t sys_now(void) {
    return (u32_t)(ns(CLOCK_MONOTONIC) / 1000000);
}
static err_t output(struct netif *n, struct pbuf *p, const ip4_addr_t *dst) {
    (void)n;
    (void)dst;
    struct ip_hdr h;
    struct tcp_hdr tcp;
    CHECK(pbuf_copy_partial(p, &h, sizeof(h), 0) == sizeof(h));
    if (IPH_PROTO(&h) != IP_PROTO_TCP)
        return ERR_OK;
    CHECK(pbuf_copy_partial(p, &tcp, sizeof(tcp), IPH_HL_BYTES(&h)) == sizeof(tcp));
    txseq = lwip_ntohl(tcp.seqno);
    txack = lwip_ntohl(tcp.ackno);
    txflags = TCPH_FLAGS(&tcp);
    tx_count++;
    return ERR_OK;
}
static err_t init_iface(struct netif *n) {
    n->mtu = 1500;
    n->name[0] = 't';
    n->name[1] = 's';
    n->output = output;
    return ERR_OK;
}
static void init(void) {
    ip4_addr_t mask, gw;
    IP4_ADDR(&local, 10, 0, 0, 1);
    IP4_ADDR(&mask, 255, 0, 0, 0);
    ip4_addr_set_zero(&gw);
    lwip_init();
    CHECK(netif_add(&iface, &local, &mask, &gw, NULL, init_iface, ip4_input) == &iface);
    netif_set_default(&iface);
    netif_set_up(&iface);
    netif_set_link_up(&iface);
}
static void packet(ip_addr_t src, unsigned sport, uint32_t seq, uint32_t ack, uint8_t flags,
                   int bytes) {
    size_t total = IP_HLEN + TCP_HLEN + bytes;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, total, PBUF_RAM);
    CHECK(p && p->len == total);
    unsigned char *b = p->payload;
    memset(b, 0, total);
    struct ip_hdr *ip = (void *)b;
    struct tcp_hdr *tcp = (void *)(b + IP_HLEN);
    IPH_VHL_SET(ip, 4, 5);
    IPH_LEN_SET(ip, lwip_htons(total));
    IPH_PROTO_SET(ip, IP_PROTO_TCP);
    IPH_TTL_SET(ip, 64);
    ip->src.addr = src.addr;
    ip->dest.addr = local.addr;
    IPH_CHKSUM_SET(ip, inet_chksum(ip, IP_HLEN));
    tcp->src = lwip_htons(sport);
    tcp->dest = lwip_htons(8080);
    tcp->seqno = lwip_htonl(seq);
    tcp->ackno = lwip_htonl(ack);
    tcp->wnd = lwip_htons(32000);
    TCPH_HDRLEN_FLAGS_SET(tcp, 5, flags);
    if (bytes)
        memset(b + IP_HLEN + TCP_HLEN, 0x5a, bytes);
    CHECK(!pbuf_remove_header(p, IP_HLEN));
    tcp->chksum = ip_chksum_pseudo(p, IP_PROTO_TCP, p->tot_len, &src, &local);
    CHECK(!pbuf_add_header(p, IP_HLEN));
    CHECK(ip4_input(p, &iface) == ERR_OK);
}
static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    CHECK(err == ERR_OK);
    if (!p) {
        eof_seen++;
        return ERR_OK;
    }
    if (refuse_next) {
        refuse_next = 0;
        return ERR_MEM;
    }
    received += p->tot_len;
    last_port = pcb->remote_port;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static void err_cb(void *arg, err_t err) {
    (void)arg;
    CHECK(err == ERR_ABRT || err == ERR_RST);
    errs++;
}
static err_t accept_cb(void *arg, struct tcp_pcb *p, err_t err) {
    (void)arg;
    CHECK(err == ERR_OK);
    accepted++;
    accepted_pcb = p;
    tcp_recv(p, recv_cb);
    tcp_err(p, err_cb);
    return ERR_OK;
}
static err_t poll_cb(void *arg, struct tcp_pcb *p) {
    (void)arg;
    (void)p;
    polls++;
    return ERR_OK;
}
static struct tcp_pcb *listener(void) {
    struct tcp_pcb *p = tcp_new();
    CHECK(p);
    CHECK(tcp_bind(p, &local, 8080) == ERR_OK);
    p = tcp_listen(p);
    CHECK(p);
    tcp_accept(p, accept_cb);
    return p;
}
static uint32_t handshake(ip_addr_t peer, uint16_t port, uint32_t seq) {
    unsigned sent = tx_count, accepted0 = accepted;
    packet(peer, port, seq, 0, TCP_SYN, 0);
    CHECK(tx_count == sent + 1 && txflags == (TCP_SYN | TCP_ACK) && txack == seq + 1);
    uint32_t own = txseq + 1;
    packet(peer, port, seq + 1, own, TCP_ACK, 0);
    CHECK(accepted == accepted0 + 1 && accepted_pcb && accepted_pcb->state == ESTABLISHED);
    CHECK(tcp_active_pcbs == accepted_pcb && accepted_pcb->remote_port == port);
    return own;
}
static struct tcp_pcb *fixture(ip_addr_t peer, uint16_t port, int state, int late) {
    struct tcp_pcb *p = tcp_new();
    CHECK(p);
    p->state = state;
    p->snd_nxt = p->snd_lbb = p->lastack = 525;
    p->snd_wl2 = 525;
    p->rcv_nxt = 500;
    p->rcv_ann_right_edge = p->rcv_nxt + p->rcv_wnd;
    if (late) {
        if (state == TIME_WAIT)
            TCP_REG(&tcp_tw_pcbs, p);
        else
            TCP_REG_ACTIVE(p);
    }
    p->local_ip = local;
    p->remote_ip = peer;
    p->local_port = 8080;
    p->remote_port = port;
    if (!late) {
        if (state == TIME_WAIT)
            TCP_REG(&tcp_tw_pcbs, p);
        else
            TCP_REG_ACTIVE(p);
    }
    tcp_recv(p, recv_cb);
    return p;
}
static void check_integrity(struct tcp_pcb *head) {
#if defined(LWIP_TCP_TUPLE_INDEX_BITS) && LWIP_TCP_TUPLE_INDEX_BITS
    struct tcp_pcb *prev = NULL;
    for (struct tcp_pcb *p = head; p; p = p->next) {
        CHECK(p->tuple_list_prev == prev);
        if (p->tuple_hash_prev)
            CHECK(*p->tuple_hash_prev == p);
        prev = p;
    }
#else
    (void)head;
#endif
}
static void correctness(void) {
    ip_addr_t peer;
    IP4_ADDR(&peer, 10, 10, 1, 20);
    struct tcp_pcb *l = listener();
    uint32_t own = handshake(peer, 32000, 9000);
    packet(peer, 32000, 9001, own, TCP_ACK, 3);
    CHECK(received == 3 && last_port == 32000);
    unsigned prevtx = tx_count;
    tcp_fasttmr();
    CHECK(tx_count > prevtx && (txflags & TCP_ACK) && txack == 9004);
    refuse_next = 1;
    packet(peer, 32000, 9004, own, TCP_ACK, 2);
    CHECK(accepted_pcb->refused_data != NULL && received == 3);
    tcp_fasttmr();
    CHECK(accepted_pcb->refused_data == NULL && received == 5);
    CHECK(tcp_write(accepted_pcb, "okay", 4, TCP_WRITE_FLAG_COPY) == ERR_OK);
    CHECK(tcp_output(accepted_pcb) == ERR_OK && accepted_pcb->unacked != NULL);
    uint32_t resend_seq = txseq;
    prevtx = tx_count;
    for (unsigned i = 0; i < 40 && tx_count == prevtx; i++)
        tcp_slowtmr();
    CHECK(tx_count > prevtx && txseq == resend_seq && accepted_pcb->nrtx > 0);
    own += 4;
    packet(peer, 32000, 9006, own, TCP_ACK, 0);
    CHECK(accepted_pcb->unacked == NULL);
    packet(peer, 32000, 9006, own, TCP_ACK | TCP_FIN, 0);
    CHECK(eof_seen == 1 && accepted_pcb->state == CLOSE_WAIT);
    CHECK(tcp_close(accepted_pcb) == ERR_OK);
    CHECK((txflags & TCP_FIN) != 0);
    packet(peer, 32000, 9007, own + 1, TCP_ACK, 0);
    CHECK(!tcp_active_pcbs && !tcp_tw_pcbs);

    own = handshake(peer, 32001, 7200);
    tcp_poll(accepted_pcb, poll_cb, 1);
    tcp_slowtmr();
    CHECK(polls == 1);
    CHECK(tcp_close(accepted_pcb) == ERR_OK);
    CHECK(accepted_pcb->state == FIN_WAIT_1);
    packet(peer, 32001, 7201, own + 1, TCP_ACK, 0);
    CHECK(accepted_pcb->state == FIN_WAIT_2);
    packet(peer, 32001, 7201, own + 1, TCP_ACK | TCP_FIN, 0);
    CHECK(!tcp_active_pcbs && tcp_tw_pcbs == accepted_pcb && tcp_tw_pcbs->state == TIME_WAIT);
    prevtx = tx_count;
    packet(peer, 32001, 7201, own + 1, TCP_ACK | TCP_FIN, 0);
    CHECK(tx_count == prevtx + 1 && txack == 7202);
    for (unsigned i = 0; i < 2 * TCP_MSL / TCP_SLOW_INTERVAL + 2; i++)
        tcp_slowtmr();
    CHECK(!tcp_tw_pcbs);
    own = handshake(peer, 32001, 8200);
    packet(peer, 32001, 8201, own, TCP_RST | TCP_ACK, 0);
    CHECK(!tcp_active_pcbs && errs == 1);

    packet(peer, 32002, 7400, 0, TCP_SYN, 0);
    CHECK(tcp_active_pcbs && tcp_active_pcbs->state == SYN_RCVD);
    for (unsigned i = 0; i < TCP_SYN_RCVD_TIMEOUT / TCP_SLOW_INTERVAL + 2; i++)
        tcp_slowtmr();
    CHECK(!tcp_active_pcbs);
    CHECK(tcp_close(l) == ERR_OK && !tcp_listen_pcbs.pcbs);

    /* A typed match can be shadowed by a newer PCB bound to another netif.
     * A later unindexed fixture is the registration pattern in upstream tests. */
    struct tcp_pcb *a = fixture(peer, 34000, ESTABLISHED, 0);
    struct tcp_pcb *bad = fixture(peer, 34000, ESTABLISHED, 0);
    bad->netif_idx = 200;
    struct tcp_pcb *other = fixture(peer, 34001, ESTABLISHED, 1);
    CHECK(tcp_active_pcbs == other);
    packet(peer, 34000, 500, 525, TCP_ACK, 1);
    CHECK(tcp_active_pcbs == a && received == 6);
    bad->netif_idx = NETIF_NO_INDEX;
    packet(peer, 34000, 501, 525, TCP_ACK, 1);
    CHECK(tcp_active_pcbs == a && received == 7);
    bad->netif_idx = 200;
    check_integrity(tcp_active_pcbs);
    packet(peer, 34001, 500, 525, TCP_ACK, 1);
    CHECK(tcp_active_pcbs == other && received == 8);
    tcp_abort(other);
    CHECK(tcp_active_pcbs == a);
    tcp_abort(a);
    packet(peer, 34000, 500, 525, TCP_ACK, 1);
    CHECK(received == 8 && tcp_active_pcbs == bad);
    tcp_abort(bad);
    CHECK(!tcp_active_pcbs);

    /* A slow timeout removes the middle node without consuming either neighbor. */
    struct tcp_pcb *tail = fixture(peer, 35000, ESTABLISHED, 0);
    struct tcp_pcb *expired = fixture(peer, 35001, SYN_RCVD, 0);
    struct tcp_pcb *head = fixture(peer, 35002, ESTABLISHED, 0);
    expired->tmr = tcp_ticks - TCP_SYN_RCVD_TIMEOUT / TCP_SLOW_INTERVAL - 1;
    tcp_slowtmr();
    CHECK(tcp_active_pcbs == head && head->next == tail && !tail->next);
    check_integrity(tcp_active_pcbs);
    tcp_abort(tail);
    tcp_abort(head);
    tail = fixture(peer, 35000, TIME_WAIT, 0);
    expired = fixture(peer, 35001, TIME_WAIT, 0);
    head = fixture(peer, 35002, TIME_WAIT, 0);
    expired->tmr = tcp_ticks - 2 * TCP_MSL / TCP_SLOW_INTERVAL - 1;
    tcp_slowtmr();
    CHECK(tcp_tw_pcbs == head && head->next == tail && !tail->next);
    check_integrity(tcp_tw_pcbs);
    tcp_abort(tail);
    tcp_abort(head);
    CHECK(!tcp_active_pcbs && !tcp_tw_pcbs);
    printf("qa=ok accepted=%u recv_bytes=%u peer_fin=%u reset=%u poll=%u\n", accepted, received,
           eof_seen, errs, polls);
}
static void idtuple(uint32_t n, ip_addr_t *peer, uint16_t *port) {
    /* Unique source address for every group of 30,000 local test ports. */
    ip4_addr_set_u32(peer, lwip_htonl(UINT32_C(0x0a01012a) + ((n / 30000u) << 8)));
    *port = 1024 + n % 30000;
}
static void bench(unsigned n, unsigned packets, int permute) {
    struct tcp_pcb **pcbs = calloc(n, sizeof(*pcbs));
    unsigned *order = calloc(n, sizeof(*order));
    CHECK(pcbs && order);
    for (unsigned i = 0; i < n; i++) {
        struct tcp_pcb *p = pcbs[i] = tcp_new();
        CHECK(p);
        order[i] = i;
        tcp_poll(p, NULL, 255);
        p->state = ESTABLISHED;
        p->local_ip = local;
        idtuple(i, &p->remote_ip, &p->remote_port);
        p->local_port = 8080;
        p->snd_nxt = p->snd_lbb = p->lastack = p->snd_wl2 = 525;
        p->rcv_nxt = 500;
        p->rcv_ann_right_edge = p->rcv_nxt + p->rcv_wnd;
    }
    if (permute)
        for (unsigned i = n - 1; i > 0; i--) {
            unsigned j = step(&datarng) % (i + 1);
            unsigned tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
    for (unsigned i = 0; i < n; i++) {
        struct tcp_pcb *p = pcbs[order[i]];
        TCP_REG_ACTIVE(p);
    }
    check_integrity(tcp_active_pcbs);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    size_t resident = live;
    uint64_t t = ns(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < packets; i++) {
        unsigned k = step(&datarng) % n;
        packet(pcbs[k]->remote_ip, pcbs[k]->remote_port, 500, 525, TCP_ACK, 0);
        CHECK(tcp_active_pcbs == pcbs[k]);
    }
    double hit_ns = (ns(CLOCK_THREAD_CPUTIME_ID) - t) / (double)packets;
    t = ns(CLOCK_THREAD_CPUTIME_ID);
    unsigned misses = packets < 64 ? packets : 64;
    for (unsigned i = 0; i < misses; i++) {
        ip_addr_t remote;
        IP4_ADDR(&remote, 10, 200, 1, 1);
        packet(remote, 3000 + i, 500, 525, TCP_ACK, 0);
    }
    double miss_ns = (ns(CLOCK_THREAD_CPUTIME_ID) - t) / (double)misses;
    t = ns(CLOCK_THREAD_CPUTIME_ID);
    tcp_fasttmr();
    double fast_ms = (ns(CLOCK_THREAD_CPUTIME_ID) - t) * 1e-6;
    t = ns(CLOCK_THREAD_CPUTIME_ID);
    tcp_slowtmr();
    double slow_ms = (ns(CLOCK_THREAD_CPUTIME_ID) - t) * 1e-6;
    check_integrity(tcp_active_pcbs);
    printf("pcbs=%u packets=%u randomized_list=%d hit_ns=%.1f miss_ns=%.1f fast_ms=%.3f "
           "slow_ms=%.3f timers_cpu_pct=%.2f pcb_bytes=%zu alloc_bytes=%zu maxrss_kib=%ld\n",
           n, packets, permute, hit_ns, miss_ns, fast_ms, slow_ms, (4 * fast_ms + 2 * slow_ms) * .1,
           sizeof(struct tcp_pcb), resident, ru.ru_maxrss);
    /* Head-to-tail destruction leaves the upstream baseline linear too. */
    while (tcp_active_pcbs)
        tcp_abandon(tcp_active_pcbs, 0);
    free(pcbs);
    free(order);
}
static unsigned positive_count(const char *text) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    CHECK(text[0] >= '0' && text[0] <= '9' && !*end && !errno && n && n <= UINT_MAX);
    return (unsigned)n;
}

int main(int argc, char **argv) {
    init();
    if (argc == 1) {
        correctness();
        return 0;
    }
    CHECK(argc == 4);
    CHECK(!strcmp(argv[3], "0") || !strcmp(argv[3], "1"));
    bench(positive_count(argv[1]), positive_count(argv[2]), argv[3][0] == '1');
    return 0;
}
