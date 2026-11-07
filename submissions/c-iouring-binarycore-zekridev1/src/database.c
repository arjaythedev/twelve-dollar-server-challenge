#define _GNU_SOURCE
#include "database.h"

#include <stdio.h>
#include <string.h>

static int bc_db_exec(sqlite3 *c, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(c, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s: %s\n", sql, err ? err : sqlite3_errstr(rc));
        sqlite3_free(err);
    }
    return rc;
}

static int bc_db_prep(sqlite3 *c, sqlite3_stmt **st, const char *sql) {
    int rc = sqlite3_prepare_v3(c, sql, -1, SQLITE_PREPARE_PERSISTENT, st, NULL);
    if (rc != SQLITE_OK)
        fprintf(stderr, "sqlite prepare: %s: %s\n", sql, sqlite3_errmsg(c));
    return rc;
}

static int bc_db_open_conn(sqlite3 **out, const char *path, int cache_kib) {
    int rc = sqlite3_open_v2(path, out,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX |
                             SQLITE_OPEN_EXRESCODE,
                             NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite open %s: %s\n", path,
                sqlite3_errmsg(*out));
        return -1;
    }
    char sql[128];
    snprintf(sql, sizeof sql, "PRAGMA mmap_size=0");
    bc_db_exec(*out, sql);
    snprintf(sql, sizeof sql, "PRAGMA cache_size=-%d", cache_kib);
    bc_db_exec(*out, sql);
    bc_db_exec(*out, "PRAGMA synchronous=NORMAL");
    bc_db_exec(*out, "PRAGMA temp_store=MEMORY");
    bc_db_exec(*out, "PRAGMA wal_autocheckpoint=0");
    sqlite3_busy_timeout(*out, 2000);
    /* Confirm WAL mode. The seed is already WAL; make sure and refuse anything
     * else. */
    sqlite3_stmt *jm;
    if (bc_db_prep(*out, &jm, "PRAGMA journal_mode=WAL") != SQLITE_OK) return -1;
    int rc2 = sqlite3_step(jm);
    if (rc2 != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(jm, 0), "wal") != 0) {
        fprintf(stderr, "sqlite: could not enable WAL\n");
        sqlite3_finalize(jm);
        return -1;
    }
    sqlite3_finalize(jm);
    return 0;
}

int bc_db_open(struct bc_db *d, const char *path, int cache_kib) {
    memset(d, 0, sizeof *d);
    if (sqlite3_initialize() != SQLITE_OK) {
        fprintf(stderr, "sqlite3_initialize failed\n");
        return -1;
    }
    if (bc_db_open_conn(&d->c, path, cache_kib) < 0) return -1;

    if (bc_db_prep(d->c, &d->st_begin, "BEGIN") ||
        bc_db_prep(d->c, &d->st_commit, "COMMIT") ||
        bc_db_prep(d->c, &d->st_rollback, "ROLLBACK") ||
        bc_db_prep(d->c, &d->st_health, "SELECT 1") ||
        bc_db_prep(d->c, &d->st_feed,
                   "SELECT p.id, p.body, p.created_at, u.username, "
                   "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                   "FROM posts p JOIN users u ON u.id = p.user_id "
                   "ORDER BY p.created_at DESC, p.id DESC LIMIT 20") ||
        bc_db_prep(d->c, &d->st_post,
                   "SELECT p.id, p.body, p.created_at, u.username, "
                   "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                   "FROM posts p JOIN users u ON u.id = p.user_id "
                   "WHERE p.id = ?1") ||
        bc_db_prep(d->c, &d->st_ins_post,
                   "INSERT INTO posts (user_id, body) VALUES (?1, ?2)") ||
        bc_db_prep(d->c, &d->st_created_at,
                   "SELECT created_at FROM posts WHERE id = ?1") ||
        bc_db_prep(d->c, &d->st_ins_like,
                   "INSERT INTO likes (user_id, post_id) "
                   "SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
                   "ON CONFLICT (user_id, post_id) DO NOTHING") ||
        bc_db_prep(d->c, &d->st_post_exists,
                   "SELECT 1 FROM posts WHERE id = ?1"))
        return -1;
    return 0;
}

void bc_db_close(struct bc_db *d) {
    if (d->st_begin) sqlite3_finalize(d->st_begin);
    if (d->st_commit) sqlite3_finalize(d->st_commit);
    if (d->st_rollback) sqlite3_finalize(d->st_rollback);
    if (d->st_health) sqlite3_finalize(d->st_health);
    if (d->st_feed) sqlite3_finalize(d->st_feed);
    if (d->st_post) sqlite3_finalize(d->st_post);
    if (d->st_ins_post) sqlite3_finalize(d->st_ins_post);
    if (d->st_created_at) sqlite3_finalize(d->st_created_at);
    if (d->st_ins_like) sqlite3_finalize(d->st_ins_like);
    if (d->st_post_exists) sqlite3_finalize(d->st_post_exists);
    if (d->c) sqlite3_close(d->c);
    if (d->c2) sqlite3_close(d->c2);
    memset(d, 0, sizeof *d);
}

