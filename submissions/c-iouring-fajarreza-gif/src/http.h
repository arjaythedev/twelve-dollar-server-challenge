#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define METHOD_UNKNOWN 0
#define METHOD_GET 1
#define METHOD_POST 2

// 32-bit SWAR constants (little-endian x86-64)
#define METHOD_GET_U32  0x20544547U // "GET " -> 'G' | ('E'<<8) | ('T'<<16) | (' '<<24)
#define METHOD_POST_U32 0x54534f50U // "POST" -> 'P' | ('O'<<8) | ('S'<<16) | ('T'<<24)

typedef struct {
    int method;
    const char *path;
    size_t path_len;
    const char *auth_token;
    size_t auth_token_len;
    int has_auth_header;
    int is_bearer;
    size_t content_length;
    const char *body;
    size_t body_len;
    int keep_alive;
} http_request_t;

/* Parse HTTP/1.1 request */
static inline int http_parse_request(const char *buf, size_t len, http_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->keep_alive = 1; // HTTP/1.1 default

    if (len < 14) return -1; // minimal request length

    // 1. Method (Single 32-bit SWAR comparison)
    const char *p = buf;
    const char *end = buf + len;
    uint32_t m4;
    memcpy(&m4, p, sizeof(m4));
    if (m4 == METHOD_GET_U32) {
        req->method = METHOD_GET;
        p += 4;
    } else if (m4 == METHOD_POST_U32 && p[4] == ' ') {
        req->method = METHOD_POST;
        p += 5;
    } else {
        req->method = METHOD_UNKNOWN;
        while (p < end && *p != ' ') p++;
        if (p < end) p++;
    }

    // 2. Path
    req->path = p;
    while (p < end && *p != ' ' && *p != '\r' && *p != '\n') p++;
    req->path_len = (size_t)(p - req->path);
    if (p >= end || *p != ' ') return -1;

    // Skip to first header line
    const char *line = strstr(p, "\r\n");
    if (!line) return -1;
    line += 2;

    // 3. Headers
    while (line < end) {
        if (line[0] == '\r' && line + 1 < end && line[1] == '\n') {
            line += 2;
            break; // End of headers
        }
        if (line[0] == '\n') {
            line += 1;
            break;
        }

        const char *next_line = strstr(line, "\r\n");
        if (!next_line) break;
        size_t hlen = (size_t)(next_line - line);

        // Check Authorization header
        if (hlen >= 14 && strncasecmp(line, "authorization:", 14) == 0) {
            req->has_auth_header = 1;
            const char *val = line + 14;
            while (val < next_line && (*val == ' ' || *val == '\t')) val++;
            if (next_line - val >= 7 && strncmp(val, "Bearer ", 7) == 0) {
                req->is_bearer = 1;
                val += 7;
                while (val < next_line && (*val == ' ' || *val == '\t')) val++;
                req->auth_token = val;
                req->auth_token_len = (size_t)(next_line - val);
                // trim trailing spaces
                while (req->auth_token_len > 0 && 
                       (req->auth_token[req->auth_token_len - 1] == ' ' || 
                        req->auth_token[req->auth_token_len - 1] == '\t')) {
                    req->auth_token_len--;
                }
            }
        } else if (hlen >= 15 && strncasecmp(line, "content-length:", 15) == 0) {
            const char *val = line + 15;
            while (val < next_line && (*val == ' ' || *val == '\t')) val++;
            req->content_length = (size_t)strtoul(val, NULL, 10);
        } else if (hlen >= 11 && strncasecmp(line, "connection:", 11) == 0) {
            const char *val = line + 11;
            while (val < next_line && (*val == ' ' || *val == '\t')) val++;
            if (strncasecmp(val, "close", 5) == 0) {
                req->keep_alive = 0;
            }
        }

        line = next_line + 2;
    }

    req->body = line;
    req->body_len = (size_t)(end - line);
    if (req->body_len > req->content_length && req->content_length > 0) {
        req->body_len = req->content_length;
    }

    return 0;
}

/* Fast JSON string escaper */
static inline size_t json_escape_string(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    size_t o = 0;
    for (size_t i = 0; i < src_len && o + 6 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"'; break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\b': dst[o++] = '\\'; dst[o++] = 'b'; break;
            case '\f': dst[o++] = '\\'; dst[o++] = 'f'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n'; break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r'; break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't'; break;
            default:
                if (c < 0x20) {
                    o += snprintf(dst + o, dst_cap - o, "\\u%04x", c);
                } else {
                    dst[o++] = (char)c;
                }
                break;
        }
    }
    return o;
}

