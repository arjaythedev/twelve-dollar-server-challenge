#define _POSIX_C_SOURCE 200809L
#define OPENSSL_API_COMPAT 0x10100000L

#include "challenge.h"
#include "json_input.h"

#include <limits.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { HEAD_LIMIT = 16384, BODY_LIMIT = 16384, LENGTH_DIGITS = 5 };
typedef struct { Slice method, target, auth, body; int close; } Request;
typedef struct { char bytes[128]; size_t size; } Header;
static const struct { int status; const char *reason; } statuses[] = {
    {200, "OK"}, {201, "Created"}, {400, "Bad Request"}, {401, "Unauthorized"},
    {404, "Not Found"}, {500, "Internal Server Error"}, {503, "Service Unavailable"}
};
static Header headers[sizeof(statuses) / sizeof(statuses[0])][2];
static unsigned char token_char[256], b64_digit[256];
static SHA256_CTX hmac_inner, hmac_outer;
static struct timespec started;
/* Requests are executed on one thread. This is only scratch space for the
 * current request's chunk stream / JSON, and no pointer survives dispatch. */
static char body_text[BODY_LIMIT + 1];

int app_init(const char *secret) {
    for (unsigned c = 0; c < 256; ++c) {
        token_char[c] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        digit(c) || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
    }
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    memset(b64_digit, 0xff, sizeof(b64_digit));
    for (unsigned i = 0; i < 64; ++i) b64_digit[(unsigned char)alphabet[i]] = i;
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        for (int close = 0; close <= 1; ++close) {
            Header *h = &headers[i][close];
            int n = snprintf(h->bytes, sizeof(h->bytes),
                             "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n%sContent-Length: %*s\r\n\r\n",
                             statuses[i].status, statuses[i].reason,
                             close ? "Connection: close\r\n" : "", LENGTH_DIGITS, "");
            if (n < 0 || (size_t)n >= sizeof(h->bytes)) return -1;
            h->size = (size_t)n;
        }
    }
    /* Cache only the HMAC key schedule. Both message digests and the MAC
     * comparison are performed anew for every authenticated request. */
    unsigned char key[SHA256_CBLOCK] = {0};
    size_t n = strlen(secret);
    if (n > sizeof(key)) {
        if (!SHA256((const unsigned char *)secret, n, key)) return -1;
    } else {
        memcpy(key, secret, n);
    }
    for (size_t i = 0; i < sizeof(key); ++i) key[i] ^= 0x36;
    SHA256_Init(&hmac_inner);
    SHA256_Update(&hmac_inner, key, sizeof(key));
    for (size_t i = 0; i < sizeof(key); ++i) key[i] ^= 0x36 ^ 0x5c;
    SHA256_Init(&hmac_outer);
    SHA256_Update(&hmac_outer, key, sizeof(key));
    OPENSSL_cleanse(key, sizeof(key));
    return clock_gettime(CLOCK_MONOTONIC, &started);
}

static const Header *response_header(int status, int close) {
    int i;
    switch (status) {
    case 200: i = 0; break; case 201: i = 1; break; case 400: i = 2; break;
    case 401: i = 3; break; case 404: i = 4; break; case 503: i = 6; break;
    default: i = 5; break;
    }
    return &headers[i][close != 0];
}

static int reply_begin(Buf *out, int status, int close) {
    const Header *h = response_header(status, close);
    out->len = 0;
    return buf_append(out, h->bytes, h->size);
}

/* The usual path patches the digit field once without moving the JSON. Grow
 * it when serving an unusually large result rather than imposing a wire cap. */
static int reply_end(Buf *out, int status, int close) {
    size_t header_len = response_header(status, close)->size;
    size_t n = out->len - header_len;
    char digits[3 * sizeof(size_t)], *end = digits + sizeof(digits), *p = end;
    do { *--p = (char)('0' + n % 10); n /= 10; } while (n);
    size_t width = (size_t)(end - p);
    if (width > LENGTH_DIGITS) {
        size_t extra = width - LENGTH_DIGITS;
        if (buf_reserve(out, extra)) return -1;
        size_t at = header_len - 4;
        memmove(out->data + at + extra, out->data + at, out->len - at);
        out->len += extra;
        header_len += extra;
    }
    memcpy(out->data + header_len - 4 - width, p, width);
    return 1;
}

