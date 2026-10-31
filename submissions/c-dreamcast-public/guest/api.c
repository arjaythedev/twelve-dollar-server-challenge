/* api.c - the five endpoints of the challenge's SPEC.md, on SQLite, for one request at a time.
 * Every request runs its own query (rule 5): only the prepared statements are kept. */
#include "api.h"
#include "hostfs.h"
#include "jwt.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *db;
static sqlite3_stmt *st_health, *st_feed, *st_post, *st_insert, *st_exists, *st_like;
static jwt_key_t key;
static uint64_t started_ms;

#define POST_COLS "SELECT p.id, p.body, p.created_at, u.username, " \
                  "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) " \
                  "FROM posts p JOIN users u ON u.id = p.user_id "

static int prep(const char *sql, sqlite3_stmt **st) {
    return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, st, 0);
}

int api_init(char *err, int errlen) {
    char secret[256];
    if (hostfs_getenv("JWT_SECRET", secret, sizeof secret) < 0) {
        snprintf(err, errlen, "JWT_SECRET is not set");
        return -1;
    }
    jwt_key_init(&key, secret, strlen(secret));
    started_ms = hostfs_time_ms();

    int rc = sqlite3_open_v2("main", &db, SQLITE_OPEN_READWRITE, "hostfs");
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db,
            "PRAGMA locking_mode=EXCLUSIVE;"  /* WAL without shared memory, see vfs_hostfs.c */
            "PRAGMA journal_mode=WAL;"
            "PRAGMA synchronous=NORMAL;"
            "PRAGMA cache_size=-3072;"        /* 3 MB of our 16 MB */
            "PRAGMA temp_store=MEMORY;", 0, 0, 0);
    if (rc == SQLITE_OK) rc = prep("SELECT 1", &st_health);
    if (rc == SQLITE_OK) rc = prep(POST_COLS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &st_feed);
    if (rc == SQLITE_OK) rc = prep(POST_COLS "WHERE p.id = ?", &st_post);
    if (rc == SQLITE_OK) rc = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", &st_insert);
    if (rc == SQLITE_OK) rc = prep("SELECT 1 FROM posts WHERE id = ?", &st_exists);
    if (rc == SQLITE_OK) rc = prep("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING", &st_like);
    if (rc != SQLITE_OK) {
        snprintf(err, errlen, "sqlite: %s", db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        return -1;
    }
    return 0;
}

/* ---- output buffer ---- */

static void put(buf_t *b, const char *s, int n) {
    if (b->len + n + 1 > b->cap) {
        int cap = b->cap ? b->cap : 1024;
        while (b->len + n + 1 > cap) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void puts_(buf_t *b, const char *s) { put(b, s, strlen(s)); }
static void putint(buf_t *b, long long v) { char t[24]; put(b, t, snprintf(t, sizeof t, "%lld", v)); }

static void putjstr(buf_t *b, const char *s, int n) {
    static const char hex[] = "0123456789abcdef";
    put(b, "\"", 1);
    int run = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        put(b, s + run, i - run);
        run = i + 1;
        switch (c) {
        case '"': put(b, "\\\"", 2); break;
        case '\\': put(b, "\\\\", 2); break;
        case '\n': put(b, "\\n", 2); break;
        case '\r': put(b, "\\r", 2); break;
        case '\t': put(b, "\\t", 2); break;
        case '\b': put(b, "\\b", 2); break;
        case '\f': put(b, "\\f", 2); break;
        default: { char u[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]}; put(b, u, 6); }
        }
    }
    put(b, s + run, n - run);
    put(b, "\"", 1);
}

static int error(buf_t *b, int status, const char *msg) {
    b->len = 0;
    puts_(b, "{\"error\":\"");
    puts_(b, msg);
    puts_(b, "\"}");
    return status;
}

/* ---- request body: a strict JSON parser that keeps the last top-level "body" string ---- */

typedef struct { const char *p, *end; } js_t;

static void ws(js_t *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static int utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
    return 4;
}

/* Parses a string at j->p (on the quote). With out != NULL, writes the decoded UTF-8 there
 * (it never grows: escapes are at least as long as what they decode to). */
