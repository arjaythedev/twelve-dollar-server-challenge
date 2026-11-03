/* dbdiff: differential test and microbenchmark for the direct B-tree reads.
 *
 *   bin/dbdiff <feed.db copy>            compare B-tree vs SQL JSON for the feed and
 *                                        every post id, then through 2,000 writes
 *   bin/dbdiff <feed.db copy> --bench    us per feed and per post get, both paths
 *
 * Works on a copy of the seed: the write cycle changes the database. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "db.h"
#include "sqlite3.h"
#include "util.h"

#define CAP 131072

static char a[CAP], b[CAP];

static int64_t max_post_id(struct db *db) {
    /* the SQL post path answers 0 for a missing id; probe upward from a known size */
    (void)db;
    return 0;
}

static int compare_post(struct db *db, int64_t id, const char *when) {
    db_reads_btree(db, 1);
    size_t na = db_post_json(db, id, a, CAP);
    db_reads_btree(db, 0);
    size_t nb = db_post_json(db, id, b, CAP);
    if (na != nb || (na != 0 && na != (size_t)-1 && memcmp(a, b, na) != 0)) {
        fprintf(stderr, "MISMATCH post %lld (%s): btree %zd bytes, sql %zd bytes\n%.*s\n%.*s\n", (long long)id, when,
                (ssize_t)na, (ssize_t)nb, na == (size_t)-1 ? 0 : (int)na, a, nb == (size_t)-1 ? 0 : (int)nb, b);
        return -1;
    }
    return 0;
}

static int compare_feed(struct db *db, const char *when) {
    db_reads_btree(db, 1);
    size_t na = db_feed_json(db, a, CAP);
    db_reads_btree(db, 0);
    size_t nb = db_feed_json(db, b, CAP);
    if (!na || na != nb || memcmp(a, b, na) != 0) {
        fprintf(stderr, "MISMATCH feed (%s): btree %zu bytes, sql %zu bytes\n%.*s\n%.*s\n", when, na, nb, (int)na, a,
                (int)nb, b);
        return -1;
    }
    return 0;
}

static int64_t count(sqlite3 *conn, const char *sql) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

static int diff_all(struct db *db, sqlite3 *conn, const char *when) {
    int64_t maxid = count(conn, "SELECT max(id) FROM posts");
    int bad = 0;
    db_read_begin(db);
    if (compare_feed(db, when)) bad++;
    for (int64_t id = 1; id <= maxid + 5; id++) {
        if (compare_post(db, id, when)) {
            if (++bad > 10) break;
        }
        if (id % 100000 == 0) {
            db_read_end(db);
            db_read_begin(db);
        }
    }
    db_read_end(db);
    printf("%s: feed and %lld post ids compared, %d mismatch(es)\n", when, (long long)maxid + 5, bad);
    return bad ? -1 : 0;
}

static uint64_t xs = 88172645463325252ull;
static uint64_t rnd(void) {
    xs ^= xs << 13;
    xs ^= xs >> 7;
    xs ^= xs << 17;
    return xs;
}

