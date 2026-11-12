#ifndef JSON_H
#define JSON_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

enum json_body_result {
    JSON_BODY_OK = 0,
    JSON_BODY_MALFORMED,   /* not valid JSON */
    JSON_BODY_MISSING,     /* no "body" member, or it is not a string, or empty after trim */
    JSON_BODY_TOO_LONG     /* more than 500 characters (code points) after trim */
};

/* Validates the whole JSON document in buf/len, takes the last top-level "body" string member
 * (duplicate keys: last wins, as JSON.parse), decodes its escapes into out (capacity cap), then
 * trims JavaScript's String.prototype.trim whitespace set and checks the 500-character limit.
 * out is NUL-terminated when the result is JSON_BODY_OK. */
enum json_body_result json_extract_body(const char *buf, size_t len, char *out, size_t cap,
                                        size_t *out_len);

/* Escapes a UTF-8 string for a JSON string literal: only '"', '\\' and U+0000..U+001F are
 * escaped; '<', '>', '&', '/' and non-ASCII bytes pass through. dst must have room for 6*n.
 * Returns the number of bytes written. */
size_t json_escape(char *dst, const char *s, size_t n);

#define ESC_ONES 0x0101010101010101ULL
#define ESC_HIGHS 0x8080808080808080ULL
static inline uint64_t esc_haszero(uint64_t x) { return (x - ESC_ONES) & ~x & ESC_HIGHS; }
/* Any byte equal to '"' or '\\', or below 0x20, in these eight bytes? */
static inline int esc_word(uint64_t v)
{
    uint64_t q = v ^ (ESC_ONES * 0x22);
    uint64_t b = v ^ (ESC_ONES * 0x5c);
    uint64_t lt = (v - ESC_ONES * 0x20) & ~v & ESC_HIGHS;
    return (esc_haszero(q) | esc_haszero(b) | lt) != 0;
}

static inline size_t json_escape_inline(char *dst, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t w = 0, i = 0;
#if defined(__AVX2__)
    {
        const __m256i quote = _mm256_set1_epi8('"');
        const __m256i bslash = _mm256_set1_epi8('\\');
        const __m256i ctl = _mm256_set1_epi8(0x1f);
        const __m256i zero = _mm256_setzero_si256();
        while (i + 32 <= n) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
            __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, quote),
                                                        _mm256_cmpeq_epi8(v, bslash)),
                                        _mm256_cmpeq_epi8(_mm256_subs_epu8(v, ctl), zero));
            if (_mm256_movemask_epi8(m)) break;
            _mm256_storeu_si256((__m256i *)(void *)(dst + w), v);
            w += 32;
            i += 32;
        }
    }
#endif
    while (i < n) {
        if (i + 8 <= n) {
            uint64_t v;
            memcpy(&v, s + i, 8);
            if (!esc_word(v)) {
                memcpy(dst + w, s + i, 8);
                w += 8;
                i += 8;
                continue;
            }
        }
        {
        unsigned char c = (unsigned char)s[i++];
        switch (c) {
        case '"':
            dst[w++] = '\\';
            dst[w++] = '"';
            break;
        case '\\':
            dst[w++] = '\\';
            dst[w++] = '\\';
            break;
        case '\n':
            dst[w++] = '\\';
            dst[w++] = 'n';
            break;
        case '\r':
            dst[w++] = '\\';
            dst[w++] = 'r';
            break;
        case '\t':
            dst[w++] = '\\';
            dst[w++] = 't';
            break;
        case 0x08:
            dst[w++] = '\\';
            dst[w++] = 'b';
            break;
        case 0x0C:
            dst[w++] = '\\';
            dst[w++] = 'f';
            break;
        default:
            if (c < 0x20) {
                dst[w++] = '\\';
                dst[w++] = 'u';
                dst[w++] = '0';
                dst[w++] = '0';
                dst[w++] = hex[c >> 4];
                dst[w++] = hex[c & 0x0F];
            } else {
                dst[w++] = (char)c;
            }
            break;
        }
        }
    }
    return w;
}

#endif
