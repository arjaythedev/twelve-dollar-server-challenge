/* The two ring operations server.c does with C11 atomics that Nexium cannot
 * express. Nexium's atomics (sync.atomic_*) are i64 only and sequentially
 * consistent: an 8-byte operation on one of these u32 words would also
 * overwrite the field next to it. Header only: @cImport's #include puts it
 * in the generated C file, and gcc inlines both functions. */
#include <stdint.h>

/* Clears bits of sq_flags. The kernel sets bits in that word with atomic
 * ops of its own, from whatever CPU completed the I/O, so a plain load and
 * store here could lose a bit it set in between. */
static inline void ring_fetch_and(uint32_t *p, uint32_t mask) { __atomic_fetch_and(p, mask, __ATOMIC_SEQ_CST); }

/* A full barrier. idle_wait switches the eventfd signal on and only then
 * checks the rings for work; without the barrier the check could be done
 * before the switch is visible, and a completion would land in neither. */
static inline void ring_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
