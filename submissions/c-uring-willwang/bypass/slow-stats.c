#include "slow-stats.h"
#include "stack.h"
#include <inttypes.h>

void stack_calendar_original_tail_report(FILE *out);

/* main dispatches SIGUSR1 on the event loop. Keep its existing PCB report. */
void stack_tail_report(FILE *out) {
    struct tcp_slow_work_stats s;
    if (!out)
        return;
    tcp_slow_work_snapshot(&s);
    fprintf(out, "lwip slow ticks=%" PRIu64 " sparse=%" PRIu64 " dense=%" PRIu64
            " wrap_refresh=%" PRIu64 " callback_fallback=%" PRIu64 " callback_sync=%" PRIu64
            " due_total=%" PRIu64 " active_total=%" PRIu64 " max_due=%" PRIu64
            " max_active=%" PRIu64 " active_visits=%" PRIu64 " sync_visits=%" PRIu64
            " timewait_visits=%" PRIu64 " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64
            " max_wall_ns=%" PRIu64 " max_cpu_ns=%" PRIu64 "\n",
            s.ticks, s.sparse, s.dense, s.wrap_refresh, s.callback_fallback, s.callback_sync,
            s.due_total, s.active_total, s.max_due, s.max_active, s.active_visits,
            s.callback_sync_visits, s.timewait_visits, s.wall_ns, s.cpu_ns,
            s.max_wall_ns, s.max_cpu_ns);
    stack_calendar_original_tail_report(out);
}
