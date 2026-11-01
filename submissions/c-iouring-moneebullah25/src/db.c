#include "db.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "db_btree.h"
#include "json.h"
#include "sqlite3.h"
#include "util.h"

#define FEED_LIMIT 20
/* The three-range-scan feed is used when the 20 newest ids span at most this
 * many ids; otherwise the reference query runs. */
#define FEED_SPAN_MAX  256
#define BODY_MAX_BYTES 2000 /* 500 code points of 4-byte UTF-8 */
#define NAME_MAX_BYTES 512

enum stmt_id {
    S_HEALTH,
    S_BEGIN,
    S_BEGIN_IMMEDIATE,
    S_COMMIT,
    S_ROLLBACK,
    S_FEED_IDS,
    S_FEED_LIKES,
    S_FEED_POSTS,
    S_FEED_REF,
    S_POST,
    S_POST_EXISTS,
    S_POST_INSERT,
    S_POST_CREATED,
    S_LIKE_INSERT,
    S_COUNT
};

static const char *const SQL[S_COUNT] = {
    [S_HEALTH] = "SELECT 1",
    [S_BEGIN] = "BEGIN",
    [S_BEGIN_IMMEDIATE] = "BEGIN IMMEDIATE",
    [S_COMMIT] = "COMMIT",
    [S_ROLLBACK] = "ROLLBACK",
    [S_FEED_IDS] = "SELECT id, created_at FROM posts ORDER BY created_at DESC, id DESC LIMIT 20",
    [S_FEED_LIKES] = "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2",
    [S_FEED_POSTS] = "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id "
                     "WHERE p.id BETWEEN ?1 AND ?2",
    [S_FEED_REF] = "SELECT p.id, p.body, p.created_at, u.username, "
                   "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
                   "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
    [S_POST] = "SELECT p.id, p.body, p.created_at, u.username, "
               "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
               "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1",
    [S_POST_EXISTS] = "SELECT 1 FROM posts WHERE id = ?1",
    [S_POST_INSERT] = "INSERT INTO posts (user_id, body) VALUES (?1, ?2)",
    [S_POST_CREATED] = "SELECT created_at FROM posts WHERE id = ?1",
    [S_LIKE_INSERT] = "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
};

struct feed_row {
    int64_t id;
    uint32_t body_len, created_len, author_len;
    int64_t like_count;
    char body[BODY_MAX_BYTES];
    char created[32];
    char author[NAME_MAX_BYTES];
};

struct db {
    sqlite3 *conn;
    sqlite3_stmt *stmt[S_COUNT];
    const struct config *cfg;
    int read_txn_open;
    struct bt *bt;  /* direct B-tree reads, or NULL when DB_READS=sql or the schema differs */
    int bt_enabled; /* db_reads_btree(): tests compare the two paths in one process */

    /* checkpointer */
    sqlite3 *ckpt_conn;
    pthread_t ckpt_thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int ckpt_started;
    int ckpt_stop;
    _Atomic uint32_t wal_frames; /* from the WAL hook, after each commit */
    uint32_t backfilled;         /* frames the last checkpoint copied into the database (checkpointer only) */
    _Atomic int hold;            /* 1 while writes wait for the WAL to be fully backfilled */
    int64_t hold_since_ns;
    uint32_t hold_retry_frames; /* no new hold until the WAL has this many frames */

    struct db_stats stats;
    pthread_mutex_t stats_mu; /* checkpoint fields are written by the checkpointer */
    struct feed_row rows[FEED_LIMIT];
};

static int wal_hook(void *arg, sqlite3 *conn, const char *name, int frames) {
    (void)conn;
    (void)name;
    struct db *db = arg;
    atomic_store_explicit(&db->wal_frames, (uint32_t)frames, memory_order_relaxed);
    return SQLITE_OK;
}

