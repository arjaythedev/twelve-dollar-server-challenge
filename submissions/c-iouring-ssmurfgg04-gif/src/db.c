/* db.c - SQLite: one read connection per worker (WAL snapshots), one
 * writer connection guarded by a short mutex. Reads never block each other
 * or the event loops; writes run begin..commit inside the mutex (WAL with
 * synchronous=NORMAL makes commits cheap, so no cross-request grouping is
 * needed and responses can be sent immediately). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "json.h"
#include "server.h"
#include "sqlite3.h"
#include "util.h"

#define FEED_SPAN 256

static sqlite3 *wdb;                    /* the one writer */
static pthread_mutex_t write_mu = PTHREAD_MUTEX_INITIALIZER;
static struct timespec start_ts;
static volatile int db_stop;           /* stops the checkpointer thread */
static sqlite3_stmt *wst_create, *wst_like, *wst_exists; /* on wdb, under write_mu */

/* instrumentation: feed handler time and busy counts, logged by the
 * checkpointer thread so bench runs leave evidence in the server log */
static _Atomic long long feed_n, feed_us, feed_max_us, feed_busy;
static _Atomic long long post_n, post_us, post_busy;
static _Atomic long long req_n, req_us, req_max_us;

void db_req_note(long long us)
{
    atomic_fetch_add(&req_n, 1);
    atomic_fetch_add(&req_us, us);
    long long mx = atomic_load(&req_max_us);
    if (us > mx) atomic_compare_exchange_strong(&req_max_us, &mx, us);
}

static inline void post_note(long long us)
{
    atomic_fetch_add(&post_n, 1);
    atomic_fetch_add(&post_us, us);
}

static inline void feed_note(long long us)
{
    atomic_fetch_add(&feed_n, 1);
    atomic_fetch_add(&feed_us, us);
    long long mx = atomic_load(&feed_max_us);
    if (us > mx) atomic_compare_exchange_strong(&feed_max_us, &mx, us);
}

static inline long long now_us_db(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* per worker: private read connection + statements, zero locking */
static __thread sqlite3 *rdb;
static __thread sqlite3_stmt *st_feed_ids, *st_like_range, *st_feed_rows, *st_feed;
static __thread sqlite3_stmt *st_post, *st_ping;
static __thread buf_t body;             /* scratch for response bodies */

static sqlite3_stmt *prep(sqlite3 *d, const char *sql)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v3(d, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s: %s\n", sqlite3_errmsg(d), sql);
        exit(1);
    }
    return s;
}

