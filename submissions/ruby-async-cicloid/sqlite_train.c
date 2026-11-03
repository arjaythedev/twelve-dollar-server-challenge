/* Profile-guided optimization workload for SQLite, run by install.sh against an instrumented build.
 * Builds a synthetic database with the challenge schema, then steps the server's exact statements
 * (prepared once, as the server does) in roughly the load test's mix. Not part of the server.
 *
 *   sqlite_train <scratch.db>
 */
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

#define POST_JSON "json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username," \
                  "'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))"

static sqlite3 *db;

static void exec(const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) { fprintf(stderr, "%s\n", error); exit(1); }
}

static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *statement;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement, NULL) != SQLITE_OK) {
        fprintf(stderr, "%s\n", sqlite3_errmsg(db));
        exit(1);
    }
    return statement;
}

static void run(sqlite3_stmt *statement) {
    while (sqlite3_step(statement) == SQLITE_ROW) (void)sqlite3_column_text(statement, 0);
    sqlite3_reset(statement);
}

int main(int argc, char **argv) {
    if (argc < 2 || sqlite3_open(argv[1], &db) != SQLITE_OK) { fprintf(stderr, "usage: sqlite_train <db>\n"); return 2; }
    exec("PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
         "PRAGMA foreign_keys=ON; PRAGMA wal_autocheckpoint=0;");
    exec("CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
         " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
         "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
         " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
         " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
         "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),"
         " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
         "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
         "CREATE INDEX posts_user_id_idx ON posts (user_id);"
         "CREATE INDEX likes_post_id_idx ON likes (post_id);");
    exec("BEGIN;"
         "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<5000)"
         " INSERT INTO users(id,username) SELECT i,'user_'||i FROM n;"
         "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<50000)"
         " INSERT INTO posts(id,user_id,body,created_at) SELECT i,1+i%5000,'post body number '||i||' with some text',"
         " strftime('%Y-%m-%dT%H:%M:%fZ','2025-01-01','+'||i||' seconds') FROM n;"
         "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<200000)"
         " INSERT OR IGNORE INTO likes(user_id,post_id) SELECT 1+(i*7)%5000,1+(i*13)%50000 FROM n;"
         "COMMIT; ANALYZE;");

    sqlite3_stmt *feed = prepare(
        "SELECT '{\"posts\":['||coalesce(group_concat(doc,',' ORDER BY created_at DESC,id DESC),'')||']}' "
        "FROM (SELECT " POST_JSON " AS doc,p.created_at,p.id FROM posts p JOIN users u ON u.id=p.user_id "
        "ORDER BY p.created_at DESC,p.id DESC LIMIT 20)");
    sqlite3_stmt *post = prepare("SELECT '{\"post\":'||" POST_JSON "||'}' FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=+?");
    sqlite3_stmt *create = prepare(
        "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING '{\"post\":'||json_object('id',id,"
        "'body',body,'created_at',created_at,'author',?3,'like_count',0)||'}'");
    sqlite3_stmt *like = prepare(
        "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) "
        "ON CONFLICT(user_id,post_id) DO NOTHING");
    sqlite3_stmt *exists = prepare("SELECT 1 FROM posts WHERE id=?");
    sqlite3_stmt *ping = prepare("SELECT 1");

    unsigned seed = 12345;
    for (int i = 0; i < 40000; i++) {
        seed = seed * 1103515245u + 12345u;
        run(feed);
        sqlite3_bind_int64(post, 1, 1 + seed % 50000);
        run(post);
        if (i % 7 == 0) {
            exec("BEGIN IMMEDIATE");
            sqlite3_bind_int64(like, 1, 1 + seed % 5000);
            sqlite3_bind_int64(like, 2, 1 + (seed >> 8) % 50000);
            run(like);
            if (sqlite3_changes(db) == 0) { sqlite3_bind_int64(exists, 1, 1 + (seed >> 8) % 50000); run(exists); }
            if (i % 49 == 0) {
                sqlite3_bind_int64(create, 1, 1 + seed % 5000);
                sqlite3_bind_text(create, 2, "a freshly written post body", -1, SQLITE_STATIC);
                sqlite3_bind_text(create, 3, "user_1", -1, SQLITE_STATIC);
                run(create);
            }
            exec("COMMIT");
        }
        if (i % 1000 == 0) { run(ping); exec("PRAGMA wal_checkpoint(PASSIVE)"); }
    }
    sqlite3_close(db);
    return 0;
}
