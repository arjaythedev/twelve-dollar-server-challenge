#include "challenge.h"
#include "sqlite3.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

const char *wal_vfs_register(void);
int wal_commit_step(sqlite3_stmt *stmt);
int reader_init(sqlite3 *db);
int reader_feed(Buf *out);
int reader_post(int64_t id, Buf *out);
void reader_close(void);

static sqlite3 *db;
static const char *vfs_name;
static sqlite3_stmt *begin_tx, *commit_tx, *rollback_tx, *health;
static sqlite3_stmt *insert_post, *insert_like, *post_exists;
static int active, broken;

/* The worker uses another connection. Only the event-loop thread uses db. */
enum Phase { IDLE, BULK, REQUEST_GAP, GAP };
static struct {
    sqlite3 *db;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    atomic_int phase, last_frames;
    int running, stop;
} ck = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static uint64_t hold_until, next_write;
static unsigned committed_rows, window_ms = 5;

static uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static struct timespec deadline_ms(int ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_nsec += (long)ms * 1000000;
    if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    return t;
}

static void *checkpoint_main(void *arg) {
    (void)arg;
    /* On Linux nice is per thread. Give the network loop first use of a busy CPU. */
    setpriority(PRIO_PROCESS, 0, 5);
    pthread_mutex_lock(&ck.mutex);
    while (!ck.stop) {
        while (!ck.stop && atomic_load(&ck.phase) == IDLE)
            pthread_cond_wait(&ck.changed, &ck.mutex);
        if (ck.stop) break;
        pthread_mutex_unlock(&ck.mutex);
        int total = -1, copied = -1;
        int rc = sqlite3_wal_checkpoint_v2(ck.db, "main", SQLITE_CHECKPOINT_PASSIVE,
                                           &total, &copied);
        pthread_mutex_lock(&ck.mutex);
        if (!ck.stop && rc == SQLITE_OK && total >= 0 &&
            (total > copied || atomic_load(&ck.last_frames) > copied)) {
            /* Give the last few frames a quiet boundary. Under continuous writes
             * an otherwise successful PASSIVE can keep chasing the end forever. */
            atomic_store(&ck.phase, REQUEST_GAP);
            struct timespec until = deadline_ms(12);
            while (!ck.stop && atomic_load(&ck.phase) == REQUEST_GAP)
                if (pthread_cond_timedwait(&ck.changed, &ck.mutex, &until)) break;
            atomic_store(&ck.phase, GAP);
            pthread_mutex_unlock(&ck.mutex);
            sqlite3_wal_checkpoint_v2(ck.db, "main", SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
            pthread_mutex_lock(&ck.mutex);
        }
        atomic_store(&ck.phase, IDLE);
        pthread_cond_broadcast(&ck.changed);
    }
    pthread_mutex_unlock(&ck.mutex);
    return NULL;
}

static int wake_checkpoint(void *arg, sqlite3 *conn, const char *name, int pages) {
    (void)arg; (void)conn; (void)name;
    /* The first write after an idle period runs immediately. Writes arriving
     * close together can share one subsequent WAL transaction; reads proceed. */
    uint64_t now = monotonic_ns();
    /* Grow the window only when a transaction actually collected edits.
     * A sparse or idle writer returns immediately to the short window. */
    if (committed_rows <= 2 || now > next_write + 50000000)
        window_ms = 5;
    else if (committed_rows >= 8 && window_ms < 32) {
        window_ms *= 2;
        if (window_ms > 32) window_ms = 32;
    }
    next_write = now + (uint64_t)window_ms * 1000000;
    atomic_store_explicit(&ck.last_frames, pages, memory_order_relaxed);
    if (pages < 16384 || atomic_load_explicit(&ck.phase, memory_order_relaxed) != IDLE)
        return SQLITE_OK;
    pthread_mutex_lock(&ck.mutex);
    if (atomic_load(&ck.phase) == IDLE) {
        atomic_store(&ck.phase, BULK);
        pthread_cond_signal(&ck.changed);
    }
    pthread_mutex_unlock(&ck.mutex);
    return SQLITE_OK;
}

static int checkpoint_start(const char *path) {
    if (sqlite3_open_v2(path, &ck.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs_name)
        != SQLITE_OK) return -1;
    if (sqlite3_exec(ck.db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;"
                     "PRAGMA cache_size=16; SELECT rootpage FROM sqlite_schema LIMIT 1",
                     NULL, NULL, NULL) != SQLITE_OK) return -1;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) return -1;
    int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc) rc = pthread_cond_init(&ck.changed, &attr);
    pthread_condattr_destroy(&attr);
    if (rc || pthread_create(&ck.thread, NULL, checkpoint_main, NULL)) return -1;
    ck.running = 1;
    sqlite3_wal_hook(db, wake_checkpoint, NULL);
    return 0;
}

