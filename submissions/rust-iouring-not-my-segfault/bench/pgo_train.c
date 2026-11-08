/* PGO training driver for twelve-loop's C unit. Why C: build.sh compiles it in the same build and
 * links it with the instrumented src/sqlite_unit.c object, so its run trains SQLite and the readers through
 * the unit's own C entry points.
 * Owns: training.db in build.sh's private training directory, created here and removed at the end, and the
 * one object trainer (the connection, the statements, the reader buffers, the flags and the counters).
 * Threading: main's thread, the only one.
 * Between steps: every statement is reset and its bindings cleared before its next use, and every
 * transaction ends before the next phase begins. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sqlite3.h"

struct twelve_buf {
    uint8_t *ptr;
    size_t cap;
    size_t len;
    size_t need;
};
int twelve_readers_open(sqlite3 *db);
void twelve_readers_close(void);
int twelve_feed(sqlite3 *db, struct twelve_buf *out, struct twelve_buf *scratch);
int twelve_post(sqlite3 *db, sqlite3_int64 id, struct twelve_buf *out);
int twelve_reader_reason(void);

enum {
    FEED_REF, POST, FEED_IDS, RANGE_LIKES, RANGE_ROWS, RANGE_DESC, HEALTH,
    CREATE, CREATED, LIKE, STMT_EXISTS, BEGIN_READ, BEGIN_WRITE, COMMIT, ROLLBACK,
    PERSISTENT_COUNT
};
static const char *const release_sql[PERSISTENT_COUNT] = {
    [FEED_REF] = "SELECT posts.id, posts.body, posts.created_at, users.username, (SELECT COUNT(*) FROM likes WHERE likes.post_id = posts.id) AS like_count FROM posts INNER JOIN users ON users.id = posts.user_id ORDER BY posts.created_at DESC, posts.id DESC LIMIT 20;",
    [POST] = "SELECT posts.id, posts.body, posts.created_at, users.username, (SELECT COUNT(*) FROM likes WHERE likes.post_id = posts.id) AS like_count FROM posts INNER JOIN users ON users.id = posts.user_id WHERE posts.id = ?1;",
    [FEED_IDS] = "SELECT posts.id FROM posts ORDER BY posts.created_at DESC, posts.id DESC LIMIT 20;",
    [RANGE_LIKES] = "SELECT likes.post_id FROM likes WHERE likes.post_id BETWEEN ?1 AND ?2;",
    [RANGE_ROWS] = "SELECT posts.id, posts.body, posts.created_at, users.username FROM posts INNER JOIN users ON users.id = posts.user_id WHERE posts.id BETWEEN ?1 AND ?2;",
    [RANGE_DESC] = "SELECT posts.id, posts.body, posts.created_at, users.username FROM posts INNER JOIN users ON users.id = posts.user_id WHERE posts.id BETWEEN ?1 AND ?2 ORDER BY posts.id DESC;",
    [HEALTH] = "SELECT 1;",
    [CREATE] = "INSERT INTO posts (user_id, body) VALUES (?1, ?2);",
    [CREATED] = "SELECT posts.created_at FROM posts WHERE posts.id = ?1;",
    [LIKE] = "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE posts.id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING;",
    [STMT_EXISTS] = "SELECT 1 FROM posts WHERE posts.id = ?1;",
    [BEGIN_READ] = "BEGIN;",
    [BEGIN_WRITE] = "BEGIN IMMEDIATE;",
    [COMMIT] = "COMMIT;",
    [ROLLBACK] = "ROLLBACK;",
};
static const char *const startup_sql[] = {
    [0] = "PRAGMA journal_mode = WAL;",
    [1] = "PRAGMA synchronous = NORMAL;",
    [2] = "PRAGMA foreign_keys = ON;",
    [3] = "PRAGMA wal_autocheckpoint = 0;",
    [4] = "PRAGMA journal_size_limit = 41943040;",
    [5] = "PRAGMA busy_timeout = 0;",
    [6] = "PRAGMA trusted_schema = OFF;",
    [7] = "PRAGMA cache_size = -32768;",
    [8] = "PRAGMA mmap_size = 0;",
    [9] = "PRAGMA foreign_keys;",
    [10] = "PRAGMA synchronous;",
    [11] = "PRAGMA wal_autocheckpoint;",
    [12] = "PRAGMA journal_size_limit;",
    [13] = "PRAGMA busy_timeout;",
    [14] = "PRAGMA trusted_schema;",
    [15] = "PRAGMA cache_size;",
    [16] = "PRAGMA mmap_size;",
};
/* The values startup_sql[9] to startup_sql[16] read back. */
static const sqlite3_int64 startup_expected[] = {
    [0] = 1, [1] = 1, [2] = 0, [3] = 41943040, [4] = 0, [5] = 0, [6] = -32768, [7] = 0,
};
static const char schema[] =
    "CREATE TABLE users (\r\n"
    "  id          INTEGER PRIMARY KEY,\r\n"
    "  username    TEXT NOT NULL UNIQUE,\r\n"
    "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))\r\n"
    ");\r\n\r\n"
    "CREATE TABLE posts (\r\n"
    "  id          INTEGER PRIMARY KEY,\r\n"
    "  user_id     INTEGER NOT NULL REFERENCES users(id),\r\n"
    "  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),\r\n"
    "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))\r\n"
    ");\r\n\r\n"
    "CREATE TABLE likes (\r\n"
    "  user_id     INTEGER NOT NULL REFERENCES users(id),\r\n"
    "  post_id     INTEGER NOT NULL REFERENCES posts(id),\r\n"
    "  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),\r\n"
    "  PRIMARY KEY (user_id, post_id)\r\n"
    ");\r\n\r\n"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);\r\n"
    "CREATE INDEX posts_user_id_idx       ON posts (user_id);\r\n"
    "CREATE INDEX likes_post_id_idx       ON likes (post_id);\r\n";
