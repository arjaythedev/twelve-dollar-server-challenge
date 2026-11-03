/* db.c - all SQLite work plus the response bodies for it.
 *
 * Same SQL and byte-identical response bodies as the io_uring submission,
 * but for the libco process model: one single-threaded event loop per
 * process, one connection per process, transactions run inline in the
 * request coroutine and always commit before the response is sent.
 * Cross-process sharing goes through WAL (no EXCLUSIVE locking mode).
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "json.h"
#include "server.h"
#include "sqlite3.h"
#include "util.h"

#define FEED_SPAN 256

static sqlite3 *db;
static sqlite3_stmt *st_feed_ids, *st_like_range, *st_feed_rows, *st_feed;
static sqlite3_stmt *st_post, *st_create, *st_like, *st_exists, *st_ping;
static sqlite3_stmt *st_begin, *st_commit, *st_rollback;
static int in_txn;
static struct timespec start_ts;
static __thread int wal_frames;

static __thread buf_t body; /* scratch for response bodies (one per worker) */

static sqlite3_stmt *prep(const char *sql)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s: %s\n", sqlite3_errmsg(db), sql);
        exit(1);
    }
    return s;
}

static void exec_or_die(const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames)
{
    (void)arg; (void)d; (void)name;
    wal_frames = frames;
    return SQLITE_OK;
}

void db_open(const char *path)
{
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "?");
        exit(1);
    }
    sqlite3_busy_timeout(db, 1000);
    /* No EXCLUSIVE locking mode here: sibling processes share this
     * database through WAL. busy_timeout bounds the worst case event-loop
     * stall when both processes want the write lock at once. */
    exec_or_die(
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA mmap_size=0;"
        "PRAGMA cache_size=-131072;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA journal_size_limit=67108864;"
        "PRAGMA wal_autocheckpoint=0;");
    sqlite3_wal_hook(db, wal_hook, NULL);

    /* the reference post select */
#define POST_SELECT                                                       \
    "SELECT p.id, p.body, p.created_at, u.username, "                     \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "               \
    "FROM posts p JOIN users u ON u.id = p.user_id "
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
    st_rollback = prep("ROLLBACK");

    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* warm the OS page cache so the first requests do not wait on disk */
    int fd = open(path, 0 /* O_RDONLY */);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);
    }
}

void db_close(void)
{
    sqlite3_close(db);
}

/* ------------------------------------------------------------ instrumentation */

static long long note_count, note_sum_us, note_max_us;

void db_req_note(long long us)
{
    note_count++;
    note_sum_us += us;
    if (us > note_max_us) note_max_us = us;
}

void db_checkpoint(void)
{
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}

long long db_wal_bytes(void)
{
    char path[4096];
    const char *p = sqlite3_db_filename(db, "main");
    if (!p || !*p) return -1;
    snprintf(path, sizeof path, "%s-wal", p);
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (long long)st.st_size;
}

long long db_req_count(void) { return note_count; }
long long db_req_sum_us(void) { return note_sum_us; }
long long db_req_max_us(void) { return note_max_us; }
void db_req_reset(void) { note_count = note_sum_us = note_max_us = 0; }

void db_thread_init(void) { /* single-threaded event loop: nothing to do */ }

int db_txn_begin(void)
{
    if (in_txn) return 1;
    int rc = sqlite3_step(st_begin);
    sqlite3_reset(st_begin);
    if (rc != SQLITE_DONE) return 0;
    in_txn = 1;
    return 1;
}

int db_in_txn(void)
{
    return in_txn;
}

