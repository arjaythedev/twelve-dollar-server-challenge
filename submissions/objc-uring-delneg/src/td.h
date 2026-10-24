// Shared declarations for the Objective-C / io_uring server.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <objc/runtime.h>
#include <objc/message.h>

#define TD_DIRECT __attribute__((objc_direct))
#define TD_LIKELY(x) __builtin_expect(!!(x), 1)
#define TD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TD_INLINE static inline __attribute__((always_inline))

// ---------------------------------------------------------------- root class
// No Foundation: every object descends from this root class. Memory is managed
// by hand (no ARC, no retain counts); long-lived objects are never freed.
__attribute__((objc_root_class))
@interface TDObject {
@public
    Class isa;
}
+ (instancetype)make;          // class_createInstance + -init
- (instancetype)init;
@end

// ---------------------------------------------------------------- byte buffer
typedef struct {
    char *p;
    uint32_t len, cap;
} TDBuf;

void td_buf_grow(TDBuf *b, uint32_t need);
TD_INLINE void td_buf_reserve(TDBuf *b, uint32_t extra) {
    if (TD_UNLIKELY(b->len + extra > b->cap)) td_buf_grow(b, b->len + extra);
}
TD_INLINE void td_buf_put(TDBuf *b, const void *s, uint32_t n) {
    td_buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}
#define td_buf_lit(b, s) td_buf_put((b), (s), (uint32_t)(sizeof(s) - 1))
TD_INLINE void td_buf_ch(TDBuf *b, char c) {
    td_buf_reserve(b, 1);
    b->p[b->len++] = c;
}
// writes the decimal form of v (no reserve check: caller reserves 21 bytes)
TD_INLINE uint32_t td_utoa(char *out, uint64_t v) {
    char tmp[24];
    int i = 24;
    do { tmp[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    uint32_t n = (uint32_t)(24 - i);
    memcpy(out, tmp + i, n);
    return n;
}
TD_INLINE void td_buf_int(TDBuf *b, int64_t v) {
    td_buf_reserve(b, 21);
    if (v < 0) { b->p[b->len++] = '-'; v = -v; }
    b->len += td_utoa(b->p + b->len, (uint64_t)v);
}
// JSON string body (no quotes): escapes '"', '\\' and control characters.
void td_buf_json_str(TDBuf *b, const char *s, uint32_t n);

// ---------------------------------------------------------------- JSON input
enum { TDJ_NONE = 0, TDJ_STRING, TDJ_NUMBER, TDJ_OTHER };
typedef struct {
    const char *key;        // top-level key to capture
    uint8_t keylen;
    uint8_t type;           // TDJ_*
    const char *v;          // raw value span (strings: without quotes, still escaped)
    uint32_t vlen;
    bool escaped;           // string contains backslash escapes
} TDJField;
// Validates the whole document. Returns -1 if it is not valid JSON, 0 if valid but
// not an object, 1 for an object (fields filled for matching top-level keys).
int td_json_object(const char *p, uint32_t n, TDJField *f, int nf);
// Decodes a captured string into out (UTF-8); returns length or -1.
int td_json_unescape(const char *s, uint32_t n, char *out, uint32_t cap);

// ---------------------------------------------------------------- SHA-256 / JWT
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t blen; } TDSha256;
void td_sha256_init(TDSha256 *c);
void td_sha256_update(TDSha256 *c, const void *d, size_t n);
void td_sha256_final(TDSha256 *c, uint8_t out[32]);

enum { TD_AUTH_OK = 0, TD_AUTH_MISSING, TD_AUTH_INVALID, TD_AUTH_PAYLOAD };
#define TD_UNAME_MAX 128

@interface TDAuth : TDObject
- (instancetype)initWithSecret:(const char *)secret TD_DIRECT;
// header value (may be NULL); on success fills uid / username
- (int)verify:(const char *)hdr length:(uint32_t)n now:(int64_t)now
          uid:(int64_t *)uid name:(char *)name nameLen:(uint32_t *)nameLen TD_DIRECT;
@end
