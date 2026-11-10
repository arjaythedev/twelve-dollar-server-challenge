#define _GNU_SOURCE
#include "checkpoint.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
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
  _Atomic int result;
  int sync_fd, stop, queued, running, error;
};

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
    while (!worker->queued && !worker->stop) pthread_cond_wait(&worker->changed, &worker->mutex);
    if (worker->stop) break;
    int bulk = worker->queued == 1;
    worker->queued = 0;
    worker->running = 1;
    pthread_mutex_unlock(&worker->mutex);
    int log = -1, copied = -1;
    int code = checkpoint_copy(worker, bulk, &log, &copied);
    pthread_mutex_lock(&worker->mutex);
    if (code != SQLITE_OK && code != SQLITE_BUSY) worker->error = code;
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
  if (pthread_cond_init(&worker->changed, NULL)) goto mutex_error;
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
  int code = worker->error;
  pthread_mutex_unlock(&worker->mutex);
  return code;
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
