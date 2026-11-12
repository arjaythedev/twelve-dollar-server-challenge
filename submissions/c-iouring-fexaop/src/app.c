#define _GNU_SOURCE
/* Routing, auth and response construction. Every response is built from rows read in this
 * pass's SQLite transaction; nothing is kept between requests. */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "http.h"
#include "json.h"
#include "jwt.h"

static void out_ch(struct out *o, char c) {
    if (out_reserve(o, 1) == 0) o->p[o->len++] = c;
}

void out_lit(struct out *o, const char *s, size_t n) {
    if (out_reserve(o, n) == 0) {
        memcpy(o->p + o->len, s, n);
        o->len += n;
    }
}
#define LIT(o, s) out_lit((o), (s), sizeof(s) - 1)

void out_clear(struct out *o) { o->len = 0; }

void out_free(struct out *o) {
    free(o->p);
    o->p = NULL;
    o->len = 0;
    o->cap = 0;
    o->small = 0;
}

/* Process-wide pool of output/partial buffers (event-loop thread only). Idle connections hold
 * none: a buffer goes back here once its response has been sent or its request completed. */
#define POOL_MAX 2048
static char *pool_ptr[POOL_MAX];
static size_t pool_cap[POOL_MAX];
static int pool_n;

static char *pool_get(size_t *cap) {
    if (pool_n > 0) {
        pool_n--;
        *cap = pool_cap[pool_n];
        return pool_ptr[pool_n];
    }
    *cap = 8192;
    return malloc(*cap);
}

static void pool_put(char *p, size_t cap) {
    if (pool_n < POOL_MAX && cap <= 65536) {
        pool_ptr[pool_n] = p;
        pool_cap[pool_n] = cap;
        pool_n++;
    } else {
        free(p);
    }
}

void out_release(struct out *o) {
    if (o->p) {
        if (o->small)
            free(o->p);
        else
            pool_put(o->p, o->cap);
    }
    o->p = NULL;
    o->len = 0;
    o->cap = 0;
    o->small = 0;
}

int out_reserve(struct out *o, size_t extra) {
    if (o->len + extra <= o->cap) return 0;
    size_t cap = o->cap;
    char *p;
    if (!o->p) {
        p = pool_get(&cap);
        if (!p) return -1;
    } else {
        p = o->p;
    }
    while (cap < o->len + extra) {
        if (cap > (1u << 30)) {
            if (p != o->p) free(p);
            return -1;
        }
        cap *= 2;
    }
    if (cap != o->cap || !o->p) {
        char *np = realloc(p, cap);
        if (!np) {
            if (p != o->p) free(p);
            return -1;
        }
        p = np;
    }
    o->p = p;
    o->cap = cap;
    o->small = 0;
    return 0;
}

/* Pending requests are short. A per-connection 8 KiB response-pool buffer would cost about
 * 2.5 GB if every one of 310k connections held one; this path starts at 512 bytes and grows
 * only for the bytes actually buffered. */
int out_reserve_partial(struct out *o, size_t extra) {
    if (o->len + extra <= o->cap) return 0;
    if (!o->p) {
        size_t cap = o->len + extra;
        if (cap < 512) cap = 512;
        o->p = malloc(cap);
        if (!o->p) return -1;
        o->cap = cap;
        o->small = 1;
        return 0;
    }
    size_t cap = o->cap;
    while (cap < o->len + extra) {
        if (cap > (1u << 30)) return -1;
        cap *= 2;
    }
    char *p = realloc(o->p, cap);
    if (!p) return -1;
    o->p = p;
    o->cap = cap;
    o->small = 1;
    return 0;
}

static void out_int(struct out *o, long long v) {
    char buf[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) buf[n++] = '-';
    if (out_reserve(o, (size_t)n) != 0) return;
    while (n > 0) o->p[o->len++] = buf[--n];
}

static void out_json(struct out *o, const char *s, size_t n) {
    if (out_reserve(o, n * 6 + 1) != 0) return;
    o->len += json_escape(o->p + o->len, s, n);
}

static inline char *litp(char *p, const char *s, size_t n) {
    memcpy(p, s, n);
    return p + n;
}

static inline char *intp(char *p, long long v) {
    char buf[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) buf[n++] = '-';
    while (n > 0) *p++ = buf[--n];
    return p;
}

static const char *status_text(int s, size_t *len) {
    switch (s) {
    case 200: *len = 2; return "OK";
    case 201: *len = 7; return "Created";
    case 400: *len = 11; return "Bad Request";
    case 401: *len = 12; return "Unauthorized";
    case 404: *len = 9; return "Not Found";
    case 500: *len = 21; return "Internal Server Error";
    case 503: *len = 19; return "Service Unavailable";
    default: *len = 5; return "Error";
    }
}

/* Headers, with a 10-digit Content-Length back-filled by finish(). One capacity check, then raw
 * writes; every response goes through here. */
