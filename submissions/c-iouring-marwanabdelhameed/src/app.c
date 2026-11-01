/* Request handling: HTTP/1.1 parsing, routing, JWT verification, SQLite access and group commit.
 * Single-threaded. Nothing is cached across requests: every response is built from a SQLite statement run
 * while the request is served, and every token's signature is verified. */
#define _GNU_SOURCE
#include "app.h"
#include "json.h"
#include "sha256.h"
#include "sqlite3.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_HEADER 8192
#define MAX_BODY 16384
#define MAX_REQUEST (MAX_HEADER + MAX_BODY + 64)
#define IN_INITIAL 2048

/* ---------- state ---------- */

static sqlite3 *rdb, *wdb;
static sqlite3_stmt *st_feed, *st_post, *st_health, *st_exist;
static sqlite3_stmt *st_ins_post, *st_ins_like, *st_begin, *st_commit, *st_rollback;
static hmac_key jwt_key;
static int in_txn;
static struct timespec t_start;

static conn **pend, **rel;
static size_t npend, pend_cap, nrel, rel_cap;

/* ---------- output buffers ---------- */

typedef struct {
    char *p;
    size_t len, cap;
} buf;

static buf rb; /* scratch for one response body, rebuilt for every request */

static void die(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

static void b_reserve(buf *b, size_t n) {
    if (b->len + n <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) die("out of memory");
    b->p = p;
    b->cap = cap;
}

static void b_put(buf *b, const char *s, size_t n) {
    b_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

#define B_LIT(b, s) b_put((b), (s), sizeof(s) - 1)

static void b_int(buf *b, long long v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%lld", v);
    b_put(b, t, (size_t)n);
}

static void b_esc(buf *b, const char *s, size_t n) {
    b_reserve(b, n * 6 + 1);
    char *o = b->p + b->len;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { *o++ = '\\'; *o++ = '"'; }
        else if (c == '\\') { *o++ = '\\'; *o++ = '\\'; }
        else if (c >= 0x20) { *o++ = (char)c; }
        else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (c == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (c == '\t') { *o++ = '\\'; *o++ = 't'; }
        else if (c == '\b') { *o++ = '\\'; *o++ = 'b'; }
        else if (c == '\f') { *o++ = '\\'; *o++ = 'f'; }
        else {
            *o++ = '\\'; *o++ = 'u'; *o++ = '0'; *o++ = '0';
            *o++ = hex[c >> 4]; *o++ = hex[c & 15];
        }
    }
    b->len = (size_t)(o - b->p);
}

/* ---------- responses ---------- */

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static void respond(conn *c, int status, const char *body, size_t n) {
    char hdr[192];
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s\r\n", status,
                     reason(status), n, c->closing ? "Connection: close\r\n" : "");
    size_t need = (size_t)c->out_len + (size_t)h + n;
    if (need > c->out_cap) {
        size_t cap = need < 1024 ? 1024 : need;
        char *p = realloc(c->out, cap);
        if (!p) die("out of memory");
        c->out = p;
        c->out_cap = (uint32_t)cap;
    }
    memcpy(c->out + c->out_len, hdr, (size_t)h);
    memcpy(c->out + c->out_len + h, body, n);
    c->out_len = (uint32_t)need;
}

static void respond_err(conn *c, int status, const char *msg) {
    char t[160];
    int n = snprintf(t, sizeof t, "{\"error\":\"%s\"}", msg);
    respond(c, status, t, (size_t)n);
}

static void respond_500(conn *c) { respond_err(c, 500, "internal server error"); }

/* ---------- base64url and JWT ---------- */

static int b64val(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-') return 62;
    if (ch == '_') return 63;
    return -1;
}

/* Decodes unpadded base64url. Returns the length, or -1 on a bad character, bad length or overflow. */
static int b64url_decode(const char *s, size_t n, uint8_t *out, size_t cap) {
    if (n % 4 == 1) return -1;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64val((unsigned char)s[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    return (int)o;
}

typedef struct {
    long long uid;
    char name[512];
    size_t name_len;
} auth_t;

#define AUTH_OK 0
#define AUTH_MISSING 1
#define AUTH_INVALID 2
#define AUTH_PAYLOAD 3

static int all_digits(const char *s, size_t n) {
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static int authenticate(const char *hv, size_t hl, auth_t *a) {
    if (!hv || hl < 7 || memcmp(hv, "Bearer ", 7) != 0) return AUTH_MISSING;
    const char *tok = hv + 7;
    size_t tl = hl - 7;
    if (tl == 0 || tl > 1024) return AUTH_INVALID;
    const char *d1 = memchr(tok, '.', tl);
    if (!d1) return AUTH_INVALID;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(tok + tl - d1 - 1));
    if (!d2) return AUTH_INVALID;
    if (memchr(d2 + 1, '.', (size_t)(tok + tl - d2 - 1))) return AUTH_INVALID;

    uint8_t hbuf[768], pbuf[768], sig[64];
    int hn = b64url_decode(tok, (size_t)(d1 - tok), hbuf, sizeof hbuf);
    if (hn < 0) return AUTH_INVALID;
    char algb[16];
    jfield hf[1] = {{.key = "alg", .str = algb, .cap = sizeof algb}};
    if (json_scan((const char *)hbuf, (size_t)hn, hf, 1) != 1) return AUTH_INVALID;
    if (hf[0].type != JV_STRING || hf[0].overflow || hf[0].len != 5 || memcmp(algb, "HS256", 5) != 0)
        return AUTH_INVALID;

    int sn = b64url_decode(d2 + 1, (size_t)(tok + tl - d2 - 1), sig, sizeof sig);
    if (sn != 32) return AUTH_INVALID;
    uint8_t mac[32];
    hmac_sha256(&jwt_key, tok, (size_t)(d2 - tok), mac);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(mac[i] ^ sig[i]);
    if (diff) return AUTH_INVALID;

    int pn = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), pbuf, sizeof pbuf);
    if (pn < 0) return AUTH_INVALID;
    char subb[40];
    jfield pf[4] = {{.key = "sub", .str = subb, .cap = sizeof subb},
                    {.key = "username", .str = a->name, .cap = sizeof a->name},
                    {.key = "exp"},
                    {.key = "nbf"}};
    if (json_scan((const char *)pbuf, (size_t)pn, pf, 4) != 1) return AUTH_INVALID;
    long long now = (long long)time(NULL);
    if (pf[2].type != JV_ABSENT) {
        if (pf[2].type != JV_NUMBER || (double)now >= pf[2].num) return AUTH_INVALID;
    }
    if (pf[3].type != JV_ABSENT) {
        if (pf[3].type != JV_NUMBER || (double)now < pf[3].num) return AUTH_INVALID;
    }
    if (pf[0].type != JV_STRING || pf[0].overflow || pf[0].len > 18 || !all_digits(subb, pf[0].len))
        return AUTH_PAYLOAD;
    long long uid = 0;
    for (size_t i = 0; i < pf[0].len; i++) uid = uid * 10 + (subb[i] - '0');
    if (uid <= 0) return AUTH_PAYLOAD;
    if (pf[1].type != JV_STRING || pf[1].overflow) return AUTH_PAYLOAD;
    a->uid = uid;
    a->name_len = pf[1].len;
    return AUTH_OK;
}

static int auth_or_fail(conn *c, const char *hv, size_t hl, auth_t *a) {
    int rc = authenticate(hv, hl, a);
    if (rc == AUTH_OK) return 1;
    if (rc == AUTH_MISSING) respond_err(c, 401, "missing bearer token");
    else if (rc == AUTH_PAYLOAD) respond_err(c, 401, "invalid token payload");
    else respond_err(c, 401, "invalid or expired token");
    return 0;
}

/* ---------- database ---------- */

static void exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql failed: %s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s: %s\n", sql, sqlite3_errmsg(db));
        exit(1);
    }
    return s;
}