/* Write a post object into the ob builder. */
static inline void bc_ob_post_obj(struct bc_ob *o, int64_t id,
                                  const uint8_t *body, int body_n,
                                  const uint8_t *created, int created_n,
                                  const uint8_t *author, int author_n,
                                  int64_t likes) {
    bc_ob_lit(o, "{\"id\":");
    bc_ob_u64(o, (uint64_t)id);
    bc_ob_lit(o, ",\"body\":");
    bc_ob_json_str(o, body, (size_t)body_n);
    bc_ob_lit(o, ",\"created_at\":");
    bc_ob_json_str(o, created, (size_t)created_n);
    bc_ob_lit(o, ",\"author\":");
    bc_ob_json_str(o, author, (size_t)author_n);
    bc_ob_lit(o, ",\"like_count\":");
    bc_ob_u64(o, (uint64_t)likes);
    bc_ob_char(o, '}');
}

int bc_db_feed(struct bc_db *d, struct bc_ob *o) {
    if (bc_db_begin(d) < 0) return -1;
    bc_ob_lit(o, "{\"posts\":[");
    int n = 0, rc;
    while ((rc = sqlite3_step(d->st_feed)) == SQLITE_ROW) {
        if (n++) bc_ob_char(o, ',');
        int64_t likes = sqlite3_column_int64(d->st_feed, 4);
        bc_ob_post_obj(o,
                       sqlite3_column_int64(d->st_feed, 0),
                       (const uint8_t *)sqlite3_column_text(d->st_feed, 1),
                       sqlite3_column_bytes(d->st_feed, 1),
                       (const uint8_t *)sqlite3_column_text(d->st_feed, 2),
                       sqlite3_column_bytes(d->st_feed, 2),
                       (const uint8_t *)sqlite3_column_text(d->st_feed, 3),
                       sqlite3_column_bytes(d->st_feed, 3),
                       likes);
        if (o->err) { sqlite3_reset(d->st_feed); return -1; }
    }
    sqlite3_reset(d->st_feed);
    if (rc != SQLITE_DONE) return -1;
    bc_ob_lit(o, "]}");
    return 0;
}

int bc_db_post(struct bc_db *d, struct bc_ob *o, int64_t id) {
    if (bc_db_begin(d) < 0) return -1;
    sqlite3_bind_int64(d->st_post, 1, id);
    int rc = sqlite3_step(d->st_post);
    sqlite3_reset(d->st_post);
    if (rc == SQLITE_ROW) {
        bc_ob_lit(o, "{\"post\":");
        int64_t likes = sqlite3_column_int64(d->st_post, 4);
        bc_ob_post_obj(o,
                       sqlite3_column_int64(d->st_post, 0),
                       (const uint8_t *)sqlite3_column_text(d->st_post, 1),
                       sqlite3_column_bytes(d->st_post, 1),
                       (const uint8_t *)sqlite3_column_text(d->st_post, 2),
                       sqlite3_column_bytes(d->st_post, 2),
                       (const uint8_t *)sqlite3_column_text(d->st_post, 3),
                       sqlite3_column_bytes(d->st_post, 3),
                       likes);
        bc_ob_char(o, '}');
        if (o->err) return -1;
        return 1;
    }
    return rc == SQLITE_DONE ? 0 : -1;
}

int bc_db_create(struct bc_db *d, struct bc_ob *o, int64_t user_id,
                 const uint8_t *body, unsigned body_len,
                 const uint8_t *author, unsigned author_len) {
    if (bc_db_begin(d) < 0) return -1;
    sqlite3_bind_int64(d->st_ins_post, 1, user_id);
    sqlite3_bind_text(d->st_ins_post, 2, (const char *)body, (int)body_len,
                      SQLITE_STATIC);
    int rc = sqlite3_step(d->st_ins_post);
    sqlite3_reset(d->st_ins_post);
    if (rc != SQLITE_DONE) return -1;
    int64_t id = sqlite3_last_insert_rowid(d->c);
    sqlite3_bind_int64(d->st_created_at, 1, id);
    rc = sqlite3_step(d->st_created_at);
    if (rc != SQLITE_ROW) { sqlite3_reset(d->st_created_at); return -1; }
    bc_ob_lit(o, "{\"post\":");
    bc_ob_post_obj(o, id, body, (int)body_len,
                   (const uint8_t *)sqlite3_column_text(d->st_created_at, 0),
                   sqlite3_column_bytes(d->st_created_at, 0),
                   author, (int)author_len, 0);
    bc_ob_char(o, '}');
    sqlite3_reset(d->st_created_at);
    if (o->err) return -1;
    return 0;
}

int bc_db_like(struct bc_db *d, int64_t user_id, int64_t post_id) {
    if (bc_db_begin(d) < 0) return -1;
    sqlite3_bind_int64(d->st_ins_like, 1, user_id);
    sqlite3_bind_int64(d->st_ins_like, 2, post_id);
    int rc = sqlite3_step(d->st_ins_like);
    sqlite3_reset(d->st_ins_like);
    if (rc != SQLITE_DONE) return -1;
    if (sqlite3_changes(d->c) == 1) return 1;   /* liked now */
    /* Already liked or the insert was skipped. Check whether the post exists. */
    sqlite3_bind_int64(d->st_post_exists, 1, post_id);
    rc = sqlite3_step(d->st_post_exists);
    sqlite3_reset(d->st_post_exists);
    if (rc == SQLITE_ROW) return 2;   /* already liked */
    return rc == SQLITE_DONE ? 0 : -1; /* 0 = no such post */
}
