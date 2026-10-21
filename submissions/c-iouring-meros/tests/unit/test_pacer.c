#include "check.h"
#include "pacer.h"

enum { MS = 1000 * 1000 };

/* Feeds the pacer `events_per_round` events every `round_ms`, for `rounds` rounds. */
static void run(pacer *p, uint64_t *now_ns, unsigned rounds, unsigned round_ms, unsigned events_per_round)
{
	for (unsigned i = 0; i < rounds; i++) {
		*now_ns += (uint64_t)round_ms * MS;
		pacer_record(p, *now_ns, events_per_round);
	}
}

int main(void)
{
	pacer p;
	uint64_t now = 1000ULL * 1000 * MS;

	pacer_init(&p, now);
	CHECK(!pacer_is_busy(&p));

	/* 100 events/s: idle. The loop keeps blocking in epoll_wait. */
	run(&p, &now, 100, 10, 1);
	CHECK(!pacer_is_busy(&p));

	/* 10,000 events/s (10 per millisecond round): busy after the first measuring window. */
	run(&p, &now, 100, 1, 10);
	CHECK(pacer_is_busy(&p));

	/* The load goes away: back to idle after the next full window. */
	run(&p, &now, 100, 1, 0);
	CHECK(!pacer_is_busy(&p));

	/* Just under the threshold stays idle, just over turns busy. */
	pacer_init(&p, now);
	run(&p, &now, 100, 10, PACER_BUSY_EVENTS_PER_SECOND / 100 - 1);
	CHECK(!pacer_is_busy(&p));
	pacer_init(&p, now);
	run(&p, &now, 100, 10, PACER_BUSY_EVENTS_PER_SECOND / 100 + 1);
	CHECK(pacer_is_busy(&p));
	TEST_MAIN_END();
}
