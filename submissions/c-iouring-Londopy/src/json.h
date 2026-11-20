/* JSON output helpers and a small strict JSON reader.
 *
 * Output: string escaping (only what JSON requires: quote, backslash, control
 * characters; non-ASCII passes through as UTF-8) and integer formatting.
 * Input: a validating parser for the two places we read JSON: POST /posts
 * bodies and JWT header/payload segments. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------- output ---------- */

static const char DIGITS2[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

/* Writes v in decimal, returns the end. */
static inline char *u64_to_dec(char *o, uint64_t v) {
    char tmp[24];
    char *p = tmp + sizeof tmp;
    while (v >= 100) {
        unsigned r = (unsigned)(v % 100);
        v /= 100;
        p -= 2;
        memcpy(p, DIGITS2 + r * 2, 2);
    }
    if (v >= 10) {
        p -= 2;
        memcpy(p, DIGITS2 + v * 2, 2);
    } else {
        *--p = (char)('0' + v);
    }
    size_t n = (size_t)(tmp + sizeof tmp - p);
    memcpy(o, p, n);
    return o + n;
}

static inline char *i64_to_dec(char *o, int64_t v) {
    if (v < 0) {
        *o++ = '-';
        return u64_to_dec(o, (uint64_t)0 - (uint64_t)v);
    }
    return u64_to_dec(o, (uint64_t)v);
}

#define SWAR_ONES 0x0101010101010101ULL
#define SWAR_HIGH 0x8080808080808080ULL
/* Nonzero if any byte of x is '"', '\\' or < 0x20. Bytes >= 0x80 never match. */
static inline uint64_t swar_needs_escape(uint64_t x) {
    uint64_t lt = (x - SWAR_ONES * 0x20) & ~x;
    uint64_t q = x ^ (SWAR_ONES * '"');
    uint64_t b = x ^ (SWAR_ONES * '\\');
    q = (q - SWAR_ONES) & ~q;
    b = (b - SWAR_ONES) & ~b;
    return (lt | q | b) & SWAR_HIGH;
}

/* Worst-case growth of json_escape: 6 output bytes per input byte. */
#define JSON_ESCAPE_MAX(n) ((n) * 6)

static inline char *json_escape_byte(char *o, unsigned char c) {
    static const char hex[] = "0123456789abcdef";
    switch (c) {
    case '"': *o++ = '\\'; *o++ = '"'; break;
    case '\\': *o++ = '\\'; *o++ = '\\'; break;
    case '\n': *o++ = '\\'; *o++ = 'n'; break;
    case '\r': *o++ = '\\'; *o++ = 'r'; break;
    case '\t': *o++ = '\\'; *o++ = 't'; break;
    case '\b': *o++ = '\\'; *o++ = 'b'; break;
    case '\f': *o++ = '\\'; *o++ = 'f'; break;
    default:
        if (c < 0x20) {
            memcpy(o, "\\u00", 4);
            o[4] = hex[c >> 4];
            o[5] = hex[c & 15];
            o += 6;
        } else {
            *o++ = (char)c;
        }
    }
    return o;
}

#ifdef __AVX2__
#include <immintrin.h>
#endif

/* Escapes s[0..n) as the inside of a JSON string (no surrounding quotes).
 * Never writes past the returned end: the 32-byte speculative store only runs
 * while 32 or more input bytes remain, and each input byte produces at least
 * one output byte. */
static inline char *json_escape(char *o, const char *s, size_t n) {
    size_t i = 0;
#ifdef __AVX2__
    const __m256i q = _mm256_set1_epi8('"'), bs = _mm256_set1_epi8('\\'), ctl = _mm256_set1_epi8(0x1F);
    while (i + 32 <= n) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(s + i));
        _mm256_storeu_si256((__m256i *)o, x); /* speculative copy; fixed up below if needed */
        __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(x, q), _mm256_cmpeq_epi8(x, bs)),
                                    _mm256_cmpeq_epi8(_mm256_min_epu8(x, ctl), x)); /* x <= 0x1F */
        unsigned mask = (unsigned)_mm256_movemask_epi8(m);
        if (!mask) {
            o += 32;
            i += 32;
            continue;
        }
        unsigned k = (unsigned)__builtin_ctz(mask);
        o += k;
        i += k;
        o = json_escape_byte(o, (unsigned char)s[i]);
        i++;
    }
#endif
    while (i + 8 <= n) {
        uint64_t x;
        memcpy(&x, s + i, 8);
        memcpy(o, s + i, 8); /* speculative copy; fixed up below if needed */
        uint64_t m = swar_needs_escape(x);
        if (!m) {
            o += 8;
            i += 8;
            continue;
        }
        unsigned k = (unsigned)__builtin_ctzll(m) >> 3; /* first byte needing escape */
        o += k;
        i += k;
        o = json_escape_byte(o, (unsigned char)s[i]);
        i++;
    }
    for (; i < n; i++) o = json_escape_byte(o, (unsigned char)s[i]);
    return o;
}

/* ---------- input ---------- */

