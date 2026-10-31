/* SPDX-License-Identifier: MIT */
/* Test-only driver for the real NASM queue-drain function; no kernel ring. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned char ring_contexts[], connections[];
extern uint64_t enobuf_queue[];
extern uint32_t generations[], enobuf_fair;
extern int uring_enobuf_drain(unsigned ring);
extern void uring_recv(unsigned fd);
static uint32_t cq_head, cq_tail, sq_head, sq_tail, sq_mask = 4095;
static uint32_t sq_array[4096];
static unsigned char sqes[4096 * 64];
static void u32(void *p, uint32_t v) { memcpy(p, &v, sizeof v); }
static void u64(void *p, uint64_t v) { memcpy(p, &v, sizeof v); }
static uint32_t get32(void *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint64_t get64(void *p) { uint64_t v; memcpy(&v, p, sizeof v); return v; }
static unsigned char *setup(unsigned ring, unsigned credits, unsigned depth,
                            unsigned entries, int valid) {
    unsigned char *r = ring_contexts + 512 * ring;
    unsigned fd = ring << 16;
    memset(r, 0, 512);
    memset(connections + 64 * fd, 0, 64);
    cq_head = 0; cq_tail = depth; sq_head = sq_tail = 0;
    u64(r + 24, (uintptr_t)sqes);
    u64(r + 32, (uintptr_t)&sq_head);
    u64(r + 40, (uintptr_t)&sq_tail);
    u64(r + 48, (uintptr_t)&sq_mask);
    u64(r + 56, (uintptr_t)sq_array);
    u64(r + 64, (uintptr_t)&cq_head);
    u64(r + 72, (uintptr_t)&cq_tail);
    u32(r + 132, entries);
    u32(r + 136, credits);
    generations[fd] = 2;
    u32(connections + 64 * fd + 60, 1 | 256);
    for (unsigned i = 0; i < entries; ++i)
        enobuf_queue[fd + i] = ((uint64_t)(valid ? 2 : 1) << 32) | fd;
    return r;
}
int main(void) {
    unsigned char *r;
    /* The previous policy demonstrably starves a nonempty completion queue. */
    enobuf_fair = 0;
    r = setup(0, 64, 1, 256, 0);
    assert(uring_enobuf_drain(0) == 0 && get32(r + 128) == 0);
    enobuf_fair = 1;
    r = setup(0, 0, 1, 256, 0);
    assert(uring_enobuf_drain(0) == 0);
    r = setup(0, 1, 1, 256, 0);
    assert(uring_enobuf_drain(0) == 1 && get32(r + 128) == 1);
    r = setup(0, 64, 1, 256, 0);
    assert(uring_enobuf_drain(0) == 64 && get32(r + 128) == 64);
    r = setup(0, 300, 1, 256, 0);
    assert(uring_enobuf_drain(0) == 128 && get32(r + 128) == 128);
    r = setup(0, 0, 0, 256, 0);
    assert(uring_enobuf_drain(0) == 128 && get32(r + 128) == 128);
    assert(sq_tail == 0); /* Stale generation tags never submit receives. */
    /* Fresh reads join the FIFO behind older paused reads, not ahead of them. */
    r = setup(0, 2, 1, 1, 1);
    memset(connections + 64, 0, 64);
    generations[1] = 2;
    u32(connections + 64 + 60, 1);
    uring_recv(1);
    assert(sq_tail == 0 && get32(r + 132) == 2);
    assert(get32(connections + 64 + 60) == (1 | 256));
    assert(uring_enobuf_drain(0) == 2 && sq_tail == 2);
    assert(get32(sqes + 4) == 0 && get32(sqes + 64 + 4) == 1);
    assert(get32(connections + 64 + 60) == (1 | 32));
    for (unsigned ring = 0; ring < 2; ++ring) {
        unsigned fd = ring << 16;
        r = setup(ring, 1, 1, 1, 1);
        assert(uring_enobuf_drain(ring) == 1);
        assert(sq_tail == 1 && sqes[0] == 27); /* IORING_OP_RECV */
        assert(get32(connections + 64 * fd + 60) == (1 | 32));
        assert(get64(sqes + 32) == ((UINT64_C(2) << 32) | fd | 0x200000));
        r = setup(ring, 1, 1, 1, 1);
        u32(connections + 64 * fd + 60, 1 | 16 | 256);
        assert(uring_enobuf_drain(ring) == 1 && sq_tail == 0);
        r = setup(ring, 1, 1, 1, 1);
        u32(connections + 64 * fd + 60, 1 | 32 | 256);
        assert(uring_enobuf_drain(ring) == 1 && sq_tail == 0);
    }
    puts("PASS: bounded fair retries, old-policy starvation, stale/closing/pending generations and ring addressing");
    return 0;
}