static int error_reply(Buf *out, int status, int close, const char *error) {
    if (reply_begin(out, status, close) ||
        buf_append(out, "{\"error\":", sizeof("{\"error\":") - 1) ||
        buf_json_string(out, error, strlen(error)) || buf_append(out, "}", 1)) return -1;
    return reply_end(out, status, close);
}

void app_error_500(Buf *out) {
    if (error_reply(out, 500, 1, "internal server error") < 0) out->len = 0;
}

static int ascii_ieq(Slice s, Slice lower) {
    if (s.n != lower.n) return 0;
    for (size_t i = 0; i < s.n; ++i)
        if (((unsigned char)s.p[i] | 32u) != (unsigned char)lower.p[i]) return 0;
    return 1;
}

static Slice trim_ows(Slice s) {
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t')) { ++s.p; --s.n; }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t')) --s.n;
    return s;
}

static int contains_token(Slice list, Slice value) {
    while (list.n) {
        const char *comma = memchr(list.p, ',', list.n);
        size_t n = comma ? (size_t)(comma - list.p) : list.n;
        if (ascii_ieq(trim_ows((Slice){list.p, n}), value)) return 1;
        if (!comma) return 0;
        list.n -= n + 1;
        list.p = comma + 1;
    }
    return 0;
}

/* 1 complete, 0 incomplete, -1 malformed. The terminating CRLF is consumed. */
static int line(const char **at, const char *end, Slice *result) {
    const char *lf = memchr(*at, '\n', (size_t)(end - *at));
    if (!lf) return 0;
    if (lf == *at || lf[-1] != '\r') return -1;
    *result = (Slice){*at, (size_t)(lf - 1 - *at)};
    *at = lf + 1;
    return 1;
}

static int header_field(Slice header, Slice *name, Slice *val) {
    const char *sep = memchr(header.p, ':', header.n);
    if (!sep || sep == header.p) return 0;
    *name = (Slice){header.p, (size_t)(sep - header.p)};
    for (size_t i = 0; i < name->n; ++i)
        if (!token_char[(unsigned char)name->p[i]]) return 0;
    *val = trim_ows((Slice){sep + 1, header.n - name->n - 1});
    return 1;
}

static int decimal(Slice s, uint64_t limit, uint64_t *out) {
    if (!s.n) return 0;
    uint64_t n = 0;
    for (size_t i = 0; i < s.n; ++i) {
        unsigned c = (unsigned char)s.p[i];
        if (!digit(c) || n > (limit - (c - '0')) / 10) return 0;
        n = n * 10 + c - '0';
    }
    *out = n;
    return 1;
}

static int positive_id(Slice s, int64_t *out) {
    uint64_t n;
    if (!decimal(s, INT64_MAX, &n) || !n) return 0;
    *out = (int64_t)n;
    return 1;
}

/* Decode HTTP/1.1 chunked bodies, including split chunks and trailers. The
 * representation has its own bound so tiny chunks cannot grow input forever. */
static int chunk_stream(const char *start, const char *end, Slice *body, const char **next) {
    const char *at = start;
    size_t total = 0;
    const size_t repr_limit = 8 * BODY_LIMIT;
    if ((size_t)(end - start) > repr_limit) end = start + repr_limit;
    for (;;) {
        Slice head;
        int rc = line(&at, end, &head);
        if (rc <= 0) return !rc && (size_t)(end - start) == repr_limit ? -1 : rc;
        uint64_t size = 0;
        size_t i = 0;
        for (; i < head.n && head.p[i] != ';'; ++i) {
            int h = hex_digit((unsigned char)head.p[i]);
            if (h < 0 || size > ((uint64_t)BODY_LIMIT - (unsigned)h) / 16) return -1;
            size = size * 16 + (unsigned)h;
        }
        if (i == 0 || size > BODY_LIMIT - total) return -1;
        if (!size) {
            do {
                rc = line(&at, end, &head);
                if (rc <= 0) return !rc && (size_t)(end - start) == repr_limit ? -1 : rc;
                if (head.n) {
                    Slice name, val;
                    if (!header_field(head, &name, &val)) return -1;
                }
            } while (head.n);
            *body = (Slice){body_text, total};
            *next = at;
            return 1;
        }
        if ((uint64_t)(end - at) < size + 2)
            return (size_t)(end - start) == repr_limit ? -1 : 0;
        if (at[size] != '\r' || at[size + 1] != '\n') return -1;
        memcpy(body_text + total, at, (size_t)size);
        total += (size_t)size;
        at += size + 2;
    }
}

