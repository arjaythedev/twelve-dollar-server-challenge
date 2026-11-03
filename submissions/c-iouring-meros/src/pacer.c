#include "pacer.h"

void pacer_init(pacer *p, uint64_t now_ns)
{
	p->window_start_ns = now_ns;
	p->events_in_window = 0;
	p->busy = false;
}

void pacer_record(pacer *p, uint64_t now_ns, unsigned events)
{
	uint64_t elapsed_ns = now_ns - p->window_start_ns;

	p->events_in_window += events;
	if (elapsed_ns < PACER_MEASURE_NS)
		return;
	/* events/s = events * 1e9 / elapsed, compared without division */
	p->busy = p->events_in_window * 1000000000ULL >= (uint64_t)PACER_BUSY_EVENTS_PER_SECOND * elapsed_ns;
	p->window_start_ns = now_ns;
	p->events_in_window = 0;
}