static size_t start(struct out *o, int status, size_t *lenoff) {
    *lenoff = o->len;
    if (out_reserve(o, 160) != 0) return o->len;
    char *p = o->p + o->len;
    p = litp(p, "HTTP/1.1 ", 9);
    p = intp(p, status);
    *p++ = ' ';
    size_t tl;
    const char *t = status_text(status, &tl);
    p = litp(p, t, tl);
    p = litp(p, "\r\nContent-Type: application/json\r\nContent-Length: ", 50);
    *lenoff = (size_t)(p - o->p);
    memset(p, '0', 10);
    p += 10;
    p = litp(p, "\r\n\r\n", 4);
    o->len = (size_t)(p - o->p);
    return o->len;
}

static void finish(struct out *o, size_t body, size_t lenoff) {
    /* start() can fail on allocation and leave no header behind. */
    if (!o->p || o->len < body || lenoff + 10 > o->cap) return;
    size_t n = o->len - body;
    for (int i = 9; i >= 0; i--) {
        o->p[lenoff + (size_t)i] = (char)('0' + (n % 10));
        n /= 10;
    }
}

static void error(struct out *o, int status, const char *msg) {
    size_t lenoff;
    size_t body = start(o, status, &lenoff);
    LIT(o, "{\"error\":\"");
    out_json(o, msg, strlen(msg));
    LIT(o, "\"}");
    finish(o, body, lenoff);
}

void app_500(struct out *o) {
    o->len = 0;
    error(o, 500, "internal server error");
}

void app_init(struct app *a, const char *secret, size_t secret_len) {
    a->secret = (const unsigned char *)secret;
    a->secret_len = secret_len;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    a->started_ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long now_seconds(void) { return (long long)time(NULL); }

static int path_is(const struct http_request *r, const char *p) {
    size_t n = strlen(p);
    return r->path_len == n && memcmp(r->path, p, n) == 0;
}

static int parse_id(const char *p, size_t n, long long *out) {
    if (n == 0 || n > 18) return -1;
    long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
        if (v > 9007199254740992LL) return -1;
    }
    if (v <= 0) return -1;
    *out = v;
    return 0;
}

static int auth(struct app *a, const struct http_request *req, struct out *o,
                struct jwt_claims *c) {
    enum jwt_result r = jwt_verify(req->has_auth ? req->auth : "", req->has_auth ? req->auth_len : 0,
                                   a->secret, a->secret_len, now_seconds(), c);
    switch (r) {
    case JWT_OK: return 0;
    case JWT_MISSING: error(o, 401, "missing bearer token"); return -1;
    case JWT_INVALID: error(o, 401, "invalid or expired token"); return -1;
    case JWT_PAYLOAD: error(o, 401, "invalid token payload"); return -1;
    }
    error(o, 401, "invalid or expired token");
    return -1;
}

/* One post object, written through a raw pointer after a single capacity check: the field
 * lengths are known, so 6x their bytes plus the fixed literals and two integers bounds the
 * escaped form exactly. This is the hot builder for both /feed (20 rows) and /posts/:id. */
static void post_obj(struct out *o, const struct row *r) {
    size_t need = 128 + 6 * ((size_t)r->body_len + (size_t)r->created_len + (size_t)r->author_len);
    if (out_reserve(o, need) != 0) return;
    char *p = o->p + o->len;
    p = litp(p, "{\"id\":", 6);
    p = intp(p, r->id);
    p = litp(p, ",\"body\":\"", 9);
    p += json_escape_inline(p, r->body, (size_t)r->body_len);
    p = litp(p, "\",\"created_at\":\"", 16);
    p += json_escape_inline(p, r->created, (size_t)r->created_len);
    p = litp(p, "\",\"author\":\"", 12);
    p += json_escape_inline(p, r->author, (size_t)r->author_len);
    p = litp(p, "\",\"like_count\":", 15);
    p = intp(p, r->likes);
    *p++ = '}';
    o->len = (size_t)(p - o->p);
}

static void feed(struct app *a, struct out *o) {
    (void)a;
    struct row rows[FEED_N];
    /* 20 x (500 code points x 4 UTF-8 bytes + 24-byte timestamp + a 255-byte username) fits. */
    char arena[65536];
    int n = db_feed(rows, arena, sizeof arena);
    if (n < 0) n = db_feed_sql(rows, arena, sizeof arena);
    if (n < 0) {
        error(o, 500, "internal server error");
        return;
    }
    size_t lenoff;
    size_t body = start(o, 200, &lenoff);
    LIT(o, "{\"posts\":[");
    for (int i = 0; i < n; i++) {
        if (i) out_ch(o, ',');
        post_obj(o, &rows[i]);
    }
    LIT(o, "]}");
    finish(o, body, lenoff);
}

