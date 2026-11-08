/* WAL checkpoints on a background thread, so their fsyncs never block the event loop.
 *
 * sqlite3_wal_hook on the main connection replaces SQLite's automatic checkpoint: after each commit it
 * records the WAL size and wakes this thread once the WAL has reached the threshold. The thread runs
 * PASSIVE checkpoints on its own connection (WAL mode: readers and the writer carry on meanwhile).
 * At a higher mark it also asks the event loop to hold writes until the checkpoint has caught up.
 * It never touches Julia, so it is outside Julia's GC and scheduler. Public SQLite API only.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.h"

static sqlite3 *ck_db;
static pthread_t ck_thread;
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv = PTHREAD_COND_INITIALIZER;
static int ck_wanted;
static int ck_threshold;
static int ck_hold_base;                /* 0: never hold */
static atomic_int ck_hold_at;           /* WAL pages at which writes are held next */
static atomic_int ck_hold;              /* writes are held: read by the event loop */
static atomic_int ck_running;           /* a checkpoint is in progress: the hook need not wake the thread */
static atomic_llong ck_hold_since_ms;
static atomic_int ck_hold_pages;        /* WAL pages when the hold began: a checkpoint must cover them all */
static int ck_restart;                  /* restart the WAL on this thread */
static char ck_restart_sql[64];
static atomic_long ck_restarts;
static atomic_long ck_count, ck_holds, ck_hold_timeouts, ck_hold_ms_total, ck_max_pages;

#define HOLD_TIMEOUT_MS 500

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void *ck_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&ck_mu);
        while (!ck_wanted) pthread_cond_wait(&ck_cv, &ck_mu);
        ck_wanted = 0;
        atomic_store(&ck_running, 1);
        pthread_mutex_unlock(&ck_mu);
        for (;;) {
            int log = 0, done = 0;
            int rc = sqlite3_wal_checkpoint_v2(ck_db, "main", SQLITE_CHECKPOINT_PASSIVE, &log, &done);
            atomic_fetch_add(&ck_count, 1);
            if (rc != SQLITE_OK && rc != SQLITE_BUSY)
                fprintf(stderr, "checkpoint failed: %s\n", sqlite3_errmsg(ck_db));
            if (!atomic_load(&ck_hold)) break;
            /* A PASSIVE checkpoint never blocks writers, so under steady writes it never finds every frame
             * copied and the WAL never starts again from the beginning: it would grow without limit. While
             * writes are held no frames are added, so it catches up; the next write then restarts the WAL. */
            long long waited = now_ms() - atomic_load(&ck_hold_since_ms);
            /* A checkpoint that began before the hold's last commit reports a stale, smaller log: it has not
             * copied everything, the next write could not restart the WAL, and the hold would fire again. */
            int caught = rc == SQLITE_OK && done == log && log >= atomic_load(&ck_hold_pages);
            if (caught || waited >= HOLD_TIMEOUT_MS || (rc != SQLITE_OK && rc != SQLITE_BUSY)) {
                if (caught) {
                    atomic_store(&ck_hold_at, ck_hold_base);
                    /* The first frame after the WAL starts again makes SQLite fsync the WAL file, even with
                     * synchronous=NORMAL, and that fsync can wait behind the checkpoint's writeback. Do it here,
                     * not in the event loop's next write: a no-op commit (same user_version), while writes are
                     * held. If a reader still pins old frames it only adds a frame: the loop restarts it later. */
                    if (ck_restart && sqlite3_exec(ck_db, ck_restart_sql, NULL, NULL, NULL) == SQLITE_OK)
                        atomic_fetch_add(&ck_restarts, 1);
                }
                else { atomic_fetch_add(&ck_hold_at, ck_hold_base); atomic_fetch_add(&ck_hold_timeouts, 1); }
                atomic_fetch_add(&ck_hold_ms_total, waited);
                atomic_store(&ck_hold, 0);
                break;
            }
            usleep(1000);               /* a reader pins some frames for a moment */
        }
        atomic_store(&ck_running, 0);
    }
    return NULL;
}

/* Runs on the event-loop thread, inside the commit: must stay cheap. */
static int ck_hook(void *arg, sqlite3 *db, const char *name, int pages) {
    (void)arg; (void)db; (void)name;
    if (pages > atomic_load(&ck_max_pages)) atomic_store(&ck_max_pages, pages);
    int hold = ck_hold_base > 0 && pages >= atomic_load(&ck_hold_at);
    if (hold) {
        atomic_store(&ck_hold_since_ms, now_ms());
        atomic_store(&ck_hold_pages, pages);
        atomic_fetch_add(&ck_holds, 1);
        atomic_store(&ck_hold, 1);
    }
    if (hold || (pages >= ck_threshold && !atomic_load(&ck_running))) {
        pthread_mutex_lock(&ck_mu);
        ck_wanted = 1;
        pthread_cond_signal(&ck_cv);
        pthread_mutex_unlock(&ck_mu);
    }
    return SQLITE_OK;
}

/* Returns 0 on success. main_db keeps being used by the caller's thread only. */
int jr_ckpt_start(sqlite3 *main_db, const char *path, int threshold_pages, int hold_pages, int restart) {
    ck_restart = restart;
    ck_threshold = threshold_pages;
    ck_hold_base = hold_pages;
    atomic_store(&ck_hold_at, hold_pages);
    int rc = sqlite3_open_v2(path, &ck_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_exec(ck_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_busy_timeout(ck_db, 0);
    sqlite3_stmt *st;
    int uv = 0;
    if (sqlite3_prepare_v2(ck_db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) uv = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    snprintf(ck_restart_sql, sizeof ck_restart_sql, "PRAGMA user_version=%d", uv);
    if (pthread_create(&ck_thread, NULL, ck_main, NULL) != 0) return SQLITE_ERROR;
    pthread_detach(ck_thread);
    sqlite3_wal_hook(main_db, ck_hook, NULL);
    return SQLITE_OK;
}

int jr_ckpt_hold(void) { return atomic_load(&ck_hold); }
long jr_ckpt_count(void) { return atomic_load(&ck_count); }
long jr_ckpt_holds(void) { return atomic_load(&ck_holds); }
long jr_ckpt_hold_timeouts(void) { return atomic_load(&ck_hold_timeouts); }
long jr_ckpt_hold_ms(void) { return atomic_load(&ck_hold_ms_total); }
long jr_ckpt_restarts(void) { return atomic_load(&ck_restarts); }
long jr_ckpt_max_pages(void) { return atomic_load(&ck_max_pages); }
