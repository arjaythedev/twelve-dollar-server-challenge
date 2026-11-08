#include "checkpoint.h"
#include <pthread.h>
#include <stdlib.h>

// One coalescing work slot per database handle. Only this thread touches the
// checkpoint connection; it never borrows the event-loop connection or records.
struct hkn_checkpoint {
  sqlite3 *connection;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int stop, queued, running, error;
};

static void *checkpoint_loop(void *argument) {
  hkn_checkpoint *worker = argument;
  pthread_mutex_lock(&worker->mutex);
  for (;;) {
    while (!worker->queued && !worker->stop) pthread_cond_wait(&worker->changed, &worker->mutex);
    if (worker->stop) break;
    worker->queued = 0;
    worker->running = 1;
    pthread_mutex_unlock(&worker->mutex);
    int code = sqlite3_wal_checkpoint_v2(worker->connection, "main", SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    pthread_mutex_lock(&worker->mutex);
    if (code != SQLITE_OK && code != SQLITE_BUSY) worker->error = code;
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
  if (pthread_cond_init(&worker->changed, NULL)) goto mutex_error;
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
  pthread_cond_destroy(&worker->changed);
  pthread_mutex_destroy(&worker->mutex);
  free(worker);
}
