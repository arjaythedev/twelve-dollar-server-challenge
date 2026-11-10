#define _GNU_SOURCE
#include "checkpoint.h"
#include "checkpoint_policy.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

// One coalescing work slot per database handle. Only this thread touches the
// checkpoint connection; it never borrows the event-loop connection or records.
struct hkn_checkpoint {
  sqlite3 *connection;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  hkn_checkpoint_policy policy;
  _Atomic uint64_t written;
  _Atomic unsigned schedule;
  _Atomic int result;
  int sync_fd, stop, queued, running, error, retry, health;
};

static int64_t checkpoint_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int checkpoint_sync(hkn_checkpoint *worker) {
  int rc;
  do { rc = fdatasync(worker->sync_fd); } while (rc && errno == EINTR);
  return rc ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

// Flush the bulk while writes continue, so the held tail does not sync a whole
// cycle's dirty pages. SQLite still owns checkpoint progress and WAL resets.
static int checkpoint_copy(hkn_checkpoint *worker, int bulk, int *log, int *copied) {
  int code = SQLITE_OK;
  for (int i = 0; i < (bulk ? 4 : 1); ++i) {
    int previous = *copied;
    code = sqlite3_wal_checkpoint_v2(worker->connection, "main", SQLITE_CHECKPOINT_PASSIVE, log, copied);
    if (code != SQLITE_OK || *log - *copied <= 128 || *copied == previous) break;
  }
  if (bulk && code == SQLITE_OK) {
    code = checkpoint_sync(worker);
    if (code == SQLITE_OK) code = sqlite3_wal_checkpoint_v2(worker->connection, "main", SQLITE_CHECKPOINT_PASSIVE, log, copied);
    if (code == SQLITE_OK) code = checkpoint_sync(worker);
  }
  return code;
}

static void *checkpoint_loop(void *argument) {
  hkn_checkpoint *worker = argument;
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
  pthread_mutex_lock(&worker->mutex);
  for (;;) {
    if (worker->stop) break;
    int64_t now = checkpoint_now();
    hkn_checkpoint_policy_sample(&worker->policy, now,
      atomic_load_explicit(&worker->written, memory_order_relaxed), worker->health);
    atomic_store_explicit(&worker->schedule, worker->policy.target | worker->retry, memory_order_relaxed);
    if (!worker->queued) {
      struct timespec until = {.tv_sec = now / 1000000000 + 1, .tv_nsec = now % 1000000000};
      pthread_cond_timedwait(&worker->changed, &worker->mutex, &until);
      continue;
    }
    int bulk = worker->queued == 1;
    worker->queued = 0;
    worker->running = 1;
    pthread_mutex_unlock(&worker->mutex);
    int log = -1, copied = -1;
    int code = checkpoint_copy(worker, bulk, &log, &copied);
    pthread_mutex_lock(&worker->mutex);
    if (code != SQLITE_OK && code != SQLITE_BUSY) worker->error = code;
    worker->retry = code != SQLITE_OK || log < 0 || copied < 0 || copied < log;
    worker->health = code == SQLITE_OK && log >= 0 && copied >= 0 &&
                     log - copied <= HKN_CHECKPOINT_STEP ? 1 : -1;
    worker->running = 0;
    atomic_store_explicit(&worker->result, worker->error ? -worker->error :
      log >= 0 && copied == log && code == SQLITE_OK ? 2 : 1, memory_order_release);
    pthread_cond_broadcast(&worker->changed);
  }
  pthread_mutex_unlock(&worker->mutex);
  return NULL;
}

hkn_checkpoint *hkn_checkpoint_new(const char *path, const char *vfs) {
  hkn_checkpoint *worker = calloc(1, sizeof *worker);
  if (!worker) return NULL;
  worker->sync_fd = -1;
  int code = sqlite3_open_v2(path, &worker->connection,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, vfs);
  if (code == SQLITE_OK) code = sqlite3_exec(worker->connection,
    "PRAGMA synchronous=NORMAL;PRAGMA wal_autocheckpoint=0;PRAGMA cache_size=-2048;", NULL, NULL, NULL);
  if (code != SQLITE_OK) goto connection_error;
  int chunk = 64 << 20;
  if (sqlite3_file_control(worker->connection, "main", SQLITE_FCNTL_CHUNK_SIZE, &chunk) != SQLITE_OK) goto connection_error;
  worker->sync_fd = open(path, O_RDONLY | O_CLOEXEC);
  if (worker->sync_fd < 0) goto connection_error;
  // Opening the file does not necessarily initialize its WAL. Prime a read
  // before handing ownership to the worker thread so its first checkpoint runs.
  code = sqlite3_exec(worker->connection, "SELECT count(*) FROM sqlite_schema", NULL, NULL, NULL);
  if (code != SQLITE_OK) goto connection_error;
  if (pthread_mutex_init(&worker->mutex, NULL)) goto connection_error;
  pthread_condattr_t clock;
  if (pthread_condattr_init(&clock)) goto mutex_error;
  int condition = pthread_condattr_setclock(&clock, CLOCK_MONOTONIC);
  if (!condition) condition = pthread_cond_init(&worker->changed, &clock);
  pthread_condattr_destroy(&clock);
  if (condition) goto mutex_error;
  hkn_checkpoint_policy_init(&worker->policy, checkpoint_now());
  atomic_init(&worker->written, 0);
  atomic_init(&worker->schedule, HKN_CHECKPOINT_MIN);
  atomic_init(&worker->result, 0);
  if (pthread_create(&worker->thread, NULL, checkpoint_loop, worker)) goto condition_error;
  return worker;
condition_error:
  pthread_cond_destroy(&worker->changed);
mutex_error:
  pthread_mutex_destroy(&worker->mutex);
connection_error:
  if (worker->sync_fd >= 0) close(worker->sync_fd);
  sqlite3_close(worker->connection);
  free(worker);
  return NULL;
}

int hkn_checkpoint_request(hkn_checkpoint *worker) {
  pthread_mutex_lock(&worker->mutex);
  worker->queued = 1;
  atomic_store_explicit(&worker->result, 0, memory_order_relaxed);
  int code = worker->error;
  pthread_cond_broadcast(&worker->changed);
  pthread_mutex_unlock(&worker->mutex);
  return code;
}

int hkn_checkpoint_tail(hkn_checkpoint *worker) {
  pthread_mutex_lock(&worker->mutex);
  worker->queued = 2;
  atomic_store_explicit(&worker->result, 0, memory_order_relaxed);
  int code = worker->error;
  pthread_cond_signal(&worker->changed);
  pthread_mutex_unlock(&worker->mutex);
  return code;
}

int hkn_checkpoint_result(hkn_checkpoint *worker) {
  return atomic_load_explicit(&worker->result, memory_order_acquire);
}

int hkn_checkpoint_wait(hkn_checkpoint *worker) {
  pthread_mutex_lock(&worker->mutex);
  while (worker->queued || worker->running) pthread_cond_wait(&worker->changed, &worker->mutex);
  // The caller may restart the WAL while quiescent. Old progress is no longer
  // evidence for the next WAL cycle; sampling alone never starts checkpoint I/O.
  worker->retry = worker->health = 0;
  atomic_store_explicit(&worker->schedule, worker->policy.target, memory_order_relaxed);
  int code = worker->error;
  pthread_mutex_unlock(&worker->mutex);
  return code;
}

void hkn_checkpoint_observe(hkn_checkpoint *worker, uint64_t written) {
  atomic_store_explicit(&worker->written, written, memory_order_relaxed);
}

unsigned hkn_checkpoint_interval(hkn_checkpoint *worker) {
  unsigned schedule = atomic_load_explicit(&worker->schedule, memory_order_relaxed);
  return schedule & HKN_CHECKPOINT_RETRY ? HKN_CHECKPOINT_STEP : schedule;
}

void hkn_checkpoint_free(hkn_checkpoint *worker) {
  if (!worker) return;
  pthread_mutex_lock(&worker->mutex);
  worker->stop = 1;
  pthread_cond_broadcast(&worker->changed);
  pthread_mutex_unlock(&worker->mutex);
  pthread_join(worker->thread, NULL);
  sqlite3_close(worker->connection);
  close(worker->sync_fd);
  pthread_cond_destroy(&worker->changed);
  pthread_mutex_destroy(&worker->mutex);
  free(worker);
}
