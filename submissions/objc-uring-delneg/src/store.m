#include "store.h"

#define POST_COLS "SELECT p.id, p.body, p.created_at, u.username, " \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "

@implementation TDStore {
    sqlite3 *db;
    sqlite3_stmt *sHealth, *sFeed, *sPost, *sCreate, *sLike, *sExists, *sBegin, *sCommit, *sRollback;
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s: %s\n", sql, sqlite3_errmsg(db));
        exit(1);
    }
    return s;
}

static void exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

- (instancetype)initWithPath:(const char *)path {
    pthread_mutex_init(&lock, NULL);
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        fprintf(stderr, "open %s: %s\n", path, db ? sqlite3_errmsg(db) : "?");
        exit(1);
    }
    // EXCLUSIVE before the first read: the WAL index then lives in heap memory
    // and transactions take no file locks.
    exec(db, "PRAGMA locking_mode=EXCLUSIVE");
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA synchronous=NORMAL");
    exec(db, "PRAGMA mmap_size=1073741824");
    exec(db, "PRAGMA cache_size=-32768");
    exec(db, "PRAGMA temp_store=MEMORY");
    exec(db, "SELECT count(*) FROM sqlite_schema"); // takes the exclusive lock now
    sHealth = prep(db, "SELECT 1");
    sFeed = prep(db, POST_COLS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    sPost = prep(db, POST_COLS "WHERE p.id = ?1");
    sCreate = prep(db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    sLike = prep(db, "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 "
                     "WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING");
    sExists = prep(db, "SELECT 1 FROM posts WHERE id = ?1");
    sBegin = prep(db, "BEGIN");
    sCommit = prep(db, "COMMIT");
    sRollback = prep(db, "ROLLBACK");
    return self;
}

TD_INLINE bool run1(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

- (bool)begin { return run1(sBegin); }
- (bool)commit { return run1(sCommit); }
- (void)rollback { if (!sqlite3_get_autocommit(db)) run1(sRollback); }

static void put_post(TDBuf *b, sqlite3_stmt *s) {
    td_buf_lit(b, "{\"id\":");
    td_buf_int(b, sqlite3_column_int64(s, 0));
    td_buf_lit(b, ",\"body\":\"");
    const char *body = (const char *)sqlite3_column_text(s, 1);
    td_buf_json_str(b, body, (uint32_t)sqlite3_column_bytes(s, 1));
    td_buf_lit(b, "\",\"created_at\":\"");
    const char *ca = (const char *)sqlite3_column_text(s, 2);
    td_buf_put(b, ca, (uint32_t)sqlite3_column_bytes(s, 2));
    td_buf_lit(b, "\",\"author\":\"");
    const char *au = (const char *)sqlite3_column_text(s, 3);
    td_buf_json_str(b, au, (uint32_t)sqlite3_column_bytes(s, 3));
    td_buf_lit(b, "\",\"like_count\":");
    td_buf_int(b, sqlite3_column_int64(s, 4));
    td_buf_ch(b, '}');
}

static int fail(TDBuf *b) {
    b->len = 0;
    td_buf_lit(b, "{\"error\":\"internal server error\"}");
    return 500;
}

- (int)health:(TDBuf *)b uptime:(int64_t)up {
    int rc = sqlite3_step(sHealth);
    sqlite3_reset(sHealth);
    if (rc == SQLITE_ROW) {
        td_buf_lit(b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        td_buf_int(b, up);
        td_buf_ch(b, '}');
        return 200;
    }
    td_buf_lit(b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    const char *m = sqlite3_errmsg(db);
    td_buf_json_str(b, m, (uint32_t)strlen(m));
    td_buf_lit(b, "\"}");
    return 503;
}

- (int)feed:(TDBuf *)b {
    sqlite3_stmt *s = sFeed;
    td_buf_lit(b, "{\"posts\":[");
    int rc, n = 0;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (n++) td_buf_ch(b, ',');
        put_post(b, s);
    }
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return fail(b);
    td_buf_lit(b, "]}");
    return 200;
}

- (int)post:(int64_t)pid into:(TDBuf *)b {
    sqlite3_stmt *s = sPost;
    sqlite3_bind_int64(s, 1, pid);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        td_buf_lit(b, "{\"post\":");
        put_post(b, s);
        td_buf_ch(b, '}');
        sqlite3_reset(s);
        return 200;
    }
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return fail(b);
    td_buf_lit(b, "{\"error\":\"post not found\"}");
    return 404;
}

- (int)create:(int64_t)uid name:(const char *)name length:(uint32_t)nl
          body:(const char *)body length:(uint32_t)bl into:(TDBuf *)b {
    sqlite3_stmt *s = sCreate;
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_text(s, 2, body, (int)bl, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW) { sqlite3_reset(s); return fail(b); }
    td_buf_lit(b, "{\"post\":{\"id\":");
    td_buf_int(b, sqlite3_column_int64(s, 0));
    td_buf_lit(b, ",\"body\":\"");
    td_buf_json_str(b, body, bl);
    td_buf_lit(b, "\",\"created_at\":\"");
    td_buf_put(b, sqlite3_column_text(s, 1), (uint32_t)sqlite3_column_bytes(s, 1));
    td_buf_lit(b, "\",\"author\":\"");
    td_buf_json_str(b, name, nl);
    td_buf_lit(b, "\",\"like_count\":0}}");
    rc = sqlite3_step(s); // finish the RETURNING statement so the insert completes
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    if (rc != SQLITE_DONE) return fail(b);
    return 201;
}

- (int)like:(int64_t)uid post:(int64_t)pid into:(TDBuf *)b {
    sqlite3_stmt *s = sLike;
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_int64(s, 2, pid);
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return fail(b);
    bool added = sqlite3_changes(db) == 1;
    if (!added) {
        s = sExists;
        sqlite3_bind_int64(s, 1, pid);
        rc = sqlite3_step(s);
        sqlite3_reset(s);
        if (rc == SQLITE_DONE) {
            td_buf_lit(b, "{\"error\":\"post not found\"}");
            return 404;
        }
        if (rc != SQLITE_ROW) return fail(b);
    }
    td_buf_lit(b, "{\"liked\":true,\"already_liked\":");
    if (added) td_buf_lit(b, "false"); else td_buf_lit(b, "true");
    td_buf_lit(b, ",\"post_id\":");
    td_buf_int(b, pid);
    td_buf_ch(b, '}');
    return added ? 201 : 200;
}
@end
