/* http.c - request parsing for exactly what this API speaks */
#define _GNU_SOURCE
#include <string.h>

#include "db.h"
#include "http.h"
#include "jwt.h"
#include "server.h"
#include "util.h"

#define MAX_HEADER 16384
#define MAX_BODY (1 << 20)

static inline int ieq(const char *a, const char *lit, size_t n)
{
    /* lit is lowercase, n its length */
    for (size_t i = 0; i < n; i++)
        if ((a[i] | 0x20) != lit[i]) return 0;
    return 1;
}

/* Parses a post id path segment: > 0 ok, 0 invalid, -1 too large to exist. */
static int64_t parse_id(const char *s, size_t n)
{
    if (!n) return 0;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (v > (INT64_MAX - 9) / 10) {
            for (; i < n; i++)
                if (s[i] < '0' || s[i] > '9') return 0;
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static void route(int slot, const char *m, size_t ml, const char *path, size_t pl,
                  const char *auth, size_t authl, const char *b, size_t bl, double now)
{
    int get = ml == 3 && !memcmp(m, "GET", 3);
    int post = ml == 4 && !memcmp(m, "POST", 4);

    if (pl >= 7 && !memcmp(path, "/posts/", 7)) {
        const char *seg = path + 7;
        size_t sl = pl - 7;
        const char *slash = memchr(seg, '/', sl);
        if (!slash && get) {
            int64_t id = parse_id(seg, sl);
            if (!id) srv_err(slot, 400, "invalid post id");
            else if (id < 0) srv_err(slot, 404, "post not found");
            else h_post(slot, id);
            return;
        }
        if (slash && post && (size_t)(path + pl - slash) == 5 && !memcmp(slash, "/like", 5)) {
            auth_t a;
            int k = check_auth(auth, authl, now, &a);
            if (k) {
                if (k == 1) srv_err(slot, 401, "missing bearer token");
                else if (k == 2) srv_err(slot, 401, "invalid or expired token");
                else srv_err(slot, 401, "invalid token payload");
                return;
            }
            int64_t id = parse_id(seg, (size_t)(slash - seg));
            if (!id) srv_err(slot, 400, "invalid post id");
            else if (id < 0) srv_err(slot, 404, "post not found");
            else h_like(slot, a.uid, id);
            return;
        }
    } else if (pl == 5 && !memcmp(path, "/feed", 5)) {
        if (get) { h_feed(slot); return; }
    } else if (pl == 6 && !memcmp(path, "/posts", 6)) {
        if (post) {
            auth_t a;
            int k = check_auth(auth, authl, now, &a);
            if (k) {
                if (k == 1) srv_err(slot, 401, "missing bearer token");
                else if (k == 2) srv_err(slot, 401, "invalid or expired token");
                else srv_err(slot, 401, "invalid token payload");
                return;
            }
            h_create(slot, a.uid, a.name, a.name_len, b, bl);
            return;
        }
    } else if (pl == 7 && !memcmp(path, "/health", 7)) {
        if (get) { h_health(slot); return; }
    }
    srv_err(slot, 404, "not found");
}

static void bad_request(int slot)
{
    srv_err_close(slot, 400, "bad request");
}

size_t handle_request(int slot, const char *p, size_t n)
{
    const char *he = memmem(p, n, "\r\n\r\n", 4);
    if (!he) {
        if (n > MAX_HEADER) { bad_request(slot); return n; }
        return 0;
    }
    const char *end = p + n;

    /* request line: METHOD SP TARGET SP HTTP/1.x */
    const char *le = memchr(p, '\r', (size_t)(he + 2 - p));
    if (!le || le[1] != '\n' || memchr(p, '\n', (size_t)(le - p))) { bad_request(slot); return n; }
    const char *sp1 = memchr(p, ' ', (size_t)(le - p));
    if (!sp1) { bad_request(slot); return n; }
    const char *tgt = sp1 + 1;
    const char *sp2 = memchr(tgt, ' ', (size_t)(le - tgt));
    if (!sp2 || le - sp2 - 1 != 8 || memcmp(sp2 + 1, "HTTP/1.", 7)) { bad_request(slot); return n; }
    int http11 = sp2[8] == '1';
    int keepalive = http11;
    const char *q = memchr(tgt, '?', (size_t)(sp2 - tgt));
    size_t pl = (size_t)((q ? q : sp2) - tgt);

    const char *auth = NULL;
    size_t authl = 0;
    size_t clen = 0;
    int has_cl = 0, chunked = 0, expect100 = 0;

    for (const char *l = le + 2; l < he + 2;) {
        const char *e = memchr(l, '\r', (size_t)(he + 2 - l));
        if (!e || e[1] != '\n' || memchr(l, '\n', (size_t)(e - l))) { bad_request(slot); return n; }
        const char *colon = memchr(l, ':', (size_t)(e - l));
        if (colon) {
            size_t nl = (size_t)(colon - l);
            const char *v = colon + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            const char *ve = e;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            switch (nl) {
            case 13:
                if (ieq(l, "authorization", 13)) { auth = v; authl = (size_t)(ve - v); }
                break;
            case 14:
                if (ieq(l, "content-length", 14)) {
                    if (v == ve) { bad_request(slot); return n; }
                    size_t cl = 0;
                    for (const char *x = v; x < ve; x++) {
                        if (*x < '0' || *x > '9') { bad_request(slot); return n; }
                        cl = cl * 10 + (size_t)(*x - '0');
                        if (cl > MAX_BODY) { srv_err_close(slot, 413, "payload too large"); return n; }
                    }
                    if (has_cl && cl != clen) { bad_request(slot); return n; }
                    clen = cl;
                    has_cl = 1;
                }
                break;
            case 10:
                if (ieq(l, "connection", 10)) {
                    if (ve - v == 5 && ieq(v, "close", 5)) keepalive = 0;
                    else if (ve - v == 10 && ieq(v, "keep-alive", 10)) keepalive = 1;
                }
                break;
            case 17:
                if (ieq(l, "transfer-encoding", 17)) {
                    if (chunked || ve - v != 7 || !ieq(v, "chunked", 7)) { bad_request(slot); return n; }
                    chunked = 1;
                }
                break;
            case 6:
                if (ieq(l, "expect", 6) && ve - v == 12 && ieq(v, "100-continue", 12)) expect100 = 1;
                break;
            }
        }
        l = e + 2;
    }
    if (chunked) { bad_request(slot); return n; } /* not spoken here */

    const char *b = he + 4;
    if ((size_t)(end - b) < clen) {
        if (expect100 && !srv_sent_continue(slot)) {
            srv_send_continue(slot);
        }
        return 0; /* body not fully arrived */
    }
    srv_reset_continue(slot);
    srv_set_conn_hdr(slot, keepalive ? (http11 ? 0 : 2) : 1);
    route(slot, p, (size_t)(sp1 - p), tgt, pl, auth, authl, b, clen, now_realtime());
    srv_set_conn_hdr(slot, 0);
    return (size_t)(b + clen - p);
}