static int request_frame(const char *s, size_t n, Request *r, size_t *used) {
    const char *at = s, *end = s + (n < HEAD_LIMIT ? n : HEAD_LIMIT);
    Slice first;
    int rc = line(&at, end, &first);
    if (rc <= 0) return !rc && n >= HEAD_LIMIT ? -1 : rc;
    const char *space = memchr(first.p, ' ', first.n);
    const char *last = first.p + first.n;
    if (!space || space == first.p) return -1;
    r->method = (Slice){first.p, (size_t)(space - first.p)};
    const char *target = space + 1;
    space = memchr(target, ' ', (size_t)(last - target));
    if (!space || space == target) return -1;
    r->target = (Slice){target, (size_t)(space - target)};
    const char *query = memchr(target, '?', r->target.n);
    if (query) r->target.n = (size_t)(query - target);
    Slice version = {space + 1, (size_t)(last - space - 1)};
    int http10 = same(version, LIT("HTTP/1.0"));
    if (!http10 && !same(version, LIT("HTTP/1.1"))) return -1;
    uint64_t content_len = 0;
    int have_length = 0, chunked = 0, keep = 0;
    for (;;) {
        Slice row, name, val;
        rc = line(&at, end, &row);
        if (rc <= 0) return !rc && n >= HEAD_LIMIT ? -1 : rc;
        if (!row.n) break;
        if (!header_field(row, &name, &val)) return -1;
        switch ((unsigned char)name.p[0] | 32u) {
        case 'c':
            if (ascii_ieq(name, LIT("content-length"))) {
                uint64_t num;
                if (!decimal(val, BODY_LIMIT, &num) || (have_length && content_len != num)) return -1;
                content_len = num;
                have_length = 1;
            } else if (ascii_ieq(name, LIT("connection"))) {
                r->close |= contains_token(val, LIT("close"));
                if (http10) keep |= contains_token(val, LIT("keep-alive"));
            }
            break;
        case 't':
            if (ascii_ieq(name, LIT("transfer-encoding"))) {
                if (chunked || !ascii_ieq(val, LIT("chunked"))) return -1;
                chunked = 1;
            }
            break;
        case 'a':
            if (ascii_ieq(name, LIT("authorization"))) {
                if (r->auth.p) return -1;
                r->auth = val;
            }
        }
    }
    r->close |= http10 && !keep;
    end = s + n;
    if (chunked) {
        if (have_length || http10) return -1;
        rc = chunk_stream(at, end, &r->body, &at);
        if (rc != 1) return rc;
    } else {
        if ((uint64_t)(end - at) < content_len) return 0;
        r->body = (Slice){at, (size_t)content_len};
        at += content_len;
    }
    *used = (size_t)(at - s);
    return 1;
}

/* Decode canonical, unpadded base64url. An invalid tail cannot be another
 * spelling of a previously accepted MAC. d has at least floor(3*n/4) bytes. */