static const char *const seed_sql[] = {
    [0] = "INSERT INTO users (id, username, created_at) VALUES (?1, ?2, ?3);",
    [1] = "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4);",
    [2] = "INSERT INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3);",
};
static const char fixed_time[] = "2025-12-31T23:59:19.409Z";
static const char training_text[] = "Warm feed text profiles SQL indexes, row lookups, exact counts and UTF-8 output in one compiled unit";

enum {
    OUTPUT_BYTES = 131072,      /* the reader's reply area, store.rs's out capacity */
    SCRATCH_BYTES = 131072,     /* the reader's scratch area, store.rs's scratch bound */
    FEED_POSTS_ITEMS = 20,      /* posts in one feed */
    SLOT_SPAN_ITEMS = 256,      /* the widest id span the range feed maps */
    DECIMAL_DIGITS_BYTES = 20,  /* the widest 64-bit decimal */
    WRITE_GROUP_ITEMS = 64,     /* draws in one read-then-write group */
};

/* The trainer's one mutable object, owned by main's thread, the only one. */
static struct trainer {
    uint8_t out_bytes[OUTPUT_BYTES];
    uint8_t scratch_bytes[SCRATCH_BYTES];
    sqlite3 *db;
    sqlite3_stmt *statements[PERSISTENT_COUNT];
    sqlite3_stmt *seed_statements[3];
    int failed;
    int readers_opened;
    unsigned feed_ok;
    unsigned post_ok;
    unsigned sql_fallback;
    volatile uint64_t consumed;
} trainer;

/* Prints the first failure's line, or a cleanup line after it, records the failure and returns -1. */
static int report(const char *stage, int code, int os_error) {
    if (trainer.failed) {
        stage = "cleanup";
    }
    fprintf(stderr, "pgo_train: failed %s (sqlite %d, errno %d)\n", stage, code & 255, os_error < 0 ? 0 : os_error);
    trainer.failed = 1;
    return -1;
}

static int sqlite_failure(const char *stage, int code) {
    int primary = code & 255;
    int os_error = 0;
    if (trainer.db && (primary == SQLITE_IOERR || primary == SQLITE_FULL || primary == SQLITE_CANTOPEN)) {
        os_error = sqlite3_system_errno(trainer.db);
    }
    return report(stage, code, os_error);
}

static int semantic(const char *stage) { return report(stage, 0, 0); }

/* A step that returned rc in place of the one result its statement allows: SQLITE_ROW in place of
 * SQLITE_DONE, or SQLITE_DONE in place of SQLITE_ROW, is a semantic failure; any other code is SQLite's. */
static int unexpected_step(const char *stage, int rc) {
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        return semantic(stage);
    }
    return sqlite_failure(stage, rc);
}

/* A length this program checked against INT_MAX, as the int SQLite takes. */
static int int_of_size(size_t value) {
    assert(value <= INT_MAX);
    return (int)value;
}

/* A count SQLite or snprintf returned, checked non-negative by its caller, as a size. */
static size_t size_of_count(int value) {
    assert(value >= 0);
    return (size_t)value;
}

/* A column value's bits for the consumed sink: C's conversion to unsigned 64-bit, modulo 2^64. */
static uint64_t value_bits(sqlite3_int64 value) {
    return (uint64_t)value;
}

static int prepare(sqlite3_stmt **out, const char *sql, int persistent, const char *stage) {
    size_t length = strlen(sql);
    if (length > INT_MAX) {
        return semantic(stage);
    }
    int rc = sqlite3_prepare_v3(trainer.db, sql, int_of_size(length), persistent ? SQLITE_PREPARE_PERSISTENT : 0, out, NULL);
    if (rc != SQLITE_OK) {
        return sqlite_failure(stage, rc);
    }
    if (!*out) {
        return semantic(stage);
    }
    return 0;
}

