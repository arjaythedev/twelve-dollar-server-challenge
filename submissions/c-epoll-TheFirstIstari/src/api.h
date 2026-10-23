#pragma once
// Endpoint handlers: route + validate + SQLite + serialize.
// No syscalls, no heap (caller scratch only). JWT secret + clock injected.
#include <stddef.h>
#include <stdint.h>
#include "db.h"
#include "http.h"
#include "jwt.h"

// Scratch layout (single-threaded: one global buffer reused per request):
// [0..4096) JWT decode | [4096..21504) request-body work | [21504..98304) response
#define API_SCRATCH (96u * 1024u)
#define API_JWT_SCRATCH 4096
#define API_BODY_OFF 4096
#define API_BODY_CAP (17u * 1024u)
#define API_RESP_OFF (4096 + 17u * 1024u)
#define API_RESP_CAP (96u * 1024u - (4096 + 17u * 1024u))

typedef struct {
  int status;            // HTTP status
  const char *body;      // into scratch (valid until next api_handle call)
  size_t body_len;
} resp_t;

typedef struct {
  db_t *db;
  const uint8_t *jwt_secret;
  size_t secret_len;
  uint64_t now_s;        // cached wall clock (seconds, for JWT exp)
  uint64_t now_mono;     // cached monotonic clock (seconds, for uptime)
  uint64_t boot_mono;    // startup monotonic time (seconds)
  char *scratch;         // API_SCRATCH bytes, reused across calls
} api_t;

// Route and handle one parsed request. Always returns a valid resp
// (errors mapped per SPEC, exact bodies).
resp_t api_handle(api_t *a, const req_t *req);
