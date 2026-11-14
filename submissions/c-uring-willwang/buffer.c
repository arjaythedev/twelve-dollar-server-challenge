#include "challenge.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <immintrin.h>
#endif

int buf_reserve(Buf *b, size_t extra) {
    if (extra <= b->cap - b->len) return 0;
    if (extra > SIZE_MAX - b->len) return -1;
    size_t need = b->len + extra;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need && cap <= SIZE_MAX / 2) cap *= 2;
    if (cap < need) cap = need;
    char *p = realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

int buf_append(Buf *b, const void *data, size_t len) {
    if (buf_reserve(b, len)) return -1;
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static const char digit_pairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static inline char *write_pair(char *dst, unsigned n) {
    memcpy(dst, digit_pairs + 2 * n, 2);
    return dst + 2;
}

static inline char *write_first_group(char *dst, unsigned n) {
    if (n < 10) {
        *dst++ = (char)('0' + n);
        return dst;
    }
    if (n < 100) return write_pair(dst, n);
    unsigned hi = n / 100;
    if (hi < 10) *dst++ = (char)('0' + hi);
    else dst = write_pair(dst, hi);
    return write_pair(dst, n % 100);
}

static inline char *write_four(char *dst, unsigned n) {
    dst = write_pair(dst, n / 100);
    return write_pair(dst, n % 100);
}

/* Emit groups directly in wire order so small IDs and counts do not need
 * a temporary buffer or a variable-size copy. */
static inline char *write_int(char *dst, int64_t value) {
    uint64_t n = (uint64_t)value;
    if (value < 0) {
        *dst++ = '-';
        n = 0 - n;
    }
    if (n < 10000) return write_first_group(dst, (unsigned)n);
    if (n < 100000000) {
        unsigned v = (unsigned)n;
        dst = write_first_group(dst, v / 10000);
        return write_four(dst, v % 10000);
    }
    unsigned groups[4];
    unsigned count = 0;
    while (n >= 10000) {
        groups[count++] = (unsigned)(n % 10000);
        n /= 10000;
    }
    dst = write_first_group(dst, (unsigned)n);
    while (count) dst = write_four(dst, groups[--count]);
    return dst;
}

int buf_int(Buf *b, int64_t value) {
    if (buf_reserve(b, 20)) return -1;
    b->len = (size_t)(write_int(b->data + b->len, value) - b->data);
    return 0;
}

/* Test one full register for bytes which require JSON escapes. */
#if defined(__SSE2__)
static inline unsigned special_bytes(__m128i v) {
    __m128i control = _mm_cmpeq_epi8(_mm_subs_epu8(v, _mm_set1_epi8(31)), _mm_setzero_si128());
    __m128i punctuation = _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')),
                                       _mm_cmpeq_epi8(v, _mm_set1_epi8('\\')));
    return (unsigned)_mm_movemask_epi8(_mm_or_si128(control, punctuation));
}
#endif

static size_t plain_run(const char *s, size_t n) {
#if defined(__SSE2__)
    if (n >= 16) {
        size_t last = n - 16;
        for (size_t i = 0; i < last; i += 16) {
            unsigned mask = special_bytes(_mm_loadu_si128((const __m128i *)(s + i)));
            if (mask) return i + (unsigned)__builtin_ctz(mask);
        }
        unsigned mask = special_bytes(_mm_loadu_si128((const __m128i *)(s + last)));
        return mask ? last + (unsigned)__builtin_ctz(mask) : n;
    }
    if (n >= 8) {
        /* Overlapping endpoints include every byte without crossing the string. */
        __m128i start = _mm_loadl_epi64((const __m128i *)s);
        __m128i end = _mm_loadl_epi64((const __m128i *)(s + n - 8));
        unsigned mask = special_bytes(_mm_unpacklo_epi64(start, end));
        if (mask & 255) return (unsigned)__builtin_ctz(mask);
        return mask ? n - 8 + (unsigned)__builtin_ctz(mask >> 8) : n;
    }
#endif
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c == '"' || c == '\\') return i;
    }
    return n;
}

int buf_json_string(Buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    if (n > SIZE_MAX - 2 || buf_reserve(b, n + 2)) return -1;
    b->data[b->len++] = '"';
    while (n) {
        size_t run = plain_run(s, n);
        if (buf_append(b, s, run)) return -1;
        s += run;
        n -= run;
        if (!n) break;
        unsigned char c = (unsigned char)*s++;
        n--;
        char escape[6] = {'\\', 0, '0', '0', hex[c >> 4], hex[c & 15]};
        size_t length = 2;
        switch (c) {
        case '\b': escape[1] = 'b'; break;
        case '\f': escape[1] = 'f'; break;
        case '\n': escape[1] = 'n'; break;
        case '\r': escape[1] = 'r'; break;
        case '\t': escape[1] = 't'; break;
        case '"': case '\\': escape[1] = (char)c; break;
        default: escape[1] = 'u'; length = 6; break;
        }
        if (buf_append(b, escape, length)) return -1;
    }
    return buf_append(b, "\"", 1);
}

