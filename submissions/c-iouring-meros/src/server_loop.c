#include "server_loop.h"

#include <stdio.h>
#include <time.h>

#include "session.h"

volatile sig_atomic_t server_stop_requested;

uint64_t server_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void queue_for_resume(void *context, int id)
{
	id_queue_push(&((app *)context)->to_resume, id);
}

static void run_batch(app *a)
{
	batch_run(a->pending, a->db, &a->conns, a->scratch, sizeof a->scratch, queue_for_resume, a);
}

void server_settle(app *a)
{
	int id;

	for (;;) {
		while (batch_has_room(a->pending) && id_queue_pop(&a->to_resume, &id))
			session_resume(a, id);
		if (batch_is_empty(a->pending))
			return;
		run_batch(a);
	}
}

void server_ensure_batch_room(app *a)
{
	if (!batch_has_room(a->pending))
		server_settle(a);
}

void server_report_slow_round(const app *a, int events, uint64_t events_done_ns, uint64_t settled_ns, uint64_t end_ns)
{
	enum { SLOW_ROUND_NS = 100 * 1000000, MAX_LINES_PER_SECOND = 4 };
	static time_t second;
	static int lines;

	if (end_ns - a->now_ns < SLOW_ROUND_NS)
		return;
	if (a->now != second) {
		second = a->now;
		lines = 0;
	}
	if (lines++ >= MAX_LINES_PER_SECOND)
		return;
	fprintf(stderr, "slow round: %.1f ms (%d events %.1f ms, batch %.1f ms, checkpoint %.1f ms)\n",
	        (double)(end_ns - a->now_ns) / 1e6, events, (double)(events_done_ns - a->now_ns) / 1e6,
	        (double)(settled_ns - events_done_ns) / 1e6, (double)(end_ns - settled_ns) / 1e6);
}

void server_sweep_idle_if_due(app *a, time_t *last_sweep)
{
	if (a->now - *last_sweep < SWEEP_INTERVAL_SECONDS)
		return;
	conn_close_idle(&a->conns, a->now, IDLE_TIMEOUT_SECONDS);
	*last_sweep = a->now;
}

void server_relieve_memory_if_critical(app *a)
{
	enum { EVICTIONS_PER_SECOND = 4096, MIN_IDLE_SECONDS = 1 };
	static time_t last;

	if (a->memory.level != MEMORY_CRITICAL || a->now == last)
		return;
	last = a->now;
	unsigned evicted = conn_evict_idle(&a->conns, a->now, MIN_IDLE_SECONDS, EVICTIONS_PER_SECOND);

	if (evicted != 0)
		fprintf(stderr, "memwatch: critical, closed %u idle connections\n", evicted);
}