#define POST_SELECT                                                                           \
    "SELECT p.id, p.body, p.created_at, u.username,"                                          \
    " (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count"                    \
    " FROM posts p JOIN users u ON u.id = p.user_id"

int app_init(const char *db_path, const char *jwt_secret) {
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    hmac_key_init(&jwt_key, jwt_secret, strlen(jwt_secret));

    if (sqlite3_open_v2(db_path, &wdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
        die("cannot open the database for writing");
    exec_sql(wdb, "PRAGMA busy_timeout=5000; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-32768;"
                  " PRAGMA journal_mode=WAL;");
    sqlite3_stmt *jm = prep(wdb, "PRAGMA journal_mode");
    if (sqlite3_step(jm) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(jm, 0), "wal") != 0)
        die("the database is not in WAL mode");
    sqlite3_finalize(jm);
    sqlite3_stmt *sy = prep(wdb, "PRAGMA synchronous");
    if (sqlite3_step(sy) != SQLITE_ROW || sqlite3_column_int(sy, 0) < 1) die("synchronous is below NORMAL");
    sqlite3_finalize(sy);

    if (sqlite3_open_v2(db_path, &rdb, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
        die("cannot open the database for reading");
    exec_sql(rdb, "PRAGMA busy_timeout=5000; PRAGMA cache_size=-65536; PRAGMA mmap_size=536870912;"
                  " PRAGMA query_only=1;");

    st_feed = prep(rdb, POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    st_post = prep(rdb, POST_SELECT " WHERE p.id = ?");
    st_health = prep(rdb, "SELECT 1");
    st_exist = prep(rdb, "SELECT 1 FROM posts WHERE id = ?");
    st_ins_post = prep(wdb, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
    st_ins_like = prep(wdb, "INSERT INTO likes (user_id, post_id) VALUES (?, ?)"
                            " ON CONFLICT (user_id, post_id) DO NOTHING");
    st_begin = prep(wdb, "BEGIN IMMEDIATE");
    st_commit = prep(wdb, "COMMIT");
    st_rollback = prep(wdb, "ROLLBACK");
    return 0;
}

static int run_simple(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc == SQLITE_DONE;
}

static int begin_if_needed(void) {
    if (in_txn) return 1;
    if (!run_simple(st_begin)) return 0;
    in_txn = 1;
    return 1;
}

static void add_pending(conn *c) {
    if (npend == pend_cap) {
        size_t cap = pend_cap ? pend_cap * 2 : 256;
        conn **p = realloc(pend, cap * sizeof *p);
        if (!p) die("out of memory");
        pend = p;
        pend_cap = cap;
    }
    pend[npend++] = c;
}

int app_has_pending(void) { return npend > 0; }

int app_commit(conn ***released) {
    int ok = 1;
    if (in_txn) {
        ok = run_simple(st_commit);
        if (!ok) {
            fprintf(stderr, "commit failed: %s\n", sqlite3_errmsg(wdb));
            run_simple(st_rollback);
        }
        in_txn = 0;
    }
    if (rel_cap < npend) {
        conn **p = realloc(rel, npend * sizeof *p);
        if (!p) die("out of memory");
        rel = p;
        rel_cap = npend;
    }
    nrel = npend;
    memcpy(rel, pend, npend * sizeof *pend);
    npend = 0;
    for (size_t i = 0; i < nrel; i++) {
        conn *c = rel[i];
        c->hold = 0;
        if (!ok) {
            c->out_len = c->hold_mark;
            respond_500(c);
        }
    }
    *released = rel;
    return (int)nrel;
}

/* ---------- handlers ---------- */

typedef struct {
    const char *method, *path;
    size_t method_len, path_len;
    const char *auth;
    size_t auth_len;
    const char *body;
    size_t body_len;
    int keep_alive;
} request;

static void put_post(buf *b, sqlite3_stmt *s) {
    const char *t;
    B_LIT(b, "{\"id\":");
    b_int(b, sqlite3_column_int64(s, 0));
    B_LIT(b, ",\"body\":\"");
    t = (const char *)sqlite3_column_text(s, 1);
    b_esc(b, t, (size_t)sqlite3_column_bytes(s, 1));
    B_LIT(b, "\",\"created_at\":\"");
    t = (const char *)sqlite3_column_text(s, 2);
    b_esc(b, t, (size_t)sqlite3_column_bytes(s, 2));
    B_LIT(b, "\",\"author\":\"");
    t = (const char *)sqlite3_column_text(s, 3);
    b_esc(b, t, (size_t)sqlite3_column_bytes(s, 3));
    B_LIT(b, "\",\"like_count\":");
    b_int(b, sqlite3_column_int64(s, 4));
    b_put(b, "}", 1);
}

static void h_health(conn *c) {
    int rc = sqlite3_step(st_health);
    sqlite3_reset(st_health);
    rb.len = 0;
    if (rc == SQLITE_ROW) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        B_LIT(&rb, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        b_int(&rb, (long long)(now.tv_sec - t_start.tv_sec));
        b_put(&rb, "}", 1);
        respond(c, 200, rb.p, rb.len);
    } else {
        B_LIT(&rb, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
        const char *m = sqlite3_errmsg(rdb);
        b_esc(&rb, m, strlen(m));
        B_LIT(&rb, "\"}");
        respond(c, 503, rb.p, rb.len);
    }
}

static void h_feed(conn *c) {
    rb.len = 0;
    B_LIT(&rb, "{\"posts\":[");
    int rc, first = 1;
    while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
        if (!first) b_put(&rb, ",", 1);
        first = 0;
        put_post(&rb, st_feed);
    }
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE) { respond_500(c); return; }
    B_LIT(&rb, "]}");
    respond(c, 200, rb.p, rb.len);
}

/* Parses a post id. Returns 1 if valid (id set, or *big set if beyond int64 range), 0 if not a positive integer. */
static int parse_id(const char *s, size_t n, long long *id, int *big) {
    *big = 0;
    if (!all_digits(s, n)) return 0;
    size_t i = 0;
    while (i < n && s[i] == '0') i++;
    if (i == n) return 0; /* zero */
    if (n - i > 18) { *big = 1; return 1; }
    long long v = 0;
    for (; i < n; i++) v = v * 10 + (s[i] - '0');
    *id = v;
    return 1;
}

static void h_get_post(conn *c, const char *ids, size_t idl) {
    long long id = 0;
    int big;
    if (!parse_id(ids, idl, &id, &big)) { respond_err(c, 400, "invalid post id"); return; }
    if (big) { respond_err(c, 404, "post not found"); return; }
    sqlite3_bind_int64(st_post, 1, id);
    int rc = sqlite3_step(st_post);
    if (rc == SQLITE_ROW) {
        rb.len = 0;
        B_LIT(&rb, "{\"post\":");
        put_post(&rb, st_post);
        b_put(&rb, "}", 1);
        sqlite3_reset(st_post);
        respond(c, 200, rb.p, rb.len);
        return;
    }
    sqlite3_reset(st_post);
    if (rc == SQLITE_DONE) respond_err(c, 404, "post not found");
    else respond_500(c);
}

/* Whitespace as String.prototype.trim treats it, at the start of s. Returns its byte length, or 0. */
static size_t ws_at(const unsigned char *s, size_t n) {
    if (n == 0) return 0;
    unsigned char c = s[0];
    if (c == ' ' || (c >= 9 && c <= 13)) return 1;
    if (c == 0xC2 && n >= 2 && (s[1] == 0xA0 || s[1] == 0x85)) return s[1] == 0xA0 ? 2 : 0;
    if (n >= 3 && c == 0xE1 && s[1] == 0x9A && s[2] == 0x80) return 3;
    if (n >= 3 && c == 0xE2 && s[1] == 0x80 && ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 || s[2] == 0xA9 || s[2] == 0xAF))
        return 3;
    if (n >= 3 && c == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) return 3;
    if (n >= 3 && c == 0xE3 && s[1] == 0x80 && s[2] == 0x80) return 3;
    if (n >= 3 && c == 0xEF && s[1] == 0xBB && s[2] == 0xBF) return 3;
    return 0;
}

/* Whitespace ending at s+n. Returns its byte length, or 0. */
static size_t ws_before(const unsigned char *s, size_t n) {
    for (size_t k = 1; k <= 3 && k <= n; k++) {
        size_t w = ws_at(s + n - k, k);
        if (w == k) return k;
    }
    return 0;
}

static size_t count_cps(const char *s, size_t n) {
    size_t cps = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) cps++;
    return cps;
}

static void h_create_post(conn *c, const request *r) {
    auth_t a;
    if (!auth_or_fail(c, r->auth, r->auth_len, &a)) return;

    static char bodybuf[MAX_BODY + 8];
    jfield f[1] = {{.key = "body", .str = bodybuf, .cap = sizeof bodybuf}};
    int rc = json_scan(r->body, r->body_len, f, 1);
    if (rc < 0) { respond_err(c, 400, "malformed JSON body"); return; }
    if (rc == 0 || f[0].type != JV_STRING || f[0].overflow) { respond_err(c, 400, "body is required"); return; }

    const unsigned char *p = (const unsigned char *)bodybuf;
    size_t n = f[0].len, w;
    while (n && (w = ws_at(p, n))) { p += w; n -= w; }
    while (n && (w = ws_before(p, n))) n -= w;
    if (n == 0) { respond_err(c, 400, "body is required"); return; }
    if (count_cps((const char *)p, n) > 500) { respond_err(c, 400, "body must be at most 500 characters"); return; }

    if (!begin_if_needed()) { respond_500(c); return; }
    sqlite3_bind_int64(st_ins_post, 1, a.uid);
    sqlite3_bind_text(st_ins_post, 2, (const char *)p, (int)n, SQLITE_STATIC);
    rc = sqlite3_step(st_ins_post);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "insert post failed: %s\n", sqlite3_errmsg(wdb));
        sqlite3_reset(st_ins_post);
        respond_500(c);
        return;
    }
    rb.len = 0;
    B_LIT(&rb, "{\"post\":{\"id\":");
    b_int(&rb, sqlite3_column_int64(st_ins_post, 0));
    B_LIT(&rb, ",\"body\":\"");
    b_esc(&rb, (const char *)p, n);
    B_LIT(&rb, "\",\"created_at\":\"");
    b_esc(&rb, (const char *)sqlite3_column_text(st_ins_post, 1), (size_t)sqlite3_column_bytes(st_ins_post, 1));
    B_LIT(&rb, "\",\"author\":\"");
    b_esc(&rb, a.name, a.name_len);
    B_LIT(&rb, "\",\"like_count\":0}}");
    rc = sqlite3_step(st_ins_post); /* run RETURNING to completion */
    sqlite3_reset(st_ins_post);
    if (rc != SQLITE_DONE) { respond_500(c); return; }
    c->hold_mark = c->out_len;
    respond(c, 201, rb.p, rb.len);
    c->hold = 1;
    add_pending(c);
}

