#ifndef TD_JSON_SCAN_H
#define TD_JSON_SCAN_H

#include <emmintrin.h>

/* Index of the first byte of p[0..n] that JSON string escaping must change (a control
   character, '"' or '\\'), or n when there is none. */
static inline int td_json_scan(const unsigned char *p, int n) {
    int i = 0;
    if (n >= 16) {
        const __m128i quote = _mm_set1_epi8('"');
        const __m128i slash = _mm_set1_epi8('\\');
        const __m128i control = _mm_set1_epi8(0x1f);
        for (;; i += 16) {
            if (i + 16 > n) {
                i = n - 16; /* the last block overlaps bytes already found clean */
            }
            __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
            __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, slash)),
                                       _mm_cmpeq_epi8(_mm_min_epu8(v, control), v));
            int mask = _mm_movemask_epi8(bad);
            if (mask) {
                return i + __builtin_ctz(mask);
            }
            if (i + 16 >= n) {
                return n;
            }
        }
    }
    for (; i < n; i++) {
        if (p[i] < 0x20 || p[i] == '"' || p[i] == '\\') {
            return i;
        }
    }
    return n;
}

#endif
