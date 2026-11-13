#ifndef HKN_DIAGNOSTICS_H
#define HKN_DIAGNOSTICS_H

#ifdef HKN_DIAGNOSTICS
#include "sqlite/sqlite3.h"
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#define HKN_DIAG(...) __VA_ARGS__

enum hkn_diag_metric { HKN_PASS, HKN_COMMIT, HKN_WORKER_WAIT, HKN_RESTART, HKN_PASSIVE, HKN_METRICS };
typedef struct { uint64_t wall, cpu; } hkn_diag_clock;
typedef struct { uint64_t count, wall, cpu, max_wall; } hkn_diag_timing;
// Each instance has exactly one thread owner. No worker counters are read by
// the event loop, and logging never holds the checkpoint mutex.
typedef struct {
  hkn_diag_timing timing[HKN_METRICS];
  hkn_diag_clock previous;
  uint64_t reported, commit_errors, complete, partial, unknown, busy, errors, log_decreases;
  int wal_max, log_last, done_last;
} hkn_diagnostics;

static inline hkn_diag_clock hkn_diag_now(void) {
  struct timespec wall = {0}, cpu = {0};
  clock_gettime(CLOCK_MONOTONIC, &wall);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
  return (hkn_diag_clock){(uint64_t)wall.tv_sec * 1000000000 + wall.tv_nsec,
                         (uint64_t)cpu.tv_sec * 1000000000 + cpu.tv_nsec};
}

static inline void hkn_diag_record(hkn_diagnostics *d, enum hkn_diag_metric metric,
                                   hkn_diag_clock start, hkn_diag_clock end) {
  hkn_diag_timing *t = &d->timing[metric];
  uint64_t wall = end.wall - start.wall;
  ++t->count;
  t->wall += wall;
  t->cpu += end.cpu - start.cpu;
  if (wall > t->max_wall) t->max_wall = wall;
}

static inline void hkn_diag_checkpoint(hkn_diagnostics *d, int code, int log, int done) {
  if (log > d->wal_max) d->wal_max = log;
  if (log >= 0 && d->log_last > log) ++d->log_decreases;
  d->log_last = log;
  d->done_last = done;
  if (code == SQLITE_BUSY) ++d->busy;
  else if (code != SQLITE_OK) ++d->errors;
  if (code != SQLITE_OK && code != SQLITE_BUSY) ++d->unknown;
  else if (log < 0 || done < 0 || done > log) ++d->unknown;
  else if (done < log) ++d->partial;
  else ++d->complete;
}

// Cumulative counters, at most once per minute while active and on clean close.
// One fprintf per line keeps main/worker records intact on a shared stderr.
static inline void hkn_diag_flush(hkn_diagnostics *d, const char *owner,
                                  uint64_t now, int closing) {
  if (!closing && d->reported && now - d->reported < 60000000000ULL) return;
  if (!d->reported && !closing) { d->reported = now; return; }
  d->reported = now;
  static const char *const names[] = {"pass", "commit", "worker_wait", "restart", "passive"};
  for (int i = 0; i < HKN_METRICS; ++i) {
    const hkn_diag_timing *t = &d->timing[i];
    if (!t->count) continue;
    fprintf(stderr, "hkn_diag owner=%s at_ns=%" PRIu64 " metric=%s count=%" PRIu64
      " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64 " max_wall_ns=%" PRIu64 "\n",
      owner, now, names[i], t->count, t->wall, t->cpu, t->max_wall);
  }
  fprintf(stderr, "hkn_diag owner=%s at_ns=%" PRIu64 " wal_max=%d commit_errors=%" PRIu64
    " complete=%" PRIu64 " partial=%" PRIu64 " unknown=%" PRIu64
    " busy=%" PRIu64 " errors=%" PRIu64 " log_last=%d done_last=%d log_decreases=%" PRIu64 "\n",
    owner, now, d->wal_max, d->commit_errors, d->complete, d->partial, d->unknown,
    d->busy, d->errors, d->log_last, d->done_last, d->log_decreases);
}

static inline void hkn_diag_pass(hkn_diagnostics *d) {
  hkn_diag_clock now = hkn_diag_now();
  if (d->previous.wall) hkn_diag_record(d, HKN_PASS, d->previous, now);
  d->previous = now;
  hkn_diag_flush(d, "main", now.wall, 0);
}
#else
#define HKN_DIAG(...)
#endif
#endif
