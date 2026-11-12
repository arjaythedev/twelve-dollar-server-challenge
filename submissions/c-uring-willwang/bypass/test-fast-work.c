/* Additional real lwIP TCP-state and no-network moving-work probes. */
#define main pcb_common_main
#define stack_protocol_malloc base_malloc
#define stack_protocol_calloc base_calloc
#include "test-pcb-index.c"
#undef stack_protocol_calloc
#undef stack_protocol_malloc
#undef main

static unsigned deny_alloc, bad_link, retry_calls, capture, cap_n;
static struct tcp_pcb *later, *earlier, *victim;
static uint16_t ord_ports[512];
static unsigned ord_flags[512];
static uint64_t wire_hash = UINT64_C(1469598103934665603);
static uint64_t trace_packets;
static unsigned verify_payload;

void *stack_protocol_malloc(size_t n) {
    return deny_alloc ? NULL : base_malloc(n);
}
void *stack_protocol_calloc(size_t n, size_t s) {
    return deny_alloc ? NULL : base_calloc(n, s);
}
static err_t observe_output(struct netif *n, struct pbuf *p, const ip4_addr_t *dst) {
    err_t result = output(n, p, dst);
    struct ip_hdr h;
    struct tcp_hdr t;
    CHECK(pbuf_copy_partial(p, &h, sizeof(h), 0) == sizeof(h));
    CHECK(pbuf_copy_partial(p, &t, sizeof(t), IPH_HL_BYTES(&h)) == sizeof(t));
    uint16_t port = lwip_ntohs(t.dest);
    if (capture) {
        CHECK(cap_n < sizeof(ord_ports) / sizeof(ord_ports[0]));
        ord_ports[cap_n] = port;
        ord_flags[cap_n++] = TCPH_FLAGS(&t);
    }
    if (verify_payload) {
        unsigned hdr = IPH_HL_BYTES(&h) + TCPH_HDRLEN_BYTES(&t);
        for (unsigned i = hdr; i < p->tot_len; ++i)
            CHECK(pbuf_get_at(p, i) == 0x5a);
    }
    wire_hash = (wire_hash ^ (lwip_ntohl(t.seqno) + ((uint64_t)lwip_ntohl(t.ackno) << 32))) *
                UINT64_C(1099511628211);
    wire_hash =
        (wire_hash ^ (p->tot_len + ((uint64_t)port << 16) + ((uint64_t)TCPH_FLAGS(&t) << 40))) *
        UINT64_C(1099511628211);
    ++trace_packets;
    return bad_link ? ERR_IF : result;
}
static int ack_at(uint16_t port) {
    for (unsigned i = 0; i < cap_n; i++)
        if (ord_ports[i] == port && ord_flags[i] == TCP_ACK)
            return (int)i;
    return -1;
}
static err_t retry_cb(void *data, struct tcp_pcb *p, struct pbuf *b, err_t err) {
    (void)data;
    CHECK(err == ERR_OK && b && b->tot_len == 1);
    ++retry_calls;
    if (retry_calls == 1)
        return ERR_MEM;
    if (retry_calls == 2) {
        tcp_set_flags(earlier, TF_ACK_DELAY);
        tcp_set_flags(later, TF_ACK_DELAY);
        tcp_abort(victim);
        victim = NULL;
        return ERR_MEM;
    }
    if (retry_calls == 3)
        return ERR_MEM;
    CHECK(retry_calls == 4);
    tcp_recved(p, b->tot_len);
    pbuf_free(b);
    return ERR_OK;
}
static struct tcp_pcb *replacement;
static unsigned abort_calls;

static err_t abort_and_replace(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    CHECK(p && p->tot_len == 1 && err == ERR_OK);
    if (++abort_calls == 1)
        return ERR_MEM;
    CHECK(abort_calls == 2);
    const ip_addr_t peer = pcb->remote_ip;
    pbuf_free(p);
    tcp_abort(pcb);
    replacement = fixture(peer, 36101, ESTABLISHED, 0);
    tcp_set_flags(replacement, TF_ACK_DELAY);
    return ERR_ABRT;
}

