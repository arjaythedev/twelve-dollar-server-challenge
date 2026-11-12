#include "http.h"

#include <stdint.h>
#include <string.h>

#define HTTP_HEAD_LIMIT ((size_t)16 * 1024)
#define HTTP_BODY_LIMIT ((size_t)1024 * 1024)

/* Case-insensitive comparison of a header field name against a lowercase literal. */
static int name_eq(const char *name, size_t nlen, const char *lit, size_t llen)
{
    if (nlen != llen)
        return 0;
    for (size_t i = 0; i < nlen; i++) {
        unsigned char a = (unsigned char)name[i];
        unsigned char b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/* Finds the next line ending at or after pos. A line ends at LF; a CR immediately
 * before the LF is not part of the line. On success returns 1 and stores the end of
 * the line content (absolute index, exclusive) and the index just past the LF.
 * Returns 0 when no LF exists in buf[pos..len). */
static int find_line(const char *buf, size_t len, size_t pos, size_t *line_end, size_t *next)
{
    const char *nl = memchr(buf + pos, '\n', len - pos);
    size_t i;

    if (nl == NULL)
        return 0;
    i = (size_t)(nl - buf);
    *line_end = (i > pos && buf[i - 1] == '\r') ? i - 1 : i;
    *next = i + 1;
    return 1;
}

enum http_result http_parse(const char *buf, size_t len, struct http_request *req, size_t *consumed)
{
    size_t line_end, next, pos, head_end;
    size_t content_length = 0;
    int have_content_length = 0;
    const char *sp1, *target, *sp2, *version, *q;

    if (req == NULL)
        return HTTP_BAD;
    memset(req, 0, sizeof(*req));
    if (consumed != NULL)
        *consumed = 0;
    if (len == 0)
        return HTTP_PARTIAL;
    if (buf == NULL)
        return HTTP_BAD;

    /* Request line: METHOD SP TARGET SP HTTP/1.x */
    if (!find_line(buf, len, 0, &line_end, &next))
        return len > HTTP_HEAD_LIMIT ? HTTP_BAD : HTTP_PARTIAL;
    if (next > HTTP_HEAD_LIMIT)
        return HTTP_BAD;

    sp1 = memchr(buf, ' ', line_end);
    if (sp1 == NULL)
        return HTTP_BAD;
    req->method_len = (size_t)(sp1 - buf);
    if (req->method_len == 0)
        return HTTP_BAD;
    req->method = buf;

    target = sp1 + 1;
    if ((size_t)(target - buf) >= line_end)
        return HTTP_BAD;
    sp2 = memchr(target, ' ', line_end - (size_t)(target - buf));
    if (sp2 == NULL)
        return HTTP_BAD;
    req->path_len = (size_t)(sp2 - target);
    if (req->path_len == 0)
        return HTTP_BAD;

    version = sp2 + 1;
    {
        size_t vlen = line_end - (size_t)(version - buf);
        if (!((vlen == 8 && memcmp(version, "HTTP/1.1", 8) == 0) ||
              (vlen == 8 && memcmp(version, "HTTP/1.0", 8) == 0)))
            return HTTP_BAD;
    }

    /* The path is the target up to the query string. */
    req->path = target;
    q = memchr(target, '?', req->path_len);
    if (q != NULL)
        req->path_len = (size_t)(q - target);

    /* Header lines until the empty line. */
    pos = next;
    for (;;) {
        const char *h, *colon, *name, *val;
        size_t hlen, nlen, vlen;

        if (!find_line(buf, len, pos, &line_end, &next))
            return len > HTTP_HEAD_LIMIT ? HTTP_BAD : HTTP_PARTIAL;
        if (next > HTTP_HEAD_LIMIT)
            return HTTP_BAD;

        h = buf + pos;
        hlen = line_end - pos;
        if (hlen == 0) {
            head_end = next;
            break;
        }

        colon = memchr(h, ':', hlen);
        if (colon == NULL)
            return HTTP_BAD;

        name = h;
        nlen = (size_t)(colon - h);
        while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t'))
            nlen--;
        if (nlen == 0)
            return HTTP_BAD;

        val = colon + 1;
        vlen = hlen - (size_t)(val - h);
        while (vlen > 0 && (*val == ' ' || *val == '\t')) {
            val++;
            vlen--;
        }
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            vlen--;

        if (name_eq(name, nlen, "authorization", 13)) {
            req->auth = val;
            req->auth_len = vlen;
            req->has_auth = 1;
        } else if (name_eq(name, nlen, "content-length", 14)) {
            size_t cl = 0;

            if (vlen == 0)
                return HTTP_BAD;
            for (size_t i = 0; i < vlen; i++) {
                unsigned char c = (unsigned char)val[i];
                if (c < '0' || c > '9')
                    return HTTP_BAD;
                cl = cl * 10 + (size_t)(c - '0');
                if (cl > HTTP_BODY_LIMIT)
                    return HTTP_BAD;
            }
            if (have_content_length && cl != content_length)
                return HTTP_BAD;
            have_content_length = 1;
            content_length = cl;
        } else if (name_eq(name, nlen, "transfer-encoding", 17)) {
            return HTTP_BAD;
        }

        pos = next;
    }

    if (len - head_end < content_length)
        return HTTP_PARTIAL;

    req->body = buf + head_end;
    req->body_len = content_length;
    if (consumed != NULL)
        *consumed = head_end + content_length;
    return HTTP_DONE;
}
