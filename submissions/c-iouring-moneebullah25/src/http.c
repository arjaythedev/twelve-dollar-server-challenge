#include "http.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "util.h"

static int ieq(const char *a, size_t alen, const char *lit) {
    size_t n = strlen(lit);
    if (alen != n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower(a[i]) != lit[i]) return 0;
    }
    return 1;
}

/* Finds the end of the header block. Returns the offset just past "\r\n\r\n"
 * or 0 when it is not there yet. */
static uint32_t find_head_end(const char *buf, uint32_t len) {
    const char *p = buf;
    const char *end = buf + len;
    while (p < end) {
        const char *q = memchr(p, '\n', (size_t)(end - p));
        if (!q) return 0;
        if (q + 2 < end && q[1] == '\r' && q[2] == '\n') return (uint32_t)(q + 3 - buf);
        if (q + 1 < end && q[1] == '\n') return (uint32_t)(q + 2 - buf); /* tolerate bare LF */
        p = q + 1;
    }
    return 0;
}

/* Scans a comma-separated Connection header for "close" or "keep-alive". */
static void connection_tokens(const char *v, size_t len, int *close_seen, int *keepalive_seen) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t start = i;
        while (i < len && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (ieq(v + start, i - start, "close")) *close_seen = 1;
        if (ieq(v + start, i - start, "keep-alive")) *keepalive_seen = 1;
    }
}

enum http_parse_status http_parse(const char *buf, uint32_t len, uint32_t max_head, uint32_t max_body,
                                  struct http_req *req) {
    memset(req, 0, sizeof *req); /* every return path leaves a defined struct */
    uint32_t head_len = find_head_end(buf, len);
    if (!head_len) return len >= max_head ? HTTP_TOO_LARGE : HTTP_PARTIAL;
    if (head_len > max_head) return HTTP_TOO_LARGE;

    const char *p = buf;
    const char *end = buf + head_len;

    /* request line: METHOD SP target SP HTTP/1.x CRLF */
    const char *sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp || sp == p) return HTTP_BAD;
    size_t mlen = (size_t)(sp - p);
    if (mlen == 3 && memcmp(p, "GET", 3) == 0) {
        req->method = HTTP_GET;
    } else if (mlen == 4 && memcmp(p, "POST", 4) == 0) {
        req->method = HTTP_POST;
    } else {
        for (size_t i = 0; i < mlen; i++) {
            if (!is_tchar(p[i])) return HTTP_BAD;
        }
        req->method = HTTP_OTHER;
    }
    p = sp + 1;
    sp = memchr(p, ' ', (size_t)(end - p));
    if (!sp || sp == p) return HTTP_BAD;
    req->path = p;
    req->path_len = (uint32_t)(sp - p);
    for (uint32_t i = 0; i < req->path_len; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch <= 0x20 || ch == 0x7f) return HTTP_BAD;
    }
    p = sp + 1;
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol) return HTTP_BAD;
    size_t vlen = (size_t)(eol - p);
    if (vlen && p[vlen - 1] == '\r') vlen--;
    if (vlen != 8 || memcmp(p, "HTTP/1.", 7) != 0 || (p[7] != '1' && p[7] != '0')) return HTTP_BAD;
    req->version_11 = p[7] == '1';
    req->keep_alive = req->version_11;
    p = eol + 1;

    /* headers */
    int64_t content_length = -1;
    int close_seen = 0, keepalive_seen = 0;
    while (p < end) {
        eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) return HTTP_BAD;
        size_t line_len = (size_t)(eol - p);
        if (line_len && p[line_len - 1] == '\r') line_len--;
        if (line_len == 0) break;                         /* the empty line */
        if (p[0] == ' ' || p[0] == '\t') return HTTP_BAD; /* obsolete line folding */
        const char *colon = memchr(p, ':', line_len);
        if (!colon || colon == p) return HTTP_BAD;
        size_t nlen = (size_t)(colon - p);
        for (size_t i = 0; i < nlen; i++) {
            if (!is_tchar(p[i])) return HTTP_BAD;
        }
        const char *v = colon + 1;
        size_t vl = line_len - nlen - 1;
        while (vl && (*v == ' ' || *v == '\t')) {
            v++;
            vl--;
        }
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
        for (size_t i = 0; i < vl; i++) {
            unsigned char ch = (unsigned char)v[i];
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return HTTP_BAD;
        }
        switch (nlen) {
        case 13:
            if (ieq(p, nlen, "authorization")) {
                req->auth = v;
                req->auth_len = (uint32_t)vl;
            }
            break;
        case 14:
            if (ieq(p, nlen, "content-length")) {
                if (content_length >= 0 || vl == 0 || vl > 10) return HTTP_BAD;
                int64_t n = 0;
                for (size_t i = 0; i < vl; i++) {
                    if (v[i] < '0' || v[i] > '9') return HTTP_BAD;
                    n = n * 10 + (v[i] - '0');
                }
                content_length = n;
            }
            break;
        case 10:
            if (ieq(p, nlen, "connection")) connection_tokens(v, vl, &close_seen, &keepalive_seen);
            break;
        case 6:
            if (ieq(p, nlen, "expect") && ieq(v, vl, "100-continue")) req->expect_100 = 1;
            break;
        case 17:
            if (ieq(p, nlen, "transfer-encoding")) return HTTP_BAD; /* chunked bodies are not supported */
            break;
        default: break;
        }
        p = eol + 1;
    }
    if (close_seen) req->keep_alive = 0;
    if (!req->version_11 && keepalive_seen && !close_seen) req->keep_alive = 1;

    req->head_len = head_len;
    if (content_length < 0) content_length = 0;
    if ((uint64_t)content_length > max_body) return HTTP_TOO_LARGE;
    req->body_len = (uint32_t)content_length;
    req->total_len = head_len + req->body_len;
    if (len < req->total_len) return HTTP_PARTIAL;
    req->body = buf + head_len;
    return HTTP_COMPLETE;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Content Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