static void fast_edges(void) {
    ip_addr_t remote;
    IP4_ADDR(&remote, 10, 12, 1, 3);
    /* A callback marks an already-visited peer and an unvisited older one,
       removes a third peer, and refuses again across an upstream scan restart. */
    later = fixture(remote, 36000, ESTABLISHED, 0);
    victim = fixture(remote, 36001, ESTABLISHED, 0);
    struct tcp_pcb *retry = fixture(remote, 36002, ESTABLISHED, 0);
    tcp_recv(retry, retry_cb);
    packet(remote, 36002, 500, 525, TCP_ACK, 1);
    CHECK(retry_calls == 1 && retry->refused_data);
    earlier = fixture(remote, 36003, ESTABLISHED, 0);
    capture = 1;
    cap_n = 0;
    tcp_fasttmr();
    CHECK(retry_calls == 2 && retry->refused_data && victim == NULL);
    CHECK(ack_at(36003) < 0 && ack_at(36000) >= 0);
    cap_n = 0;
    tcp_fasttmr();
    CHECK(retry_calls == 3 && ack_at(36003) >= 0);
    cap_n = 0;
    tcp_fasttmr();
    CHECK(retry_calls == 4 && !retry->refused_data);
    cap_n = 0;
    tcp_fasttmr();
    CHECK(!cap_n && retry_calls == 4);

    /* An off-head application close hits allocation failure for two whole
       ticks, then sends exactly one FIN. tcp_send_empty_ack() has separate
       allocator and link-failure retry mark paths outside tcp_input. */
    deny_alloc = 1;
    CHECK(tcp_close(later) == ERR_OK && (later->flags & TF_CLOSEPEND));
    tcp_fasttmr();
    tcp_fasttmr();
    CHECK(later->state == ESTABLISHED && (later->flags & TF_CLOSEPEND));
    deny_alloc = 0;
    cap_n = 0;
    tcp_fasttmr();
    CHECK(later->state == FIN_WAIT_1 && !(later->flags & TF_CLOSEPEND));
    CHECK(cap_n == 1 && (ord_flags[0] & TCP_FIN));
    deny_alloc = 1;
    CHECK(tcp_send_empty_ack(retry) == ERR_BUF);
    deny_alloc = 0;
    cap_n = 0;
    tcp_fasttmr();
    CHECK(ack_at(36002) >= 0 && !(retry->flags & TF_ACK_DELAY));
    bad_link = 1;
    CHECK(tcp_send_empty_ack(retry) == ERR_IF);
    bad_link = 0;
    cap_n = 0;
    tcp_fasttmr();
    CHECK(ack_at(36002) >= 0 && !(retry->flags & TF_ACK_DELAY));
    /* Exercise wrap frequently without slow ticks, then enter the slow scan.
       Off-front work in exactly the 256th generation must still be serviced. */
    for (unsigned i = 0; i < 12; i++) {
        for (unsigned j = 0; j < 255; j++)
            tcp_fasttmr();
        cap_n = 0;
        tcp_set_flags(retry, TF_ACK_DELAY);
        tcp_fasttmr();
        CHECK(ack_at(36002) >= 0);
    }
    unsigned p = polls;
    tcp_poll(earlier, poll_cb, 1);
    tcp_slowtmr();
    CHECK(polls == p + 1);
    capture = 0;
    tcp_abort(earlier);
    tcp_abort(retry);
    tcp_abort(later);
    CHECK(!tcp_active_pcbs);
    /* A PCB allocated by an aborting callback starts with the current timer
       generation. A restart must skip it until the next tick, even if the
       allocator reuses the same address that the callback just freed. */
    size_t previous_live = live;
    struct tcp_pcb *aborting = fixture(remote, 36100, ESTABLISHED, 0);
    tcp_recv(aborting, abort_and_replace);
    packet(remote, 36100, 500, 525, TCP_ACK, 1);
    CHECK(abort_calls == 1 && aborting->refused_data != NULL);
    capture = 1;
    cap_n = 0;
    tcp_fasttmr();
    CHECK(abort_calls == 2 && tcp_active_pcbs == replacement);
    CHECK(ack_at(36101) < 0);
    cap_n = 0;
    tcp_fasttmr();
    CHECK(ack_at(36101) >= 0);
    capture = 0;
    tcp_abort(replacement);
    CHECK(!tcp_active_pcbs && live == previous_live);

    printf("edges=ok retry_calls=%u trace_packets=%llu hash=%016llx\n", retry_calls,
           (unsigned long long)trace_packets, (unsigned long long)wire_hash);
}

static unsigned response_size(unsigned bucket) {
    if (bucket < 100)
        return 3670;
    if (bucket < 200)
        return 256;
    if (bucket < 215)
        return 104;
    return 225;
}

