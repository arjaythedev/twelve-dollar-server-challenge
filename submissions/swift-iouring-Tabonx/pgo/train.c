// PGO training for the database layer (Sources/CSQLite/db.c with the SQLite amalgamation): builds a
// synthetic database with the challenge schema (no seed data), then runs the server's reads and
// writes in the same mix and pass shape for a few seconds.
//   train <schema.sql> <db path> <seconds>
#include "sqlite3.h"
#include "feeddb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void ex(sqlite3 *db, const char *sql) {
    char *err = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err);
        exit(1);
    }
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static unsigned long sink;
static void touch(int cursor) {
    uint32_t n = 0;
    const uint8_t *p = fd_payload(cursor, &n);
    if (p && n) sink += p[0] + p[n - 1];
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("schema"); return 1; }
    static char schema[1 << 16];
    schema[fread(schema, 1, sizeof schema - 1, f)] = 0;
    fclose(f);
    remove(argv[2]);
    sqlite3_initialize();
    sqlite3 *db;
    if (sqlite3_open(argv[2], &db) != SQLITE_OK) return 1;
    ex(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;");
    ex(db, schema);
    ex(db, "BEGIN;"
       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 20000)"
       " INSERT INTO users(id, username) SELECT i, 'user_' || i FROM n;"
       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 100000)"
       " INSERT INTO posts(id, user_id, body, created_at) SELECT i, 1 + abs(random()) % 20000,"
       " substr('lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor ', 1 + abs(random()) % 40, 20 + abs(random()) % 120),"
       " strftime('%Y-%m-%dT%H:%M:%fZ', 1735689600 + i * 30, 'unixepoch') FROM n;"
       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 400000)"
       " INSERT OR IGNORE INTO likes(user_id, post_id) SELECT 1 + abs(random()) % 20000, 1 + abs(random()) % 100000 FROM n;"
       "COMMIT; PRAGMA journal_mode=WAL;");
    sqlite3_close(db);

    char err[256];
    if (fd_open(argv[2], err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    int64_t newest = 100000;
    double end = now() + atof(argv[3]);
    long requests = 0;
    double t0 = now();
    uint32_t counts[64];
    while (now() < end) {
        for (int k = 0; k < 24; k++, requests++) {
            if (fd_begin() != 0) { fprintf(stderr, "begin failed\n"); return 1; }
            int r = rand() % 217;
            if (r < 100) {
                int64_t ids[20];
                int n = 0;
                for (int ok = fd_first(FD_FEED_IDX); ok == 1 && n < 20; ok = fd_next(FD_FEED_IDX)) {
                    touch(FD_FEED_IDX);
                    ids[n] = newest - n;
                    n++;
                }
                for (int i = n - 1; i >= 0; i--)
                    if (fd_seek(FD_POSTS, ids[i]) == 1) touch(FD_POSTS);
                for (int i = 0; i < n; i++)
                    if (fd_seek(FD_USERS, 1 + (ids[i] * 7919) % 20000) == 1) touch(FD_USERS);
                memset(counts, 0, sizeof counts);
                fd_count_likes(newest - 19, newest, counts);
                sink += counts[0];
            } else if (r < 200) {
                int64_t id = newest - rand() % 20;
                if (fd_seek(FD_POSTS, id) == 1) touch(FD_POSTS);
                if (fd_seek(FD_USERS, 1 + rand() % 20000) == 1) touch(FD_USERS);
                uint32_t c = 0;
                fd_count_likes(id, id, &c);
                sink += c;
            } else if (r < 215) {
                int64_t id = newest - rand() % 20;
                if (fd_seek(FD_POSTS, id) == 1) sink += fd_insert_like(1 + rand() % 20000, id);
            } else {
                static const uint8_t body[] = "user_1 says hi at 2026-10-25T20:00:00.000Z (VU 1, iter 1)";
                uint8_t created[64];
                int created_len = 0;
                int64_t id = 0;
                if (fd_insert_post(1 + rand() % 20000, body, sizeof body - 1, &id, created, &created_len) == 0) newest = id;
            }
        }
        fd_commit();
        fd_tick();
    }
    printf("trained, %.0f requests/s, sink %lu\n", requests / (now() - t0), sink & 0xff);
    return 0;
}
