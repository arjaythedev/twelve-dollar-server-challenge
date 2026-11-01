// PGO training for SQLite: builds a synthetic database with the challenge schema (no seed data),
// then runs the server's statements in the same mix and batch shape for a few seconds.
//   train <schema.sql> <db path> <seconds>
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *db;
static void ex(const char *sql) {
    char *err = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) { fprintf(stderr, "%s: %s\n", sql, err); exit(1); }
}
static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, sqlite3_errmsg(db)); exit(1);
    }
    return s;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static long sink;
static void drain(sqlite3_stmt *s) {
    while (sqlite3_step(s) == SQLITE_ROW)
        for (int c = 0; c < sqlite3_column_count(s); c++) sink += sqlite3_column_bytes(s, c) + sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("schema"); return 1; }
    static char schema[1 << 16];
    schema[fread(schema, 1, sizeof schema - 1, f)] = 0;
    fclose(f);
    remove(argv[2]);
    if (sqlite3_open(argv[2], &db) != SQLITE_OK) return 1;
    ex("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;");
    ex(schema);
    ex("BEGIN;"
       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 20000)"
       " INSERT INTO users(id, username) SELECT i, 'user_' || i FROM n;"
       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 100000)"
       " INSERT INTO posts(id, user_id, body, created_at) SELECT i, 1 + abs(random()) % 20000,"
       " substr('lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor ', 1 + abs(random()) % 40, 20 + abs(random()) % 120),"
       " strftime('%Y-%m-%dT%H:%M:%fZ', 1735689600 + i * 30, 'unixepoch') FROM n;"
       "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 400000)"
       " INSERT OR IGNORE INTO likes(user_id, post_id) SELECT 1 + abs(random()) % 20000, 1 + abs(random()) % 100000 FROM n;"
       "COMMIT; ANALYZE;");
    ex("PRAGMA journal_mode=WAL; PRAGMA locking_mode=EXCLUSIVE; PRAGMA synchronous=NORMAL;"
       "PRAGMA cache_size=-131072; PRAGMA mmap_size=0; PRAGMA wal_autocheckpoint=0;");

    sqlite3_stmt *ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    sqlite3_stmt *likes = prep("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    sqlite3_stmt *rows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id"
                              " WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC");
    sqlite3_stmt *post = prep("SELECT p.id, p.body, p.created_at, (SELECT username FROM users WHERE id = p.user_id),"
                              " (SELECT count(*) FROM likes WHERE post_id = ?1) FROM posts p WHERE p.id = ?1");
    sqlite3_stmt *exists = prep("SELECT 1 FROM posts WHERE id = ?1");
    sqlite3_stmt *insertPost = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    sqlite3_stmt *insertLike = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
    sqlite3_stmt *begin = prep("BEGIN"), *commit = prep("COMMIT");

    sqlite3_int64 newest = 100000;
    double end = now() + atof(argv[3]);
    int frames = 0;
    long batches = 0;
    double t0 = now();
    while (now() < end) {
        sqlite3_step(begin); sqlite3_reset(begin);
        for (int k = 0; k < 24; k++) {
            int r = rand() % 217;
            if (r < 100) {
                drain(ids);
                sqlite3_bind_int64(likes, 1, newest - 19); sqlite3_bind_int64(likes, 2, newest); drain(likes);
                sqlite3_bind_int64(rows, 1, newest - 19); sqlite3_bind_int64(rows, 2, newest); drain(rows);
            } else if (r < 200) {
                sqlite3_bind_int64(post, 1, newest - rand() % 20); drain(post);
            } else if (r < 215) {
                sqlite3_bind_int64(exists, 1, newest - rand() % 20); drain(exists);
                sqlite3_bind_int64(insertLike, 1, 1 + rand() % 20000);
                sqlite3_bind_int64(insertLike, 2, newest - rand() % 20);
                drain(insertLike);
            } else {
                static const char body[] = "user_1 says hi at 2026-10-25T20:00:00.000Z (VU 1, iter 1)";
                sqlite3_bind_int64(insertPost, 1, 1 + rand() % 20000);
                sqlite3_bind_text(insertPost, 2, body, sizeof body - 1, SQLITE_STATIC);
                drain(insertPost);
                newest = sqlite3_last_insert_rowid(db);
            }
        }
        sqlite3_step(commit); sqlite3_reset(commit);
        if (++frames % 400 == 0) sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, 0, 0);
        batches++;
    }
    printf("trained, %.0f requests/s, sink %ld\n", batches * 24 / (now() - t0), sink & 0xff);
    sqlite3_close(db);
    return 0;
}
