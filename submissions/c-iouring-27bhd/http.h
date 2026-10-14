#ifndef HTTP_H
#define HTTP_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define ROUTE_UNKNOWN 0
#define ROUTE_HEALTH 1
#define ROUTE_FEED 2
#define ROUTE_GET_POST 3
#define ROUTE_CREATE_POST 4
#define ROUTE_LIKE_POST 5

#define HTTP_PARSE_OK 0
#define HTTP_PARSE_ERROR -1
#define HTTP_PARSE_INCOMPLETE -2

typedef struct {
    int route;
    int64_t post_id;
    bool id_valid;
    const char *token;
    size_t token_len;
    const char *body;
    size_t body_len;
    bool keep_alive;
} http_request_t;

static inline void http_init_request(http_request_t *req) {
    req->route = ROUTE_UNKNOWN;
    req->post_id = 0;
    req->id_valid = true;
    req->token = NULL;
    req->token_len = 0;
    req->body = NULL;
    req->body_len = 0;
    req->keep_alive = true;
}

static inline int http_parse_request(const char *buf, size_t len, http_request_t *req) {
    http_init_request(req);
    const char *end = buf + len;

    const char *lf = (const char *)memchr(buf, '\n', len);
    if (!lf) return HTTP_PARSE_INCOMPLETE;
    if (lf == buf || *(lf - 1) != '\r') return HTTP_PARSE_ERROR;
    const char *first_crlf = lf - 1;
    if ((size_t)(first_crlf - buf) < 12) return HTTP_PARSE_ERROR;

    bool is_post = false;
    if (memcmp(buf, "GET ", 4) == 0) {
        const char *p = buf + 4;
        if (memcmp(p, "/feed", 5) == 0 && (p[5] == ' ' || p[5] == '?')) {
            req->route = ROUTE_FEED;
        } else if (memcmp(p, "/posts/", 7) == 0) {
            req->route = ROUTE_GET_POST;
            const char *id_str = p + 7;
            const char *space = (const char *)memchr(id_str, ' ', (size_t)(first_crlf - id_str));
            if (!space) return HTTP_PARSE_ERROR;
            size_t id_len = (size_t)(space - id_str);
            if (id_len == 0 || id_str[0] < '1' || id_str[0] > '9') {
                req->id_valid = false;
            } else {
                int64_t id = 0;
                for (size_t i = 0; i < id_len; i++) {
                    if (id_str[i] < '0' || id_str[i] > '9') {
                        req->id_valid = false;
                        break;
                    }
                    id = id * 10 + (id_str[i] - '0');
                }
                req->post_id = id;
            }
        } else if (memcmp(p, "/health", 7) == 0 && (p[7] == ' ' || p[7] == '?')) {
            req->route = ROUTE_HEALTH;
        } else {
            req->route = ROUTE_UNKNOWN;
        }
    } else if (memcmp(buf, "POST ", 5) == 0) {
        is_post = true;
        const char *p = buf + 5;
        if (memcmp(p, "/posts ", 7) == 0 || memcmp(p, "/posts?", 7) == 0) {
            req->route = ROUTE_CREATE_POST;
        } else if (memcmp(p, "/posts/", 7) == 0) {
            req->route = ROUTE_LIKE_POST;
            const char *id_str = p + 7;
            const char *slash = (const char *)memchr(id_str, '/', (size_t)(first_crlf - id_str));
            if (slash && (first_crlf - slash) >= 6 && memcmp(slash, "/like ", 6) == 0) {
                size_t id_len = (size_t)(slash - id_str);
                if (id_len == 0 || id_str[0] < '1' || id_str[0] > '9') {
                    req->id_valid = false;
                } else {
                    int64_t id = 0;
                    for (size_t i = 0; i < id_len; i++) {
                        if (id_str[i] < '0' || id_str[i] > '9') {
                            req->id_valid = false;
                            break;
                        }
                        id = id * 10 + (id_str[i] - '0');
                    }
                    req->post_id = id;
                }
            } else {
                req->route = ROUTE_UNKNOWN;
            }
        } else {
            req->route = ROUTE_UNKNOWN;
        }
    } else {
        req->route = ROUTE_UNKNOWN;
    }

    /* Single-pass header scan using memchr */
    size_t content_length = 0;
    const char *h = first_crlf + 2;
    const char *body_start = NULL;

    while (h < end) {
        const char *nl = (const char *)memchr(h, '\n', (size_t)(end - h));
        if (!nl) return HTTP_PARSE_INCOMPLETE;
        if (nl == h || *(nl - 1) != '\r') return HTTP_PARSE_ERROR;
        const char *next = nl - 1;
        size_t hlen = (size_t)(next - h);

        if (hlen == 0) {
            body_start = nl + 1;
            break;
        }

        if (is_post) {
            if (hlen > 14 && strncasecmp(h, "authorization:", 14) == 0) {
                const char *val = h + 14;
                while (val < next && (*val == ' ' || *val == '\t')) val++;
                if ((next - val) >= 7 && strncasecmp(val, "bearer ", 7) == 0) {
                    req->token = val + 7;
                    req->token_len = (size_t)(next - req->token);
                }
            } else if (hlen > 15 && strncasecmp(h, "content-length:", 15) == 0) {
                const char *val = h + 15;
                while (val < next && (*val == ' ' || *val == '\t')) val++;
                size_t cl = 0;
                while (val < next && *val >= '0' && *val <= '9') {
                    cl = cl * 10 + (size_t)(*val - '0');
                    val++;
                }
                content_length = cl;
            }
        }
        if (hlen > 11 && (h[0] == 'C' || h[0] == 'c') && strncasecmp(h, "connection:", 11) == 0) {
            const char *val = h + 11;
            while (val < next && (*val == ' ' || *val == '\t')) val++;
            if ((next - val) >= 5 && strncasecmp(val, "close", 5) == 0) {
                req->keep_alive = false;
            }
        }
        h = nl + 1;
    }

    if (!body_start) return HTTP_PARSE_INCOMPLETE;

    size_t actual_body = (size_t)(end - body_start);
    if (content_length > 0) {
        if (actual_body < content_length) return HTTP_PARSE_INCOMPLETE;
        req->body = body_start;
        req->body_len = content_length;
    } else {
        req->body = body_start;
        req->body_len = actual_body;
    }

    return HTTP_PARSE_OK;
}

#endif /* HTTP_H */