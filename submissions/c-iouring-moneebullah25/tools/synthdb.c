/* synthdb: builds a synthetic database with the challenge schema plus a
 * tokens.json, for PGO training and local experiments. It never touches the
 * seed; the data is random text from a fixed PRNG.
 *
 *   synthdb out.db tokens.json <users> <posts> <likes> <jwt-secret> [token-count]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jwt.h"
#include "sqlite3.h"

static const char SCHEMA[] =
    "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
    " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
    "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id),"
    " post_id INTEGER NOT NULL REFERENCES posts(id),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
    "CREATE INDEX posts_user_id_idx ON posts (user_id);"
    "CREATE INDEX likes_post_id_idx ON likes (post_id);";

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static const char *WORDS[] = {"server",  "database", "request", "latency", "coffee", "weekend",   "deploy",
                              "bug",     "feature",  "test",    "kernel",  "socket", "keepalive", "pool",
                              "fast",    "slow",     "honest",  "careful", "today",  "tomorrow",  "something",
                              "nothing", "nginx",    "linux",   "ubuntu"};

static void fail(sqlite3 *db, const char *what) {
    fprintf(stderr, "%s: %s\n", what, sqlite3_errmsg(db));
    exit(1);
}

#define TS_CAP 64

static void iso_time(int64_t ms, char out[TS_CAP]) {
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&s, &tm);
    snprintf(out, TS_CAP, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ms % 1000));
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: synthdb out.db tokens.json users posts likes secret [token-count]\n");
        return 2;
    }
    const char *path = argv[1], *tokens_path = argv[2], *secret = argv[6];
    long users = atol(argv[3]), posts = atol(argv[4]), likes = atol(argv[5]);
    long ntokens = argc > 7 ? atol(argv[7]) : (users < 2000 ? users : 2000);
    remove(path);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s-wal", path);
    remove(tmp);
    snprintf(tmp, sizeof tmp, "%s-shm", path);
    remove(tmp);

    sqlite3_initialize();
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail(db, "open");
    if (sqlite3_exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;", 0, 0, 0) != SQLITE_OK) fail(db, "pragma");
    if (sqlite3_exec(db, SCHEMA, 0, 0, 0) != SQLITE_OK) fail(db, "schema");
    if (sqlite3_exec(db, "BEGIN", 0, 0, 0) != SQLITE_OK) fail(db, "begin");

    sqlite3_stmt *st;
    char ts[TS_CAP];
    int64_t end_ms = 1767225600000LL; /* 2026-01-01T00:00:00Z */
    if (sqlite3_prepare_v2(db, "INSERT INTO users (id, username, created_at) VALUES (?,?,?)", -1, &st, 0))
        fail(db, "p");
    for (long i = 1; i <= users; i++) {
        snprintf(tmp, sizeof tmp, "%s_%s_%ld", WORDS[rnd() % 25], WORDS[rnd() % 25], i);
        iso_time(end_ms - 730LL * 86400000 + (int64_t)(rnd() % (365u * 86400u)) * 1000, ts);
        sqlite3_bind_int64(st, 1, i);
        sqlite3_bind_text(st, 2, tmp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, ts, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) fail(db, "user");
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?,?,?,?)", -1, &st, 0))
        fail(db, "p");
    double step = 365.0 * 86400000 / (double)posts;
    for (long i = 1; i <= posts; i++) {
        int words = 4 + (int)(rnd() % 40);
        size_t o = 0;
        for (int w = 0; w < words && o < 480; w++) {
            o += (size_t)snprintf(tmp + o, sizeof tmp - o, "%s%s", w ? " " : "", WORDS[rnd() % 25]);
        }
        iso_time(end_ms - 365LL * 86400000 + (int64_t)(step * (double)(i - 1)) + (int64_t)(rnd() % 1000), ts);
        sqlite3_bind_int64(st, 1, i);
        sqlite3_bind_int64(st, 2, 1 + (int64_t)(rnd() % (uint32_t)users));
        sqlite3_bind_text(st, 3, tmp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, ts, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) fail(db, "post");
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO likes (user_id, post_id, created_at) VALUES (?,?,?)", -1, &st, 0))
        fail(db, "p");
    for (long i = 0; i < likes; i++) {
        /* skew toward newer posts, like the real load does */
        int64_t pid = 1 + (int64_t)(rnd() % (uint32_t)posts);
        if (rnd() % 4 == 0) pid = posts - (int64_t)(rnd() % 100);
        iso_time(end_ms - (int64_t)(rnd() % (365u * 86400u)) * 1000, ts);
        sqlite3_bind_int64(st, 1, 1 + (int64_t)(rnd() % (uint32_t)users));
        sqlite3_bind_int64(st, 2, pid);
        sqlite3_bind_text(st, 3, ts, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) fail(db, "like");
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(db, "COMMIT; ANALYZE; PRAGMA journal_mode=WAL;", 0, 0, 0) != SQLITE_OK) fail(db, "commit");
    sqlite3_close(db);

    /* tokens */
    FILE *f = fopen(tokens_path, "w");
    if (!f) {
        perror(tokens_path);
        return 1;
    }
    struct hmac_key key;
    hmac_key_init(&key, secret, strlen(secret));
    int64_t iat = (int64_t)time(NULL);
    fputc('[', f);
    rng = 12345;
    for (long i = 1; i <= ntokens; i++) {
        /* regenerate the same username sequence as above */
        char name[128];
        snprintf(name, sizeof name, "%s_%s_%ld", WORDS[rnd() % 25], WORDS[rnd() % 25], i);
        rnd(); /* the created_at draw */
        char payload[256], tok[512];
        int pl = snprintf(payload, sizeof payload, "{\"sub\":\"%ld\",\"username\":\"%s\",\"iat\":%lld,\"exp\":%lld}", i,
                          name, (long long)iat, (long long)(iat + 3650LL * 86400));
        size_t tl = jwt_sign(&key, payload, (size_t)pl, tok, sizeof tok);
        fprintf(f, "%s{\"id\":%ld,\"username\":\"%s\",\"token\":\"%.*s\"}", i > 1 ? "," : "", i, name, (int)tl, tok);
    }
    fputs("]\n", f);
    fclose(f);
    printf("synthdb: %ld users, %ld posts, %ld likes -> %s; %ld tokens -> %s\n", users, posts, likes, path, ntokens,
           tokens_path);
    return 0;
}