static int reset(sqlite3_stmt *stmt, const char *stage, int preceding) {
    int rc = sqlite3_reset(stmt);
    int clear = sqlite3_clear_bindings(stmt);
    int status = 0;
    if (rc != SQLITE_OK && rc != preceding) {
        status = sqlite_failure(stage, rc);
    }
    if (clear != SQLITE_OK) {
        status = sqlite_failure(stage, clear);
    }
    return status;
}

static int bind_integer(sqlite3_stmt *stmt, int at, sqlite3_int64 value, const char *stage) {
    int rc = sqlite3_bind_int64(stmt, at, value);
    if (rc != SQLITE_OK) {
        return sqlite_failure(stage, rc);
    }
    return 0;
}

static int bind_text(sqlite3_stmt *stmt, int at, const char *value, size_t length, const char *stage) {
    if (length > INT_MAX) {
        return semantic(stage);
    }
    int rc = sqlite3_bind_text(stmt, at, value, int_of_size(length), SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        return sqlite_failure(stage, rc);
    }
    return 0;
}

static int done(sqlite3_stmt *stmt, const char *stage, const char *reset_stage) {
    int rc = sqlite3_step(stmt);
    int status = 0;
    if (rc != SQLITE_DONE) {
        status = unexpected_step(stage, rc);
    }
    if (reset(stmt, reset_stage, rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    return status;
}

static int control(int index, const char *stage) { return done(trainer.statements[index], stage, stage); }

static int guard(const char *stage, int autocommit) {
    if (sqlite3_get_autocommit(trainer.db) != autocommit) {
        return semantic(stage);
    }
    return 0;
}

static int column_text(sqlite3_stmt *stmt, int column, const char *stage) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    int length = sqlite3_column_bytes(stmt, column);
    if (length < 0 || (length != 0 && !text)) {
        return semantic(stage);
    }
    size_t count = size_of_count(length);
    trainer.consumed += count;
    for (size_t i = 0; i < count; ++i) {
        trainer.consumed += text[i];
    }
    return 0;
}

static int consume_post(sqlite3_stmt *stmt, int count_column) {
    trainer.consumed += value_bits(sqlite3_column_int64(stmt, 0));
    for (int column = 1; column <= 3; ++column) {
        if (column_text(stmt, column, "step") != 0) {
            return -1;
        }
    }
    if (count_column) {
        trainer.consumed += value_bits(sqlite3_column_int64(stmt, 4));
    }
    return 0;
}

static int seed_post(int64_t id, int64_t user, int add_likes) {
    char body[sizeof("training post ") - 1 + DECIMAL_DIGITS_BYTES + sizeof(": ") - 1 + sizeof(training_text)]; /* written before read: snprintf fills it */
    int length = snprintf(body, sizeof(body), "training post %" PRId64 ": %s", id, training_text);
    if (length < 0 || size_of_count(length) >= sizeof(body)) {
        return semantic("seed");
    }
    sqlite3_stmt *stmt = trainer.seed_statements[1];
    if (bind_integer(stmt, 1, id, "seed") != 0 || bind_integer(stmt, 2, user, "seed") != 0
        || bind_text(stmt, 3, body, size_of_count(length), "seed") != 0
        || bind_text(stmt, 4, fixed_time, sizeof(fixed_time) - 1, "seed") != 0) {
        (void)reset(stmt, "seed", SQLITE_OK); /* the bind failure is already the one reported */
        return -1;
    }
    if (done(stmt, "seed", "seed") != 0) {
        return -1;
    }
    if (add_likes) {
        for (int j = 0; j < 4; ++j) {
            stmt = trainer.seed_statements[2];
            if (bind_integer(stmt, 1, 1 + ((id + j) % 1000), "seed") != 0 || bind_integer(stmt, 2, id, "seed") != 0
                || bind_text(stmt, 3, fixed_time, sizeof(fixed_time) - 1, "seed") != 0) {
                (void)reset(stmt, "seed", SQLITE_OK); /* the bind failure is already the one reported */
                return -1;
            }
            if (done(stmt, "seed", "seed") != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int health(void) {
    sqlite3_stmt *stmt = trainer.statements[HEALTH];
    int rc = sqlite3_step(stmt);
    int status = 0;
    if (rc != SQLITE_ROW) {
        status = unexpected_step("step", rc);
    } else if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) != 1) {
        status = semantic("step");
    }
    if (status == 0) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            status = unexpected_step("step", rc);
        }
    }
    if (reset(stmt, "reset", rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    return status;
}

static int reference_feed(void) {
    sqlite3_stmt *stmt = trainer.statements[FEED_REF];
    int rc = SQLITE_OK;
    int status = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (consume_post(stmt, 1) != 0) {
            status = -1;
            break;
        }
    }
    if (status == 0 && rc != SQLITE_DONE) {
        status = sqlite_failure("step", rc);
    }
    if (reset(stmt, "reset", rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    return status;
}

static int post_sql(sqlite3_int64 id, int *found) {
    sqlite3_stmt *stmt = trainer.statements[POST];
    *found = 0;
    if (bind_integer(stmt, 1, id, "bind") != 0) {
        (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
        return -1;
    }
    int rc = sqlite3_step(stmt);
    int status = 0;
    if (rc == SQLITE_ROW) {
        *found = 1;
        status = consume_post(stmt, 1);
        if (status == 0) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                status = semantic("step");
            }
        }
    }
    if (status == 0 && rc != SQLITE_DONE) {
        status = sqlite_failure("step", rc);
    }
    if (reset(stmt, "reset", rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    return status;
}

static int slot_for(sqlite3_int64 id, sqlite3_int64 low, const int slots[SLOT_SPAN_ITEMS]) {
    if (id < low || (uint64_t)id - (uint64_t)low >= SLOT_SPAN_ITEMS) {
        return -1;
    }
    return slots[(size_t)((uint64_t)id - (uint64_t)low)];
}

static int range_feed(void) { // NOLINT(readability-function-size): the range algorithm's discovery, span check, likes scan and row selection share its id, count and slot arrays
    sqlite3_int64 ids[FEED_POSTS_ITEMS]; /* written before read: the discovery fills ids[0..count) */
    uint64_t likes[FEED_POSTS_ITEMS] = {0};
    int slots[SLOT_SPAN_ITEMS]; /* written before read: the span check fills it */
    unsigned char found[FEED_POSTS_ITEMS] = {0};
    size_t count = 0;
    sqlite3_stmt *stmt = trainer.statements[FEED_IDS];
    int rc = SQLITE_OK;
    int status = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == FEED_POSTS_ITEMS) {
            status = semantic("step");
            break;
        }
        ids[count] = sqlite3_column_int64(stmt, 0);
        ++count;
    }
    if (status == 0 && rc != SQLITE_DONE) {
        status = sqlite_failure("step", rc);
    }
    if (reset(stmt, "reset", rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    if (status != 0 || count == 0) {
        return status;
    }
    sqlite3_int64 low = ids[0];
    sqlite3_int64 high = ids[0];
    for (size_t i = 1; i < count; ++i) {
        if (ids[i] < low) {
            low = ids[i];
        }
        if (ids[i] > high) {
            high = ids[i];
        }
    }
    if ((uint64_t)high - (uint64_t)low >= SLOT_SPAN_ITEMS) {
        return reference_feed();
    }
    memset(slots, -1, sizeof(slots));
    int contiguous = 1;
    for (size_t i = 0; i < count; ++i) {
        size_t at = (size_t)((uint64_t)ids[i] - (uint64_t)low);
        if (slots[at] >= 0) {
            return reference_feed();
        }
        slots[at] = int_of_size(i);
        sqlite3_int64 expected = 0;
        if (__builtin_sub_overflow(ids[0], i, &expected) || ids[i] != expected) {
            contiguous = 0;
        }
    }
    stmt = trainer.statements[RANGE_LIKES];
    if (bind_integer(stmt, 1, low, "bind") != 0 || bind_integer(stmt, 2, high, "bind") != 0) {
        (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        int slot = slot_for(id, low, slots);
        if (slot >= 0) {
            if (likes[slot] == UINT64_MAX) {
                status = semantic("step");
                break;
            }
            ++likes[slot];
        }
    }
    if (status == 0 && rc != SQLITE_DONE) {
        status = sqlite_failure("step", rc);
    }
    if (reset(stmt, "reset", rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    if (status != 0) {
        return -1;
    }
    stmt = trainer.statements[contiguous ? RANGE_DESC : RANGE_ROWS];
    if (bind_integer(stmt, 1, low, "bind") != 0 || bind_integer(stmt, 2, high, "bind") != 0) {
        (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
        return -1;
    }
    size_t row = 0;
    int matched = 1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        int slot = slot_for(id, low, slots);
        if (contiguous && (row >= count || ids[row] != id)) {
            matched = 0;
        }
        if (slot >= 0) {
            if (found[slot]) {
                matched = 0;
            }
            found[slot] = 1;
            trainer.consumed += likes[slot];
            if (consume_post(stmt, 0) != 0) {
                status = -1;
                break;
            }
        } else if (contiguous) {
            matched = 0;
        }
        ++row;
    }
    if (status == 0 && rc != SQLITE_DONE) {
        status = sqlite_failure("step", rc);
    }
    if (reset(stmt, "reset", rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc) != 0) {
        status = -1;
    }
    if (status != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!found[i]) {
            matched = 0;
        }
    }
    if (contiguous && row != count) {
        matched = 0;
    }
    if (matched) {
        return 0;
    }
    return reference_feed();
}

static int reader_call(int feed, sqlite3_int64 id, int missing) {
    struct twelve_buf out = {.ptr = trainer.out_bytes, .cap = sizeof(trainer.out_bytes), .len = 0, .need = 0};
    struct twelve_buf scratch = {.ptr = trainer.scratch_bytes, .cap = sizeof(trainer.scratch_bytes), .len = 0, .need = 0};
    int result = 0;
    if (feed) {
        result = twelve_feed(trainer.db, &out, &scratch);
    } else {
        result = twelve_post(trainer.db, id, &out);
    }
    int reason = twelve_reader_reason();
    if (result < 0 || result > 3 || out.len > out.cap || scratch.len > scratch.cap
        || (result == 2 ? (reason < 1 || reason > 6) : reason != 0)) {
        return semantic("reader");
    }
    if (result == 0) {
        if (missing || out.need != 0 || scratch.need != 0) {
            return semantic("reader");
        }
        if (feed) {
            ++trainer.feed_ok;
        } else {
            ++trainer.post_ok;
        }
        for (size_t i = 0; i < out.len; ++i) {
            trainer.consumed += out.ptr[i];
        }
        return 0;
    }
    if (result == 1) {
        if (missing) {
            return 0;
        }
        return semantic("reader");
    }
    ++trainer.sql_fallback;
    if (feed) {
        return range_feed();
    }
    int found = 0;
    if (post_sql(id, &found) != 0) {
        return -1;
    }
    if (found != missing) {
        return 0;
    }
    return semantic("reader");
}

/* Every failure here is reported and recorded in trainer.failed, which sets the exit status; each (void)
 * below drops the -1 such a report returns. */
static int cleanup(int acquired, int descriptor_closed) {
    for (int i = 0; i < PERSISTENT_COUNT; ++i) {
        if (trainer.statements[i]) {
            (void)reset(trainer.statements[i], "reset", SQLITE_OK); /* recorded in trainer.failed */
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (trainer.seed_statements[i]) {
            (void)reset(trainer.seed_statements[i], "reset", SQLITE_OK); /* recorded in trainer.failed */
        }
    }
    if (trainer.db && !sqlite3_get_autocommit(trainer.db)) {
        if (trainer.statements[ROLLBACK]) {
            (void)control(ROLLBACK, "step"); /* recorded in trainer.failed */
        } else {
            (void)semantic("step"); /* recorded in trainer.failed */
        }
    }
    for (int i = 0; i < PERSISTENT_COUNT; ++i) {
        if (trainer.statements[i]) {
            int rc = sqlite3_finalize(trainer.statements[i]);
            trainer.statements[i] = NULL;
            if (rc != SQLITE_OK) {
                (void)sqlite_failure("finalize", rc); /* recorded in trainer.failed */
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (trainer.seed_statements[i]) {
            int rc = sqlite3_finalize(trainer.seed_statements[i]);
            trainer.seed_statements[i] = NULL;
            if (rc != SQLITE_OK) {
                (void)sqlite_failure("finalize", rc); /* recorded in trainer.failed */
            }
        }
    }
    if (trainer.readers_opened) {
        twelve_readers_close();
        trainer.readers_opened = 0;
    }
    int closed = descriptor_closed;
    if (trainer.db) {
        int rc = sqlite3_close(trainer.db);
        if (rc == SQLITE_OK) {
            trainer.db = NULL;
            closed = 1;
        } else {
            (void)sqlite_failure("close", rc); /* recorded in trainer.failed */
            closed = 0;
        }
    }
    if (acquired && closed) {
        const char *const owned[] = {[0] = "training.db", [1] = "training.db-wal", [2] = "training.db-shm"};
        for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i) {
            if (unlink(owned[i]) != 0) {
                int code = errno;
                if (code != ENOENT) {
                    (void)report("cleanup", 0, code); /* recorded in trainer.failed */
                }
            }
        }
    }
    return trainer.failed ? 1 : 0;
}

/* Each step stops the training at its first failure, which report has printed and recorded in
 * trainer.failed; a (void) below drops the -1 such a report returns. */
int main(int argc, char **argv) { // NOLINT(readability-function-size): the training sequence's steps share the connection, the statements and the buffers, and each runs once
    (void)argv;
    if (argc != 1) {
        (void)semantic("arguments"); /* reported: the run stops here */
        return 1;
    }

    /* Descriptors: 0, 1 and 2 are open before the first open, so training.db never takes one of them. */
    for (int fd = 0; fd <= 2; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) {
            continue;
        }
        int code = errno;
        if (code != EBADF) {
            (void)report("descriptors", 0, code); /* reported: the run stops here */
            return 1;
        }
        int opened = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (opened < 0) {
            code = errno;
            (void)report("descriptors", 0, code); /* reported: the run stops here */
            return 1;
        }
        if (opened != fd) {
            (void)semantic("descriptors"); /* reported: the run stops here */
            return 1;
        }
    }

    /* Acquire: training.db is created here, exclusively and without following a link, or refused. */
    int descriptor = open("training.db", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        int code = errno;
        (void)report("temporary", 0, code); /* reported: the run stops here */
        return 1;
    }
    if (close(descriptor) != 0) {
        int code = errno;
        (void)report("temporary", 0, code); /* reported: the run stops here */
        return cleanup(1, 0);
    }

    /* Open: the request-style connection on unix-excl. */
    int rc = sqlite3_open_v2("training.db", &trainer.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl");
    if (rc != SQLITE_OK) {
        (void)sqlite_failure("open", rc); /* reported: the run stops here */
        return cleanup(1, 1);
    }

    /* Startup: every release pragma, then its read-back. */
    for (size_t i = 0; i < sizeof(startup_sql) / sizeof(startup_sql[0]); ++i) {
        sqlite3_stmt *stmt = NULL;
        if (prepare(&stmt, startup_sql[i], 0, "pragma") != 0) {
            if (stmt) {
                (void)sqlite3_finalize(stmt); /* the prepare failure is already the one reported */
            }
            goto out_cleanup;
        }
        int status = 0;
        int step = sqlite3_step(stmt);
        if (i == 0) {
            if (step != SQLITE_ROW) {
                status = unexpected_step("pragma", step);
            } else {
                const unsigned char *text = sqlite3_column_text(stmt, 0);
                int length = sqlite3_column_bytes(stmt, 0);
                if (!text || length != 3 || memcmp(text, "wal", 3) != 0) {
                    status = semantic("pragma");
                }
            }
            if (status == 0) {
                step = sqlite3_step(stmt);
            }
        } else if (i >= 9) {
            if (step != SQLITE_ROW) {
                status = unexpected_step("pragma", step);
            } else if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 0) != startup_expected[i - 9]) {
                status = semantic("pragma");
            }
            if (status == 0) {
                step = sqlite3_step(stmt);
            }
        } else {
            while (step == SQLITE_ROW) {
                step = sqlite3_step(stmt);
            }
        }
        if (status == 0 && step != SQLITE_DONE) {
            status = sqlite_failure("pragma", step);
        }
        if (reset(stmt, "pragma", step == SQLITE_DONE || step == SQLITE_ROW ? SQLITE_OK : step) != 0) {
            status = -1;
        }
        int finalized = sqlite3_finalize(stmt);
        if (finalized != SQLITE_OK) {
            status = sqlite_failure("pragma", finalized);
        }
        if (status != 0) {
            goto out_cleanup;
        }
    }

    /* Seed: the transaction-control statements, the schema, then the synthetic rows in one IMMEDIATE unit. */
    for (int i = BEGIN_READ; i <= ROLLBACK; ++i) {
        if (prepare(&trainer.statements[i], release_sql[i], 1, "prepare") != 0) {
            goto out_cleanup;
        }
    }
    rc = sqlite3_exec(trainer.db, schema, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        (void)sqlite_failure("schema", rc); /* reported: the run stops here */
        goto out_cleanup;
    }
    for (int i = 0; i < 3; ++i) {
        if (prepare(&trainer.seed_statements[i], seed_sql[i], 0, "seed") != 0) {
            goto out_cleanup;
        }
    }
    if (control(BEGIN_WRITE, "seed") != 0 || guard("seed", 0) != 0) {
        goto out_cleanup;
    }
    for (int64_t user = 1; user <= 1000; ++user) {
        char name[sizeof("training-user-") + DECIMAL_DIGITS_BYTES]; /* written before read: snprintf fills it */
        int length = snprintf(name, sizeof(name), "training-user-%" PRId64, user);
        if (length < 0 || size_of_count(length) >= sizeof(name)) {
            (void)semantic("seed"); /* reported: the run stops here */
            goto out_cleanup;
        }
        sqlite3_stmt *stmt = trainer.seed_statements[0];
        if (bind_integer(stmt, 1, user, "seed") != 0 || bind_text(stmt, 2, name, size_of_count(length), "seed") != 0
            || bind_text(stmt, 3, fixed_time, sizeof(fixed_time) - 1, "seed") != 0) {
            (void)reset(stmt, "seed", SQLITE_OK); /* the bind failure is already the one reported */
            goto out_cleanup;
        }
        if (done(stmt, "seed", "seed") != 0) {
            goto out_cleanup;
        }
    }
    for (int64_t post = 1; post <= 10000; ++post) {
        if (seed_post(post, 1 + ((post - 1) % 1000), 1) != 0) {
            goto out_cleanup;
        }
    }
    if (seed_post(10003, 1, 0) != 0 || seed_post(10006, 2, 0) != 0) {
        goto out_cleanup;
    }
    if (control(COMMIT, "seed") != 0 || guard("seed", 1) != 0) {
        goto out_cleanup;
    }
    for (int i = 0; i < 3; ++i) {
        rc = sqlite3_finalize(trainer.seed_statements[i]);
        trainer.seed_statements[i] = NULL;
        if (rc != SQLITE_OK) {
            (void)sqlite_failure("finalize", rc); /* reported: the run stops here */
            goto out_cleanup;
        }
    }
    for (int i = 0; i < BEGIN_READ; ++i) {
        if (prepare(&trainer.statements[i], release_sql[i], 1, "prepare") != 0) {
            goto out_cleanup;
        }
    }
    rc = twelve_readers_open(trainer.db);
    if (rc != 0) {
        (void)semantic("reader"); /* reported: the run stops here */
        goto out_cleanup;
    }
    trainer.readers_opened = 1;

    /* Probes: the release rollback text, the missing post through reader and SQL, then the declared TXN
     * fall-back outside a transaction and its SQL answer. */
    if (control(BEGIN_READ, "step") != 0 || health() != 0 || control(ROLLBACK, "step") != 0 || guard("step", 1) != 0) {
        goto out_cleanup;
    }
    if (control(BEGIN_READ, "step") != 0) {
        goto out_cleanup;
    }
    {
        int found = 0;
        if (reader_call(0, 10001, 1) != 0 || post_sql(10001, &found) != 0) {
            goto out_cleanup;
        }
        if (found) {
            (void)semantic("reader"); /* reported: the run stops here */
            goto out_cleanup;
        }
    }
    if (control(COMMIT, "step") != 0 || guard("step", 1) != 0) {
        goto out_cleanup;
    }
    {
        struct twelve_buf out = {.ptr = trainer.out_bytes, .cap = sizeof(trainer.out_bytes), .len = 0, .need = 0};
        struct twelve_buf scratch = {.ptr = trainer.scratch_bytes, .cap = sizeof(trainer.scratch_bytes), .len = 0, .need = 0};
        int result = twelve_feed(trainer.db, &out, &scratch);
        int reason = twelve_reader_reason();
        if (result != 2 || reason != 1 || out.len > out.cap || scratch.len > scratch.cap) {
            (void)semantic("reader"); /* reported: the run stops here */
            goto out_cleanup;
        }
        ++trainer.sql_fallback;
    }
    if (control(BEGIN_READ, "step") != 0 || range_feed() != 0 || control(COMMIT, "step") != 0 || guard("step", 1) != 0) {
        goto out_cleanup;
    }

    /* Training loop: 100,000 draws in groups of WRITE_GROUP_ITEMS; each group's reads run in one read
     * transaction, then its writes, in draw order, in one IMMEDIATE transaction. */
    {
        struct pending_write { int create; sqlite3_int64 user; sqlite3_int64 post; } writes[WRITE_GROUP_ITEMS]; /* written before read: each group fills writes[0..count) */
        for (unsigned first = 0; first < 100000; first += WRITE_GROUP_ITEMS) {
            unsigned end = first + WRITE_GROUP_ITEMS;
            if (end > 100000) {
                end = 100000;
            }
            size_t count = 0;
            for (unsigned i = first; i < end; ++i) {
                unsigned route = i % 217;
                if (route >= 200) {
                    writes[count].create = route >= 215;
                    writes[count].user = 1 + (i % 1000);
                    writes[count].post = 1 + (i % 10000);
                    ++count;
                }
            }
            if (control(BEGIN_READ, "step") != 0 || guard("step", 0) != 0) {
                goto out_cleanup;
            }
            for (unsigned i = first; i < end; ++i) {
                if (i % 217 < 100) {
                    int status = 0;
                    switch (i % 3) {
                        case 0: status = reader_call(1, 0, 0); break;
                        case 1: status = range_feed(); break;
                        default: status = reference_feed(); break;
                    }
                    if (status != 0) {
                        goto out_cleanup;
                    }
                }
            }
            for (unsigned i = first; i < end; ++i) {
                if (i % 217 >= 100 && i % 217 < 200) {
                    sqlite3_int64 id = 1 + (i % 10000);
                    if (i % 2 == 0) {
                        if (reader_call(0, id, 0) != 0) {
                            goto out_cleanup;
                        }
                    } else {
                        int found = 0;
                        if (post_sql(id, &found) != 0) {
                            goto out_cleanup;
                        }
                        if (!found) {
                            (void)semantic("step"); /* reported: the run stops here */
                            goto out_cleanup;
                        }
                    }
                }
            }
            if (health() != 0 || control(COMMIT, "step") != 0 || guard("step", 1) != 0) {
                goto out_cleanup;
            }
            if (count != 0) {
                if (control(BEGIN_WRITE, "step") != 0 || guard("step", 0) != 0) {
                    goto out_cleanup;
                }
                for (size_t i = 0; i < count; ++i) {
                    if (writes[i].create) {
                        /* Create: the CREATE insert, then the CREATED select for the new row's timestamp. */
                        char body[sizeof("training create: ") - 1 + 57 + 1]; /* written before read: snprintf fills it */
                        int length = snprintf(body, sizeof(body), "training create: %.57s", training_text);
                        if (length < 0 || size_of_count(length) >= sizeof(body) || length != 74) {
                            (void)semantic("step"); /* reported: the run stops here */
                            goto out_cleanup;
                        }
                        sqlite3_stmt *stmt = trainer.statements[CREATE];
                        if (bind_integer(stmt, 1, writes[i].user, "bind") != 0
                            || bind_text(stmt, 2, body, size_of_count(length), "bind") != 0) {
                            (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
                            goto out_cleanup;
                        }
                        if (done(stmt, "step", "reset") != 0 || guard("step", 0) != 0) {
                            goto out_cleanup;
                        }
                        sqlite3_int64 id = sqlite3_last_insert_rowid(trainer.db);
                        stmt = trainer.statements[CREATED];
                        if (bind_integer(stmt, 1, id, "bind") != 0) {
                            (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
                            goto out_cleanup;
                        }
                        int step = sqlite3_step(stmt);
                        int status = 0;
                        if (step != SQLITE_ROW) {
                            status = unexpected_step("step", step);
                        } else {
                            status = column_text(stmt, 0, "step");
                        }
                        if (status == 0) {
                            step = sqlite3_step(stmt);
                            if (step != SQLITE_DONE) {
                                status = unexpected_step("step", step);
                            }
                        }
                        if (reset(stmt, "reset", step == SQLITE_ROW || step == SQLITE_DONE ? SQLITE_OK : step) != 0) {
                            status = -1;
                        }
                        if (status != 0 || guard("step", 0) != 0) {
                            goto out_cleanup;
                        }
                    } else {
                        /* Like: the LIKE insert, then the STMT_EXISTS select when it changed no row. */
                        sqlite3_stmt *stmt = trainer.statements[LIKE];
                        if (bind_integer(stmt, 1, writes[i].user, "bind") != 0
                            || bind_integer(stmt, 2, writes[i].post, "bind") != 0) {
                            (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
                            goto out_cleanup;
                        }
                        if (done(stmt, "step", "reset") != 0) {
                            goto out_cleanup;
                        }
                        int changes = sqlite3_changes(trainer.db);
                        if (guard("step", 0) != 0) {
                            goto out_cleanup;
                        }
                        if (changes == 1) {
                            continue;
                        }
                        if (changes != 0) {
                            (void)semantic("step"); /* reported: the run stops here */
                            goto out_cleanup;
                        }
                        stmt = trainer.statements[STMT_EXISTS];
                        if (bind_integer(stmt, 1, writes[i].post, "bind") != 0) {
                            (void)reset(stmt, "reset", SQLITE_OK); /* the bind failure is already the one reported */
                            goto out_cleanup;
                        }
                        int step = sqlite3_step(stmt);
                        int status = 0;
                        if (step != SQLITE_ROW) {
                            status = unexpected_step("step", step);
                        } else {
                            trainer.consumed += value_bits(sqlite3_column_int64(stmt, 0));
                        }
                        if (status == 0) {
                            step = sqlite3_step(stmt);
                            if (step != SQLITE_DONE) {
                                status = unexpected_step("step", step);
                            }
                        }
                        if (reset(stmt, "reset", step == SQLITE_ROW || step == SQLITE_DONE ? SQLITE_OK : step) != 0) {
                            status = -1;
                        }
                        if (status != 0 || guard("step", 0) != 0) {
                            goto out_cleanup;
                        }
                    }
                }
                if (control(COMMIT, "step") != 0 || guard("step", 1) != 0) {
                    goto out_cleanup;
                }
            }
        }
        if (!trainer.feed_ok || !trainer.post_ok || !trainer.sql_fallback) {
            (void)semantic("reader"); /* reported: the run stops here */
            goto out_cleanup;
        }
    }

out_cleanup:
    return cleanup(1, 1);
}