int db_open(const char *path) {
    vfs_name = wal_vfs_register();
    if (!vfs_name) {
        fputs("could not register the SQLite WAL VFS\n", stderr);
        return -1;
    }
    /* A freshly copied database can fill the file cache before serving a
     * request. Discard its clean pages once, then let reads warm the working
     * set. Close this fd before SQLite acquires POSIX locks on the inode. */
    int backing_file = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (backing_file >= 0) {
        (void)posix_fadvise(backing_file, 0, 0, POSIX_FADV_DONTNEED);
        close(backing_file);
    }
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, vfs_name);
    if (rc == SQLITE_OK) {
        sqlite3_stmt *mode = NULL;
        rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL", -1, &mode, NULL);
        if (rc == SQLITE_OK && (sqlite3_step(mode) != SQLITE_ROW ||
            sqlite3_stricmp((const char *)sqlite3_column_text(mode, 0), "wal"))) rc = SQLITE_ERROR;
        sqlite3_finalize(mode);
    }
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;"
                              "PRAGMA cache_size=-65536; PRAGMA mmap_size=0;"
                              "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    struct Statement { sqlite3_stmt **dst; const char *sql; };
    const struct Statement stmts[] = {
        {&begin_tx, "BEGIN"}, {&commit_tx, "COMMIT"}, {&rollback_tx, "ROLLBACK"},
        {&health, "SELECT 1"},
        {&insert_post, "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at"},
        {&insert_like, "INSERT INTO likes(user_id,post_id) SELECT ?1,id FROM posts WHERE id=?2 "
                       "ON CONFLICT(user_id,post_id) DO NOTHING"},
        {&post_exists, "SELECT 1 FROM posts WHERE id=?1"}
    };
    for (size_t i = 0; rc == SQLITE_OK && i < sizeof(stmts)/sizeof(*stmts); i++)
        rc = sqlite3_prepare_v3(db, stmts[i].sql, -1, SQLITE_PREPARE_PERSISTENT, stmts[i].dst, NULL);
    if (rc == SQLITE_OK) rc = reader_init(db);
    if (rc != SQLITE_OK || checkpoint_start(path)) {
        fprintf(stderr, "database initialization: %s (%d)\n", db_error(), rc);
        return -1;
    }
    return 0;
}

void db_close(void) {
    if (ck.running) {
        pthread_mutex_lock(&ck.mutex);
        ck.stop = 1;
        pthread_cond_broadcast(&ck.changed);
        pthread_mutex_unlock(&ck.mutex);
        pthread_join(ck.thread, NULL);
        pthread_cond_destroy(&ck.changed);
    }
    sqlite3_close(ck.db);
    reader_close();
    sqlite3_finalize(begin_tx); sqlite3_finalize(commit_tx); sqlite3_finalize(rollback_tx);
    sqlite3_finalize(health); sqlite3_finalize(insert_post); sqlite3_finalize(insert_like);
    sqlite3_finalize(post_exists);
    sqlite3_close(db);
}

const char *db_error(void) {
    return db ? sqlite3_errmsg(db) : "could not open database";
}

static int step_and_reset(sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    return rc;
}

