#include "store.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

int td_opt_ckpt = 250;    // wal_autocheckpoint pages (--ckpt=N): smaller, more frequent checkpoint stalls
int td_opt_ckptproc = 1; // --ckptproc=0: inline autocheckpoint, EXCLUSIVE locking
long long td_opt_mmap = 0; // mmap_size bytes (--mmap=N); 0: page cache reads beat per-page mmap objects

#define POST_COLS "SELECT p.id, p.body, p.created_at, u.username, " \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "

@implementation TDStore {
    sqlite3 *db;
    sqlite3_stmt *sFeedIds, *sFeedRange, *sLikeRange;
    TDBuf rows;
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

static int wal_frames_hook(void *p, sqlite3 *d, const char *name, int frames) {
    (void)d; (void)name;
    ((TDStore *)p)->walFrames = frames;
    return SQLITE_OK;
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
    // Inline mode: EXCLUSIVE before the first read, so the WAL index lives in heap
    // memory and transactions take no file locks. Checkpointer mode: NORMAL
    // locking (shared-memory WAL index), since a second process checkpoints.
    if (!td_opt_ckptproc) exec(db, "PRAGMA locking_mode=EXCLUSIVE");
    exec(db, "PRAGMA journal_mode=WAL");
    exec(db, "PRAGMA synchronous=NORMAL");
    char mm[64];
    snprintf(mm, sizeof mm, "PRAGMA mmap_size=%lld", td_opt_mmap);
    exec(db, mm);
    exec(db, "PRAGMA cache_size=-32768");
    exec(db, "PRAGMA temp_store=MEMORY");
    if (td_opt_ckptproc) sqlite3_wal_hook(db, wal_frames_hook, self);   // replaces autocheckpoint
    else sqlite3_wal_autocheckpoint(db, td_opt_ckpt);
    exec(db, "SELECT count(*) FROM sqlite_schema"); // takes the exclusive lock now
    sHealth = prep(db, "SELECT 1");
    sFeed = prep(db, POST_COLS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    sPost = prep(db, POST_COLS "WHERE p.id = ?1");
    sFeedIds = prep(db, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    sFeedRange = prep(db, "SELECT p.id, p.body, p.created_at, u.username FROM posts p "
                          "JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2");
    sLikeRange = prep(db, "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    // No RETURNING: it buffers rows in an ephemeral table (a page cache built and
    // torn down per insert). created_at is written in the schema default's format.
    sCreate = prep(db, "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)");
    sLike = prep(db, "INSERT INTO likes (user_id, post_id, created_at) SELECT ?1, ?2, ?3 "
                     "WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING");
    sExists = prep(db, "SELECT 1 FROM posts WHERE id = ?1");
    sBegin = prep(db, "BEGIN");
    sCommit = prep(db, "COMMIT");
    sRollback = prep(db, "ROLLBACK");
    return self;
}

- (void)useInlineCheckpoints {
    sqlite3_wal_autocheckpoint(db, td_opt_ckpt);
}

// UTC "YYYY-MM-DDTHH:MM:SS.mmmZ" (24 bytes, no NUL), as strftime('%Y-%m-%dT%H:%M:%fZ','now').
static void iso_now(char *o) {
    static time_t lastSec = -1;
    static char head[20];               // "YYYY-MM-DDTHH:MM:SS"
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    if (t.tv_sec != lastSec) {
        struct tm g;
        gmtime_r(&t.tv_sec, &g);
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d:%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                 g.tm_hour, g.tm_min, g.tm_sec);
        memcpy(head, tmp, 19);
        lastSec = t.tv_sec;
    }
    memcpy(o, head, 19);
    unsigned ms = (unsigned)(t.tv_nsec / 1000000);
    o[19] = '.';
    o[20] = (char)('0' + ms / 100);
    o[21] = (char)('0' + ms / 10 % 10);
    o[22] = (char)('0' + ms % 10);
    o[23] = 'Z';
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

// The general feed query: one correlated count per post.
- (int)feedGeneral:(TDBuf *)b TD_DIRECT {
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

#define FEED_SPAN 256
typedef struct { uint32_t body, bodyLen, ca, caLen, au, auLen; int64_t likes; bool seen; } FeedRow;

// Finds the 20 newest ids from the (created_at, id) index. When they sit in a
// narrow id range, fetches their rows with one rowid range scan and their like
// counts by tallying one likes_post_id_idx range scan, then restores the index order.
// All three statements run in the batch's transaction, so they see one snapshot.
- (int)feed:(TDBuf *)b {
    int64_t ids[20];
    int n = 0, rc;
    sqlite3_stmt *s = sFeedIds;
    while (n < 20 && (rc = sqlite3_step(s)) == SQLITE_ROW) ids[n++] = sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
    if (n == 0) { td_buf_lit(b, "{\"posts\":[]}"); return 200; }
    int64_t lo = ids[0], hi = ids[0];
    for (int i = 1; i < n; i++) { if (ids[i] < lo) lo = ids[i]; if (ids[i] > hi) hi = ids[i]; }
    if (hi - lo >= FEED_SPAN) return [self feedGeneral:b];
    int8_t pos[FEED_SPAN];
    memset(pos, -1, sizeof pos);
    for (int i = 0; i < n; i++) pos[ids[i] - lo] = (int8_t)i;
    FeedRow row[20];
    memset(row, 0, sizeof row);
    rows.len = 0;
    s = sFeedRange;
    sqlite3_bind_int64(s, 1, lo);
    sqlite3_bind_int64(s, 2, hi);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(s, 0);
        int k = pos[id - lo];
        if (k < 0) continue;
        FeedRow *r = &row[k];
        r->seen = true;
        r->bodyLen = (uint32_t)sqlite3_column_bytes(s, 1);
        r->body = rows.len; td_buf_put(&rows, sqlite3_column_text(s, 1), r->bodyLen);
        r->caLen = (uint32_t)sqlite3_column_bytes(s, 2);
        r->ca = rows.len; td_buf_put(&rows, sqlite3_column_text(s, 2), r->caLen);
        r->auLen = (uint32_t)sqlite3_column_bytes(s, 3);
        r->au = rows.len; td_buf_put(&rows, sqlite3_column_text(s, 3), r->auLen);
    }
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return fail(b);
    s = sLikeRange;
    sqlite3_bind_int64(s, 1, lo);
    sqlite3_bind_int64(s, 2, hi);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        int k = pos[sqlite3_column_int64(s, 0) - lo];
        if (k >= 0) row[k].likes++;
    }
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return fail(b);
    td_buf_lit(b, "{\"posts\":[");
    bool first = true;
    for (int i = 0; i < n; i++) {
        FeedRow *r = &row[i];
        if (!r->seen) continue;
        if (!first) td_buf_ch(b, ',');
        first = false;
        td_buf_lit(b, "{\"id\":");
        td_buf_int(b, ids[i]);
        td_buf_lit(b, ",\"body\":\"");
        td_buf_json_str(b, rows.p + r->body, r->bodyLen);
        td_buf_lit(b, "\",\"created_at\":\"");
        td_buf_put(b, rows.p + r->ca, r->caLen);
        td_buf_lit(b, "\",\"author\":\"");
        td_buf_json_str(b, rows.p + r->au, r->auLen);
        td_buf_lit(b, "\",\"like_count\":");
        td_buf_int(b, r->likes);
        td_buf_ch(b, '}');
    }
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
    char ts[24];
    iso_now(ts);
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_text(s, 2, body, (int)bl, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, ts, 24, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return fail(b);
    td_buf_lit(b, "{\"post\":{\"id\":");
    td_buf_int(b, sqlite3_last_insert_rowid(db));
    td_buf_lit(b, ",\"body\":\"");
    td_buf_json_str(b, body, bl);
    td_buf_lit(b, "\",\"created_at\":\"");
    td_buf_put(b, ts, 24);
    td_buf_lit(b, "\",\"author\":\"");
    td_buf_json_str(b, name, nl);
    td_buf_lit(b, "\",\"like_count\":0}}");
    return 201;
}

- (int)like:(int64_t)uid post:(int64_t)pid into:(TDBuf *)b {
    sqlite3_stmt *s = sLike;
    sqlite3_bind_int64(s, 1, uid);
    sqlite3_bind_int64(s, 2, pid);
    char ts[24];
    iso_now(ts);
    sqlite3_bind_text(s, 3, ts, 24, SQLITE_STATIC);
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

// ---------------------------------------------------------------- checkpointer process
// Runs in a child forked before SQLite or io_uring exist in the parent. For each
// request byte it runs one PASSIVE checkpoint on its own connection (NORMAL
// locking, never writes) and answers {rc, frames in WAL, frames backfilled}.
void td_ckpt_child(const char *path, int req, int done) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    sqlite3 *cdb = NULL;
    char c;
    for (;;) {
        ssize_t n = read(req, &c, 1);
        if (n == 0) _exit(0);
        if (n < 0) { if (errno == EINTR) continue; _exit(1); }
        if (!cdb) {
            if (sqlite3_open_v2(path, &cdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) _exit(1);
            sqlite3_busy_timeout(cdb, 0);
            exec(cdb, "PRAGMA synchronous=NORMAL");
            sqlite3_wal_autocheckpoint(cdb, 0);
        }
        int r[3] = { 0, -1, -1 };
        r[0] = sqlite3_wal_checkpoint_v2(cdb, "main", SQLITE_CHECKPOINT_PASSIVE, &r[1], &r[2]);
        if (write(done, r, sizeof r) != (ssize_t)sizeof r) _exit(1);
    }
}