static inline const char *json_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline char *utf8_put(char *o, uint32_t cp) {
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* Parses a JSON string starting at the opening quote. If out is non-NULL, the
 * decoded UTF-8 is written there (the decoded form is never longer than the
 * encoded form, so out needs at most e - p bytes) and *outlen is set.
 * Returns the position after the closing quote, or NULL if malformed. */
static const char *json_string(const char *p, const char *e, char *out, size_t *outlen) {
    if (p >= e || *p != '"') return NULL;
    p++;
    char *o = out;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            if (out) *outlen = (size_t)(o - out);
            return p + 1;
        }
        if (c < 0x20) return NULL;
        if (c != '\\') {
            if (out) *o++ = (char)c;
            p++;
            continue;
        }
        if (++p >= e) return NULL;
        char esc = *p++;
        char d = 0;
        switch (esc) {
        case '"': d = '"'; break;
        case '\\': d = '\\'; break;
        case '/': d = '/'; break;
        case 'b': d = '\b'; break;
        case 'f': d = '\f'; break;
        case 'n': d = '\n'; break;
        case 'r': d = '\r'; break;
        case 't': d = '\t'; break;
        case 'u': {
            if (e - p < 4) return NULL;
            int h0 = hexval(p[0]), h1 = hexval(p[1]), h2 = hexval(p[2]), h3 = hexval(p[3]);
            if ((h0 | h1 | h2 | h3) < 0) return NULL;
            uint32_t cp = (uint32_t)(h0 << 12 | h1 << 8 | h2 << 4 | h3);
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                int l0 = hexval(p[2]), l1 = hexval(p[3]), l2 = hexval(p[4]), l3 = hexval(p[5]);
                if ((l0 | l1 | l2 | l3) >= 0) {
                    uint32_t lo = (uint32_t)(l0 << 12 | l1 << 8 | l2 << 4 | l3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* lone surrogate */
            if (out) o = utf8_put(o, cp);
            continue;
        }
        default:
            return NULL;
        }
        if (out) *o++ = d;
    }
    return NULL;
}

/* Validates a JSON number per RFC 8259. Returns the end or NULL. */
static inline const char *json_number(const char *p, const char *e) {
    if (p < e && *p == '-') p++;
    if (p >= e) return NULL;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < e && *p >= '0' && *p <= '9') p++;
    } else {
        return NULL;
    }
    if (p < e && *p == '.') {
        p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    return p;
}

/* Skips (and validates) any JSON value. Returns the end or NULL. */
static const char *json_skip(const char *p, const char *e, int depth) {
    if (depth > 64) return NULL;
    p = json_ws(p, e);
    if (p >= e) return NULL;
    switch (*p) {
    case '"':
        return json_string(p, e, NULL, NULL);
    case '{':
        p = json_ws(p + 1, e);
        if (p < e && *p == '}') return p + 1;
        for (;;) {
            p = json_string(json_ws(p, e), e, NULL, NULL);
            if (!p) return NULL;
            p = json_ws(p, e);
            if (p >= e || *p != ':') return NULL;
            p = json_skip(p + 1, e, depth + 1);
            if (!p) return NULL;
            p = json_ws(p, e);
            if (p >= e) return NULL;
            if (*p == '}') return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    case '[':
        p = json_ws(p + 1, e);
        if (p < e && *p == ']') return p + 1;
        for (;;) {
            p = json_skip(p, e, depth + 1);
            if (!p) return NULL;
            p = json_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ']') return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    case 't':
        return (e - p >= 4 && memcmp(p, "true", 4) == 0) ? p + 4 : NULL;
    case 'f':
        return (e - p >= 5 && memcmp(p, "false", 5) == 0) ? p + 5 : NULL;
    case 'n':
        return (e - p >= 4 && memcmp(p, "null", 4) == 0) ? p + 4 : NULL;
    default:
        return json_number(p, e);
    }
}

/* Iterates the members of a top-level JSON object. For each member, calls
 * cb(ctx, key, keylen, value_start, value_end) with the raw value span. The
 * key is decoded into keybuf (members with keys longer than keycap are
 * validated but reported with keylen = SIZE_MAX). Returns 0 if the whole input
 * is exactly one valid JSON object (surrounding whitespace allowed), 1 if it is
 * valid JSON but not an object, -1 if malformed. */
typedef void (*json_member_cb)(void *ctx, const char *key, size_t keylen, const char *v, const char *ve);

static int json_object_each(const char *p, const char *e, json_member_cb cb, void *ctx) {
    p = json_ws(p, e);
    if (p >= e) return -1;
    if (*p != '{') {
        const char *q = json_skip(p, e, 0);
        if (!q || json_ws(q, e) != e) return -1;
        return 1;
    }
    p = json_ws(p + 1, e);
    if (p < e && *p == '}') {
        p++;
    } else {
        for (;;) {
            char keybuf[32];
            size_t keylen = 0;
            const char *k = json_ws(p, e);
            const char *ke = json_string(k, e, NULL, NULL);
            if (!ke) return -1;
            if ((size_t)(ke - k) <= sizeof keybuf) json_string(k, e, keybuf, &keylen);
            else keylen = (size_t)-1;
            p = json_ws(ke, e);
            if (p >= e || *p != ':') return -1;
            const char *v = json_ws(p + 1, e);
            const char *ve = json_skip(v, e, 1);
            if (!ve) return -1;
            cb(ctx, keybuf, keylen, v, ve);
            p = json_ws(ve, e);
            if (p >= e) return -1;
            if (*p == '}') { p++; break; }
            if (*p != ',') return -1;
            p++;
        }
    }
    return json_ws(p, e) == e ? 0 : -1;
}

#endif