static int exec(sqlite3 *conn, const char *sql, char *err, size_t err_cap) {
    char *msg = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK) {
        snprintf(err, err_cap, "%s: %s", sql, msg ? msg : sqlite3_errstr(rc));
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static int pragma_text_is(sqlite3 *conn, const char *sql, const char *want) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) &&
             strcmp((const char *)sqlite3_column_text(st, 0), want) == 0;
    sqlite3_finalize(st);
    return ok;
}

static sqlite3 *open_conn(const struct config *cfg, int is_ckpt, char *err, size_t err_cap) {
    sqlite3 *conn = NULL;
    /* unix-excl: an exclusive lock against other processes, with the WAL index
     * in heap memory shared by our two connections instead of fcntl locks on
     * every transaction. */
    int rc = sqlite3_open_v2(cfg->db_path, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl");
    if (rc != SQLITE_OK) {
        snprintf(err, err_cap, "open %s: %s", cfg->db_path, conn ? sqlite3_errmsg(conn) : sqlite3_errstr(rc));
        if (conn) sqlite3_close(conn);
        return NULL;
    }
    /* A busy wait on the loop thread would stall every connection; nothing
     * should ever be busy (PASSIVE checkpoints do not take the write lock),
     * so keep it short and let the batch fail with 500s instead. */
    sqlite3_busy_timeout(conn, is_ckpt ? 0 : 50);
    char buf[128];
    snprintf(buf, sizeof buf, "PRAGMA cache_size=-%u", is_ckpt ? 2048u : cfg->sqlite_cache_kib);
    const char *pragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA mmap_size=0",
        "PRAGMA temp_store=MEMORY",
        "PRAGMA wal_autocheckpoint=0",
        "PRAGMA journal_size_limit=67108864",
        buf,
    };
    for (size_t i = 0; i < sizeof pragmas / sizeof pragmas[0]; i++) {
        if (exec(conn, pragmas[i], err, err_cap)) {
            sqlite3_close(conn);
            return NULL;
        }
    }
    if (!pragma_text_is(conn, "PRAGMA journal_mode", "wal")) {
        snprintf(err, err_cap, "journal_mode is not wal");
        sqlite3_close(conn);
        return NULL;
    }
    return conn;
}