static void h_like(conn *c, const request *r, const char *ids, size_t idl) {
    auth_t a;
    if (!auth_or_fail(c, r->auth, r->auth_len, &a)) return;
    long long id = 0;
    int big;
    if (!parse_id(ids, idl, &id, &big)) { respond_err(c, 400, "invalid post id"); return; }
    if (big) { respond_err(c, 404, "post not found"); return; }

    /* Only committed posts exist for a client, and the read connection sees exactly those. */
    sqlite3_bind_int64(st_exist, 1, id);
    int rc = sqlite3_step(st_exist);
    sqlite3_reset(st_exist);
    if (rc == SQLITE_DONE) { respond_err(c, 404, "post not found"); return; }
    if (rc != SQLITE_ROW) { respond_500(c); return; }

    if (!begin_if_needed()) { respond_500(c); return; }
    sqlite3_bind_int64(st_ins_like, 1, a.uid);
    sqlite3_bind_int64(st_ins_like, 2, id);
    rc = sqlite3_step(st_ins_like);
    int changed = sqlite3_changes(wdb);
    sqlite3_reset(st_ins_like);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "insert like failed: %s\n", sqlite3_errmsg(wdb));
        respond_500(c);
        return;
    }
    char t[96];
    int n = snprintf(t, sizeof t, "{\"liked\":true,\"already_liked\":%s,\"post_id\":%lld}",
                     changed ? "false" : "true", id);
    c->hold_mark = c->out_len;
    respond(c, changed ? 201 : 200, t, (size_t)n);
    c->hold = 1;
    add_pending(c);
}

