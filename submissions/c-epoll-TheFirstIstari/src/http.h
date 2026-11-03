#pragma once
// In-place HTTP/1.1 request parser. No heap, no syscalls, no mutation of the
// input except in-place compaction of chunked bodies. Stateless across calls:
// the caller accumulates bytes in its own buffer and re-scans on new data.
#include <stddef.h>
#include <stdint.h>

#define HTTP_MAX_TOTAL (16u * 1024u)  // SPEC: bodies over 16KB are out of scope

typedef struct {
  const char *method; size_t mlen;
  const char *path;   size_t plen;    // query string stripped
  const char *auth;   size_t auth_len;// Authorization value, NULL if absent
  int         conn_close;             // client sent Connection: close
  int         expect_continue;        // client sent Expect: 100-continue
  const char *body;   size_t body_len;
  size_t      consumed;               // bytes of buf belonging to this request
  int         headers_done;           // header fields valid even if body pending
} req_t;

// Scan buf[0..len) for one complete request.
// Returns 1 complete (out filled), 0 need-more-data,
// -1 framing error (close), -2 over HTTP_MAX_TOTAL (close, out of scope).
// On 1 with chunked encoding the body is compacted in place (buf mutated);
// out->body points into buf and out->consumed covers the whole request.
int http_parse(char *buf, size_t len, req_t *out);