static void exec_or_die(sqlite3 *d, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(d, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

void db_thread_init(void)
{
    if (rdb) return;
    if (sqlite3_open_v2(sqlite3_db_filename(wdb, "main"), &rdb,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open reader: %s\n", rdb ? sqlite3_errmsg(rdb) : "?");
        exit(1);
    }
    sqlite3_busy_timeout(rdb, 5000);
    exec_or_die(rdb, "PRAGMA cache_size=-16384;");

#define POST_SELECT                                                       \
    "SELECT p.id, p.body, p.created_at, u.username, "                     \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "               \
    "FROM posts p JOIN users u ON u.id = p.user_id "
    st_feed = prep(rdb, POST_SELECT "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_feed_ids = prep(rdb, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    st_like_range = prep(rdb, "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    st_feed_rows = prep(rdb, "SELECT p.id, p.body, p.created_at, u.username FROM posts p "
                             "JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2");
    st_post = prep(rdb, POST_SELECT "WHERE p.id = ?1");
    st_ping = prep(rdb, "SELECT 1");
}

static void *checkpointer_main(void *arg);

void db_open(const char *path)
{
    if (sqlite3_open_v2(path, &wdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open %s: %s\n", path, wdb ? sqlite3_errmsg(wdb) : "?");
        exit(1);
    }
    sqlite3_busy_timeout(wdb, 5000);
    /* WAL lets every worker read its own snapshot while the writer runs.
     * wal_autocheckpoint stays off: our checkpointer thread owns resets,
     * because PASSIVE checkpoints starve under always-busy readers and an
     * unreset WAL grows forever (every page lookup then scans wal-index
     * hash sections with no bound). */
    exec_or_die(wdb,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA mmap_size=0;"
        "PRAGMA cache_size=-65536;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA journal_size_limit=67108864;"
        "PRAGMA wal_autocheckpoint=0;");

    wst_create = prep(wdb,
        "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    wst_like = prep(wdb,
        "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 "
        "WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
        "ON CONFLICT (user_id, post_id) DO NOTHING");
    wst_exists = prep(wdb, "SELECT 1 FROM posts WHERE id = ?1");

    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* warm the OS page cache so the first requests do not wait on disk */
    int fd = open(path, 0 /* O_RDONLY */);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);
    }

    /* background checkpointer: TRUNCATE every 2 s so the WAL and its
     * wal-index stay small no matter how busy the readers are */
    pthread_t cp;
    if (pthread_create(&cp, NULL, checkpointer_main, NULL) == 0)
        pthread_detach(cp);
}

/* One full checkpoint (reset + truncate) every 2 seconds. TRUNCATE briefly
 * blocks new read transactions and waits out the in-flight ones (~micro-
 * seconds each here), which is the only way the reset happens under a
 * constant read load. Without it the WAL grows without bound. */
static void *checkpointer_main(void *arg)
{
    (void)arg;
    while (!db_stop) {
        struct timespec ts = { 2, 0 };
        nanosleep(&ts, NULL);
        if (db_stop) break;
        pthread_mutex_lock(&write_mu);
        int busy = 0, ck = sqlite3_wal_checkpoint_v2(wdb, NULL,
                          SQLITE_CHECKPOINT_TRUNCATE, NULL, &busy);
        pthread_mutex_unlock(&write_mu);
        if (ck != SQLITE_OK) {
            static time_t last;
            time_t now = time(NULL);
            if (now != last) {
                last = now;
                fprintf(stderr, "checkpoint: %s (busy=%d)\n",
                        sqlite3_errmsg(wdb), busy);
            }
        }
        long long fn = atomic_load(&feed_n), fu = atomic_load(&feed_us);
        long long pn = atomic_load(&post_n), pu = atomic_load(&post_us);
        if (fn + pn >= 50) {
            char walp[512];
            long long walsz = -1;
            snprintf(walp, sizeof walp, "%s-wal", sqlite3_db_filename(wdb, "main"));
            struct stat st;
            if (stat(walp, &st) == 0) walsz = (long long)st.st_size;
            fprintf(stderr,
                "dbstats: feed n=%lld avg=%.1fms max=%.1fms busy=%lld | "
                "post n=%lld avg=%.1fms busy=%lld | wal=%lldKB\n",
                fn, fn ? (double)fu / fn / 1000.0 : 0.0,
                (double)atomic_load(&feed_max_us) / 1000.0,
                atomic_load(&feed_busy),
                pn, pn ? (double)pu / pn / 1000.0 : 0.0,
                atomic_load(&post_busy), walsz < 0 ? -1 : walsz / 1024);
            long long rn = atomic_load(&req_n);
            if (rn)
                fprintf(stderr, "dbstats: req n=%lld avg=%.1fms max=%.1fms\n",
                        rn, (double)atomic_load(&req_us) / rn / 1000.0,
                        (double)atomic_load(&req_max_us) / 1000.0);
        }
    }
    return NULL;
}

void db_close(void)
{
    db_stop = 1;
    if (rdb) sqlite3_close(rdb);
    sqlite3_close(wdb);
}

/* ---------------------------------------------------------------- writers */

/* Runs fn inside BEGIN IMMEDIATE .. COMMIT on the writer connection while
 * holding write_mu. fn returns 0 on success; anything else rolls back. */
static int write_txn(int (*fn)(void *arg), void *arg)
{
    int ok = 0;
    pthread_mutex_lock(&write_mu);
    for (int attempt = 0; attempt < 20 && !ok; attempt++) {
        char *err = NULL;
        if (sqlite3_exec(wdb, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
            sqlite3_free(err);
            struct timespec ts = { 0, 2000000 }; /* 2ms */
            nanosleep(&ts, NULL);
            continue;
        }
        if (fn(arg) == 0) {
            if (sqlite3_exec(wdb, "COMMIT", NULL, NULL, &err) == SQLITE_OK) {
                ok = 1;
            } else {
                sqlite3_free(err);
                sqlite3_exec(wdb, "ROLLBACK", NULL, NULL, NULL);
            }
        } else {
            sqlite3_exec(wdb, "ROLLBACK", NULL, NULL, NULL);
        }
        if (!ok) {
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
        }
    }
    pthread_mutex_unlock(&write_mu);
    return ok;
}

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

typedef struct {
    int64_t uid;
    char *body_heap;          /* ownership passes to the txn */
    const char *body;
    size_t body_len;
    const char *uname;
    size_t ulen;
    /* out */
    int64_t id;
    char created[64];
    int created_len;
} create_arg_t;

static int create_fn(void *argp)
{
    create_arg_t *a = argp;
    sqlite3_stmt *s = wst_create;
    sqlite3_bind_int64(s, 1, a->uid);
    sqlite3_bind_text(s, 2, a->body, (int)a->body_len, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_reset(s);
        return -1;
    }
    a->id = sqlite3_column_int64(s, 0);
    int n = sqlite3_column_bytes(s, 1);
    if (n > (int)sizeof a->created - 1) n = (int)sizeof a->created - 1;
    memcpy(a->created, sqlite3_column_text(s, 1), (size_t)n);
    a->created_len = n;
    int rc = sqlite3_step(s);   /* drain to DONE so the row is written */
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

void h_create(int slot, int64_t uid, const char *uname, size_t ulen,
              const char *b, size_t blen)
{
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

    create_arg_t a = {
        .uid = uid,
        .body = (const char *)p,
        .body_len = len,
        .uname = uname,
        .ulen = ulen,
    };
    int ok = write_txn(create_fn, &a);
    free(heap);
    if (!ok) { srv_err_500(slot); return; }

    body.len = 0;
    if (buf_reserve(&body, 160 + 6 * (len + ulen + (size_t)a.created_len))) {
        srv_err_500(slot);
        return;
    }
    char *o = body.p;
    memcpy(o, "{\"post\":{\"id\":", 14); o += 14;
    o = u64_to(o, (uint64_t)a.id);
    memcpy(o, ",\"body\":\"", 9); o += 9;
    o = j_escape(o, (const unsigned char *)a.body, a.body_len);
    memcpy(o, "\",\"created_at\":\"", 16); o += 16;
    o = j_escape(o, (const unsigned char *)a.created, (size_t)a.created_len);
    memcpy(o, "\",\"author\":\"", 12); o += 12;
    o = j_escape(o, (const unsigned char *)uname, ulen);
    memcpy(o, "\",\"like_count\":0}}", 18); o += 18;
    respond(slot, 201, body.p, (uint32_t)(o - body.p), 0);
}

typedef struct {
    int64_t uid;
    int64_t pid;
    int inserted;      /* 1 = new like, 0 = already liked or missing post */
    int exists;        /* 1 = post exists */
} like_arg_t;

static int like_fn(void *argp)
{
    like_arg_t *a = argp;
    sqlite3_stmt *s = wst_like;
    sqlite3_bind_int64(s, 1, a->uid);
    sqlite3_bind_int64(s, 2, a->pid);
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if (rc != SQLITE_DONE) return -1;

    a->inserted = sqlite3_changes(wdb) > 0;
    if (a->inserted) return 0;

    /* conflict (already liked) or the post does not exist */
    sqlite3_stmt *t = wst_exists;
    sqlite3_bind_int64(t, 1, a->pid);
    rc = sqlite3_step(t);
    sqlite3_reset(t);
    a->exists = (rc == SQLITE_ROW);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? 0 : -1;
}

void h_like(int slot, int64_t uid, int64_t pid)
{
    like_arg_t a = { .uid = uid, .pid = pid };
    if (!write_txn(like_fn, &a)) { srv_err_500(slot); return; }
    if (!a.inserted && !a.exists) { srv_err(slot, 404, "post not found"); return; }

    char rb[96], *o = rb;
    memcpy(o, "{\"liked\":true,\"already_liked\":", 30); o += 30;
    if (a.inserted) { memcpy(o, "false", 5); o += 5; }
    else { memcpy(o, "true", 4); o += 4; }
    memcpy(o, ",\"post_id\":", 11); o += 11;
    o = u64_to(o, (uint64_t)pid);
    *o++ = '}';
    respond(slot, a.inserted ? 201 : 200, rb, (size_t)(o - rb), 0);
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
 * Each response still comes from queries for that request. If a write
 * lands between the queries, the shape check catches it and the reference
 * query re-reads one consistent snapshot. */
void h_feed(int slot)
{
    long long t0 = now_us_db();
    int64_t ids[20], likes[20] = { 0 };
    uint32_t seg[20], seglen[20];
    int8_t slot_of[FEED_SPAN];
    int n = 0, rc;

    while (n < 20 && (rc = sqlite3_step(st_feed_ids)) == SQLITE_ROW)
        ids[n++] = sqlite3_column_int64(st_feed_ids, 0);
    sqlite3_reset(st_feed_ids);
    if (rc == SQLITE_BUSY) atomic_fetch_add(&feed_busy, 1);
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
    if (rc == SQLITE_BUSY) atomic_fetch_add(&feed_busy, 1);
    if (rc != SQLITE_DONE) { srv_err_500(slot); feed_note(now_us_db() - t0); return; }

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
    if (rc == SQLITE_BUSY) atomic_fetch_add(&feed_busy, 1);
    if (rc != SQLITE_DONE) { srv_err_500(slot); feed_note(now_us_db() - t0); return; }
    if (found != n) { h_feed_ref(slot); feed_note(now_us_db() - t0); return; } /* concurrent writer: retry */

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
    feed_note(now_us_db() - t0);
}

void h_post(int slot, int64_t id)
{
    long long t0 = now_us_db();
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
    if (rc == SQLITE_BUSY) atomic_fetch_add(&post_busy, 1);
    post_note(now_us_db() - t0);
    if (rc == SQLITE_DONE) { srv_err(slot, 404, "post not found"); return; }
    srv_err_500(slot);
}

void h_health(int slot)
{
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
    const char *m = sqlite3_errmsg(rdb);
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
