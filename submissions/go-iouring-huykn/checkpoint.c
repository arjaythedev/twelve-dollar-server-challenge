#include "checkpoint.h"
#include "checkpoint_policy.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

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
  int stop, queued, running, error, retry, health;
};

static int64_t checkpoint_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *checkpoint_loop(void *argument) {
  hkn_checkpoint *worker = argument;
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
    worker->queued = 0;
    worker->running = 1;
    pthread_mutex_unlock(&worker->mutex);
    int log = -1, copied = -1;
    int code = sqlite3_wal_checkpoint_v2(worker->connection, "main", SQLITE_CHECKPOINT_PASSIVE, &log, &copied);
    pthread_mutex_lock(&worker->mutex);
    if (code != SQLITE_OK && code != SQLITE_BUSY) worker->error = code;
    worker->retry = code != SQLITE_OK || log < 0 || copied < 0 || copied < log;
    worker->health = code == SQLITE_OK && log >= 0 && copied >= 0 &&
                     log - copied <= HKN_CHECKPOINT_STEP ? 1 : -1;
    worker->running = 0;
    pthread_cond_broadcast(&worker->changed);
  }
  pthread_mutex_unlock(&worker->mutex);
  return NULL;
}

hkn_checkpoint *hkn_checkpoint_new(const char *path, const char *vfs) {
  hkn_checkpoint *worker = calloc(1, sizeof *worker);
  if (!worker) return NULL;
  int code = sqlite3_open_v2(path, &worker->connection,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, vfs);
  if (code == SQLITE_OK) code = sqlite3_exec(worker->connection,
    "PRAGMA synchronous=NORMAL;PRAGMA wal_autocheckpoint=0;PRAGMA cache_size=-2048;", NULL, NULL, NULL);
  if (code != SQLITE_OK) goto connection_error;
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
  if (pthread_create(&worker->thread, NULL, checkpoint_loop, worker)) goto condition_error;
  return worker;
condition_error:
  pthread_cond_destroy(&worker->changed);
mutex_error:
  pthread_mutex_destroy(&worker->mutex);
connection_error:
  sqlite3_close(worker->connection);
  free(worker);
  return NULL;
}

int hkn_checkpoint_request(hkn_checkpoint *worker) {
  pthread_mutex_lock(&worker->mutex);
  worker->queued = 1;
  int code = worker->error;
  pthread_cond_broadcast(&worker->changed);
  pthread_mutex_unlock(&worker->mutex);
  return code;
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
  pthread_cond_destroy(&worker->changed);
  pthread_mutex_destroy(&worker->mutex);
  free(worker);
}
