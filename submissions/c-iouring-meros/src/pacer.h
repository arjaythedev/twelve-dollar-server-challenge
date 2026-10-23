/*
 * pacer: decides when the event loop should let work pile up before it looks again.
 *
 * Under steady load every event-loop round holds one or two events, so each request pays for its own
 * sleep and wake-up, and the batch has nothing to share. When events arrive faster than
 * PACER_BUSY_EVENTS_PER_SECOND, the loop sleeps PACER_WINDOW_US between rounds instead: the same
 * work then arrives in rounds of ten or more events, which share one wake-up, one feed query and
 * one transaction. The cost is at most one window of latency, far inside the 500 ms budget.
 */
#ifndef PACER_H
#define PACER_H

#include <stdbool.h>
#include <stdint.h>

#ifndef PACER_WINDOW_US
#define PACER_WINDOW_US 1000
#endif

#ifndef PACER_BUSY_EVENTS_PER_SECOND
#define PACER_BUSY_EVENTS_PER_SECOND 3000
#endif

enum {
	PACER_MEASURE_NS = 50 * 1000 * 1000, /* the rate is measured over windows of this length */
};

typedef struct pacer {
	uint64_t window_start_ns;
	uint64_t events_in_window;
	bool busy;
} pacer;

void pacer_init(pacer *p, uint64_t now_ns);

/* Records that a round handled `events` events at time `now_ns` (monotonic). */
void pacer_record(pacer *p, uint64_t now_ns, unsigned events);

static inline bool pacer_is_busy(const pacer *p)
{
	return p->busy;
}

#endif
