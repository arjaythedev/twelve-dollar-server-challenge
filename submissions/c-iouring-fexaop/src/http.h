#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

enum http_result {
    HTTP_DONE = 0,
    HTTP_PARTIAL,  /* more bytes are needed */
    HTTP_BAD       /* malformed head, chunked body, or a head/body over the limits */
};

struct http_request {
    const char *method;
    size_t method_len;
    const char *path;       /* target without a query string */
    size_t path_len;
    const char *auth;       /* raw Authorization header value, trimmed */
    size_t auth_len;
    int has_auth;
    const char *body;
    size_t body_len;
};

/* Parses one HTTP/1.1 request from buf/len, which may contain less than a whole request or more
 * than one. On HTTP_DONE, req points into buf and *consumed is the request's byte length. Head
 * limit 16 KiB, body limit 1 MiB. Only the request line, Authorization, Content-Length and
 * Transfer-Encoding are inspected; other headers are skipped. */
enum http_result http_parse(const char *buf, size_t len, struct http_request *req, size_t *consumed);

#endif
