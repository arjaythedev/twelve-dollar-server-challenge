/* WAL checkpoints on a background thread, so their fsyncs never block the event loop.
 *
 * sqlite3_wal_hook on the writing connection (the writer thread's below, or the event loop's) replaces
 * SQLite's automatic checkpoint: after each commit it records the WAL size and wakes this thread once the
 * WAL has reached the threshold. The thread runs PASSIVE checkpoints on its own connection (WAL mode:
 * readers and the writer carry on meanwhile). At a higher mark it also asks the writer to hold writes
 * until the checkpoint has caught up.
 * It never touches Julia, so it is outside Julia's GC and scheduler. Public SQLite API only.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
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
static atomic_int ck_hold;              /* writes are held: read by whoever writes */
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

/* Runs on the writing thread, inside the commit: must stay cheap. */
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
    /* dev A/B only (dev/ct.sh PRAGMAS=): the event loop's extra statements, here too. Some act where the WAL
     * restarts, which is usually this connection (journal_size_limit: the restarting commit truncates). */
    const char *extra = getenv("JR_PRAGMAS");
    if (extra && *extra) sqlite3_exec(ck_db, extra, NULL, NULL, NULL);
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

/* ---- writer thread (io_uring transport; off by default, JR_WRITER=on; PLAN 5.9) ----
 *
 * The event loop validates a POST, appends it to a batch and goes on serving reads; this thread runs one
 * batch at a time on its own connection (one transaction, so group commit comes for free: the batch is
 * everything that arrived while the previous one committed), then writes the eventfd. The loop answers the
 * batch's requests only after that, so no reply claims an uncommitted row. A slow write (a page fault, a WAL
 * hold, the disk) now stalls the writes only, not the reads. The batch memory belongs to the loop (Julia) and
 * is not touched by it while the batch is in flight. Public SQLite API only. */
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>

typedef struct {
    int64_t kind;           /* 1: insert post, 2: insert like */
    int64_t uid, pid;       /* pid: the liked post */
    int64_t off, len;       /* post body: bytes[off : off+len) */
    int64_t id;             /* out: new post id */
    int32_t rc;             /* out: post 1 ok; like 1 new, 2 already liked; -1 not written (error) */
    int32_t tlen;           /* out: created_at */
    char ts[32];
} jr_job;                   /* mirrored by WJob in src/db.jl */

static sqlite3 *w_db;
static sqlite3_stmt *w_post, *w_like, *w_begin, *w_commit, *w_rollback;
static pthread_t w_thread;
static pthread_mutex_t w_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t w_cv = PTHREAD_COND_INITIALIZER;
static jr_job *w_jobs;
static long w_n;
static const char *w_bytes;
static int w_efd = -1;
static atomic_long w_batches, w_jobs_total, w_max_us, w_hold_waits;

static long long now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int w_step_reset(sqlite3_stmt *st) {
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return rc;
}