int db_commit(void)
{
    if (!in_txn) return 1;
    in_txn = 0;
    int rc = sqlite3_step(st_commit);
    sqlite3_reset(st_commit);
    if (rc != SQLITE_DONE) {
        sqlite3_step(st_rollback);
        sqlite3_reset(st_rollback);
        return 0;
    }
    if (wal_frames >= 4000) {
        sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        wal_frames = 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- writers */

/* JS String.prototype.trim whitespace: ASCII, NBSP and the common Zs set */
static size_t js_trim(const unsigned char **sp, const unsigned char **ep)
{
    const unsigned char *p = *sp, *e = *ep;
    for (;;) {
        if (p < e && (*p == ' ' || (*p >= '\t' && *p <= '\r'))) { p++; continue; }
        if (e - p >= 2 && p[0] == 0xC2 && p[1] == 0xA0) { p += 2; continue; }
        if (e - p >= 3) {
            uint32_t u = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if ((p[0] & 0xF0) == 0xE0 &&
                (u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 ||
                 u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF)) {
                p += 3;
                continue;
            }
        }
        break;
    }
    for (;;) {
        if (e > p && (e[-1] == ' ' || (e[-1] >= '\t' && e[-1] <= '\r'))) { e--; continue; }
        if (e - p >= 2 && e[-2] == 0xC2 && e[-1] == 0xA0) { e -= 2; continue; }
        if (e - p >= 3) {
            const unsigned char *q = e - 3;
            uint32_t u = ((uint32_t)(q[0] & 0x0F) << 12) | ((uint32_t)(q[1] & 0x3F) << 6) | (q[2] & 0x3F);
            if ((q[0] & 0xF0) == 0xE0 &&
                (u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 ||
                 u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF)) {
                e -= 3;
                continue;
            }
        }
        break;
    }
    *sp = p;
    *ep = e;
    return (size_t)(e - p);
}


/* per-endpoint counters (single-threaded event loop; read by server.cpp) */
static long long n_feed, n_post, n_like, n_create, n_health;
long long db_nfeed(void) { return n_feed; }
long long db_npost(void) { return n_post; }
long long db_nlike(void) { return n_like; }
long long db_ncreate(void) { return n_create; }
long long db_nhealth(void) { return n_health; }

void h_create(int slot, int64_t uid, const char *uname, size_t ulen,
              const char *b, size_t blen)
{
    n_create++;
    jfield_t f[1] = { { .name = "body" } };
    int root = json_fields(b, blen, f, 1);
    if (!root) { srv_err(slot, 400, "malformed JSON body"); return; }
    if (f[0].type != JT_STRING) { srv_err(slot, 400, "body is required"); return; }

    char stackbuf[2048];
    char *heap = NULL;
    size_t raw = (size_t)(f[0].ve - f[0].v);
    if (raw > sizeof stackbuf) {
        heap = malloc(raw);
        if (!heap) { srv_err_500(slot); return; }
    }
    char *dst = heap ? heap : stackbuf;
    size_t n = j_unescape(f[0].v, f[0].ve, dst);
    if (n == (size_t)-1) {
        free(heap);
        srv_err(slot, 400, "malformed JSON body");
        return;
    }
    const unsigned char *p = (const unsigned char *)dst, *e = p + n;
    size_t len = js_trim(&p, &e);
    if (len == 0) {
        free(heap);
        srv_err(slot, 400, "body is required");
        return;
    }
    size_t chars = 0;
    for (const unsigned char *q = p; q < e; q++) chars += (*q & 0xC0) != 0x80;
    if (chars > 500) {
        free(heap);
        srv_err(slot, 400, "body must be at most 500 characters");
        return;
    }

    if (!db_txn_begin()) {
        free(heap);
        srv_err_500(slot);
        return;
    }
    sqlite3_bind_int64(st_create, 1, uid);
    sqlite3_bind_text(st_create, 2, (const char *)p, (int)len, SQLITE_STATIC);
    int rc = sqlite3_step(st_create);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(st_create);
        free(heap);
        srv_err_500(slot);
        return;
    }
    int64_t id = sqlite3_column_int64(st_create, 0);
    const unsigned char *ca = sqlite3_column_text(st_create, 1);
    int nca = sqlite3_column_bytes(st_create, 1);

    /* build while the RETURNING row is alive */
    body.len = 0;
    if (buf_reserve(&body, 160 + 6 * (len + ulen + (size_t)nca))) {
        sqlite3_reset(st_create);
        free(heap);
        srv_err_500(slot);
        return;
    }
    char *o = body.p;
    memcpy(o, "{\"post\":{\"id\":", 14); o += 14;
    o = u64_to(o, (uint64_t)id);
    memcpy(o, ",\"body\":\"", 9); o += 9;
    o = j_escape(o, p, len);
    memcpy(o, "\",\"created_at\":\"", 16); o += 16;
    o = j_escape(o, ca, (size_t)nca);
    memcpy(o, "\",\"author\":\"", 12); o += 12;
    o = j_escape(o, (const unsigned char *)uname, ulen);
    memcpy(o, "\",\"like_count\":0}}", 18); o += 18;
    body.len = (uint32_t)(o - body.p);

    rc = sqlite3_step(st_create); /* finish the statement so the row is written */
    sqlite3_reset(st_create);
    free(heap);
    if (rc != SQLITE_DONE) { srv_err_500(slot); return; }
    respond(slot, 201, body.p, body.len, 0);
}

void h_like(int slot, int64_t uid, int64_t pid)
{
    n_like++;
    if (!db_txn_begin()) { srv_err_500(slot); return; }
    sqlite3_bind_int64(st_like, 1, uid);
    sqlite3_bind_int64(st_like, 2, pid);
    int rc = sqlite3_step(st_like);
    sqlite3_reset(st_like);
    if (rc != SQLITE_DONE) { srv_err_500(slot); return; }

    int inserted = sqlite3_changes(db) > 0;
    if (!inserted) {
        /* conflict (already liked) or the post does not exist */
        sqlite3_bind_int64(st_exists, 1, pid);
        rc = sqlite3_step(st_exists);
        sqlite3_reset(st_exists);
        if (rc == SQLITE_DONE) { srv_err(slot, 404, "post not found"); return; }
        if (rc != SQLITE_ROW) { srv_err_500(slot); return; }
    }
    char rb[96], *o = rb;
    memcpy(o, "{\"liked\":true,\"already_liked\":", 30); o += 30;
    if (inserted) { memcpy(o, "false", 5); o += 5; }
    else { memcpy(o, "true", 4); o += 4; }
    memcpy(o, ",\"post_id\":", 11); o += 11;
    o = u64_to(o, (uint64_t)pid);
    *o++ = '}';
    respond(slot, inserted ? 201 : 200, rb, (size_t)(o - rb), 0);
}

/* ---------------------------------------------------------------- readers */

/* Appends one post object from a row of (id, body, created_at, username).
 * Returns the new end pointer, or NULL if out of memory. */
static char *put_post(buf_t *b, sqlite3_stmt *s, int64_t likes)
{
    const unsigned char *pb = sqlite3_column_text(s, 1);
    int nb = sqlite3_column_bytes(s, 1);
    const unsigned char *pc = sqlite3_column_text(s, 2);
    int nc = sqlite3_column_bytes(s, 2);
    const unsigned char *pu = sqlite3_column_text(s, 3);
    int nu = sqlite3_column_bytes(s, 3);
    if (buf_reserve(b, 160 + 6 * ((size_t)nb + (size_t)nc + (size_t)nu))) return NULL;
    char *o = b->p + b->len;
    memcpy(o, "{\"id\":", 6); o += 6;
    o = u64_to(o, (uint64_t)sqlite3_column_int64(s, 0));
    memcpy(o, ",\"body\":\"", 9); o += 9;
    o = j_escape(o, pb, (size_t)nb);
    memcpy(o, "\",\"created_at\":\"", 16); o += 16;
    o = j_escape(o, pc, (size_t)nc);
    memcpy(o, "\",\"author\":\"", 12); o += 12;
    o = j_escape(o, pu, (size_t)nu);
    memcpy(o, "\",\"like_count\":", 15); o += 15;
    o = u64_to(o, (uint64_t)likes);
    *o++ = '}';
    b->len = (uint32_t)(o - b->p);
    return o;
}

/* The feed with the reference query: one correlated count(*) per post. */
static void h_feed_ref(int slot)
{
    body.len = 0;
    BUF_LIT(&body, "{\"posts\":[");
    int rc, first = 1;
    while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
        if (!first) buf_put(&body, ",", 1);
        first = 0;
        if (!put_post(&body, st_feed, sqlite3_column_int64(st_feed, 4))) {
            sqlite3_reset(st_feed);
            srv_err_500(slot);
            return;
        }
    }
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE) { srv_err_500(slot); return; }
    BUF_LIT(&body, "]}");
    respond(slot, 200, body.p, body.len, 0);
}

