/* util.h - buffers, small helpers */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *p;
    uint32_t len, cap;
} buf_t;

void buf_init(buf_t *b);
int buf_reserve(buf_t *b, size_t extra);      /* 0 ok, -1 oom */
int buf_put(buf_t *b, const void *s, size_t n);
void buf_free(buf_t *b);
#define BUF_LIT(b, s) buf_put((b), (s), sizeof(s) - 1)

/* Fast unsigned decimal formatting; returns the end pointer. */
static inline char *u64_to(char *o, uint64_t v)
{
    int n = 1;
    for (uint64_t x = v; x >= 10; x /= 10) n++;
    char *e = o + n;
    do { *--e = (char)('0' + v % 10); v /= 10; } while (v);
    return o + n;
}

/* Monotonic milliseconds since an arbitrary start (used for stalls/uptime). */
uint64_t now_mono_ms(void);

/* Microsecond clock for request-path instrumentation. */
long long now_mono_us(void);

/* Wall clock with sub-second precision (JWT exp/nbf). */
double now_realtime(void);
#ifdef __cplusplus
}
#endif