static void route(conn *c, const request *r) {
    const char *p = r->path;
    size_t n = r->path_len;
    int get = r->method_len == 3 && memcmp(r->method, "GET", 3) == 0;
    int post = r->method_len == 4 && memcmp(r->method, "POST", 4) == 0;

    if (get && n == 7 && memcmp(p, "/health", 7) == 0) { h_health(c); return; }
    if (get && n == 5 && memcmp(p, "/feed", 5) == 0) { h_feed(c); return; }
    if (post && n == 6 && memcmp(p, "/posts", 6) == 0) { h_create_post(c, r); return; }
    if ((get || post) && n > 7 && memcmp(p, "/posts/", 7) == 0) {
        const char *ids = p + 7;
        size_t rest = n - 7;
        const char *slash = memchr(ids, '/', rest);
        if (!slash) {
            if (get) { h_get_post(c, ids, rest); return; }
        } else if (post && (size_t)(p + n - slash) == 5 && memcmp(slash, "/like", 5) == 0) {
            h_like(c, r, ids, (size_t)(slash - ids));
            return;
        }
    }
    respond_err(c, 404, "not found");
}

/* ---------- HTTP parsing ---------- */

static int ieq(const char *a, size_t n, const char *lower) {
    size_t m = strlen(lower);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = a[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (ch != lower[i]) return 0;
    }
    return 1;
}

