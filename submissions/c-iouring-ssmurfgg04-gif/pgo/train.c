/* pgo/train.c - trains the SQLite PGO profile on a synthetic database.
 *
 * Creates the challenge schema with small synthetic data (the file is kept,
 * so the same database can then drive the server's PGO run) and runs the
 * exact prepared statements the server runs, in the same mix. Uses no
 * challenge seed data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlite3.h"

static sqlite3 *db;
static sqlite3_stmt *st_feed_ids, *st_like_range, *st_feed_rows, *st_feed;
static sqlite3_stmt *st_post, *st_create, *st_like, *st_exists, *st_ping;
static sqlite3_stmt *st_begin, *st_commit;

static sqlite3_stmt *prep(const char *sql)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s: %s\n", sqlite3_errmsg(db), sql);
        exit(1);
    }
    return s;
}

#define POST_SELECT                                                              \
    "SELECT p.id, p.body, p.created_at, u.username, "                            \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "                      \
    "FROM posts p JOIN users u ON u.id = p.user_id "

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: train <db-path>\n");
        return 2;
    }
    const char *path = argv[1];
    unlink(path);
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "cannot create %s\n", path);
        return 1;
    }
    sqlite3_exec(db,
        "CREATE TABLE users ("
        "  id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
        "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE posts ("
        "  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
        "  body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
        "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE likes ("
        "  user_id INTEGER NOT NULL REFERENCES users(id),"
        "  post_id INTEGER NOT NULL REFERENCES posts(id),"
        "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "  PRIMARY KEY (user_id, post_id));"
        "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
        "CREATE INDEX posts_user_id_idx ON posts (user_id);"
        "CREATE INDEX likes_post_id_idx ON likes (post_id);"
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;",
        NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    char sql[256];
    for (int i = 1; i <= 200; i++) {
        snprintf(sql, sizeof sql,
                 "INSERT INTO users (id, username) VALUES (%d, 'user_%d')", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    static const char *words[] = { "the", "a", "server", "database", "latency", "cache",
                                   "index", "query", "benchmark", "cpu", "memory", "socket" };
    char body[600];
    for (int i = 1; i <= 4000; i++) {
        int n = 6 + i % 30;
        int off = snprintf(body, sizeof body, "post %d says", i);
        for (int j = 0; j < n && off < 480; j++)
            off += snprintf(body + off, sizeof body - (size_t)off, " %s", words[(i + j) % 12]);
        snprintf(sql, sizeof sql, "INSERT INTO posts (id, user_id, body) VALUES (%d, %d, '%s')",
                 i, 1 + i % 200, body);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    for (int i = 0; i < 12000; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO likes (user_id, post_id) VALUES (%d, %d)",
                 1 + i % 200, 1 + i % 4000);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(db, "ANALYZE", NULL, NULL, NULL);

    st_feed = prep(POST_SELECT "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_post = prep(POST_SELECT "WHERE p.id = ?1");
    st_feed_ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    st_like_range = prep("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    st_feed_rows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p "
                        "JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2");
    st_create = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    st_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 "
                   "WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
                   "ON CONFLICT (user_id, post_id) DO NOTHING");
    st_exists = prep("SELECT 1 FROM posts WHERE id = ?1");
    st_ping = prep("SELECT 1");
    st_begin = prep("BEGIN IMMEDIATE");
    st_commit = prep("COMMIT");

    /* the workload mix the server sees */
    for (int round = 0; round < 400; round++) {
        int64_t ids[20], likes[20] = { 0 };
        int n = 0, rc;

        sqlite3_step(st_ping); sqlite3_reset(st_ping);

        while (n < 20 && (rc = sqlite3_step(st_feed_ids)) == SQLITE_ROW)
            ids[n++] = sqlite3_column_int64(st_feed_ids, 0);
        sqlite3_reset(st_feed_ids);
        int64_t mn = ids[0], mx = ids[0];
        for (int k = 1; k < n; k++) {
            if (ids[k] < mn) mn = ids[k];
            if (ids[k] > mx) mx = ids[k];
        }
        if (mx - mn < 256) {
            sqlite3_bind_int64(st_like_range, 1, mn);
            sqlite3_bind_int64(st_like_range, 2, mx);
            while (sqlite3_step(st_like_range) == SQLITE_ROW) { }
            sqlite3_reset(st_like_range);
            sqlite3_bind_int64(st_feed_rows, 1, mn);
            sqlite3_bind_int64(st_feed_rows, 2, mx);
            while (sqlite3_step(st_feed_rows) == SQLITE_ROW) { }
            sqlite3_reset(st_feed_rows);
        } else {
            while (sqlite3_step(st_feed) == SQLITE_ROW) { }
            sqlite3_reset(st_feed);
        }

        for (int k = 0; k < 8; k++) {
            sqlite3_bind_int64(st_post, 1, 1 + (round * 8 + k) % 4000);
            while (sqlite3_step(st_post) == SQLITE_ROW) { }
            sqlite3_reset(st_post);
        }

        /* writes: one small group transaction per round */
        sqlite3_step(st_begin); sqlite3_reset(st_begin);
        for (int k = 0; k < 4; k++) {
            sqlite3_bind_int64(st_create, 1, 1 + k);
            sqlite3_bind_text(st_create, 2, body, 300, SQLITE_STATIC);
            while (sqlite3_step(st_create) == SQLITE_ROW) { }
            sqlite3_reset(st_create);
            sqlite3_bind_int64(st_like, 1, 1 + (round + k) % 200);
            sqlite3_bind_int64(st_like, 2, 1 + (round + k) % 4000);
            sqlite3_step(st_like);
            sqlite3_reset(st_like);
            sqlite3_bind_int64(st_exists, 2, 1 + (round + k) % 4000);
            sqlite3_bind_int64(st_exists, 1, 1 + (round + k) % 4000);
            sqlite3_step(st_exists);
            sqlite3_reset(st_exists);
        }
        sqlite3_step(st_commit); sqlite3_reset(st_commit);
    }

    sqlite3_close(db);
    printf("train: ok (%s kept)\n", path);
    return 0;
}
