#define HKN_DIAGNOSTICS
#include "../diagnostics.h"
#include <assert.h>
#include <errno.h>

int main(void) {
  hkn_diagnostics d = {0};
  // SQLITE_OK can leave work pending; invalid outputs must not imply success.
  hkn_diag_checkpoint(&d, SQLITE_OK, 100, 40);
  hkn_diag_checkpoint(&d, SQLITE_OK, -1, -1);
  hkn_diag_checkpoint(&d, SQLITE_IOERR, 100, 100);
  hkn_diag_checkpoint(&d, SQLITE_BUSY, 100, 40);
  hkn_diag_checkpoint(&d, SQLITE_OK, 10, 10); // A new WAL generation.
  assert(d.partial == 2 && d.complete == 1 && d.unknown == 2);
  assert(d.busy == 1 && d.errors == 1 && d.wal_max == 100);
  assert(d.log_last == 10 && d.done_last == 10 && d.log_decreases == 1);

  // Sleeping consumes wall time without charging that wait as thread CPU.
  hkn_diag_clock start = hkn_diag_now();
  struct timespec delay = {0, 20000000};
  while (nanosleep(&delay, &delay) && errno == EINTR) {}
  hkn_diag_record(&d, HKN_WORKER_WAIT, start, hkn_diag_now());
  assert(d.timing[HKN_WORKER_WAIT].wall >= 20000000);
  assert(d.timing[HKN_WORKER_WAIT].cpu < d.timing[HKN_WORKER_WAIT].wall);

  // Two reports: one after 60 seconds, then a close; other calls stay silent.
  hkn_diag_flush(&d, "fixture", 1, 0);
  hkn_diag_flush(&d, "fixture", 60000000000ULL, 0);
  hkn_diag_flush(&d, "fixture", 60000000001ULL, 0);
  hkn_diag_flush(&d, "fixture", 60000000002ULL, 0);
  hkn_diag_flush(&d, "fixture", 60000000003ULL, 1);
  return 0;
}