static int jstring(js_t *j, char *out, int *outlen) {
    if (j->p >= j->end || *j->p != '"') return -1;
    j->p++;
    int n = 0;
    while (j->p < j->end) {
        unsigned char c = *j->p++;
        if (c == '"') { if (outlen) *outlen = n; return 0; }
        if (c < 0x20) return -1;
        if (c != '\\') { if (out) out[n] = c; n++; continue; }
        if (j->p >= j->end) return -1;
        char e = *j->p++, d;
        switch (e) {
        case '"': d = '"'; break;
        case '\\': d = '\\'; break;
        case '/': d = '/'; break;
        case 'b': d = '\b'; break;
        case 'f': d = '\f'; break;
        case 'n': d = '\n'; break;
        case 'r': d = '\r'; break;
        case 't': d = '\t'; break;
        case 'u': {
            if (j->end - j->p < 4) return -1;
            int cp = hex4(j->p);
            if (cp < 0) return -1;
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                int lo = hex4(j->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    j->p += 6;
                }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD; /* lone surrogate */
            char t[4];
            int k = utf8(t, cp);
            if (out) memcpy(out + n, t, k);
            n += k;
            continue;
        }
        default: return -1;
        }
        if (out) out[n] = d;
        n++;
    }
    return -1;
}

static int jvalue(js_t *j, int depth);

static int jnumber(js_t *j) {
    const char *p = j->p;
    if (p < j->end && *p == '-') p++;
    if (p >= j->end) return -1;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (p < j->end && *p >= '0' && *p <= '9') p++;
    else return -1;
    if (p < j->end && *p == '.') {
        p++;
        if (p >= j->end || *p < '0' || *p > '9') return -1;
        while (p < j->end && *p >= '0' && *p <= '9') p++;
    }
    if (p < j->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < j->end && (*p == '+' || *p == '-')) p++;
        if (p >= j->end || *p < '0' || *p > '9') return -1;
        while (p < j->end && *p >= '0' && *p <= '9') p++;
    }
    j->p = p;
    return 0;
}

static int jlit(js_t *j, const char *w) {
    int n = strlen(w);
    if (j->end - j->p < n || memcmp(j->p, w, n)) return -1;
    j->p += n;
    return 0;
}

/* Object; at depth 0 remembers where the value of the last "body" key starts. */
static const char *body_at;

static int jobject(js_t *j, int depth) {
    j->p++;
    ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return 0; }
    for (;;) {
        ws(j);
        const char *k = j->p;
        int klen;
        char kbuf[8];
        if (jstring(j, 0, &klen) < 0) return -1;
        int is_body = 0;
        if (depth == 0 && klen == 4) {
            js_t t = {k, j->end};
            jstring(&t, kbuf, 0);
            is_body = memcmp(kbuf, "body", 4) == 0;
        }
        ws(j);
        if (j->p >= j->end || *j->p != ':') return -1;
        j->p++;
        ws(j);
        if (is_body) body_at = j->p;
        if (jvalue(j, depth + 1) < 0) return -1;
        ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; return 0; }
        return -1;
    }
}

static int jvalue(js_t *j, int depth) {
    if (depth > 64 || j->p >= j->end) return -1;
    switch (*j->p) {
    case '{': return jobject(j, depth);
    case '[':
        j->p++;
        ws(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return 0; }
        for (;;) {
            ws(j);
            if (jvalue(j, depth + 1) < 0) return -1;
            ws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == ']') { j->p++; return 0; }
            return -1;
        }
    case '"': return jstring(j, 0, 0);
    case 't': return jlit(j, "true");
    case 'f': return jlit(j, "false");
    case 'n': return jlit(j, "null");
    default: return jnumber(j);
    }
}

