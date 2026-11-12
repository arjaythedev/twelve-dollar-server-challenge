/* Exercise real checksummed Ethernet traffic without reading the target PCB
 * to construct an incoming packet. Distinct peers issue a burst of requests,
 * receive short/long writes and send cumulative ACKs on later input turns. */
#define main tuple_fixture_main
#include "test-pcb-index.c"
#undef main
#include "netif/ethernet.h"

static uint64_t acknowledged_bytes;

static err_t acknowledged(void *arg, struct tcp_pcb *pcb, u16_t bytes) {
    (void)arg;
    (void)pcb;
    acknowledged_bytes += bytes;
    return ERR_OK;
}

static void frame(ip_addr_t peer, u16_t port, u32_t seq, u32_t ack, unsigned flags,
                  const unsigned char *data, unsigned length) {
    _Alignas(max_align_t) unsigned char bytes[1500] = {0};
    size_t total = 14 + IP_HLEN + TCP_HLEN + length;
    CHECK(total <= sizeof(bytes));
    bytes[0] = bytes[6] = 2;
    bytes[5] = 1;
    bytes[11] = 2;
    bytes[12] = 8;
    struct ip_hdr *ip = (void *)(bytes + 14);
    struct tcp_hdr *tcp = (void *)(bytes + 14 + IP_HLEN);
    IPH_VHL_SET(ip, 4, 5);
    IPH_LEN_SET(ip, lwip_htons((u16_t)(total - 14)));
    IPH_TTL_SET(ip, 64);
    IPH_PROTO_SET(ip, IP_PROTO_TCP);
    ip->src.addr = peer.addr;
    ip->dest.addr = local.addr;
    IPH_CHKSUM_SET(ip, inet_chksum(ip, IP_HLEN));
    tcp->src = lwip_htons(port);
    tcp->dest = lwip_htons(8080);
    tcp->seqno = lwip_htonl(seq);
    tcp->ackno = lwip_htonl(ack);
    tcp->wnd = lwip_htons(32000);
    TCPH_HDRLEN_FLAGS_SET(tcp, 5, flags);
    if (length) memcpy(bytes + 14 + IP_HLEN + TCP_HLEN, data, length);
    u16_t tcp_len = (u16_t)(TCP_HLEN + length);
    struct pbuf body = {.payload = tcp, .len = tcp_len, .tot_len = tcp_len};
    tcp->chksum = ip_chksum_pseudo(&body, IP_PROTO_TCP, body.len, &peer, &local);
    struct pbuf *packet_buf = pbuf_alloc(PBUF_RAW, (u16_t)total, PBUF_RAM);
    CHECK(packet_buf && pbuf_take(packet_buf, bytes, (u16_t)total) == ERR_OK);
    CHECK(ethernet_input(packet_buf, &iface) == ERR_OK);
}

struct client {
    uint32_t seq, ack, generation, batch;
};

struct turn {
    unsigned chosen, reply;
    uint32_t ack;
    ip_addr_t address;
    uint16_t port;
};

static void client_tuple(unsigned chosen, unsigned generation, ip_addr_t *address, uint16_t *port) {
    idtuple(chosen, address, port);
    address->addr ^= lwip_htonl((generation & 7u) << 20);
}

static void open_flow(unsigned chosen, struct client *client, struct tcp_pcb **flow) {
    ip_addr_t address;
    uint16_t port;
    client_tuple(chosen, client->generation, &address, &port);
    *flow = fixture(address, port, ESTABLISHED, 0);
    struct tcp_pcb *p = *flow;
    p->snd_wnd = p->snd_wnd_max = 32000;
    p->snd_wl1 = p->rcv_nxt;
    p->mss = 1460;
    p->cwnd = 4 * p->mss;
    p->ssthresh = 10 * p->mss;
    tcp_sent(p, acknowledged);
    client->seq = 500;
    client->ack = 525;
}

static struct tcp_pcb *find_peer(int tw, ip_addr_t peer, uint16_t port, u8_t iface_idx, int indexed) {
    struct tcp_pcb *p, *prior = NULL;
    int full = 1;
    if (indexed) {
        p = tcp_index_input_head(tw, &local, &peer, 8080, port, &full);
    } else {
        p = tw ? tcp_tw_pcbs : tcp_active_pcbs;
    }
    for (; p; prior = p, p = full ? p->next : p->tuple_next) {
        if ((p->netif_idx == NETIF_NO_INDEX || p->netif_idx == iface_idx) &&
            p->local_port == 8080 && p->remote_port == port &&
            ip_addr_eq(&p->local_ip, &local) && ip_addr_eq(&p->remote_ip, &peer)) {
            if (indexed && !tw) tcp_index_promote(p, full || prior != NULL);
            return p;
        }
    }
    return NULL;
}

