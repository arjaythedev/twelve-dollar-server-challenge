#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef long long i64;

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

#define FEED_SIZE      20
#define MAX_BODY_CHARS 500
#define MAX_HEAD       16384
#define MAX_BODY       16384
#define HDR_ROOM       128

enum { M_OTHER = 0, M_GET = 1, M_POST = 2 };
/* Job kinds are defined by the application layer (gen/app_gen.c). */

/* Parsed HTTP/1.1 request (http.c) */
typedef struct req_t {
  u8 method, has_auth, close;
  const u8 *path, *auth, *body;
  u32 path_len, auth_len, body_len;
} req_t;

/* Output buffer chain (out.c). `refs` counts owners of a shared response:
 * 0 = single owner, N>0 = refcounted. */
typedef struct obuf {
  const u8 *data;
  u32 len, cap;
  u8 *mem;
  struct obuf *next;
  u32 refs;
} obuf;

typedef struct out_t {
  u8 *start, *p, *end;
  obuf *b;
} out_t;

/* Per-request job inside a batch (uring.c / poll.c / app.c) */
typedef struct job_t {
  u32 conn;
  u16 gen;
  u8 kind, is_write, close_after;
  i64 id;
  i64 uid;
  const u8 *name;
  u32 name_len;
  const u8 *text;
  u32 text_len;
  obuf *resp;
} job_t;

/* JWT verification (jwt.c) */
enum { JWT_OK = 0, JWT_INVALID = 1, JWT_PAYLOAD = 2 };
typedef struct claims_t {
  i64 uid;
  u8 name[512];
  u32 name_len;
} claims_t;

/* JSON input scanner (json.c) */
enum { JT_NONE = 0, JT_STRING = 1, JT_NUMBER = 2, JT_OTHER = 3 };
typedef struct jmember {
  const char *key;
  u32 key_len;
  u8 type;
  const u8 *v;
  u32 v_len;
  u8 escaped;
} jmember;

/* Wall clock seconds, updated once per event-loop iteration */
extern i64 now_s;

#endif