/* JavaScript's String.prototype.trim whitespace, as UTF-8 code points */
static int js_space(unsigned cp) {
    return cp == 9 || cp == 10 || cp == 11 || cp == 12 || cp == 13 || cp == 32 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

static unsigned decode(const unsigned char *s, int n, int *k) {
    unsigned c = s[0];
    if (c < 0x80 || n < 2) { *k = 1; return c; }
    if (c < 0xE0) { *k = 2; return (c & 31) << 6 | (s[1] & 63); }
    if (c < 0xF0 || n < 4) { *k = n < 3 ? n : 3; return (c & 15) << 12 | (s[1] & 63) << 6 | (s[2] & 63); }
    *k = 4;
    return (c & 7) << 18 | (s[1] & 63) << 12 | (s[2] & 63) << 6 | (s[3] & 63);
}

/* ---- handlers ---- */

static int post_json(buf_t *b, sqlite3_stmt *st) {
    puts_(b, "{\"id\":");
    putint(b, sqlite3_column_int64(st, 0));
    puts_(b, ",\"body\":");
    putjstr(b, (const char *)sqlite3_column_text(st, 1), sqlite3_column_bytes(st, 1));
    puts_(b, ",\"created_at\":");
    putjstr(b, (const char *)sqlite3_column_text(st, 2), sqlite3_column_bytes(st, 2));
    puts_(b, ",\"author\":");
    putjstr(b, (const char *)sqlite3_column_text(st, 3), sqlite3_column_bytes(st, 3));
    puts_(b, ",\"like_count\":");
    putint(b, sqlite3_column_int64(st, 4));
    puts_(b, "}");
    return 0;
}

static long long parse_id(const char *s, int n) {
    if (n < 1 || n > 15) return -1;
    long long v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v > 0 ? v : -1;
}

static int auth(buf_t *b, const char *hdr, int hlen, jwt_result_t *who) {
    if (hdr == 0 || hlen < 7 || memcmp(hdr, "Bearer ", 7) != 0) return error(b, 401, "missing bearer token");
    const char *tok = hdr + 7;
    int len = hlen - 7;
    jwt_verify_batch(&key, 1, &tok, &len, (int64_t)(hostfs_time_ms() / 1000), who);
    if (who->status == JWT_INVALID) return error(b, 401, "invalid or expired token");
    if (who->status != JWT_OK) return error(b, 401, "invalid token payload");
    return 0;
}

static int health(buf_t *b) {
    int rc = sqlite3_step(st_health);
    sqlite3_reset(st_health);
    if (rc != SQLITE_ROW) {
        puts_(b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
        const char *m = sqlite3_errmsg(db);
        putjstr(b, m, strlen(m));
        puts_(b, "}");
        return 503;
    }
    puts_(b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    putint(b, (long long)((hostfs_time_ms() - started_ms) / 1000));
    puts_(b, "}");
    return 200;
}

static int feed(buf_t *b) {
    puts_(b, "{\"posts\":[");
    int rc, n = 0;
    while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
        if (n++) puts_(b, ",");
        post_json(b, st_feed);
    }
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE) return error(b, 500, "internal server error");
    puts_(b, "]}");
    return 200;
}

static int get_post(buf_t *b, long long id) {
    sqlite3_bind_int64(st_post, 1, id);
    int rc = sqlite3_step(st_post);
    int status = 200;
    if (rc == SQLITE_ROW) {
        puts_(b, "{\"post\":");
        post_json(b, st_post);
        puts_(b, "}");
    } else {
        status = error(b, rc == SQLITE_DONE ? 404 : 500, rc == SQLITE_DONE ? "post not found" : "internal server error");
    }
    sqlite3_reset(st_post);
    return status;
}

static int create(buf_t *b, const jwt_result_t *who, const char *body, int blen) {
    js_t j = {body, body + blen};
    body_at = 0;
    ws(&j);
    if (jvalue(&j, 0) < 0) return error(b, 400, "malformed JSON body");
    ws(&j);
    if (j.p != j.end) return error(b, 400, "malformed JSON body");
    if (body_at == 0 || *body_at != '"') return error(b, 400, "body is required");

    char *text = malloc(blen + 1);
    if (!text) return error(b, 500, "internal server error");
    int n;
    js_t s = {body_at, body + blen};
    jstring(&s, text, &n);
    /* trim like JavaScript, then count UTF-16 code units like JavaScript's .length */
    int start = 0, end = n, k;
    while (start < end && js_space(decode((unsigned char *)text + start, end - start, &k))) start += k;
    while (end > start) {
        int back = end - 1;
        while (back > start && ((unsigned char)text[back] & 0xC0) == 0x80) back--;
        if (!js_space(decode((unsigned char *)text + back, end - back, &k))) break;
        end = back;
    }
    int units = 0;
    for (int i = start; i < end; i += k) units += decode((unsigned char *)text + i, end - i, &k) >= 0x10000 ? 2 : 1;
    if (units == 0) { free(text); return error(b, 400, "body is required"); }
    if (units > 500) { free(text); return error(b, 400, "body must be at most 500 characters"); }

    sqlite3_bind_int64(st_insert, 1, who->uid);
    sqlite3_bind_text(st_insert, 2, text + start, end - start, SQLITE_STATIC);
    int rc = sqlite3_step(st_insert);
    int status = 201;
    if (rc == SQLITE_ROW) {
        puts_(b, "{\"post\":{\"id\":");
        putint(b, sqlite3_column_int64(st_insert, 0));
        puts_(b, ",\"body\":");
        putjstr(b, text + start, end - start);
        puts_(b, ",\"created_at\":");
        putjstr(b, (const char *)sqlite3_column_text(st_insert, 1), sqlite3_column_bytes(st_insert, 1));
        puts_(b, ",\"author\":");
        putjstr(b, who->username, who->username_len);
        puts_(b, ",\"like_count\":0}}");
        rc = sqlite3_step(st_insert); /* runs the statement to completion: the commit */
    }
    sqlite3_reset(st_insert);
    free(text);
    if (rc != SQLITE_DONE) return error(b, 500, "internal server error");
    return status;
}

static int like(buf_t *b, const jwt_result_t *who, long long id) {
    sqlite3_bind_int64(st_exists, 1, id);
    int rc = sqlite3_step(st_exists);
    sqlite3_reset(st_exists);
    if (rc == SQLITE_DONE) return error(b, 404, "post not found");
    if (rc != SQLITE_ROW) return error(b, 500, "internal server error");
    sqlite3_bind_int64(st_like, 1, who->uid);
    sqlite3_bind_int64(st_like, 2, id);
    rc = sqlite3_step(st_like);
    sqlite3_reset(st_like);
    if (rc != SQLITE_DONE) return error(b, 500, "internal server error");
    int fresh = sqlite3_changes(db) > 0;
    puts_(b, fresh ? "{\"liked\":true,\"already_liked\":false,\"post_id\":" : "{\"liked\":true,\"already_liked\":true,\"post_id\":");
    putint(b, id);
    puts_(b, "}");
    return fresh ? 201 : 200;
}

int api_handle(const char *method, int mlen, const char *path, int plen, const char *authz, int alen,
               const char *body, int blen, buf_t *out, int *route) {
    const char *q = memchr(path, '?', plen);
    if (q) plen = q - path;
    int get = mlen == 3 && memcmp(method, "GET", 3) == 0;
    int post = mlen == 4 && memcmp(method, "POST", 4) == 0;
    jwt_result_t who;
    *route = ROUTE_OTHER;

    if (get && plen == 7 && memcmp(path, "/health", 7) == 0) { *route = ROUTE_HEALTH; return health(out); }
    if (get && plen == 5 && memcmp(path, "/feed", 5) == 0) { *route = ROUTE_FEED; return feed(out); }
    if (post && plen == 6 && memcmp(path, "/posts", 6) == 0) {
        *route = ROUTE_CREATE;
        int s = auth(out, authz, alen, &who);
        return s ? s : create(out, &who, body, blen);
    }
    if (plen > 7 && memcmp(path, "/posts/", 7) == 0) {
        const char *id = path + 7;
        int idlen = plen - 7;
        if (post && idlen > 5 && memcmp(path + plen - 5, "/like", 5) == 0) {
            *route = ROUTE_LIKE;
            int s = auth(out, authz, alen, &who);
            if (s) return s;
            long long v = parse_id(id, idlen - 5);
            return v < 0 ? error(out, 400, "invalid post id") : like(out, &who, v);
        }
        if (get && memchr(id, '/', idlen) == 0) {
            *route = ROUTE_POST;
            long long v = parse_id(id, idlen);
            return v < 0 ? error(out, 400, "invalid post id") : get_post(out, v);
        }
    }
    return error(out, 404, "not found");
}
