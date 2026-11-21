#define main relay_program_main
#include "relay.c"
#undef main
#include "test-frames.h"
static struct worker worker;

static void setup(void) {
    open_test_ports();
    for (unsigned p = 0; p < MAX_PORTS; ++p)
        init_fifo(&fifos[p]);
}
static void blocked_writes_preserve_frames(void) {
    setup();
    delay = 3000000000ULL;
    int snd = 4096;
    assert(!setsockopt(ports[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd)));
    for (unsigned i = 0; i < 24; i++) {
        submit(1, 0, i, 700, 0);
        assert(read_source(&worker, 1));
    }
    assert(!send_due(&worker, 1));
    expect_empty(0);
    struct fifo *q = &fifos[1];
    for (unsigned i = 0; i < q->size; i++)
        q->packets[q->sequence[i]].due = now();
    unsigned delivered = 0;
    while (q->size) {
        q->retry_at = 0;
        send_due(&worker, 1);
        while (delivered < worker.local.port[0].written)
            verify(0, 1, 0, delivered++, 700, 0);
    }
    assert(delivered == 24 && worker.local.port[1].write_retries > 0 &&
           worker.local.port[0].write_drop == 0);
    assert(q->allocated == 24 && q->returned == 24);
    expect_empty(0);
    delay = 0;
    submit(1, 0, 24, 4096, 1);
    assert(read_source(&worker, 1));
    assert(send_due(&worker, 1));
    verify(0, 1, 0, 24, 4096, 1);
    assert(worker.local.gso == 1 && worker.local.partial == 25 && !worker.local.bad);
    submit(2, MAX_PORTS, 99, 128, 0);
    assert(read_source(&worker, 2));
    assert(send_due(&worker, 2));
    for (unsigned p = 0; p < MAX_PORTS; p++) {
        if (p != 2)
            verify(p, 2, MAX_PORTS, 99, 128, 0);
        expect_empty(p);
    }
    submit(3, 0, 25, 4096, 0);
    assert(read_source(&worker, 3));
    assert(worker.local.bad == 1 && !fifos[3].size);
}
static void full_fifo_preserves_kernel_input(void) {
    setup();
    delay = 10000000000ULL;
    for (unsigned i = 0; i < DEPTH; i++) {
        submit(1, 0, i, 72, 0);
        assert(read_source(&worker, 1));
    }
    struct fifo *q = &fifos[1];
    assert(q->size == DEPTH && worker.local.port[1].queue_peak == DEPTH);
    submit(1, 0, DEPTH, 72, 0);
    assert(!read_source(&worker, 1));
    assert(worker.local.port[1].full_polls >= 1);
    assert(worker.local.port[1].read == DEPTH && worker.local.queue_overflow == 0);
    q->packets[q->sequence[q->head]].due = now();
    assert(send_due(&worker, 1));
    verify(0, 1, 0, 0, 72, 0);
    assert(read_source(&worker, 1));
    assert(q->size == DEPTH && worker.local.port[1].read == DEPTH + 1);
}
int main(void) {
    run_case(blocked_writes_preserve_frames);
    run_case(full_fifo_preserves_kernel_input);
    puts("Sharded relay passed blocked-write FIFO, multicast, GSO, delay and full-FIFO checks.");
    return 0;
}