static void check_order(void) {
    enum { N = 192 };
    struct tcp_pcb *pcbs[N] = {0};
    ip_addr_t peer;
    init();
    correctness();
    IP4_ADDR(&peer, 10, 1, 1, 2);
    uint64_t checked = 0, hits = 0;
    for (unsigned phase = 0; phase < 5; ++phase) {
        for (unsigned i = 0; i < 12000; ++i) {
            unsigned k = step(&datarng) % N;
            unsigned tuple = step(&datarng) % 31, iface_idx = 1 + step(&datarng) % 3;
            uint16_t port = 18000 + tuple;
            if (!(i & 7)) {
                if (pcbs[k]) tcp_abandon(pcbs[k], 0);
                pcbs[k] = fixture(peer, port, (i & 16) ? ESTABLISHED : TIME_WAIT, phase == 2 && (i & 128));
                pcbs[k]->netif_idx = (i & 64) ? NETIF_NO_INDEX : iface_idx;
            } else if (pcbs[k] && !(i % 17)) {
                pcbs[k]->netif_idx = (i & 1) ? NETIF_NO_INDEX : iface_idx;
            }
            for (unsigned tw = 0; tw < 2; ++tw) {
                struct tcp_pcb *want = find_peer(tw, peer, port, iface_idx, 0);
                struct tcp_pcb *got = find_peer(tw, peer, port, iface_idx, 1);
                CHECK(want == got);
                ++checked;
                hits += got != NULL;
            }
        }
        check_integrity(tcp_active_pcbs);
        check_integrity(tcp_tw_pcbs);
        for (unsigned i = 0; i < N; ++i) {
            if (pcbs[i]) {
                tcp_abandon(pcbs[i], 0);
                pcbs[i] = NULL;
            }
        }
        CHECK(!tcp_active_pcbs && !tcp_tw_pcbs);
    }
    printf("ordered-oracle comparisons=%llu hits=%llu\n",
           (unsigned long long)checked, (unsigned long long)hits);
}

int main(int argc, char **argv) {
    if (argc == 1) {
        check_order();
        return 0;
    }
    CHECK(argc == 5);
    unsigned count = positive_count(argv[1]), iterations = positive_count(argv[2]);
    unsigned batch = positive_count(argv[3]);
    unsigned recycle_period = strcmp(argv[4], "0") ? positive_count(argv[4]) : 0;
    CHECK(batch <= count && batch <= 4096 && iterations < UINT_MAX - batch);
    init();
    iface.flags |= NETIF_FLAG_ETHARP;
    struct tcp_pcb **population = calloc(count, sizeof(*population));
    struct client *clients = calloc(count, sizeof(*clients));
    struct turn *work = calloc(batch, sizeof(*work));
    CHECK(population && clients && work);
    for (unsigned i = 0; i < count; ++i) open_flow(i, &clients[i], &population[i]);

    static const unsigned char request[] = "GET /posts/10 HTTP/1.1\r\nHost: ordinary\r\n\r\n";
    static const unsigned char response[3670] = "HTTP/1.1 200 OK\r\n";
    const unsigned request_len = sizeof(request) - 1;
    CHECK(iterations <= UINT_MAX / request_len);
    uint64_t expected = 0;
    unsigned recycled = 0;
    tcp_ticks = 17;
    uint64_t begin = ns(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned base = 0; base < iterations; base += batch) {
        unsigned n = LWIP_MIN(batch, iterations - base);
        for (unsigned j = 0; j < n; ++j) {
            unsigned id;
            do {
                id = step(&datarng) % count;
            } while (clients[id].batch == base + 1);
            struct client *c = &clients[id];
            c->batch = base + 1;
            if (recycle_period && (base + j) % recycle_period == 0) {
                tcp_abandon(population[id], 0);
                ++c->generation;
                open_flow(id, c, &population[id]);
                ++recycled;
            }
            struct turn *w = &work[j];
            w->chosen = id;
            w->reply = (base + j) & 1 ? sizeof(response) : 240;
            w->ack = c->ack;
            client_tuple(id, c->generation, &w->address, &w->port);
            frame(w->address, w->port, c->seq, c->ack, TCP_ACK | TCP_PSH, request, request_len);
            c->seq += request_len;
            struct tcp_pcb *p = tcp_active_pcbs;
            CHECK(p == population[id] && p->rcv_nxt == c->seq);
            CHECK(tcp_write(p, response, (u16_t)w->reply, TCP_WRITE_FLAG_COPY) == ERR_OK);
            CHECK(tcp_output(p) == ERR_OK);
        }
        for (unsigned round = 0; round < 2; ++round) {
            for (unsigned j = 0; j < n; ++j) {
                struct turn *w = &work[j];
                unsigned acked = LWIP_MIN(w->reply, 2920u);
                if (round) {
                    if (acked == w->reply) continue;
                    acked = w->reply;
                }
                struct client *c = &clients[w->chosen];
                c->ack = w->ack + acked;
                frame(w->address, w->port, c->seq, c->ack, TCP_ACK, NULL, 0);
                struct tcp_pcb *p = tcp_active_pcbs;
                CHECK(p == population[w->chosen] && p->lastack == c->ack);
                if (acked == w->reply) {
                    CHECK(p->unacked == NULL && p->unsent == NULL);
                    expected += w->reply;
                }
            }
        }
        if (base / 1024 != (base + n) / 1024) ++tcp_ticks;
    }
    double per_turn = (double)(ns(CLOCK_THREAD_CPUTIME_ID) - begin) / iterations;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    CHECK(expected == acknowledged_bytes && received == iterations * request_len);
    printf("population=%u turns=%u batch=%u recycled=%u delivered=%u acknowledged=%llu ns_turn=%.2f maxrss_kib=%ld\n",
           count, iterations, batch, recycled, received, (unsigned long long)acknowledged_bytes, per_turn, ru.ru_maxrss);
    while (tcp_active_pcbs) tcp_abandon(tcp_active_pcbs, 0);
    free(work);
    free(clients);
    free(population);
    return 0;
}