static int base64url(Slice s, unsigned char *d, size_t *len) {
    size_t four = s.n & ~(size_t)3, j = 0;
    const unsigned char *p = (const unsigned char *)s.p;
    for (size_t i = 0; i < four; i += 4) {
        unsigned a = b64_digit[p[i]], b = b64_digit[p[i + 1]];
        unsigned c = b64_digit[p[i + 2]], e = b64_digit[p[i + 3]];
        if ((a | b | c | e) > 63) return 0;
        d[j++] = (unsigned char)((a << 2) | (b >> 4));
        d[j++] = (unsigned char)((b << 4) | (c >> 2));
        d[j++] = (unsigned char)((c << 6) | e);
    }
    size_t rem = s.n - four;
    if (rem == 1) return 0;
    if (rem) {
        unsigned a = b64_digit[p[four]], b = b64_digit[p[four + 1]];
        if ((a | b) > 63) return 0;
        d[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (rem == 2) {
            if (b & 15) return 0;
        } else {
            unsigned c = b64_digit[p[four + 2]];
            if (c > 63 || (c & 3)) return 0;
            d[j++] = (unsigned char)((b << 4) | (c >> 2));
        }
    }
    *len = j;
    return 1;
}

typedef struct { int64_t uid; Slice name; } Identity;

static const char *authorize(Slice header, Identity *user, char *decoded) {
    static const char invalid[] = "invalid or expired token";
    if (header.n < 7 || memcmp(header.p, "Bearer ", 7)) return "missing bearer token";
    const char *token = header.p + 7, *end = header.p + header.n;
    const char *first = memchr(token, '.', (size_t)(end - token));
    if (!first) return invalid;
    const char *last = memchr(first + 1, '.', (size_t)(end - first - 1));
    if (!last || end - last != 44) return invalid;
    unsigned char supplied[SHA256_DIGEST_LENGTH], calculated[SHA256_DIGEST_LENGTH];
    size_t n;
    if (!base64url((Slice){last + 1, 43}, supplied, &n) || n != sizeof(supplied)) return invalid;
    SHA256_CTX inner = hmac_inner, outer = hmac_outer;
    SHA256_Update(&inner, token, (size_t)(last - token));
    SHA256_Final(calculated, &inner);
    SHA256_Update(&outer, calculated, sizeof(calculated));
    SHA256_Final(calculated, &outer);
    if (CRYPTO_memcmp(supplied, calculated, sizeof(calculated)) != 0) return invalid;

    Slice encoded_header = {token, (size_t)(first - token)};
    JsonField alg[] = {FIELD("alg")};
    JsonKind kind;
    if (!base64url(encoded_header, (unsigned char *)decoded, &n) ||
        !json_project(decoded, n, alg, 1, &kind) || kind != J_OBJECT ||
        alg[0].value.kind != J_STRING || !same(alg[0].value.text, LIT("HS256"))) return invalid;

    JsonField claims[] = {FIELD("exp"), FIELD("sub"), FIELD("username")};
    Slice payload = {first + 1, (size_t)(last - first - 1)};
    if (!base64url(payload, (unsigned char *)decoded, &n)) return invalid;
    decoded[n] = '\0';
    if (!json_project(decoded, n, claims, 3, &kind) || kind != J_OBJECT ||
        claims[0].value.kind != J_NUMBER) return invalid;
    Slice e = claims[0].value.text;
    char *ep;
    double exp = strtod(e.p, &ep);
    struct timespec now;
    if (ep != e.p + e.n || !isfinite(exp) || clock_gettime(CLOCK_REALTIME, &now) ||
        exp <= (double)now.tv_sec + (double)now.tv_nsec * 1e-9) return invalid;
    if (claims[1].value.kind != J_STRING || !positive_id(claims[1].value.text, &user->uid) ||
        claims[2].value.kind != J_STRING) return "invalid token payload";
    user->name = claims[2].value.text;
    return NULL;
}

static int js_space(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0d);
    return c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000 || c == 0xfeff;
}

static size_t trim_body(Slice *s) {
    const char *p = s->p, *end = p + s->n, *first = NULL, *last = NULL;
    size_t seen = 0, chars = 0;
    while (p < end) {
        uint32_t cp;
        int n = utf8_read(p, end, &cp);
        if (!n) return 0; /* json_text already validated this string */
        if (!js_space(cp)) {
            if (!first) first = p;
            last = p + n;
            chars = seen + 1;
        }
        if (first) ++seen;
        p += n;
    }
    *s = first ? (Slice){first, (size_t)(last - first)} : (Slice){end, 0};
    return chars;
}

static int database_reply(Buf *out, int status, int close) {
    if (status == 200 || status == 201) return reply_end(out, status, close);
    if (status == 404) return error_reply(out, 404, close, "post not found");
    return error_reply(out, 500, close, "internal server error");
}