static void one_post(struct out *o, long long id) {
    struct row r;
    int rc = db_post(id, &r);
    if (rc < 0) {
        error(o, 500, "internal server error");
        return;
    }
    if (rc == 0) {
        error(o, 404, "post not found");
        return;
    }
    size_t lenoff;
    size_t body = start(o, 200, &lenoff);
    LIT(o, "{\"post\":");
    post_obj(o, &r);
    out_ch(o, '}');
    finish(o, body, lenoff);
}

static void create(struct app *a, const struct http_request *req, struct out *o) {
    struct jwt_claims c;
    if (auth(a, req, o, &c)) return;
    char bodybuf[4096];
    size_t blen = 0;
    enum json_body_result jr =
        json_extract_body(req->body, req->body_len, bodybuf, sizeof bodybuf, &blen);
    switch (jr) {
    case JSON_BODY_OK: break;
    case JSON_BODY_MALFORMED: error(o, 400, "malformed JSON body"); return;
    case JSON_BODY_MISSING: error(o, 400, "body is required"); return;
    case JSON_BODY_TOO_LONG: error(o, 400, "body must be at most 500 characters"); return;
    }
    long long id;
    char created[64];
    int clen = 0;
    if (db_insert_post(c.user_id, bodybuf, (int)blen, &id, created, &clen) != 0) {
        error(o, 500, "internal server error");
        return;
    }
    size_t lenoff;
    size_t body = start(o, 201, &lenoff);
    LIT(o, "{\"post\":{\"id\":");
    out_int(o, id);
    LIT(o, ",\"body\":\"");
    out_json(o, bodybuf, blen);
    LIT(o, "\",\"created_at\":\"");
    out_json(o, created, (size_t)clen);
    LIT(o, "\",\"author\":\"");
    out_json(o, c.username, (size_t)c.username_len);
    LIT(o, "\",\"like_count\":0}}");
    finish(o, body, lenoff);
}

static void like(struct app *a, const struct http_request *req, const char *idp, size_t idn,
                 struct out *o) {
    struct jwt_claims c;
    if (auth(a, req, o, &c)) return;
    long long id;
    if (parse_id(idp, idn, &id) != 0) {
        error(o, 400, "invalid post id");
        return;
    }
    int ex = db_post_exists(id);
    if (ex < 0) {
        error(o, 500, "internal server error");
        return;
    }
    if (ex == 0) {
        error(o, 404, "post not found");
        return;
    }
    int fresh = db_insert_like(c.user_id, id);
    if (fresh < 0) {
        error(o, 500, "internal server error");
        return;
    }
    size_t lenoff;
    size_t body = start(o, fresh ? 201 : 200, &lenoff);
    if (fresh)
        LIT(o, "{\"liked\":true,\"already_liked\":false,\"post_id\":");
    else
        LIT(o, "{\"liked\":true,\"already_liked\":true,\"post_id\":");
    out_int(o, id);
    out_ch(o, '}');
    finish(o, body, lenoff);
}

static void health(struct app *a, struct out *o) {
    if (db_health() != 0) {
        size_t lenoff;
        size_t body = start(o, 503, &lenoff);
        LIT(o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database error\"}");
        finish(o, body, lenoff);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    long long up = (now - a->started_ns) / 1000000000LL;
    size_t lenoff;
    size_t body = start(o, 200, &lenoff);
    LIT(o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    out_int(o, up);
    out_ch(o, '}');
    finish(o, body, lenoff);
}

void app_handle(struct app *a, const struct http_request *req, struct out *o) {
    if (req->method_len == 3 && memcmp(req->method, "GET", 3) == 0) {
        if (path_is(req, "/feed")) {
            feed(a, o);
        } else if (path_is(req, "/health")) {
            health(a, o);
        } else if (req->path_len > 7 && memcmp(req->path, "/posts/", 7) == 0) {
            const char *idp = req->path + 7;
            size_t idn = req->path_len - 7;
            if (memchr(idp, '/', idn)) {
                error(o, 404, "not found");
                return;
            }
            long long id;
            if (parse_id(idp, idn, &id) != 0) {
                error(o, 400, "invalid post id");
                return;
            }
            one_post(o, id);
        } else {
            error(o, 404, "not found");
        }
    } else if (req->method_len == 4 && memcmp(req->method, "POST", 4) == 0) {
        if (path_is(req, "/posts")) {
            create(a, req, o);
        } else if (req->path_len >= 13 && memcmp(req->path, "/posts/", 7) == 0 &&
                   memcmp(req->path + req->path_len - 5, "/like", 5) == 0) {
            const char *idp = req->path + 7;
            size_t idn = req->path_len - 7 - 5;
            if (memchr(idp, '/', idn)) {
                error(o, 404, "not found");
                return;
            }
            like(a, req, idp, idn, o);
        } else {
            error(o, 404, "not found");
        }
    } else {
        error(o, 404, "not found");
    }
}