/* The 20 newest posts nearly always have adjacent ids: three range scans
 * (ids from the index, like counts, rows) beat 20 correlated subqueries.
 * Each response still comes from queries for that request. */
void h_feed(int slot)
{
    n_feed++;
    int64_t ids[20], likes[20] = { 0 };
    uint32_t seg[20], seglen[20];
    int8_t slot_of[FEED_SPAN];
    int n = 0, rc;

    while (n < 20 && (rc = sqlite3_step(st_feed_ids)) == SQLITE_ROW)
        ids[n++] = sqlite3_column_int64(st_feed_ids, 0);
    sqlite3_reset(st_feed_ids);
    if (n < 20 && rc != SQLITE_DONE) { srv_err_500(slot); return; }
    if (!n) { respond(slot, 200, "{\"posts\":[]}", 12, 0); return; }

    int64_t mn = ids[0], mx = ids[0];
    for (int k = 1; k < n; k++) {
        if (ids[k] < mn) mn = ids[k];
        if (ids[k] > mx) mx = ids[k];
    }
    if (mx - mn >= FEED_SPAN) { h_feed_ref(slot); return; }
    memset(slot_of, -1, (size_t)(mx - mn + 1));
    for (int k = 0; k < n; k++) slot_of[ids[k] - mn] = (int8_t)k;

    sqlite3_bind_int64(st_like_range, 1, mn);
    sqlite3_bind_int64(st_like_range, 2, mx);
    while ((rc = sqlite3_step(st_like_range)) == SQLITE_ROW) {
        int8_t k = slot_of[sqlite3_column_int64(st_like_range, 0) - mn];
        if (k >= 0) likes[k]++;
    }
    sqlite3_reset(st_like_range);
    if (rc != SQLITE_DONE) { srv_err_500(slot); return; }

    int found = 0;
    body.len = 0;
    sqlite3_bind_int64(st_feed_rows, 1, mn);
    sqlite3_bind_int64(st_feed_rows, 2, mx);
    while ((rc = sqlite3_step(st_feed_rows)) == SQLITE_ROW) {
        int8_t k = slot_of[sqlite3_column_int64(st_feed_rows, 0) - mn];
        if (k < 0) continue;
        seg[k] = body.len;
        if (!put_post(&body, st_feed_rows, likes[k])) {
            sqlite3_reset(st_feed_rows);
            srv_err_500(slot);
            return;
        }
        seglen[k] = body.len - seg[k];
        found++;
    }
    sqlite3_reset(st_feed_rows);
    if (rc != SQLITE_DONE) { srv_err_500(slot); return; }
    if (found != n) { h_feed_ref(slot); return; } /* concurrent writer: retry */

    size_t total = 10 + body.len + (size_t)(n - 1) + 2;
    char *o = resp_alloc(slot, 200, total, 0);
    if (!o) return;
    memcpy(o, "{\"posts\":[", 10); o += 10;
    for (int k = 0; k < n; k++) {
        if (k) *o++ = ',';
        memcpy(o, body.p + seg[k], seglen[k]);
        o += seglen[k];
    }
    memcpy(o, "]}", 2);
}

