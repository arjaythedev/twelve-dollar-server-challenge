/* Build-time PGO trainer for the SQLite amalgamation.
 *
 * It creates a disposable synthetic database with the challenge schema and runs
 * the server's own statements in the load test's proportions. It never reads the
 * challenge seed and is not part of the shipped binary.
 *
 * Kept in C because the profile it produces is a gcc profile for sqlite3.c. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

static const char *SCHEMA =
    "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
    "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
    " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
    "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id),"
    " post_id INTEGER NOT NULL REFERENCES posts(id),"
    " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
    " PRIMARY KEY (user_id, post_id));"
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
    "CREATE INDEX posts_user_id_idx ON posts (user_id);"
    "CREATE INDEX likes_post_id_idx ON likes (post_id);";

static void bail(sqlite3 *db, const char *what)
{
    fprintf(stderr, "pgo: %s: %s\n", what, db ? sqlite3_errmsg(db) : "?");
    exit(1);
}

static void exec(sqlite3 *db, const char *sql)
{
    char *err = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
        fprintf(stderr, "pgo: %s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "train.db";
    remove(path);

    sqlite3 *db = 0;
    if (sqlite3_open(path, &db) != SQLITE_OK) bail(db, "open");
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA synchronous=NORMAL");
    exec(db, "PRAGMA temp_store=MEMORY");
    exec(db, SCHEMA);

    const int USERS = 5000, POSTS = 40000, LIKES = 160000;
    char body[160];
    sqlite3_stmt *ins_u, *ins_p, *ins_l;
    sqlite3_prepare_v2(db, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &ins_u, 0);
    sqlite3_prepare_v2(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2)", -1, &ins_p, 0);
    sqlite3_prepare_v2(db, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &ins_l, 0);

    exec(db, "BEGIN");
    for (int i = 1; i <= USERS; i++) {
        snprintf(body, sizeof body, "train_user_%d", i);
        sqlite3_bind_int64(ins_u, 1, i);
        sqlite3_bind_text(ins_u, 2, body, -1, SQLITE_STATIC);
        if (sqlite3_step(ins_u) != SQLITE_DONE) bail(db, "user");
        sqlite3_reset(ins_u);
    }
    for (int i = 1; i <= POSTS; i++) {
        snprintf(body, sizeof body,
                 "training post %d with some ordinary words to exercise the serializer", i);
        sqlite3_bind_int64(ins_p, 1, (i % USERS) + 1);
        sqlite3_bind_text(ins_p, 2, body, -1, SQLITE_STATIC);
        if (sqlite3_step(ins_p) != SQLITE_DONE) bail(db, "post");
        sqlite3_reset(ins_p);
    }
    unsigned seed = 12345;
    for (int i = 0; i < LIKES; i++) {
        seed = seed * 1103515245u + 12345u;
        int pid = (int)((seed >> 8) % (unsigned)POSTS) + 1;
        seed = seed * 1103515245u + 12345u;
        int uid = (int)((seed >> 8) % (unsigned)USERS) + 1;
        sqlite3_bind_int64(ins_l, 1, uid);
        sqlite3_bind_int64(ins_l, 2, pid);
        sqlite3_step(ins_l);
        sqlite3_reset(ins_l);
    }
    exec(db, "COMMIT");
    sqlite3_finalize(ins_u);
    sqlite3_finalize(ins_p);
    sqlite3_finalize(ins_l);

    const char *cols =
        "SELECT p.id, p.body, p.created_at, u.username,"
        " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
        " FROM posts p JOIN users u ON u.id = p.user_id";
    char sql[512];
    sqlite3_stmt *q_feed, *q_get, *q_ins, *q_like, *q_exists;
    snprintf(sql, sizeof sql, "%s ORDER BY p.created_at DESC, p.id DESC LIMIT 20", cols);
    sqlite3_prepare_v2(db, sql, -1, &q_feed, 0);
    snprintf(sql, sizeof sql, "%s WHERE p.id = ?1", cols);
    sqlite3_prepare_v2(db, sql, -1, &q_get, 0);
    sqlite3_prepare_v2(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2)"
                           " RETURNING id, created_at", -1, &q_ins, 0);
    sqlite3_prepare_v2(db, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2)"
                           " ON CONFLICT (user_id, post_id) DO NOTHING", -1, &q_like, 0);
    sqlite3_prepare_v2(db, "SELECT 1 FROM posts WHERE id = ?1", -1, &q_exists, 0);

    for (int i = 0; i < 2000; i++) {
        while (sqlite3_step(q_feed) == SQLITE_ROW) {
        }
        sqlite3_reset(q_feed);

        int64_t pid = (int64_t)(i * 37 % POSTS) + 1;
        sqlite3_bind_int64(q_get, 1, pid);
        sqlite3_step(q_get);
        sqlite3_reset(q_get);

        sqlite3_bind_int64(q_exists, 1, pid);
        sqlite3_step(q_exists);
        sqlite3_reset(q_exists);

        sqlite3_bind_int64(q_like, 1, (i % USERS) + 1);
        sqlite3_bind_int64(q_like, 2, pid);
        sqlite3_step(q_like);
        sqlite3_reset(q_like);

        if (i % 50 == 0) {
            snprintf(body, sizeof body, "pgo insert %d", i);
            sqlite3_bind_int64(q_ins, 1, (i % USERS) + 1);
            sqlite3_bind_text(q_ins, 2, body, -1, SQLITE_STATIC);
            sqlite3_step(q_ins);
            sqlite3_reset(q_ins);
        }
    }

    sqlite3_finalize(q_feed);
    sqlite3_finalize(q_get);
    sqlite3_finalize(q_ins);
    sqlite3_finalize(q_like);
    sqlite3_finalize(q_exists);
    sqlite3_close(db);
    remove(path);
    return 0;
}
