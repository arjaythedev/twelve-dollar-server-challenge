/*
 * Profile-guided optimization trainer for SQLite (build.sh, pass 1).
 *
 * Builds a small synthetic database with the challenge schema (no seed data is used or
 * linked into the server) and runs the server's own statements in roughly the load test's
 * mix: per 217 requests, 100 feeds, 100 single posts, 15 likes and 2 new posts, in batched
 * transactions with the same pragmas the server uses.
 *
 *   train <path-to-scratch.db>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

static sqlite3 *db;

static void die(const char *what) {
    fprintf(stderr, "train: %s: %s\n", what, db ? sqlite3_errmsg(db) : "?");
    exit(1);
}

static void exec(const char *sql) {
    if (sqlite3_exec(db, sql, 0, 0, 0) != SQLITE_OK) die(sql);
}

static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, 0) != SQLITE_OK) die(sql);
    return s;
}

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static unsigned next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 11);
}

static int wal_frames;
static int on_wal(void *arg, sqlite3 *d, const char *name, int frames) {
    (void)arg; (void)d; (void)name;
    wal_frames = frames;
    return SQLITE_OK;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: train <db>\n"); return 2; }
    if (sqlite3_initialize() != SQLITE_OK) return 1;
    remove(argv[1]);
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0) != SQLITE_OK) die("open");

    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
         "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
         "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
         "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
         "  body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
         "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
         "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id),"
         "  post_id INTEGER NOT NULL REFERENCES posts(id),"
         "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
         "  PRIMARY KEY (user_id, post_id));"
         "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
         "CREATE INDEX posts_user_id_idx ON posts (user_id);"
         "CREATE INDEX likes_post_id_idx ON likes (post_id);");

    /* 1,000 users, 10,000 posts with increasing timestamps, 40,000 likes. */
    exec("BEGIN");
    {
        sqlite3_stmt *u = prepare("INSERT INTO users(id,username,created_at) VALUES(?1,?2,'2025-01-01T00:00:00.000Z')");
        char name[32];
        for (int i = 1; i <= 1000; i++) {
            snprintf(name, sizeof name, "user_%d", i);
            sqlite3_bind_int(u, 1, i);
            sqlite3_bind_text(u, 2, name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(u) != SQLITE_DONE) die("users");
            sqlite3_reset(u);
        }
        sqlite3_finalize(u);
        sqlite3_stmt *p = prepare("INSERT INTO posts(id,user_id,body,created_at) VALUES(?1,?2,?3,?4)");
        char body[512], ts[32];
        for (int i = 1; i <= 10000; i++) {
            int len = 20 + (int)(next() % 200);
            for (int k = 0; k < len; k++) body[k] = (char)('a' + next() % 26);
            body[len] = 0;
            snprintf(ts, sizeof ts, "2025-06-%02dT%02d:%02d:%02d.%03dZ",
                     1 + i / 400 % 28, i / 60 % 24, i % 60, i * 7 % 60, i % 1000);
            sqlite3_bind_int(p, 1, i);
            sqlite3_bind_int(p, 2, 1 + (int)(next() % 1000));
            sqlite3_bind_text(p, 3, body, len, SQLITE_TRANSIENT);
            sqlite3_bind_text(p, 4, ts, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(p) != SQLITE_DONE) die("posts");
            sqlite3_reset(p);
        }
        sqlite3_finalize(p);
        sqlite3_stmt *l = prepare("INSERT OR IGNORE INTO likes(user_id,post_id) VALUES(?1,?2)");
        for (int i = 0; i < 40000; i++) {
            sqlite3_bind_int(l, 1, 1 + (int)(next() % 1000));
            sqlite3_bind_int(l, 2, 1 + (int)(next() % 10000));
            if (sqlite3_step(l) != SQLITE_DONE) die("likes");
            sqlite3_reset(l);
        }
        sqlite3_finalize(l);
    }
    exec("COMMIT; ANALYZE; PRAGMA wal_checkpoint(TRUNCATE);");
    sqlite3_close(db);

    /* Reopen exactly as the server does. */
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) die("reopen");
    exec("PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
         "PRAGMA foreign_keys=ON; PRAGMA wal_autocheckpoint=0; PRAGMA mmap_size=1073741824;"
         "PRAGMA temp_store=MEMORY; PRAGMA journal_size_limit=67108864;");
    sqlite3_wal_hook(db, on_wal, 0);

    const char *ref = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id)"
                      " FROM posts p JOIN users u ON u.id=p.user_id";
    char sql[512];
    sqlite3_stmt *feedIds = prepare("SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20");
    sqlite3_stmt *likeRange = prepare("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    sqlite3_stmt *postRange = prepare("SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id"
                                      " WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC");
    snprintf(sql, sizeof sql, "%s ORDER BY p.created_at DESC,p.id DESC LIMIT 20", ref);
    sqlite3_stmt *feedRef = prepare(sql);
    snprintf(sql, sizeof sql, "%s WHERE p.id=?1", ref);
    sqlite3_stmt *post = prepare(sql);
    sqlite3_stmt *insPost = prepare("INSERT INTO posts(user_id,body) VALUES(?1,?2)");
    sqlite3_stmt *createdAt = prepare("SELECT created_at FROM posts WHERE id=?1");
    sqlite3_stmt *insLike = prepare("INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2)"
                                    " ON CONFLICT(user_id,post_id) DO NOTHING");
    sqlite3_stmt *exists = prepare("SELECT 1 FROM posts WHERE id=?1");
    sqlite3_stmt *begin = prepare("BEGIN"), *commit = prepare("COMMIT");

    long long sink = 0;
    long long maxId = 10000;
    for (int op = 0; op < 120000; op++) {
        if (op % 48 == 0) { sqlite3_step(begin); sqlite3_reset(begin); }
        unsigned r = next() % 217;
        if (r < 100) {
            long long ids[20]; int n = 0;
            while (sqlite3_step(feedIds) == SQLITE_ROW && n < 20) ids[n++] = sqlite3_column_int64(feedIds, 0);
            sqlite3_reset(feedIds);
            if (n > 0) {
                sqlite3_bind_int64(likeRange, 1, ids[n - 1]);
                sqlite3_bind_int64(likeRange, 2, ids[0]);
                while (sqlite3_step(likeRange) == SQLITE_ROW) sink += sqlite3_column_int64(likeRange, 0);
                sqlite3_reset(likeRange);
                sqlite3_bind_int64(postRange, 1, ids[n - 1]);
                sqlite3_bind_int64(postRange, 2, ids[0]);
                while (sqlite3_step(postRange) == SQLITE_ROW) {
                    sink += sqlite3_column_int64(postRange, 0);
                    sink += sqlite3_column_bytes(postRange, 1) + sqlite3_column_bytes(postRange, 2)
                          + sqlite3_column_bytes(postRange, 3);
                    sink += (long long)(size_t)sqlite3_column_text(postRange, 1);
                }
                sqlite3_reset(postRange);
            }
            if (op % 50 == 0) {
                while (sqlite3_step(feedRef) == SQLITE_ROW) sink += sqlite3_column_int64(feedRef, 4);
                sqlite3_reset(feedRef);
            }
        } else if (r < 200) {
            sqlite3_bind_int64(post, 1, 1 + next() % maxId);
            if (sqlite3_step(post) == SQLITE_ROW)
                sink += sqlite3_column_int64(post, 4) + sqlite3_column_bytes(post, 1)
                      + (long long)(size_t)sqlite3_column_text(post, 3);
            sqlite3_reset(post);
        } else if (r < 215) {
            long long pid = 1 + next() % (maxId + 5);
            sqlite3_bind_int64(insLike, 1, 1 + next() % 1000);
            sqlite3_bind_int64(insLike, 2, pid);
            if (sqlite3_step(insLike) != SQLITE_DONE) die("like");
            sqlite3_reset(insLike);
            if (sqlite3_changes(db) == 0) {
                sqlite3_bind_int64(exists, 1, pid);
                sink += sqlite3_step(exists);
                sqlite3_reset(exists);
            }
        } else {
            char body[160];
            int len = snprintf(body, sizeof body, "user_%u says hi at 2026-10-23T00:00:00.000Z (VU %u, iter %d)",
                               next() % 1000, next() % 100000, op);
            sqlite3_bind_int64(insPost, 1, 1 + next() % 1000);
            sqlite3_bind_text(insPost, 2, body, len, SQLITE_TRANSIENT);
            if (sqlite3_step(insPost) != SQLITE_DONE) die("post");
            sqlite3_reset(insPost);
            long long id = sqlite3_last_insert_rowid(db);
            maxId = id;
            sqlite3_bind_int64(createdAt, 1, id);
            if (sqlite3_step(createdAt) == SQLITE_ROW) sink += sqlite3_column_bytes(createdAt, 0);
            sqlite3_reset(createdAt);
        }
        if (op % 48 == 47) {
            if (sqlite3_step(commit) != SQLITE_DONE) die("commit");
            sqlite3_reset(commit);
            if (wal_frames >= 4096) {
                sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_RESTART, 0, 0);
                wal_frames = 0;
            }
        }
    }
    if (sqlite3_get_autocommit(db) == 0) exec("COMMIT");
    sqlite3_close(db);
    printf("train: done (%lld)\n", sink & 0xff);
    return 0;
}