void db_begin_pass(void) {
    /* Only the loop acknowledges a gap, and only between complete transactions.
     * Readers keep serving their current snapshots while the worker catches up. */
    if (atomic_load_explicit(&ck.phase, memory_order_acquire) != REQUEST_GAP) return;
    pthread_mutex_lock(&ck.mutex);
    if (atomic_load(&ck.phase) == REQUEST_GAP) {
        int pages = atomic_load_explicit(&ck.last_frames, memory_order_relaxed);
        hold_until = monotonic_ns() + (pages >= 32768 ? 100000000u : 25000000u);
        atomic_store(&ck.phase, GAP);
        pthread_cond_signal(&ck.changed);
    }
    pthread_mutex_unlock(&ck.mutex);
}

int db_writes_deferred(void) {
    uint64_t now = monotonic_ns();
    return now < next_write ||
           (atomic_load_explicit(&ck.phase, memory_order_acquire) == GAP && now < hold_until);
}

static int begin_request(void) {
    if (active && sqlite3_get_autocommit(db)) broken = 1;
    if (broken) return -1;
    if (active) return 0;
    active = step_and_reset(begin_tx) == SQLITE_DONE;
    return active ? 0 : -1;
}

int db_pass_read_only(void) {
    return !broken && (!active || (!sqlite3_get_autocommit(db) &&
                                   sqlite3_txn_state(db, "main") != SQLITE_TXN_WRITE));
}

int db_commit_pass(void) {
    int failed = 0;
    if (active) {
        failed = broken || sqlite3_get_autocommit(db);
        if (!failed) {
            failed = wal_commit_step(commit_tx) != SQLITE_DONE;
            sqlite3_reset(commit_tx);
        }
        if (failed && !sqlite3_get_autocommit(db)) step_and_reset(rollback_tx);
    }
    committed_rows = 0;
    active = broken = 0;
    return failed ? -1 : 0;
}

int db_health(void) {
    return !begin_request() && step_and_reset(health) == SQLITE_ROW ? 0 : -1;
}

int db_feed(Buf *body) {
    return !begin_request() && reader_feed(body) == SQLITE_OK ? 200 : 500;
}

int db_post(int64_t id, Buf *body) {
    if (begin_request()) return 500;
    int rc = reader_post(id, body);
    return rc == SQLITE_OK ? 200 : rc == SQLITE_NOTFOUND ? 404 : 500;
}

#define LIT(buf, s) buf_append((buf), (s), sizeof(s) - 1)

int db_create(int64_t uid, const char *text, size_t text_len,
              const char *author, size_t author_len, Buf *body) {
    if (begin_request() || text_len > INT_MAX) return 500;
    int rc = sqlite3_bind_int64(insert_post, 1, uid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(insert_post, 2, text, (int)text_len, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(insert_post);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(insert_post, 0);
        const char *stamp = (const char *)sqlite3_column_text(insert_post, 1);
        int n = sqlite3_column_bytes(insert_post, 1);
        if (!stamp || LIT(body, "{\"post\":{\"id\":") || buf_int(body, id) ||
            LIT(body, ",\"body\":") || buf_json_string(body, text, text_len) ||
            LIT(body, ",\"created_at\":") || buf_json_string(body, stamp, (size_t)n) ||
            LIT(body, ",\"author\":") || buf_json_string(body, author, author_len) ||
            LIT(body, ",\"like_count\":0}}")) {
            /* The DML has executed and the response couldn't be constructed. */
            broken = 1;
            rc = SQLITE_NOMEM;
        } else rc = sqlite3_step(insert_post);
    }
    sqlite3_reset(insert_post);
    sqlite3_clear_bindings(insert_post);
    if (rc != SQLITE_DONE) return 500;
    ++committed_rows;
    return 201;
}

int db_like(int64_t uid, int64_t id) {
    if (begin_request()) return 500;
    if (sqlite3_bind_int64(insert_like, 1, uid) != SQLITE_OK ||
        sqlite3_bind_int64(insert_like, 2, id) != SQLITE_OK ||
        step_and_reset(insert_like) != SQLITE_DONE) return 500;
    if (sqlite3_changes(db)) {
        ++committed_rows;
        return 201;
    }
    if (sqlite3_bind_int64(post_exists, 1, id) != SQLITE_OK) return 500;
    int rc = step_and_reset(post_exists);
    return rc == SQLITE_ROW ? 200 : rc == SQLITE_DONE ? 404 : 500;
}