const char *http_date_now(void) {
    static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static char cached[80]; /* HTTP_DATE_LEN + 1 in practice; sized for any int the compiler allows in tm */
    static time_t cached_s;
    time_t now = time(NULL);
    if (now != cached_s || !cached[0]) {
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(cached, sizeof cached, "%s, %02d %s %04d %02d:%02d:%02d GMT", DAYS[tm.tm_wday], tm.tm_mday,
                 MONTHS[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
        cached_s = now;
    }
    return cached;
}

size_t http_write_head(char *out, size_t cap, int status, size_t content_length, enum http_conn conn,
                       const char *date) {
    static const char ctype[] = "\r\nContent-Type: application/json\r\nContent-Length: ";
    static const char close_hdr[] = "Connection: close\r\n";
    static const char keep_hdr[] = "Connection: keep-alive\r\n";
    const char *r = reason(status);
    size_t rlen = strlen(r);
    size_t need = 9 + 3 + 1 + rlen + sizeof ctype - 1 + 20 + 2 + sizeof keep_hdr - 1 + 6 + HTTP_DATE_LEN + 2 + 2;
    if (cap < need) return 0;
    char *o = out;
    memcpy(o, "HTTP/1.1 ", 9);
    o += 9;
    o[0] = (char)('0' + status / 100);
    o[1] = (char)('0' + (status / 10) % 10);
    o[2] = (char)('0' + status % 10);
    o[3] = ' ';
    o += 4;
    memcpy(o, r, rlen);
    o += rlen;
    memcpy(o, ctype, sizeof ctype - 1);
    o += sizeof ctype - 1;
    o += fmt_u64(o, content_length);
    o[0] = '\r';
    o[1] = '\n';
    o += 2;
    if (conn == HTTP_CONN_CLOSE) {
        memcpy(o, close_hdr, sizeof close_hdr - 1);
        o += sizeof close_hdr - 1;
    } else if (conn == HTTP_CONN_KEEP_ALIVE) {
        memcpy(o, keep_hdr, sizeof keep_hdr - 1);
        o += sizeof keep_hdr - 1;
    }
    if (date) {
        memcpy(o, "Date: ", 6);
        o += 6;
        memcpy(o, date, HTTP_DATE_LEN);
        o += HTTP_DATE_LEN;
        o[0] = '\r';
        o[1] = '\n';
        o += 2;
    }
    o[0] = '\r';
    o[1] = '\n';
    o += 2;
    return (size_t)(o - out);
}