void h_post(int slot, int64_t id)
{
    n_post++;
    sqlite3_bind_int64(st_post, 1, id);
    int rc = sqlite3_step(st_post);
    if (rc == SQLITE_ROW) {
        body.len = 0;
        BUF_LIT(&body, "{\"post\":");
        if (!put_post(&body, st_post, sqlite3_column_int64(st_post, 4))) {
            sqlite3_reset(st_post);
            srv_err_500(slot);
            return;
        }
        BUF_LIT(&body, "}");
        sqlite3_reset(st_post);
        respond(slot, 200, body.p, body.len, 0);
        return;
    }
    sqlite3_reset(st_post);
    if (rc == SQLITE_DONE) { srv_err(slot, 404, "post not found"); return; }
    srv_err_500(slot);
}

void h_health(int slot)
{
    n_health++;
    int rc = sqlite3_step(st_ping);
    sqlite3_reset(st_ping);
    if (rc == SQLITE_ROW) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        char rb[96], *o = rb;
        memcpy(o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36); o += 36;
        o = u64_to(o, (uint64_t)(ts.tv_sec - start_ts.tv_sec));
        *o++ = '}';
        respond(slot, 200, rb, (size_t)(o - rb), 0);
        return;
    }
    const char *m = sqlite3_errmsg(db);
    body.len = 0;
    BUF_LIT(&body, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    if (buf_reserve(&body, 6 * strlen(m) + 32)) {
        respond(slot, 503, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"\"}", 51, 0);
        return;
    }
    body.len = (uint32_t)(j_escape(body.p + body.len, (const unsigned char *)m, strlen(m)) - body.p);
    BUF_LIT(&body, "\"}");
    respond(slot, 503, body.p, body.len, 0);
}