/* Copy a full vector and return a mask of bytes that need JSON escaping. */
#if defined(__AVX2__)
static inline __m256i copy_checked_32(char *dst, const char *s) {
    __m256i v = _mm256_loadu_si256((const __m256i *)s);
    _mm256_storeu_si256((__m256i *)dst, v);
    __m256i controls = _mm256_cmpeq_epi8(_mm256_subs_epu8(v, _mm256_set1_epi8(31)),
                                        _mm256_setzero_si256());
    __m256i quotes = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'));
    __m256i slashes = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'));
    return _mm256_or_si256(controls, _mm256_or_si256(quotes, slashes));
}
#endif

/* Check the bytes as they are copied. Overlapping first and last loads keep
 * both ends within the input even when it ends just before an unmapped page. */
static inline unsigned copy_plain(char *dst, const char *s, size_t n) {
#if defined(__AVX2__)
    if (n >= 32) {
        __m256i mask = _mm256_setzero_si256();
        size_t last = n - 32;
        for (size_t i = 0; i < last; i += 32)
            mask = _mm256_or_si256(mask, copy_checked_32(dst + i, s + i));
        return (unsigned)_mm256_movemask_epi8(
            _mm256_or_si256(mask, copy_checked_32(dst + last, s + last)));
    }
#endif
#if defined(__SSE2__)
    if (n >= 16 && n <= 32) {
        __m128i start = _mm_loadu_si128((const __m128i *)s);
        __m128i end = _mm_loadu_si128((const __m128i *)(s + n - 16));
        _mm_storeu_si128((__m128i *)dst, start);
        _mm_storeu_si128((__m128i *)(dst + n - 16), end);
        return special_bytes(start) | special_bytes(end);
    }
    if (n >= 8 && n < 16) {
        __m128i start = _mm_loadl_epi64((const __m128i *)s);
        __m128i end = _mm_loadl_epi64((const __m128i *)(s + n - 8));
        _mm_storel_epi64((__m128i *)dst, start);
        _mm_storel_epi64((__m128i *)(dst + n - 8), end);
        return special_bytes(_mm_unpacklo_epi64(start, end));
    }
#endif
    if (n) memcpy(dst, s, n);
    return plain_run(s, n) != n;
}

/* Reserve and fill one whole row for the common case of unchanged UTF-8;
 * unusual strings use the same complete escaping routine as other responses. */
int buf_post_row(Buf *b, int64_t id, const char *body, size_t bl,
                 const char *timestamp, size_t tl, const char *name, size_t nl, int64_t likes) {
    /* Lengths come from SQLite columns; on the target they are at most INT_MAX. */
    if (buf_reserve(b, 128 + bl + tl + nl)) return -1;
    char *dst = b->data + b->len;
#define WRITE_LITERAL(text) do { memcpy(dst, text, sizeof(text) - 1); dst += sizeof(text) - 1; } while (0)
    WRITE_LITERAL("{\"id\":");
    dst = write_int(dst, id);
    WRITE_LITERAL(",\"body\":\"");
    unsigned escape = copy_plain(dst, body, bl);
    dst += bl;
    WRITE_LITERAL("\",\"created_at\":\"");
    escape |= copy_plain(dst, timestamp, tl);
    dst += tl;
    WRITE_LITERAL("\",\"author\":\"");
    escape |= copy_plain(dst, name, nl);
    dst += nl;
    WRITE_LITERAL("\",\"like_count\":");
    dst = write_int(dst, likes);
    *dst++ = '}';
#undef WRITE_LITERAL
    /* b->len still points to the start of this row if we need to escape. */
    if (escape) {
#define APPEND_LITERAL(text) buf_append(b, text, sizeof(text) - 1)
        return APPEND_LITERAL("{\"id\":") || buf_int(b, id) ||
               APPEND_LITERAL(",\"body\":") || buf_json_string(b, body, bl) ||
               APPEND_LITERAL(",\"created_at\":") || buf_json_string(b, timestamp, tl) ||
               APPEND_LITERAL(",\"author\":") || buf_json_string(b, name, nl) ||
               APPEND_LITERAL(",\"like_count\":") || buf_int(b, likes) || APPEND_LITERAL("}");
#undef APPEND_LITERAL
    }
    b->len = (size_t)(dst - b->data);
    return 0;
}
