#ifndef TD_ROW_OUT_H
#define TD_ROW_OUT_H

#include <emmintrin.h>
#include <string.h>
#include "td_feed.h"

#define TD_NEEDS_ESCAPE(c) ((c) < 0x20 || (c) == '"' || (c) == '\\')

/* Writes p[0..n] as a JSON string, escaped the way JSON.stringify does, and returns the end.
   Clean 16-byte blocks are copied as they are scanned; the first dirty block restarts bytewise. */
static inline unsigned char *td_put_string(unsigned char *d, const unsigned char *p, int n) {
    static const char hex[] = "0123456789abcdef";
    int i = 0;
    *d++ = '"';
    if (n >= 16) {
        const __m128i quote = _mm_set1_epi8('"');
        const __m128i slash = _mm_set1_epi8('\\');
        const __m128i control = _mm_set1_epi8(0x1f);
        for (;; i += 16) {
            if (i + 16 > n) {
                i = n - 16; /* the last block overlaps bytes already copied clean */
            }
            __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
            __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, slash)),
                                       _mm_cmpeq_epi8(_mm_min_epu8(v, control), v));
            _mm_storeu_si128((__m128i *)(d + i), v); /* clean bytes before a dirty one are needed too */
            int mask = _mm_movemask_epi8(bad);
            if (mask) {
                i += __builtin_ctz(mask);
                break;
            }
            if (i + 16 >= n) {
                d[n] = '"';
                return d + n + 1;
            }
        }
    }
    unsigned char *o = d + i;
    for (; i < n; i++) {
        unsigned char c = p[i];
        if (!TD_NEEDS_ESCAPE(c)) {
            *o++ = c;
            continue;
        }
        *o++ = '\\';
        switch (c) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '\n': *o++ = 'n'; break;
        case '\r': *o++ = 'r'; break;
        case '\t': *o++ = 't'; break;
        case 0x08: *o++ = 'b'; break;
        case 0x0c: *o++ = 'f'; break;
        default:
            *o++ = 'u'; *o++ = '0'; *o++ = '0';
            *o++ = hex[c >> 4]; *o++ = hex[c & 15];
        }
    }
    *o++ = '"';
    return o;
}

/* Writes a non-negative decimal number and returns the end. */
static inline unsigned char *td_put_uint(unsigned char *d, unsigned long long v) {
    static const char pairs[] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    int digits = 1;
    for (unsigned long long limit = 10; v >= limit && digits < 20; limit *= 10) digits++;
    unsigned char *e = d + digits;
    unsigned char *end = e;
    while (v >= 100) {
        unsigned r = (unsigned)(v % 100) * 2;
        v /= 100;
        e -= 2;
        memcpy(e, pairs + r, 2);
    }
    if (v >= 10) memcpy(e - 2, pairs + v * 2, 2);
    else e[-1] = (unsigned char)('0' + v);
    return end;
}

/* One feed or post row object, in the key order of the reference API. Returns the end;
   the caller has reserved six bytes per body and
   author byte plus 160 for the fixed text and numbers. */
static inline unsigned char *td_put_row(unsigned char *d, const td_feed_row *r) {
    memcpy(d, "{\"id\":", 6); d += 6;
    d = td_put_uint(d, (unsigned long long)r->id);
    memcpy(d, ",\"body\":", 8); d += 8;
    d = td_put_string(d, r->body, r->body_len);
    memcpy(d, ",\"created_at\":\"", 15); d += 15;
    memcpy(d, r->created, 24); d += 24;
    memcpy(d, "\",\"author\":", 11); d += 11;
    d = td_put_string(d, r->user, r->user_len);
    memcpy(d, ",\"like_count\":", 14); d += 14;
    d = td_put_uint(d, (unsigned long long)r->likes);
    *d++ = '}';
    return d;
}

#endif
