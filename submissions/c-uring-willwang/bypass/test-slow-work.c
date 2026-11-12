/* All frames remain in a local netif. Compare the exact timed TCP trace. */
#define main common_pcb_main
#include "test-pcb-index.c"
#undef main

static unsigned virtual_tick, frames, polls_mutated, abort_events, keep_probes;
static uint64_t trace = UINT64_C(1469598103934665603);
static struct tcp_pcb *past, *future, *victim, *new_peer, *error_victim;
static ip_addr_t remote;
static void mix(uint32_t v) {
    trace ^= v;
    trace *= UINT64_C(1099511628211);
}
static void enroll(struct tcp_pcb *p) {
#if LWIP_TCP_SLOW_WORK
    tcp_slow_work_enroll(p);
#else
    (void)p;
#endif
}
static void wake(struct tcp_pcb *p) {
#if LWIP_TCP_SLOW_WORK
    tcp_slow_work_wake(p);
#else
    (void)p;
#endif
}
static struct tcp_pcb *idle(unsigned port) {
    struct tcp_pcb *p = fixture(remote, port, ESTABLISHED, 0);
    tcp_poll(p, NULL, 255);
    p->mss = 1460;
    p->cwnd = 16384;
    p->ssthresh = 32768;
    p->snd_wnd = 32000;
    enroll(p);
    return p;
}
static err_t trace_output(struct netif *n, struct pbuf *p, const ip4_addr_t *dst) {
    struct ip_hdr ip;
    struct tcp_hdr tcp;
    CHECK(pbuf_copy_partial(p, &ip, sizeof(ip), 0) == sizeof(ip));
    CHECK(pbuf_copy_partial(p, &tcp, sizeof(tcp), IPH_HL_BYTES(&ip)) == sizeof(tcp));
    err_t result = output(n, p, dst);
    mix(virtual_tick);
    mix(lwip_ntohs(tcp.dest));
    mix(lwip_ntohl(tcp.seqno));
    mix(lwip_ntohl(tcp.ackno));
    mix(TCPH_FLAGS(&tcp));
    mix(p->tot_len);
    unsigned head = IPH_HL_BYTES(&ip) + TCPH_HDRLEN_BYTES(&tcp);
    for (unsigned i = head; i < p->tot_len; i++)
        mix(pbuf_get_at(p, i));
    if (lwip_ntohs(tcp.dest) == 39004 && TCPH_FLAGS(&tcp) == TCP_ACK && txseq == 524)
        ++keep_probes;
    frames++;
    return result;
}
static void tick(void) {
    ++virtual_tick;
    tcp_fasttmr();
    tcp_fasttmr();
    tcp_slowtmr();
}
static err_t mutate(void *arg, struct tcp_pcb *pcb) {
    (void)arg;
    ++polls_mutated;
    CHECK(virtual_tick == 20 && polls_mutated == 1);
    /* Earlier and later dormant PCBs must expose different counter values
       at this exact point in the original active-list traversal. */
    CHECK(past->polltmr == 20 && future->polltmr == 19);
    tcp_abort(victim);
    victim = NULL;
    new_peer = idle(39013);
    CHECK(tcp_write(future, "abcde", 5, TCP_WRITE_FLAG_COPY) == ERR_OK);
    CHECK(tcp_output(future) == ERR_OK && future->rtime == 0 && future->unacked);
    tcp_poll(pcb, NULL, 255);
    enroll(pcb);
    mix(0xabc0);
    mix(virtual_tick);
    return ERR_OK;
}
static void expired(void *arg, err_t err) {
    (void)arg;
    CHECK(err == ERR_ABRT && error_victim);
    ++abort_events;
    mix(0xeb);
    mix(virtual_tick);
    tcp_abort(error_victim);
    error_victim = NULL;
}
int main(void) {
    init();
    iface.output = trace_output;
    IP4_ADDR(&remote, 10, 17, 0, 3);
    tcp_ticks = UINT32_MAX - 127;
    struct tcp_pcb *background[64];
    for (unsigned i = 0; i < 64; i++)
        background[i] = idle(40000 + i);
    future = idle(39010);
    victim = idle(39011);
    struct tcp_pcb *caller = idle(39012);
    tcp_poll(caller, mutate, 20);
    past = idle(39014);
    tick();
#if LWIP_TCP_SLOW_WORK
    CHECK(background[0]->slow_parked && background[0]->polltmr == 0);
#endif
    while (virtual_tick < 20)
        tick();
    CHECK(polls_mutated == 1 && !victim && new_peer && new_peer->polltmr == 0);
    CHECK(future->rtime == 1 && future->unacked);
    packet(remote, 39010, future->rcv_nxt, future->snd_nxt, TCP_ACK, 0);
    CHECK(future->unacked == NULL);

    /* Explicitly enroll a retransmitting flow only while it is idle, then
       drive a real write, lost response, backoff and cumulative ACK. */
    struct tcp_pcb *rto = idle(39000);
    CHECK(tcp_write(rto, "retransmit", 10, TCP_WRITE_FLAG_COPY) == ERR_OK);
    CHECK(tcp_output(rto) == ERR_OK && rto->unacked);
    uint32_t original_seq = txseq;
    for (unsigned i = 0; i < 80 && !rto->nrtx; i++)
        tick();
    CHECK(rto->nrtx > 0 && rto->cwnd == rto->mss && txseq == original_seq);
    packet(remote, 39000, rto->rcv_nxt, rto->snd_nxt, TCP_ACK, 0);
    CHECK(!rto->unacked && rto->rtime < 0);

    struct tcp_pcb *persist = idle(39001);
    wake(persist);
    persist->snd_wnd = 0;
    CHECK(tcp_write(persist, "window", 6, TCP_WRITE_FLAG_COPY) == ERR_OK);
    CHECK(tcp_output(persist) == ERR_OK && persist->persist_backoff && !persist->unacked);
    for (unsigned i = 0; i < 15 && !persist->persist_probe; i++)
        tick();
    CHECK(persist->persist_probe > 0 && persist->unsent);
    packet(remote, 39001, persist->rcv_nxt, persist->snd_nxt, TCP_ACK, 0);
    CHECK(!persist->persist_backoff && persist->unacked);
    packet(remote, 39001, persist->rcv_nxt, persist->snd_nxt, TCP_ACK, 0);
    CHECK(!persist->unacked);

    struct tcp_pcb *ooo = idle(39008);
    wake(ooo);
    ooo->rto = 3;
    enroll(ooo);
    packet(remote, 39008, ooo->rcv_nxt + 128, ooo->snd_nxt, TCP_ACK | TCP_PSH, 4);
    CHECK(ooo->ooseq);
    unsigned ooseq_wait = (unsigned)ooo->rto * TCP_OOSEQ_TIMEOUT;
    for (unsigned i = 1; i < ooseq_wait; i++) {
        tick();
        CHECK(ooo->ooseq);
    }
    tick();
    CHECK(!ooo->ooseq);

    struct tcp_pcb *closing = idle(39009);
    CHECK(tcp_close(closing) == ERR_OK && closing->state == FIN_WAIT_1);
    packet(remote, 39009, closing->rcv_nxt, closing->snd_nxt, TCP_ACK, 0);
    CHECK(closing->state == FIN_WAIT_2);
    packet(remote, 39009, closing->rcv_nxt, closing->snd_nxt, TCP_ACK | TCP_FIN, 0);
    CHECK(tcp_tw_pcbs == closing && closing->state == TIME_WAIT);
    unsigned timewait_tick = virtual_tick;

    struct tcp_pcb *keep = idle(39004);
    wake(keep);
    ip_set_option(keep, SOF_KEEPALIVE);
    keep->keep_idle = 2000;
    keep->keep_intvl = 1000;
    keep->keep_cnt = 6;
    enroll(keep);
    unsigned start = virtual_tick;
    while (virtual_tick < start + 4)
        tick();
    CHECK(keep_probes == 0);
    tick();
    CHECK(keep_probes == 1);
    tick();
    CHECK(keep_probes == 1);
    tick();
    CHECK(keep_probes == 2);
    packet(remote, 39004, keep->rcv_nxt, keep->snd_nxt, TCP_ACK, 0);
    CHECK(!keep->keep_cnt_sent);
    wake(keep);
    ip_reset_option(keep, SOF_KEEPALIVE);
    enroll(keep);

    /* Remove and reuse the same tuple before its booked deadline. */
    struct tcp_pcb *reuse = idle(39005);
    wake(reuse);
    ip_set_option(reuse, SOF_KEEPALIVE);
    reuse->keep_idle = 1000;
    enroll(reuse);
    tcp_abort(reuse);
    reuse = idle(39005);
    unsigned old_frames = frames;
    for (unsigned i = 0; i < 5; i++)
        tick();
    CHECK(frames == old_frames);
    struct tcp_pcb *syn = fixture(remote, 39006, SYN_RCVD, 0);
    error_victim = idle(39007);
    tcp_err(syn, expired);
    syn->tmr = tcp_ticks - TCP_SYN_RCVD_TIMEOUT / TCP_SLOW_INTERVAL - 1;
    tick();
    CHECK(abort_events == 1 && !error_victim);

    /* Detached peers still poll every two ticks; changing policy while
       parked must first materialize its old virtual poll counter. */
    tcp_poll(reuse, poll_cb, 2);
    unsigned from = polls;
    for (unsigned i = 0; i < 8; i++)
        tick();
    CHECK(polls >= from + 4);
    tcp_poll(reuse, NULL, 255);
    enroll(reuse);

    /* Shared 8-bit timer generation and 32-bit tcp_ticks both wrap. */
    while (virtual_tick < timewait_tick + 2 * TCP_MSL / TCP_SLOW_INTERVAL)
        tick();
    CHECK(tcp_tw_pcbs == closing);
    tick();
    CHECK(!tcp_tw_pcbs);
    while (virtual_tick < 650)
        tick();
    for (unsigned i = 0; i < 64; i++) {
        wake(background[i]);
        CHECK(background[i]->polltmr == (650 % 255));
    }
    check_integrity(tcp_active_pcbs);
    CHECK(tcp_ticks == (u32_t)(UINT32_MAX - 127u + 650u));
    while (tcp_active_pcbs)
        tcp_abandon(tcp_active_pcbs, 0);
    CHECK(!tcp_tw_pcbs);
    printf("ticks=%u tcp_frames=%u keep_probes=%u poll_mutations=%u "
           "error_mutations=%u digest=%016llx\n",
           virtual_tick, frames, keep_probes, polls_mutated, abort_events,
           (unsigned long long)trace);
    return 0;
}
