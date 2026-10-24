// config.h — everything the build decides (config/*.sh pass these as -D flags; the defaults here are the
// submitted build). app.c includes this once; nothing here allocates or runs.
#ifndef CURSED_CONFIG_H
#define CURSED_CONFIG_H
// ---------------------------------------------------------------- cursedness (config/cursed.sh): SQLite and JWT modes
#ifndef APP_DB_DIRECT
#define APP_DB_DIRECT 1
#endif
#ifndef APP_DB_VFS
#define APP_DB_VFS 1
#endif
#ifndef APP_DB_CKPT
#define APP_DB_CKPT 2  // 2 thread, 3 uring, 1 inline, 0 SQLite autocheckpoint
#endif
#ifndef APP_DB_EXCL
#define APP_DB_EXCL 1
#endif
#ifndef APP_DB_WAL_DROP
#define APP_DB_WAL_DROP 1
#endif
#ifndef APP_SQLITE_LINK
#define APP_SQLITE_LINK 0  // 0: unity (engine.c #includes sqlite3.c); 1: separate unit, public API only
#endif
#ifndef APP_DB_CACHE_KIB
#define APP_DB_CACHE_KIB 8192
#endif
#ifndef APP_JWT_SCALAR
#define APP_JWT_SCALAR 0
#endif
#if APP_JWT_SCALAR
#define JWT_VERIFY jwt_verify_batch_scalar
#else
#define JWT_VERIFY jwt_verify_batch
#endif
// ---------------------------------------------------------------- parser selection (config/parsers.sh: PARSER_FAST /
// PARSER_SLOW) APP_FAST  fast path for every request:   2 batch  = bp_parse lanes for the first request per connection
// (>= APP_BP_MIN
//                                                     connections in the wakeup), fast_parse for the rest  [default]
//                                           1 single = fast_parse per request
//                                           3 learn  = fast_parse_learn (learned per-route header layout)
//                                           0 slow   = no fast path: every request goes through the slow parser
// APP_SLOW  FP_FALLBACK path (all behind picohttpparser's phr_parse_request signature):
//           0 fb = fbparse.h [default] · 1 pico · 2 llhttp · 3 uws (HTTP/1.1 only) · 4 hparse (Zig)
//           5 httparse (Rust)
// Checks for test builds: -DAPP_CHECK_LANES (bp_parse == fast_parse per lane), -DAPP_CHECK_SLOW (fb == pico).
#ifndef APP_FAST
#if defined(APP_LANE_PARSE) && !APP_LANE_PARSE
#define APP_FAST 1
#else
#define APP_FAST 2
#endif
#endif
#ifndef APP_SLOW
#ifdef APP_PICO
#define APP_SLOW 1
#else
#define APP_SLOW 0
#endif
#endif
#undef APP_LANE_PARSE
#define APP_LANE_PARSE (APP_FAST == 2)
#if APP_LANE_PARSE
#include "bp.h"
#else
#include BP_HTTP_H
#endif
#if APP_SLOW != 0 || defined(APP_CHECK_SLOW)
#include "picohttpparser.h"  // struct phr_header (and pico itself for APP_SLOW 1 / the fb check)
#endif
#define SLOW_SIG(name)                                                                                                \
  int name(const char *buf, size_t len, const char **method, size_t *method_len, const char **path, size_t *path_len, \
           int *minor_version, struct phr_header *headers, size_t *num_headers, size_t last_len)
#if APP_SLOW == 0
#include "fbparse.h"
#define SLOW_PARSE fb_parse_request
#define SLOW_NAME  "fbparse"
typedef struct fb_header slow_hdr;
#else
typedef struct phr_header slow_hdr;
#if APP_SLOW == 1
#define SLOW_PARSE phr_parse_request
#define SLOW_NAME  "picohttpparser"
#elif APP_SLOW == 2
SLOW_SIG(lx_parse_request);
#define SLOW_PARSE lx_parse_request
#define SLOW_NAME  "llhttp"
#elif APP_SLOW == 3
SLOW_SIG(uw_parse_request);
#define SLOW_PARSE uw_parse_request
#define SLOW_NAME  "uWS"
#elif APP_SLOW == 4
SLOW_SIG(hp_parse_request);
#define SLOW_PARSE hp_parse_request
#define SLOW_NAME  "hparse"
#elif APP_SLOW == 5
SLOW_SIG(hx_parse_request);
#define SLOW_PARSE hx_parse_request
#define SLOW_NAME  "httparse"
#else
#error "APP_SLOW must be 0..5"
#endif
#endif
#if APP_FAST == 3
#define FAST_PARSE(b, n, r) fast_parse_learn(b, n, r, &app_learn)
#elif APP_FAST == 0
#define FAST_PARSE(b, n, r) ((void)(b), (void)(n), (void)(r), FP_FALLBACK)
#else
#define FAST_PARSE(b, n, r) fast_parse(b, n, r)
#endif
#if APP_FAST == 2
#define FAST_NAME "batch"
#elif APP_FAST == 1
#define FAST_NAME "single"
#elif APP_FAST == 3
#define FAST_NAME "learn"
#else
#define FAST_NAME "slow"
#endif
#ifndef APP_SQLITE_NPATCH
#define APP_SQLITE_NPATCH 1
#endif
#ifndef APP_BP_MIN
#define APP_BP_MIN 8  // measured on a 1-vCPU Xeon: +10% at N=1, ±0 at 8, −4/−5% at 16/32
#endif
// ---------------------------------------------------------------- batch bounds (app.c)
#ifndef APP_RELEASE_CHUNK
#define APP_RELEASE_CHUNK 1024  // held POST connections re-presented per loop tick once the WAL has drained
#endif
#ifndef APP_ARENA_KEEP
#define APP_ARENA_KEEP (8u << 20)  // a reply arena grown past this by one batch is freed after it (next starts at 1 MiB)
#endif
#endif
