/* Small helpers shared by the modules: character classes, integer formatting,
 * clocks and logging. */
#ifndef UTIL_H
#define UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* RFC 9110 token characters, used for method and header names. */
static inline int is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
           c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

/* Writes the decimal digits of v (no terminator); returns the count. */
static inline size_t fmt_u64(char *out, uint64_t v) {
    char tmp[20];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static inline size_t fmt_i64(char *out, int64_t v) {
    if (v < 0) {
        out[0] = '-';
        return 1 + fmt_u64(out + 1, (uint64_t)(-(v + 1)) + 1);
    }
    return fmt_u64(out, (uint64_t)v);
}

/* Parses an unsigned decimal of at most 18 digits; returns -1 on junk. */
static inline int parse_u64(const char *s, size_t len, uint64_t *out) {
    if (len == 0 || len > 18) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static inline int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static inline int64_t wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    return ts.tv_sec;
}

/* Logging goes to stderr with a monotonic timestamp; the hot path never logs. */
__attribute__((format(printf, 1, 2))) static inline void log_msg(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    fprintf(stderr, "[%7.3f] %.*s\n", (double)now_ns() / 1e9, n, buf);
}

#endif