static int token_has(const char *v, size_t n, const char *lower) {
    size_t m = strlen(lower);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m) {
            char ch = v[i + k];
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            if (ch != lower[k]) break;
            k++;
        }
        if (k == m) return 1;
    }
    return 0;
}

#define P_MORE 0
#define P_BAD (-1)
#define P_BIG (-2)

/* Returns the request length when a whole request is buffered, P_MORE if more bytes are needed, else an error. */
static long parse_request(conn *c, request *r, size_t *need) {
    const char *b = c->in;
    size_t n = c->in_len;
    const char *he = NULL;
    size_t scan = n > MAX_HEADER ? MAX_HEADER : n;
    for (size_t i = 3; i < scan; i++) {
        if (b[i] == '\n' && b[i - 1] == '\r' && b[i - 2] == '\n' && b[i - 3] == '\r') { he = b + i + 1; break; }
    }
    if (!he) {
        if (n >= MAX_HEADER) return P_BIG;
        *need = n + 1;
        return P_MORE;
    }
    size_t hlen = (size_t)(he - b);

    const char *eol = memchr(b, '\r', hlen);
    const char *sp1 = memchr(b, ' ', (size_t)(eol - b));
    if (!sp1) return P_BAD;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
    if (!sp2) return P_BAD;
    r->method = b;
    r->method_len = (size_t)(sp1 - b);
    r->path = sp1 + 1;
    size_t tl = (size_t)(sp2 - sp1 - 1);
    const char *q = memchr(r->path, '?', tl);
    r->path_len = q ? (size_t)(q - r->path) : tl;
    size_t vl = (size_t)(eol - sp2 - 1);
    int http11 = vl == 8 && memcmp(sp2 + 1, "HTTP/1.1", 8) == 0;
    if (!http11 && !(vl == 8 && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0)) return P_BAD;
    r->keep_alive = http11;
    r->auth = NULL;
    r->auth_len = 0;
    size_t content_length = 0;

    const char *line = eol + 2;
    while (line < he - 2) {
        const char *le = memchr(line, '\r', (size_t)(he - line));
        const char *colon = memchr(line, ':', (size_t)(le - line));
        if (!colon) return P_BAD;
        const char *vs = colon + 1;
        while (vs < le && (*vs == ' ' || *vs == '\t')) vs++;
        const char *ve = le;
        while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t nl = (size_t)(colon - line), vn = (size_t)(ve - vs);
        if (ieq(line, nl, "content-length")) {
            if (!all_digits(vs, vn) || vn > 9) return P_BAD;
            content_length = 0;
            for (size_t i = 0; i < vn; i++) content_length = content_length * 10 + (size_t)(vs[i] - '0');
        } else if (ieq(line, nl, "authorization")) {
            r->auth = vs;
            r->auth_len = vn;
        } else if (ieq(line, nl, "connection")) {
            if (token_has(vs, vn, "close")) r->keep_alive = 0;
            else if (token_has(vs, vn, "keep-alive")) r->keep_alive = 1;
        } else if (ieq(line, nl, "transfer-encoding")) {
            return P_BAD;
        }
        line = le + 2;
    }
    if (content_length > MAX_BODY) return P_BIG;
    size_t total = hlen + content_length;
    if (n < total) {
        *need = total;
        return P_MORE;
    }
    r->body = he;
    r->body_len = content_length;
    return (long)total;
}