struct db *db_open(const struct config *cfg, char *err, size_t err_cap) {
    struct db *db = calloc(1, sizeof *db);
    if (!db) {
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    db->cfg = cfg;
    pthread_mutex_init(&db->mu, NULL);
    pthread_cond_init(&db->cv, NULL);
    pthread_mutex_init(&db->stats_mu, NULL);
    db->conn = open_conn(cfg, 0, err, err_cap);
    if (!db->conn) {
        free(db);
        return NULL;
    }
    for (int i = 0; i < S_COUNT; i++) {
        int rc = sqlite3_prepare_v3(db->conn, SQL[i], -1, SQLITE_PREPARE_PERSISTENT, &db->stmt[i], NULL);
        if (rc != SQLITE_OK) {
            snprintf(err, err_cap, "prepare '%s': %s", SQL[i], sqlite3_errmsg(db->conn));
            db_close(db);
            return NULL;
        }
    }
    sqlite3_wal_hook(db->conn, wal_hook, db);
    db->hold_retry_frames = cfg->hold_frames;
    /* Reads go through the B-tree cursors unless asked otherwise; the SQL
     * statements above stay prepared as the fallback for every case the
     * direct path declines. */
    const char *reads = getenv("DB_READS");
    if (!reads || strcmp(reads, "sql") != 0) {
        char berr[256];
        db->bt = bt_open(db->conn, berr, sizeof berr);
        if (!db->bt) log_msg("%s; reads use SQL", berr);
    }
    db->bt_enabled = db->bt != NULL;
    log_msg("reads: %s", db->bt ? "btree" : "sql");
    return db;
}

void db_close(struct db *db) {
    if (!db) return;
    if (db->ckpt_started) {
        pthread_mutex_lock(&db->mu);
        db->ckpt_stop = 1;
        pthread_cond_signal(&db->cv);
        pthread_mutex_unlock(&db->mu);
        pthread_join(db->ckpt_thread, NULL);
    }
    bt_close(db->bt);
    for (int i = 0; i < S_COUNT; i++) sqlite3_finalize(db->stmt[i]);
    if (db->ckpt_conn) sqlite3_close(db->ckpt_conn);
    if (db->conn) {
        /* leave a clean, fully checkpointed file behind on a normal exit */
        sqlite3_wal_checkpoint_v2(db->conn, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        sqlite3_close(db->conn);
    }
    free(db);
}

/* ------------------------------------------------------------------ checkpointer */

static void *ckpt_main(void *arg) {
    struct db *db = arg;
    /* Low priority, not SCHED_IDLE: the loop thread still wins most of the
     * single CPU, but a loop at 100% would starve an idle-class thread and
     * the WAL could never be backfilled. */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    for (;;) {
        pthread_mutex_lock(&db->mu);
        if (!db->ckpt_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long wait_ns = atomic_load_explicit(&db->hold, memory_order_relaxed) ? 1000000L : 100000000L;
            ts.tv_nsec += wait_ns;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&db->cv, &db->mu, &ts);
        }
        int stop = db->ckpt_stop;
        pthread_mutex_unlock(&db->mu);
        if (stop) break;

        uint32_t frames = atomic_load_explicit(&db->wal_frames, memory_order_relaxed);
        int hold = atomic_load_explicit(&db->hold, memory_order_acquire);
        /* The WAL restarted since the last checkpoint: nothing is backfilled. */
        if (frames < db->backfilled) db->backfilled = 0;
        /* Only frames that are not yet in the database file count; the WAL
         * total stays high until it restarts, which would otherwise trigger a
         * checkpoint (two fsyncs) on every wake-up. */
        if (frames - db->backfilled < db->cfg->ckpt_frames && !hold) continue;

        int64_t t0 = now_ns();
        int n_log = -1, n_ckpt = -1;
        int rc = sqlite3_wal_checkpoint_v2(db->ckpt_conn, NULL, SQLITE_CHECKPOINT_PASSIVE, &n_log, &n_ckpt);
        int64_t dt = now_ns() - t0;
        pthread_mutex_lock(&db->stats_mu);
        db->stats.checkpoints++;
        db->stats.checkpoint_ns_total += (uint64_t)dt;
        if ((uint64_t)dt > db->stats.checkpoint_ns_max) db->stats.checkpoint_ns_max = (uint64_t)dt;
        pthread_mutex_unlock(&db->stats_mu);
        if (rc != SQLITE_OK && rc != SQLITE_BUSY) log_msg("checkpoint: %s", sqlite3_errstr(rc));
        if (rc == SQLITE_OK && n_ckpt >= 0) db->backfilled = (uint32_t)n_ckpt;
        /* Fully backfilled while writes were held: the next write transaction
         * restarts the WAL from frame 1, so the hold can end. */
        if (hold && rc == SQLITE_OK && n_log >= 0 && n_log == n_ckpt) {
            atomic_store_explicit(&db->hold, 0, memory_order_release);
        }
    }
    return NULL;
}

int db_checkpointer_start(struct db *db) {
    char err[256];
    db->ckpt_conn = open_conn(db->cfg, 1, err, sizeof err);
    if (!db->ckpt_conn) {
        log_msg("checkpointer: %s", err);
        return -1;
    }
    /* A connection only attaches to the WAL after its first read. */
    if (exec(db->ckpt_conn, "SELECT count(*) FROM sqlite_master", err, sizeof err)) {
        log_msg("checkpointer: %s", err);
        return -1;
    }
    int rc = pthread_create(&db->ckpt_thread, NULL, ckpt_main, db);
    if (rc) {
        log_msg("checkpointer: pthread_create: %s", strerror(rc));
        return -1;
    }
    db->ckpt_started = 1;
    return 0;
}

static void ckpt_kick(struct db *db) {
    pthread_mutex_lock(&db->mu);
    pthread_cond_signal(&db->cv);
    pthread_mutex_unlock(&db->mu);
}

int db_writes_held(struct db *db) {
    if (!atomic_load_explicit(&db->hold, memory_order_acquire)) return 0;
    int64_t held_ns = now_ns() - db->hold_since_ns;
    if (held_ns > (int64_t)db->cfg->hold_timeout_ms * 1000000) {
        /* The checkpointer could not catch up in time: resume writes and try
         * again once the WAL has grown by another hold_frames. */
        atomic_store_explicit(&db->hold, 0, memory_order_release);
        db->hold_retry_frames = atomic_load_explicit(&db->wal_frames, memory_order_relaxed) + db->cfg->hold_frames;
        db->stats.hold_timeouts++;
        db->stats.hold_ns_total += (uint64_t)held_ns;
        if ((uint64_t)held_ns > db->stats.hold_ns_max) db->stats.hold_ns_max = (uint64_t)held_ns;
        return 0;
    }
    return 1;
}

/* Called after each commit: decides whether to hold writes so the WAL restarts. */
static void after_commit(struct db *db) {
    uint32_t frames = atomic_load_explicit(&db->wal_frames, memory_order_relaxed);
    if (!db->ckpt_started) return;
    if (frames >= db->cfg->ckpt_frames) ckpt_kick(db);
    if (frames >= db->hold_retry_frames && !atomic_load_explicit(&db->hold, memory_order_relaxed)) {
        db->hold_since_ns = now_ns();
        db->hold_retry_frames = frames + db->cfg->hold_frames;
        db->stats.holds++;
        atomic_store_explicit(&db->hold, 1, memory_order_release);
        ckpt_kick(db);
    }
}

/* Accounts a hold that the checkpointer ended. */
static void note_hold_end(struct db *db) {
    static int64_t last_seen_since;
    if (db->hold_since_ns && db->hold_since_ns != last_seen_since &&
        !atomic_load_explicit(&db->hold, memory_order_acquire)) {
        int64_t held_ns = now_ns() - db->hold_since_ns;
        last_seen_since = db->hold_since_ns;
        db->stats.hold_ns_total += (uint64_t)held_ns;
        if ((uint64_t)held_ns > db->stats.hold_ns_max) db->stats.hold_ns_max = (uint64_t)held_ns;
    }
}

/* ------------------------------------------------------------------ statements */

static int step_done(struct db *db, enum stmt_id id) {
    sqlite3_stmt *st = db->stmt[id];
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        log_msg("sqlite: %s: %s", SQL[id], sqlite3_errmsg(db->conn));
        return -1;
    }
    return 0;
}

