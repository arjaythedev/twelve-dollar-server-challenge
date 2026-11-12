#ifndef CHALLENGE_LWIP_SLOW_STATS_H
#define CHALLENGE_LWIP_SLOW_STATS_H
#include <stdint.h>

struct tcp_slow_work_stats {
    uint64_t ticks, sparse, dense, wrap_refresh, callback_fallback, callback_sync;
    uint64_t due_total, active_total, max_due, max_active;
    uint64_t active_visits, callback_sync_visits, timewait_visits;
    uint64_t wall_ns, cpu_ns, max_wall_ns, max_cpu_ns;
};

/* Read only on the same NO_SYS issuer that owns the PCB lists. */
void tcp_slow_work_snapshot(struct tcp_slow_work_stats *out);
#endif
