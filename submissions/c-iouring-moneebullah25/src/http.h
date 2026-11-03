/* HTTP/1.1 request parsing (in place, no allocation) and response headers. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdint.h>

enum http_method { HTTP_GET, HTTP_POST, HTTP_OTHER };

struct http_req {
    enum http_method method;
    const char *path;
    uint32_t path_len;
    const char *auth; /* Authorization value without surrounding whitespace, or NULL */
    uint32_t auth_len;
    const char *body;
    uint32_t body_len;
    uint32_t head_len;  /* request line + headers + empty line */
    uint32_t total_len; /* head_len + body_len once complete */
    uint8_t keep_alive; /* keep the connection after the response */
    uint8_t expect_100; /* "Expect: 100-continue" was present */
    uint8_t version_11;
};

enum http_parse_status {
    HTTP_COMPLETE,  /* req is filled; total_len bytes were consumed */
    HTTP_PARTIAL,   /* need more bytes */
    HTTP_BAD,       /* malformed: answer 400 and close */
    HTTP_TOO_LARGE, /* headers or body over the limits: answer 413 and close */
};

enum http_parse_status http_parse(const char *buf, uint32_t len, uint32_t max_head, uint32_t max_body,
                                  struct http_req *req);

#define HTTP_DATE_LEN 29 /* "Sun, 06 Nov 1994 08:49:37 GMT" */

enum http_conn { HTTP_CONN_DEFAULT, HTTP_CONN_CLOSE, HTTP_CONN_KEEP_ALIVE };

/* The current time as an RFC 9110 Date value, cached per second. */
const char *http_date_now(void);

/* Writes "HTTP/1.1 <status> <reason>\r\nContent-Type: application/json\r\n
 * Content-Length: N\r\n[Connection: ...\r\n][Date: ...\r\n]\r\n". Returns the length. */
size_t http_write_head(char *out, size_t cap, int status, size_t content_length, enum http_conn conn, const char *date);

#endif