static int write_cycle(struct db *db, sqlite3 *conn) {
    int64_t nposts = count(conn, "SELECT max(id) FROM posts"), nusers = count(conn, "SELECT max(id) FROM users");
    int bad = 0, done = 0, batches = 0;
    while (done < 2000) {
        int n = 1 + (int)(rnd() % 32);
        if (db_write_begin(db)) {
            fprintf(stderr, "write_begin failed\n");
            return -1;
        }
        for (int i = 0; i < n; i++, done++) {
            int64_t uid = 1 + (int64_t)(rnd() % (uint64_t)nusers);
            if (rnd() % 100 < 30) {
                char body[600];
                int bl = snprintf(body, sizeof body, "dbdiff post %d \"quoted\" \\ back\\slash \x01 ctl \xc3\xa9 %llu",
                                  done, (unsigned long long)rnd());
                int64_t id;
                char created[32];
                if (db_create_post(db, uid, body, (size_t)bl, &id, created))
                    bad++;
                else if (id > nposts)
                    nposts = id;
            } else {
                int64_t pid = 1 + (int64_t)(rnd() % (uint64_t)nposts);
                if (db_like(db, uid, pid) < 0) bad++;
            }
        }
        if (db_write_commit(db)) {
            fprintf(stderr, "commit failed\n");
            return -1;
        }
        batches++;
        /* the feed after every batch, as the app reads it: in a shared read txn */
        db_read_begin(db);
        if (compare_feed(db, "during writes")) bad++;
        /* and a handful of posts, including the newest */
        for (int i = 0; i < 5; i++) {
            int64_t pid = i == 0 ? nposts : 1 + (int64_t)(rnd() % (uint64_t)nposts);
            if (compare_post(db, pid, "during writes")) bad++;
        }
        db_read_end(db);
        if (bad > 10) break;
    }
    printf("write cycle: %d writes in %d batches, %d mismatch(es)\n", done, batches, bad);
    return bad ? -1 : 0;
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void bench(struct db *db, sqlite3 *conn) {
    int64_t maxid = count(conn, "SELECT max(id) FROM posts");
    const int rounds = 20000, batch = 32;
    for (int mode = 1; mode >= 0; mode--) {
        db_reads_btree(db, mode);
        /* warm */
        db_read_begin(db);
        for (int i = 0; i < 2000; i++) db_feed_json(db, a, CAP);
        for (int i = 0; i < 2000; i++) db_post_json(db, maxid - (int64_t)(rnd() % 20000), a, CAP);
        db_read_end(db);
        double best_feed = 1e9, best_post = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            double t0 = now_us();
            for (int i = 0; i < rounds; i += batch) {
                db_read_begin(db);
                for (int j = 0; j < batch; j++) db_feed_json(db, a, CAP);
                db_read_end(db);
            }
            double feed = (now_us() - t0) / rounds;
            t0 = now_us();
            for (int i = 0; i < rounds; i += batch) {
                db_read_begin(db);
                for (int j = 0; j < batch; j++) db_post_json(db, maxid - (int64_t)(rnd() % 20000), a, CAP);
                db_read_end(db);
            }
            double post = (now_us() - t0) / rounds;
            if (feed < best_feed) best_feed = feed;
            if (post < best_post) best_post = post;
        }
        printf("%-6s feed %.2f us   post %.2f us   (best of 3 x %d, batches of %d in one read txn)\n",
               mode ? "btree" : "sql", best_feed, best_post, rounds, batch);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dbdiff <db copy> [--bench]\n");
        return 2;
    }
    if (sqlite3_config(SQLITE_CONFIG_MULTITHREAD) != SQLITE_OK || sqlite3_initialize() != SQLITE_OK) return 1;
    struct config cfg;
    config_defaults(&cfg);
    cfg.db_path = argv[1];
    char err[512];
    struct db *db = db_open(&cfg, err, sizeof err);
    if (!db) {
        fprintf(stderr, "open: %s\n", err);
        return 1;
    }
    if (db_reads_btree(db, 1) < 0) {
        fprintf(stderr, "btree reads unavailable\n");
        return 1;
    }
    sqlite3 *conn = NULL;
    sqlite3_open_v2(argv[1], &conn, SQLITE_OPEN_READONLY, "unix-excl"); /* same process: shares the lock */
    int rc = 0;
    if (argc > 2 && strcmp(argv[2], "--bench") == 0) {
        bench(db, conn);
    } else {
        if (diff_all(db, conn, "before writes")) rc = 1;
        if (write_cycle(db, conn)) rc = 1;
        if (diff_all(db, conn, "after writes")) rc = 1;
        struct db_stats st;
        db_get_stats(db, &st);
        printf("btree fallbacks during the run: %llu (feed range fallbacks %llu)\n",
               (unsigned long long)st.btree_fallbacks, (unsigned long long)st.feed_fallbacks);
    }
    sqlite3_close(conn);
    db_close(db);
    (void)max_post_id;
    return rc;
}