/* Parse post body from JSON: {"body":"..."} */
#define POST_BODY_OK 0
#define POST_BODY_MALFORMED 1
#define POST_BODY_REQUIRED 2
#define POST_BODY_TOO_LONG 3

static inline int parse_post_body(const char *raw, size_t raw_len, char *out_body, size_t out_cap, size_t *out_len) {
    if (!raw || raw_len == 0) return POST_BODY_MALFORMED;

    // Check basic JSON syntax
    const char *p = raw;
    const char *end = raw + raw_len;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '{') return POST_BODY_MALFORMED;

    const char *key = strstr(p, "\"body\"");
    if (!key) {
        // If not malformed JSON, then body is missing -> body is required
        // Check if there is closing brace
        const char *close_brace = memchr(p, '}', (size_t)(end - p));
        if (!close_brace) return POST_BODY_MALFORMED;
        return POST_BODY_REQUIRED;
    }

    const char *colon = strchr(key + 6, ':');
    if (!colon) return POST_BODY_MALFORMED;
    p = colon + 1;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return POST_BODY_MALFORMED;
    if (*p != '"') {
        // numeric, boolean, null, or array/object -> body is required
        return POST_BODY_REQUIRED;
    }
    p++; // skip starting quote

    // Extract and unescape string until unescaped closing quote
    char unescaped[2048];
    size_t u_len = 0;
    int closed = 0;
    while (p < end && u_len + 1 < sizeof(unescaped)) {
        if (*p == '"') {
            closed = 1;
            p++;
            break;
        }
        if (*p == '\\') {
            p++;
            if (p >= end) return POST_BODY_MALFORMED;
            if (*p == '"') unescaped[u_len++] = '"';
            else if (*p == '\\') unescaped[u_len++] = '\\';
            else if (*p == '/') unescaped[u_len++] = '/';
            else if (*p == 'b') unescaped[u_len++] = '\b';
            else if (*p == 'f') unescaped[u_len++] = '\f';
            else if (*p == 'n') unescaped[u_len++] = '\n';
            else if (*p == 'r') unescaped[u_len++] = '\r';
            else if (*p == 't') unescaped[u_len++] = '\t';
            else if (*p == 'u') {
                // \uXXXX
                if (p + 4 >= end) return POST_BODY_MALFORMED;
                // For ASCII or simple unicode, keep raw or decode
                unescaped[u_len++] = '?';
                p += 4;
            } else {
                unescaped[u_len++] = *p;
            }
            p++;
        } else {
            unescaped[u_len++] = *p++;
        }
    }
    if (!closed) return POST_BODY_MALFORMED;

    // Check rest of JSON up to closing brace
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '}') return POST_BODY_MALFORMED;

    // Trim leading and trailing whitespace
    size_t start_idx = 0;
    while (start_idx < u_len && isspace((unsigned char)unescaped[start_idx])) start_idx++;
    if (start_idx >= u_len) return POST_BODY_REQUIRED; // empty after trim

    size_t end_idx = u_len;
    while (end_idx > start_idx && isspace((unsigned char)unescaped[end_idx - 1])) end_idx--;

    size_t trimmed_len = end_idx - start_idx;
    if (trimmed_len == 0) return POST_BODY_REQUIRED;
    if (trimmed_len > 500) return POST_BODY_TOO_LONG;
    if (trimmed_len >= out_cap) trimmed_len = out_cap - 1;

    memcpy(out_body, unescaped + start_idx, trimmed_len);
    out_body[trimmed_len] = '\0';
    *out_len = trimmed_len;

    return POST_BODY_OK;
}

/* HTTP response formatters */
static inline size_t format_response(char *buf, size_t cap, int status, const char *status_str,
                                     const char *body, size_t body_len, int keep_alive) {
    const char *conn = keep_alive ? "keep-alive" : "close";
    int header_len = snprintf(buf, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, status_str, body_len, conn);

    if (header_len < 0 || (size_t)header_len + body_len > cap) return 0;
    memcpy(buf + header_len, body, body_len);
    return (size_t)header_len + body_len;
}

static inline size_t format_error(char *buf, size_t cap, int status, const char *status_str,
                                  const char *msg, int keep_alive) {
    char body[256];
    int body_len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return format_response(buf, cap, status, status_str, body, (size_t)body_len, keep_alive);
}

#endif