int db_health(struct db *db, char *err, size_t err_cap) {
    sqlite3_stmt *st = db->stmt[S_HEALTH];
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_ROW) {
        snprintf(err, err_cap, "%s", sqlite3_errmsg(db->conn));
        return -1;
    }
    return 0;
}

void db_read_begin(struct db *db) {
    if (db->cfg->no_batch_read_txn || db->read_txn_open) return;
    if (step_done(db, S_BEGIN) == 0) db->read_txn_open = 1;
}

void db_read_end(struct db *db) {
    bt_read_end(db->bt); /* a snapshot the direct reads opened outside the shared transaction */
    if (!db->read_txn_open) return;
    db->read_txn_open = 0;
    if (step_done(db, S_COMMIT)) step_done(db, S_ROLLBACK);
}

int db_write_begin(struct db *db) {
    db_read_end(db);
    return step_done(db, S_BEGIN_IMMEDIATE);
}

int db_write_commit(struct db *db) {
    if (step_done(db, S_COMMIT)) {
        if (!sqlite3_get_autocommit(db->conn)) step_done(db, S_ROLLBACK);
        return -1;
    }
    after_commit(db);
    note_hold_end(db);
    return 0;
}

void db_write_rollback(struct db *db) {
    if (!sqlite3_get_autocommit(db->conn)) step_done(db, S_ROLLBACK);
}

