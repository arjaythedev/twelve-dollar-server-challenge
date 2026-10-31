/* HTTP/1.1 request parser: one request at a time, zero-copy, no allocation. */
#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#include "strview.h"

enum {
	HTTP_MAX_HEADER_BYTES = 8192,
	HTTP_MAX_REQUEST_BYTES = 65536,
};

typedef enum http_method {
	HTTP_METHOD_OTHER,
	HTTP_METHOD_GET,
	HTTP_METHOD_POST,
} http_method;

/* All views point into the buffer that was parsed. */
typedef struct http_request {
	http_method method;
	strview path;            /* without the query string */
	strview authorization;   /* header value, empty when absent */
	strview body;
	bool close_after;        /* HTTP/1.0 without keep-alive, or "Connection: close" */
	bool expects_continue;   /* "Expect: 100-continue" */
	bool headers_complete;   /* meaningful when the parse result is HTTP_NEED_MORE */
	size_t total_len;        /* bytes the request occupies: headers + body */
} http_request;

typedef enum http_parse_result {
	HTTP_COMPLETE,
	HTTP_NEED_MORE,
	HTTP_MALFORMED,
	HTTP_TOO_LARGE,
} http_parse_result;

http_parse_result http_parse_request(strview input, http_request *request);

#endif