static void grow_input(conn *c, size_t need) {
    if (need <= c->in_cap && c->in_len < c->in_cap) return;
    size_t cap = c->in_cap;
    while (cap < need || cap <= c->in_len) cap *= 2;
    if (cap > MAX_REQUEST) cap = MAX_REQUEST;
    char *p = realloc(c->in, cap);
    if (!p) die("out of memory");
    c->in = p;
    c->in_cap = (uint32_t)cap;
}

void app_process(conn *c) {
    while (!c->hold && !c->closing && !c->dead && c->in_len > 0) {
        request r;
        size_t need = 0;
        long rc = parse_request(c, &r, &need);
        if (rc == P_MORE) {
            if (need > MAX_REQUEST) need = MAX_REQUEST;
            if (c->in_len >= MAX_REQUEST) { c->closing = 1; respond_err(c, 413, "payload too large"); return; }
            grow_input(c, need);
            return;
        }
        if (rc == P_BAD || rc == P_BIG) {
            c->closing = 1;
            if (rc == P_BIG) respond_err(c, 413, "payload too large");
            else respond_err(c, 400, "bad request");
            return;
        }
        if (!r.keep_alive) c->closing = 1;
        route(c, &r);
        size_t used = (size_t)rc;
        memmove(c->in, c->in + used, c->in_len - used);
        c->in_len -= (uint32_t)used;
        if (c->in_len == 0 && c->in_cap > 8192) {
            char *p = realloc(c->in, IN_INITIAL);
            if (p) { c->in = p; c->in_cap = IN_INITIAL; }
        }
    }
}