int db_txn_active(struct db *db) {
    return !sqlite3_get_autocommit(db->conn);
}

size_t db_write_post_json(char *out, size_t cap, int64_t id, const char *body, size_t body_len, const char *created_at,
                          size_t created_len, const char *author, size_t author_len, int64_t like_count) {
    /* {"id":N,"body":S,"created_at":"T","author":S,"like_count":N}; the two
     * strings check their own fit as they are written (0 = did not fit). */
    size_t fixed = 6 + 20 + 8 + 15 + created_len + 11 + 14 + 20 + 1;
    if (cap < fixed) return 0;
    char *o = out;
    memcpy(o, "{\"id\":", 6);
    o += 6;
    o += fmt_i64(o, id);
    memcpy(o, ",\"body\":", 8);
    o += 8;
    size_t w = json_write_string(o, cap - fixed - (size_t)(o - out), body, body_len);
    if (!w) return 0;
    o += w;
    memcpy(o, ",\"created_at\":\"", 15);
    o += 15;
    memcpy(o, created_at, created_len);
    o += created_len;
    memcpy(o, "\",\"author\":", 11);
    o += 11;
    w = json_write_string(o, cap - fixed - (size_t)(o - out), author, author_len);
    if (!w) return 0;
    o += w;
    memcpy(o, ",\"like_count\":", 14);
    o += 14;
    o += fmt_i64(o, like_count);
    *o++ = '}';
    return (size_t)(o - out);
}

/* Writes one row of the reference-shaped statements (id, body, created_at,
 * username, like_count) straight from the column pointers. */
static size_t write_row(sqlite3_stmt *st, char *out, size_t cap) {
    const char *body = (const char *)sqlite3_column_text(st, 1);
    const char *created = (const char *)sqlite3_column_text(st, 2);
    const char *user = (const char *)sqlite3_column_text(st, 3);
    if (!body || !created || !user) return 0;
    return db_write_post_json(out, cap, sqlite3_column_int64(st, 0), body, (size_t)sqlite3_column_bytes(st, 1), created,
                              (size_t)sqlite3_column_bytes(st, 2), user, (size_t)sqlite3_column_bytes(st, 3),
                              sqlite3_column_int64(st, 4));
}

static size_t feed_reference(struct db *db, char *out, size_t cap) {
    sqlite3_stmt *st = db->stmt[S_FEED_REF];
    size_t o = 0;
    int n = 0, rc;
    memcpy(out, "{\"posts\":[", 10);
    o = 10;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n++ && o < cap) out[o++] = ',';
        size_t w = write_row(st, out + o, cap - o);
        if (!w) {
            sqlite3_reset(st);
            return 0;
        }
        o += w;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE || o + 2 > cap) {
        if (rc != SQLITE_DONE) log_msg("sqlite: feed: %s", sqlite3_errmsg(db->conn));
        return 0;
    }
    out[o++] = ']';
    out[o++] = '}';
    return o;
}

/* The feed as three range scans: the 20 newest (created_at, id) from the
 * covering index, then one scan of likes_post_id_idx and one of posts+users
 * over the id span. Falls back to the reference query when the span is wide.
 * Every value still comes from SQLite on this request (rule 5). */
