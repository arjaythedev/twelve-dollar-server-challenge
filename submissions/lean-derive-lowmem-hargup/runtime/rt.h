/* Runtime interface shared by the transport, the SQLite access layer and the generated
 * application code (gen/app_gen.c). Everything is compiled as one translation unit
 * (runtime/server.c), so these are static definitions rather than a linked ABI. */
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef SQLITE_AMALGAMATION   /* the amalgamation, included first, defines the same names */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef long long i64;
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---- requests --------------------------------------------------------------------------- */

enum { M_OTHER = 0, M_GET = 1, M_POST = 2 };

typedef struct {
  u8 method;
  u8 close;            /* client asked for Connection: close (or HTTP/1.0 without keep-alive) */
  u8 has_auth;
  const u8 *path; u32 path_len;      /* without the query string */
  const u8 *auth; u32 auth_len;      /* Authorization header value */
  const u8 *body; u32 body_len;
} req_t;

/* ---- responses -------------------------------------------------------------------------- */

/* A response is either a static constant (generated) or bytes in an owned buffer. The owned
 * buffer reserves HDR_ROOM bytes in front of the body so the header can be written right
 * before it once the body length is known. */
#define HDR_ROOM 96

typedef struct obuf {
  struct obuf *next;   /* per-connection send queue */
  const u8 *data;      /* first byte to send */
  u32 len;             /* bytes to send */
  u32 cap;             /* capacity of mem (0 = static data, nothing to free) */
  u8 *mem;             /* owned allocation (HDR_ROOM + body capacity) */
} obuf;

typedef struct {
  u8 *p, *end;         /* write cursor into the body area */
  u8 *start;           /* first body byte */
  obuf *b;
} out_t;

/* ---- jobs ------------------------------------------------------------------------------- */

#include "job_gen.h"

#endif