static int write_route(const Request *r, int create, Slice id_str, Buf *out) {
    char decoded[HEAD_LIMIT + 1];
    Identity user;
    const char *error = authorize(r->auth, &user, decoded);
    if (error) return error_reply(out, 401, r->close, error);

    if (create) {
        if (r->body.p != body_text) memcpy(body_text, r->body.p, r->body.n);
        JsonField field[] = {FIELD("body")};
        JsonKind kind;
        if (!json_project(body_text, r->body.n, field, 1, &kind))
            return error_reply(out, 400, r->close, "malformed JSON body");
        if (field[0].value.kind != J_STRING)
            return error_reply(out, 400, r->close, "body is required");
        Slice text = field[0].value.text;
        size_t chars = trim_body(&text);
        if (!chars) return error_reply(out, 400, r->close, "body is required");
        if (chars > 500) return error_reply(out, 400, r->close, "body must be at most 500 characters");
        if (reply_begin(out, 201, r->close)) return -1;
        int status = db_create(user.uid, text.p, text.n, user.name.p, user.name.n, out);
        return database_reply(out, status, r->close);
    }

    int64_t id;
    if (!positive_id(id_str, &id)) return error_reply(out, 400, r->close, "invalid post id");
    int status = db_like(user.uid, id);
    if (status != 200 && status != 201) return database_reply(out, status, r->close);
    char body[100];
    int n = snprintf(body, sizeof(body), "{\"liked\":true,\"already_liked\":%s,\"post_id\":%lld}",
                     status == 200 ? "true" : "false", (long long)id);
    if (reply_begin(out, status, r->close) || buf_append(out, body, (size_t)n)) return -1;
    return reply_end(out, status, r->close);
}

static int health(Buf *out, int close) {
    if (db_health() != 0) {
        const char *reason = db_error();
        const char prefix[] = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":";
        if (!reason) reason = "database error";
        if (reply_begin(out, 503, close) || buf_append(out, prefix, sizeof(prefix) - 1) ||
            buf_json_string(out, reason, strlen(reason)) || buf_append(out, "}", 1)) return -1;
        return reply_end(out, 503, close);
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long long uptime = (long long)(t.tv_sec - started.tv_sec) - (t.tv_nsec < started.tv_nsec);
    char body[100];
    int n = snprintf(body, sizeof(body), "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%lld}", uptime);
    if (reply_begin(out, 200, close) || buf_append(out, body, (size_t)n)) return -1;
    return reply_end(out, 200, close);
}

static int post_id_slice(Slice target, int like, Slice *id) {
    size_t trailer = like ? sizeof("/like") - 1 : 0;
    size_t prefix = sizeof("/posts/") - 1;
    if (target.n < prefix + trailer || memcmp(target.p, "/posts/", prefix)) return 0;
    if (like && memcmp(target.p + target.n - trailer, "/like", trailer)) return 0;
    *id = (Slice){target.p + prefix, target.n - prefix - trailer};
    return memchr(id->p, '/', id->n) == NULL;
}

int app_request(const char *wire, size_t n, size_t *consumed, Buf *out, int *close_after) {
    *consumed = 0;
    *close_after = 0;
    if (!n) return 0;
    Request r = {0};
    int ready = request_frame(wire, n, &r, consumed);
    if (!ready) return 0;
    if (ready < 0) {
        *consumed = n;
        *close_after = 1;
        return error_reply(out, 400, 1, "bad request");
    }
    *close_after = r.close;
    Slice id_str = {0};
    if (same(r.method, LIT("GET"))) {
        if (same(r.target, LIT("/feed"))) {
            if (reply_begin(out, 200, r.close)) return -1;
            return database_reply(out, db_feed(out), r.close);
        }
        if (post_id_slice(r.target, 0, &id_str)) {
            int64_t id;
            if (!positive_id(id_str, &id)) return error_reply(out, 400, r.close, "invalid post id");
            if (reply_begin(out, 200, r.close)) return -1;
            return database_reply(out, db_post(id, out), r.close);
        }
        if (same(r.target, LIT("/health"))) return health(out, r.close);
    } else if (same(r.method, LIT("POST"))) {
        if (same(r.target, LIT("/posts"))) return write_route(&r, 1, id_str, out);
        if (post_id_slice(r.target, 1, &id_str)) return write_route(&r, 0, id_str, out);
    }
    return error_reply(out, 404, r.close, "not found");
}