static size_t feed_ranges(struct db *db, char *out, size_t cap) {
    sqlite3_stmt *st = db->stmt[S_FEED_IDS];
    struct feed_row *rows = db->rows;
    int n = 0, rc;
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < FEED_LIMIT) {
        rows[n].id = sqlite3_column_int64(st, 0);
        rows[n].like_count = 0;
        rows[n].body_len = 0;
        if (rows[n].id < lo) lo = rows[n].id;
        if (rows[n].id > hi) hi = rows[n].id;
        n++;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        log_msg("sqlite: feed ids: %s", sqlite3_errmsg(db->conn));
        return 0;
    }
    if (n == 0) {
        memcpy(out, "{\"posts\":[]}", 12);
        return 12;
    }
    if (hi - lo + 1 > FEED_SPAN_MAX) {
        db->stats.feed_fallbacks++;
        return feed_reference(db, out, cap);
    }
    /* map id -> row index for the span */
    int8_t idx[FEED_SPAN_MAX];
    memset(idx, -1, sizeof idx);
    for (int i = 0; i < n; i++) idx[rows[i].id - lo] = (int8_t)i;

    /* One index entry per like, tallied here: cheaper than GROUP BY count(*)
     * (no aggregate, no group boundaries) for the few hundred likes a feed
     * spans. Every like is still read on this request. */
    st = db->stmt[S_FEED_LIKES];
    sqlite3_bind_int64(st, 1, lo);
    sqlite3_bind_int64(st, 2, hi);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t pid = sqlite3_column_int64(st, 0);
        if (pid >= lo && pid <= hi && idx[pid - lo] >= 0) rows[idx[pid - lo]].like_count++;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        log_msg("sqlite: feed likes: %s", sqlite3_errmsg(db->conn));
        return 0;
    }

    st = db->stmt[S_FEED_POSTS];
    sqlite3_bind_int64(st, 1, lo);
    sqlite3_bind_int64(st, 2, hi);
    int found = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t pid = sqlite3_column_int64(st, 0);
        if (pid < lo || pid > hi || idx[pid - lo] < 0) continue;
        struct feed_row *r = &rows[idx[pid - lo]];
        const char *body = (const char *)sqlite3_column_text(st, 1);
        const char *created = (const char *)sqlite3_column_text(st, 2);
        const char *user = (const char *)sqlite3_column_text(st, 3);
        int bl = sqlite3_column_bytes(st, 1), cl = sqlite3_column_bytes(st, 2), ul = sqlite3_column_bytes(st, 3);
        if (!body || !created || !user || bl > BODY_MAX_BYTES || cl > 31 || ul > NAME_MAX_BYTES) {
            sqlite3_reset(st);
            db->stats.feed_fallbacks++;
            return feed_reference(db, out, cap);
        }
        memcpy(r->body, body, (size_t)bl);
        memcpy(r->created, created, (size_t)cl);
        memcpy(r->author, user, (size_t)ul);
        r->body_len = (uint32_t)bl;
        r->created_len = (uint32_t)cl;
        r->author_len = (uint32_t)ul;
        found++;
    }
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        log_msg("sqlite: feed posts: %s", sqlite3_errmsg(db->conn));
        return 0;
    }
    if (found != n) {
        db->stats.feed_fallbacks++;
        return feed_reference(db, out, cap);
    }
    size_t o = 10;
    memcpy(out, "{\"posts\":[", 10);
    for (int i = 0; i < n; i++) {
        if (i && o < cap) out[o++] = ',';
        size_t w = db_write_post_json(out + o, cap - o, rows[i].id, rows[i].body, rows[i].body_len, rows[i].created,
                                      rows[i].created_len, rows[i].author, rows[i].author_len, rows[i].like_count);
        if (!w) return 0;
        o += w;
    }
    if (o + 2 > cap) return 0;
    out[o++] = ']';
    out[o++] = '}';
    return o;
}

size_t db_feed_json(struct db *db, char *out, size_t cap) {
    if (cap < 16) return 0;
    if (db->cfg->feed_reference) return feed_reference(db, out, cap);
    if (db->bt && db->bt_enabled) {
        size_t n = bt_feed_json(db->bt, out, cap);
        if (n) return n;
        db->stats.btree_fallbacks++;
    }
    return feed_ranges(db, out, cap);
}

