#define main relay_program_main
#include "relay.c"
#undef main
#include "../tap-sharded/test-frames.h"
static struct worker worker, consumers[MAX_PORTS];

static void setup(void) {
    open_test_ports();
    init_outbound();
}
static void broadcast_waits_for_every_destination(void) {
    setup();
    delay = 5000000000ULL;
    int snd = 4096;
    assert(!setsockopt(ports[1], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd)));
    char filler[700] = {0}, tmp[1000];
    unsigned fillers = 0;
    while (write(ports[1], filler, sizeof(filler)) == (ssize_t)sizeof(filler))
        ++fillers;
    assert(errno == EAGAIN && fillers);
    submit(0, MAX_PORTS, 11, 4096, 1);
    assert(read_service(&worker));
    struct shared_frame *p = &outbound.frames[0];
    assert(p->refs == 4 && outbound.allocated == 1);
    for (unsigned i = 1; i < 5; i++) {
        assert(lanes[i].ids[0] == 0);
        assert(!deliver_service(&consumers[i], i));
    }
    p->due = now();
    assert(!deliver_service(&consumers[1], 1));
    assert(consumers[1].local.lane[1].write_retries == 1);
    for (unsigned i = 2; i < 5; i++) {
        assert(deliver_service(&consumers[i], i));
        verify(i, 0, MAX_PORTS, 11, 4096, 1);
    }
    assert(reclaim_service() == 3 && p->refs == 1 && !outbound.returned);
    delay = 0;
    submit(0, 1, 12, 107, 0);
    assert(read_service(&worker));
    assert(outbound.allocated == 2 && lanes[1].ids[1] == 1);
    while (fillers--)
        assert(read(peers[1], tmp, sizeof(tmp)) == (ssize_t)sizeof(filler));
    lanes[1].retry_at = 0;
    assert(deliver_service(&consumers[1], 1));
    verify(1, 0, MAX_PORTS, 11, 4096, 1);
    /* The 4-KiB frame may occupy the entire deliberately tiny send buffer. */
    lanes[1].retry_at = 0;
    deliver_service(&consumers[1], 1);
    verify(1, 0, 1, 12, 107, 0);
    expect_empty(1);
    assert(reclaim_service() == 2 && outbound.returned == 2 && !p->refs);
    assert(worker.local.service_admitted == 2 && consumers[1].local.lane[1].delivered == 2);
    for (unsigned i = 1; i < 5; i++)
        assert(consumers[i].local.lane[i].dropped == 0);
}
static void full_lane_preserves_order(void) {
    setup();
    delay = 5000000000ULL;
    for (unsigned i = 0; i < DEPTH + 1; i++) {
        submit(0, 3, i, 72, 0);
        read_service(&worker);
    }
    assert(outbound.blocked && worker.local.lane[3].enqueued == DEPTH &&
           worker.local.port[0].read == DEPTH + 1);
    submit(0, 3, DEPTH + 1, 72, 0);
    assert(!read_service(&worker));
    assert(worker.local.port[0].read == DEPTH + 1 && worker.local.lane[3].full_polls >= 2);
    outbound.frames[lanes[3].ids[0]].due = now();
    assert(deliver_service(&consumers[3], 3));
    verify(3, 0, 3, 0, 72, 0);
    assert(read_service(&worker));
    assert(worker.local.port[0].read == DEPTH + 2 && worker.local.lane[3].enqueued == DEPTH + 1);
    assert(outbound.blocked && lanes[3].ids[0] == DEPTH && worker.local.port[3].write_drop == 0);
}
enum { TEST_FRAMES = 320 };
static _Atomic int quit_test;
static void *traffic_thread(void *arg) {
    unsigned dst = (unsigned)(uintptr_t)arg;
    while (!atomic_load(&quit_test)) {
        int progress = dst ? deliver_service(&consumers[dst], dst) : read_service(&worker);
        if (!progress)
            sched_yield();
    }
    return NULL;
}
static size_t size_for(unsigned n) {
    return n % 13 == 0 ? 4096 : 100 + n % 803;
}
static void concurrent_consumers_preserve_bytes(void) {
    setup();
    delay = 75000;
    pthread_t threads[MAX_PORTS];
    for (unsigned p = 0; p < MAX_PORTS; p++)
        assert(!pthread_create(&threads[p], NULL, traffic_thread, (void *)(uintptr_t)p));
    unsigned issued = 0, done = 0, next[MAX_PORTS] = {0, 0, 1, 2, 3};
    uint64_t deadline = now() + 10000000000ULL;
    unsigned char buf[8192], wanted[8192];
    while (done < TEST_FRAMES) {
        assert(now() < deadline);
        if (issued < TEST_FRAMES) {
            unsigned dst = issued % 4 + 1;
            size_t n = size_for(issued);
            make_frame(wanted, 0, dst, issued, n, n > 1518);
            ssize_t rc = write(peers[0], wanted, n);
            if (rc == (ssize_t)n)
                ++issued;
            else
                assert(rc < 0 && errno == EAGAIN);
        }
        for (unsigned dst = 1; dst < MAX_PORTS; dst++) {
            ssize_t nr = read(peers[dst], buf, sizeof(buf));
            if (nr < 0) {
                assert(errno == EAGAIN);
                continue;
            }
            unsigned seq = next[dst];
            size_t n = size_for(seq);
            assert(nr == (ssize_t)n);
            make_frame(wanted, 0, dst, seq, n, n > 1518);
            assert(!memcmp(wanted, buf, n));
            next[dst] += 4;
            ++done;
        }
        sched_yield();
    }
    atomic_store(&quit_test, 1);
    for (unsigned i = 0; i < MAX_PORTS; i++)
        assert(!pthread_join(threads[i], NULL));
    reclaim_service();
    assert(outbound.returned == outbound.allocated && worker.local.service_admitted == TEST_FRAMES);
    for (unsigned i = 1; i < MAX_PORTS; i++) {
        assert(consumers[i].local.lane[i].written == TEST_FRAMES / 4 &&
               consumers[i].local.lane[i].dropped == 0);
        assert(worker.local.lane[i].enqueued == TEST_FRAMES / 4);
    }
}
int main(void) {
    run_case(broadcast_waits_for_every_destination);
    run_case(full_lane_preserves_order);
    run_case(concurrent_consumers_preserve_bytes);
    puts("Split relay passed multicast ownership, backpressure and concurrent ordering checks.");
    return 0;
}