static void moving(unsigned n, unsigned reqs, unsigned ticks) {
    /* Fully inspect every output data byte in the small regression case. */
    verify_payload = n <= 128;
    struct tcp_pcb **pcbs = calloc(n, sizeof(*pcbs));
    unsigned *order = calloc(n, sizeof(*order));
    CHECK(pcbs && order);
    for (unsigned i = 0; i < n; i++) {
        ip_addr_t peer;
        uint16_t port;
        idtuple(i, &peer, &port);
        struct tcp_pcb *p = pcbs[i] = tcp_new();
        CHECK(p);
        p->state = ESTABLISHED;
        p->local_ip = local;
        p->remote_ip = peer;
        p->local_port = 8080;
        p->remote_port = port;
        p->snd_nxt = p->snd_lbb = p->lastack = p->snd_wl2 = 525;
        p->rcv_nxt = 500;
        p->rcv_ann_right_edge = p->rcv_nxt + p->rcv_wnd;
        p->mss = TCP_MSS;
        p->cwnd = 16000;
        p->ssthresh = 32768;
        p->snd_wnd = 32000;
        tcp_poll(p, NULL, 255);
        tcp_recv(p, recv_cb);
        tcp_nagle_disable(p);
        ip_set_option(p, SOF_KEEPALIVE);
        p->keep_idle = 75000;
        p->keep_intvl = 10000;
        p->keep_cnt = 6;
        order[i] = i;
    }
    for (unsigned i = n - 1; i; i--) {
        unsigned j = step(&datarng) % (i + 1), k = order[i];
        order[i] = order[j];
        order[j] = k;
    }
    for (unsigned i = 0; i < n; i++)
        TCP_REG_ACTIVE(pcbs[order[i]]);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    size_t resident = live;
    static uint8_t body[3670];
    memset(body, 0x5a, sizeof(body));
    uint64_t packet_ns = 0, fast_ns = 0, slow_ns = 0, total = 0;
    unsigned nslow = 0;
    for (unsigned tick = 0; tick < ticks; tick++) {
        uint64_t t = ns(CLOCK_THREAD_CPUTIME_ID);
        for (unsigned j = 0; j < reqs; j++) {
            unsigned idx = step(&datarng) % n;
            struct tcp_pcb *p = pcbs[idx];
            unsigned bucket = step(&datarng) % 217, size = response_size(bucket);
            unsigned bytes = bucket >= 215 ? 345 : 164;
            packet(p->remote_ip, p->remote_port, p->rcv_nxt, p->snd_nxt, TCP_ACK | TCP_PSH, bytes);
            CHECK(tcp_active_pcbs == p);
            CHECK(tcp_write(p, body, size, TCP_WRITE_FLAG_COPY) == ERR_OK);
            CHECK(tcp_output(p) == ERR_OK && p->unacked);
            for (unsigned ack = 0; p->unacked && ack < 6; ++ack)
                packet(p->remote_ip, p->remote_port, p->rcv_nxt, p->snd_nxt, TCP_ACK, 0);
            CHECK(!p->unacked && !p->unsent);
            total++;
        }
        packet_ns += ns(CLOCK_THREAD_CPUTIME_ID) - t;
        t = ns(CLOCK_THREAD_CPUTIME_ID);
        tcp_fasttmr();
        fast_ns += ns(CLOCK_THREAD_CPUTIME_ID) - t;
        if (tick % 2 == 1) {
            t = ns(CLOCK_THREAD_CPUTIME_ID);
            tcp_slowtmr();
            slow_ns += ns(CLOCK_THREAD_CPUTIME_ID) - t;
            nslow++;
        }
    }
    printf("pcbs=%u reqs_per_tick=%u ticks=%u reqs=%llu packets=%llu "
           "rcv_bytes=%u hash=%016llx "
           "packet_us_req=%.4f fast_ms_tick=%.4f slow_ms_tick=%.4f "
           "total_us_req=%.4f nominal_timer_cpu_pct=%.3f "
           "pcb_bytes=%zu alloc_B=%zu rss_KiB=%ld\n",
           n, reqs, ticks, (unsigned long long)total, (unsigned long long)trace_packets, received,
           (unsigned long long)wire_hash, packet_ns / (1000. * total), fast_ns / (1e6 * ticks),
           slow_ns / (1e6 * nslow), (fast_ns + slow_ns + packet_ns) / (1000. * total),
           100. * (fast_ns + slow_ns) / (250000000. * ticks), sizeof(struct tcp_pcb), resident,
           ru.ru_maxrss);
    while (tcp_active_pcbs)
        tcp_abandon(tcp_active_pcbs, 0);
    free(pcbs);
    free(order);
}
int main(int argc, char **argv) {
    init();
    iface.output = observe_output;
    if (argc == 1) {
        fast_edges();
        return 0;
    }
    CHECK(argc == 4);
    unsigned ticks = positive_count(argv[3]);
    CHECK(ticks >= 2);
    moving(positive_count(argv[1]), positive_count(argv[2]), ticks);
    return 0;
}