size_t db_post_json(struct db *db, int64_t id, char *out, size_t cap) {
    if (db->bt && db->bt_enabled && !db->cfg->feed_reference) {
        size_t n = bt_post_json(db->bt, id, out, cap);
        if (n != (size_t)-1) return n;
        db->stats.btree_fallbacks++;
    }
    sqlite3_stmt *st = db->stmt[S_POST];
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        sqlite3_reset(st);
        return 0;
    }
    if (rc != SQLITE_ROW) {
        log_msg("sqlite: post %lld: %s", (long long)id, sqlite3_errmsg(db->conn));
        sqlite3_reset(st);
        return (size_t)-1;
    }
    size_t o = 0;
    if (cap > 10) {
        memcpy(out, "{\"post\":", 8);
        o = 8;
        size_t w = write_row(st, out + o, cap - o);
        o = w ? o + w : 0;
    }
    sqlite3_reset(st);
    if (!o || o + 1 > cap) return (size_t)-1;
    out[o++] = '}';
    return o;
}

int db_create_post(struct db *db, int64_t user_id, const char *body, size_t body_len, int64_t *id_out,
                   char created_at[32]) {
    sqlite3_stmt *st = db->stmt[S_POST_INSERT];
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_bind_text(st, 2, body, (int)body_len, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (rc != SQLITE_DONE) {
        log_msg("sqlite: insert post: %s", sqlite3_errmsg(db->conn));
        return -1;
    }
    int64_t id = sqlite3_last_insert_rowid(db->conn);
    st = db->stmt[S_POST_CREATED];
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    const char *t = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    int tl = t ? sqlite3_column_bytes(st, 0) : 0;
    if (!t || tl <= 0 || tl > 31) {
        sqlite3_reset(st);
        log_msg("sqlite: read back post %lld: %s", (long long)id, sqlite3_errmsg(db->conn));
        return -1;
    }
    memcpy(created_at, t, (size_t)tl);
    created_at[tl] = 0;
    sqlite3_reset(st);
    *id_out = id;
    return 0;
}

int db_like(struct db *db, int64_t user_id, int64_t post_id) {
    sqlite3_stmt *st = db->stmt[S_POST_EXISTS];
    sqlite3_bind_int64(st, 1, post_id);
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_ROW) {
        log_msg("sqlite: post exists: %s", sqlite3_errmsg(db->conn));
        return -1;
    }
    st = db->stmt[S_LIKE_INSERT];
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_bind_int64(st, 2, post_id);
    rc = sqlite3_step(st);
    sqlite3_reset(st);
    if (rc != SQLITE_DONE) {
        log_msg("sqlite: insert like: %s", sqlite3_errmsg(db->conn));
        return -1;
    }
    return sqlite3_changes(db->conn) ? 2 : 1;
}

int db_reads_btree(struct db *db, int on) {
    if (!db->bt) return -1;
    int was = db->bt_enabled;
    db->bt_enabled = on != 0;
    return was;
}

void db_warm(struct db *db) {
    /* Touch what the load touches: the newest posts, their likes and authors,
     * and a few hundred random post lookups. The results are discarded. */
    char *buf = malloc(65536);
    if (!buf) return;
    db_read_begin(db);
    for (int i = 0; i < 20; i++) db_feed_json(db, buf, 65536);
    sqlite3_stmt *st = db->stmt[S_FEED_IDS];
    int64_t top = 0;
    if (sqlite3_step(st) == SQLITE_ROW) top = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    for (int64_t id = top; id > 0 && id > top - 2000; id--) db_post_json(db, id, buf, 65536);
    db_read_end(db);
    free(buf);
}

void db_get_stats(struct db *db, struct db_stats *out) {
    pthread_mutex_lock(&db->stats_mu);
    *out = db->stats;
    pthread_mutex_unlock(&db->stats_mu);
    out->wal_frames = atomic_load_explicit(&db->wal_frames, memory_order_relaxed);
}