static void w_run(jr_job *J, long n, const char *B) {
    /* The checkpoint thread holds writes while it catches up (see ck_main): wait here, not in the loop. */
    if (atomic_load(&ck_hold)) {
        atomic_fetch_add(&w_hold_waits, 1);
        while (atomic_load(&ck_hold)) usleep(200);
    }
    long long t0 = now_us();
    int ok = w_step_reset(w_begin) == SQLITE_DONE;
    for (long i = 0; ok && i < n; i++) {
        jr_job *j = &J[i];
        if (j->kind == 1) {
            sqlite3_bind_int64(w_post, 1, j->uid);
            sqlite3_bind_text(w_post, 2, B + j->off, (int)j->len, SQLITE_STATIC);
            if (sqlite3_step(w_post) == SQLITE_ROW) {
                j->id = sqlite3_column_int64(w_post, 0);
                const unsigned char *t = sqlite3_column_text(w_post, 1);
                int tl = sqlite3_column_bytes(w_post, 1);
                if (tl > (int)sizeof j->ts) tl = sizeof j->ts;
                memcpy(j->ts, t, tl);
                j->tlen = tl;
                ok = sqlite3_step(w_post) == SQLITE_DONE;
                j->rc = 1;
            } else ok = 0;
            sqlite3_reset(w_post);
        } else {
            sqlite3_bind_int64(w_like, 1, j->uid);
            sqlite3_bind_int64(w_like, 2, j->pid);
            ok = w_step_reset(w_like) == SQLITE_DONE;
            if (ok) j->rc = sqlite3_changes(w_db) == 1 ? 1 : 2;
        }
    }
    if (ok) ok = w_step_reset(w_commit) == SQLITE_DONE;
    if (!ok) {
        /* Nothing of this batch is committed: every request gets a 500. */
        fprintf(stderr, "writer: batch of %ld failed: %s\n", n, sqlite3_errmsg(w_db));
        if (!sqlite3_get_autocommit(w_db)) w_step_reset(w_rollback);
        for (long i = 0; i < n; i++) J[i].rc = -1;
    }
    long us = (long)(now_us() - t0);
    if (us > atomic_load(&w_max_us)) atomic_store(&w_max_us, us);
    atomic_fetch_add(&w_batches, 1);
    atomic_fetch_add(&w_jobs_total, n);
}

static void *w_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&w_mu);
        while (w_jobs == NULL) pthread_cond_wait(&w_cv, &w_mu);
        jr_job *J = w_jobs; long n = w_n; const char *B = w_bytes;
        pthread_mutex_unlock(&w_mu);
        w_run(J, n, B);
        pthread_mutex_lock(&w_mu);
        w_jobs = NULL;
        pthread_mutex_unlock(&w_mu);
        uint64_t one = 1;
        while (write(w_efd, &one, sizeof one) < 0) usleep(100);
    }
    return NULL;
}

/* Opens the writer's connection and starts the thread. Returns the eventfd that signals a finished batch,
 * or -1. */
int jr_w_start(const char *path) {
    if (sqlite3_open_v2(path, &w_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) return -1;
    sqlite3_exec(w_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(w_db, "PRAGMA cache_size=-16384", NULL, NULL, NULL);
    sqlite3_busy_timeout(w_db, 5000);
    if (sqlite3_prepare_v3(w_db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
                           -1, SQLITE_PREPARE_PERSISTENT, &w_post, NULL) != SQLITE_OK ||
        sqlite3_prepare_v3(w_db, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) "
                           "ON CONFLICT (user_id, post_id) DO NOTHING", -1, SQLITE_PREPARE_PERSISTENT, &w_like, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(w_db, "BEGIN", -1, &w_begin, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(w_db, "COMMIT", -1, &w_commit, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(w_db, "ROLLBACK", -1, &w_rollback, NULL) != SQLITE_OK) {
        fprintf(stderr, "writer: %s\n", sqlite3_errmsg(w_db));
        return -1;
    }
    w_efd = eventfd(0, EFD_CLOEXEC);
    if (w_efd < 0) return -1;
    if (pthread_create(&w_thread, NULL, w_main, NULL) != 0) return -1;
    pthread_detach(w_thread);
    return w_efd;
}

sqlite3 *jr_w_db(void) { return w_db; }

/* Hands a batch to the thread. Only called when the previous batch has signalled the eventfd. */
void jr_w_submit(jr_job *jobs, long n, const char *bytes) {
    pthread_mutex_lock(&w_mu);
    w_n = n; w_bytes = bytes; w_jobs = jobs;
    pthread_cond_signal(&w_cv);
    pthread_mutex_unlock(&w_mu);
}

long jr_w_batches(void) { return atomic_load(&w_batches); }
long jr_w_jobs(void) { return atomic_load(&w_jobs_total); }
long jr_w_hold_waits(void) { return atomic_load(&w_hold_waits); }
long jr_w_take_max_us(void) { return atomic_exchange(&w_max_us, 0); }
