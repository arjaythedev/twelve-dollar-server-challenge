/* A small strict JSON (RFC 8259) validator that picks named fields out of a top-level object,
 * plus string decoding and escaping helpers. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

enum { JT_NONE = 0, JT_STRING, JT_NUMBER, JT_OTHER };

typedef struct {
    const char *name;   /* key to look for (plain ASCII) */
    int type;           /* JT_* of the last occurrence; JT_NONE if absent */
    const char *v, *ve; /* raw value span (for strings: the bytes between the quotes) */
} jfield;

typedef struct { const char *p, *e; } jcur;

static inline void j_ws(jcur *c)
{
    while (c->p < c->e && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static inline int hexv(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch |= 0x20;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static int j_hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv((unsigned char)p[i]);
        if (h < 0) return -1;
        v = v << 4 | h;
    }
    return v;
}

/* Length of a valid UTF-8 sequence at p (bounded by e), or 0 if invalid. */
static int utf8_len(const unsigned char *p, const unsigned char *e)
{
    unsigned c = p[0];
    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;
    if (c < 0xE0) {
        if (e - p < 2 || (p[1] & 0xC0) != 0x80) return 0;
        return 2;
    }
    if (c < 0xF0) {
        if (e - p < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;
        if (c == 0xED && p[1] >= 0xA0) return 0; /* surrogates */
        return 3;
    }
    if (c < 0xF5) {
        if (e - p < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

/* Scans a string starting after its opening quote; on success c->p is just past the closing quote. */
static int j_string(jcur *c)
{
    const unsigned char *p = (const unsigned char *)c->p, *e = (const unsigned char *)c->e;
    while (p < e) {
        unsigned ch = *p;
        if (ch == '"') { c->p = (const char *)p + 1; return 1; }
        if (ch < 0x20) return 0;
        if (ch == '\\') {
            if (e - p < 2) return 0;
            switch (p[1]) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': p += 2; break;
            case 'u': {
                if (e - p < 6) return 0;
                int u = j_hex4((const char *)p + 2);
                if (u < 0) return 0;
                p += 6;
                if (u >= 0xD800 && u < 0xDC00) { /* needs a low surrogate */
                    if (e - p < 6 || p[0] != '\\' || p[1] != 'u') return 0;
                    int l = j_hex4((const char *)p + 2);
                    if (l < 0xDC00 || l > 0xDFFF) return 0;
                    p += 6;
                } else if (u >= 0xDC00 && u < 0xE000) return 0;
                break;
            }
            default: return 0;
            }
        } else if (ch < 0x80) {
            p++;
        } else {
            int n = utf8_len(p, e);
            if (!n) return 0;
            p += n;
        }
    }
    return 0;
}

static int j_number(jcur *c)
{
    const char *p = c->p, *e = c->e;
    if (p < e && *p == '-') p++;
    if (p >= e) return 0;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') while (p < e && *p >= '0' && *p <= '9') p++;
    else return 0;
    if (p < e && *p == '.') {
        p++;
        if (p >= e || *p < '0' || *p > '9') return 0;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return 0;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    c->p = p;
    return 1;
}

static int j_lit(jcur *c, const char *lit, size_t n)
{
    if ((size_t)(c->e - c->p) < n || memcmp(c->p, lit, n)) return 0;
    c->p += n;
    return 1;
}

/* Validates one value; returns its JT_* type, or 0 if malformed. */
static int j_value(jcur *c, int depth)
{
    if (depth > 64) return 0;
    j_ws(c);
    if (c->p >= c->e) return 0;
    switch (*c->p) {
    case '"': c->p++; return j_string(c) ? JT_STRING : 0;
    case '{':
        c->p++; j_ws(c);
        if (c->p < c->e && *c->p == '}') { c->p++; return JT_OTHER; }
        for (;;) {
            j_ws(c);
            if (c->p >= c->e || *c->p != '"') return 0;
            c->p++;
            if (!j_string(c)) return 0;
            j_ws(c);
            if (c->p >= c->e || *c->p != ':') return 0;
            c->p++;
            if (!j_value(c, depth + 1)) return 0;
            j_ws(c);
            if (c->p >= c->e) return 0;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == '}') { c->p++; return JT_OTHER; }
            return 0;
        }
    case '[':
        c->p++; j_ws(c);
        if (c->p < c->e && *c->p == ']') { c->p++; return JT_OTHER; }
        for (;;) {
            if (!j_value(c, depth + 1)) return 0;
            j_ws(c);
            if (c->p >= c->e) return 0;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == ']') { c->p++; return JT_OTHER; }
            return 0;
        }
    case 't': return j_lit(c, "true", 4) ? JT_OTHER : 0;
    case 'f': return j_lit(c, "false", 5) ? JT_OTHER : 0;
    case 'n': return j_lit(c, "null", 4) ? JT_OTHER : 0;
    default: return j_number(c) ? JT_NUMBER : 0;
    }
}

/* Decodes the JSON string body [s, e) (already validated) into out; returns the decoded length.
 * out must hold at least e - s bytes (decoding never grows). */
static size_t j_unescape(const char *s, const char *e, char *out)
{
    char *o = out;
    while (s < e) {
        const char *bs = memchr(s, '\\', e - s);
        if (!bs) { memcpy(o, s, e - s); o += e - s; break; }
        memcpy(o, s, bs - s); o += bs - s;
        s = bs + 1;
        char ch = *s++;
        switch (ch) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            uint32_t u = j_hex4(s);
            s += 4;
            if (u >= 0xD800 && u < 0xDC00) {
                uint32_t l = j_hex4(s + 2);
                s += 6;
                u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
            }
            if (u < 0x80) *o++ = u;
            else if (u < 0x800) { *o++ = 0xC0 | u >> 6; *o++ = 0x80 | (u & 0x3F); }
            else if (u < 0x10000) { *o++ = 0xE0 | u >> 12; *o++ = 0x80 | (u >> 6 & 0x3F); *o++ = 0x80 | (u & 0x3F); }
            else { *o++ = 0xF0 | u >> 18; *o++ = 0x80 | (u >> 12 & 0x3F); *o++ = 0x80 | (u >> 6 & 0x3F); *o++ = 0x80 | (u & 0x3F); }
            break;
        }
        default: *o++ = ch; break; /* " \ / */
        }
    }
    return o - out;
}

/* Parses a complete JSON document. Returns 0 if malformed, JT_OTHER if valid and the top level is an
 * object (fields filled in, last occurrence of a key wins), 1 if valid but not an object. */
static int json_fields(const char *s, size_t n, jfield *f, int nf)
{
    jcur c = {s, s + n};
    for (int i = 0; i < nf; i++) f[i].type = JT_NONE;
    j_ws(&c);
    if (c.p < c.e && *c.p == '{') {
        c.p++; j_ws(&c);
        if (c.p < c.e && *c.p == '}') { c.p++; goto done; }
        for (;;) {
            j_ws(&c);
            if (c.p >= c.e || *c.p != '"') return 0;
            const char *ks = ++c.p;
            if (!j_string(&c)) return 0;
            const char *ke = c.p - 1;
            char kbuf[32];
            size_t kl = 0;
            int kok = 0;
            if (ke - ks < (ptrdiff_t)sizeof kbuf) { kl = j_unescape(ks, ke, kbuf); kok = 1; }
            j_ws(&c);
            if (c.p >= c.e || *c.p != ':') return 0;
            c.p++; j_ws(&c);
            const char *vs = c.p;
            int t = j_value(&c, 1);
            if (!t) return 0;
            if (kok) {
                for (int i = 0; i < nf; i++) {
                    if (strlen(f[i].name) == kl && !memcmp(f[i].name, kbuf, kl)) {
                        f[i].type = t;
                        f[i].v = t == JT_STRING ? vs + 1 : vs;
                        f[i].ve = t == JT_STRING ? c.p - 1 : c.p;
                    }
                }
            }
            j_ws(&c);
            if (c.p >= c.e) return 0;
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return 0;
        }
    done:
        j_ws(&c);
        return c.p == c.e ? JT_OTHER : 0;
    }
    if (!j_value(&c, 0)) return 0;
    j_ws(&c);
    return c.p == c.e ? 1 : 0;
}

/* JSON-escape table: 0 = copy as-is, otherwise the escape char ('u' for \u00XX). */
static const char JESC[256] = {
    ['\0'] = 'u', [1] = 'u', [2] = 'u', [3] = 'u', [4] = 'u', [5] = 'u', [6] = 'u', [7] = 'u',
    ['\b'] = 'b', ['\t'] = 't', ['\n'] = 'n', [11] = 'u', ['\f'] = 'f', ['\r'] = 'r', [14] = 'u', [15] = 'u',
    [16] = 'u', [17] = 'u', [18] = 'u', [19] = 'u', [20] = 'u', [21] = 'u', [22] = 'u', [23] = 'u',
    [24] = 'u', [25] = 'u', [26] = 'u', [27] = 'u', [28] = 'u', [29] = 'u', [30] = 'u', [31] = 'u',
    ['"'] = '"', ['\\'] = '\\',
};

/* Writes s as JSON string contents (no quotes) to o; o needs 6*n + 16 bytes of room. Returns the end.
 * The common case (nothing to escape) moves 16 bytes per step. */
static inline char *json_escape(char *o, const unsigned char *s, size_t n)
{
    const unsigned char *e = s + n;
#ifdef __SSE2__
    const __m128i quote = _mm_set1_epi8('"'), bslash = _mm_set1_epi8('\\'), ctl = _mm_set1_epi8(0x1F);
    for (;;) {
        while (e - s >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)s);
            __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, bslash)),
                                     _mm_cmpeq_epi8(_mm_max_epu8(v, ctl), ctl)); /* v <= 0x1F */
            _mm_storeu_si128((__m128i *)o, v);
            unsigned bits = _mm_movemask_epi8(m);
            if (!bits) { s += 16; o += 16; continue; }
            unsigned k = __builtin_ctz(bits);
            s += k; o += k;
            break;
        }
        if (e - s < 16) break;
        /* *s needs escaping */
        char x = JESC[*s];
        *o++ = '\\';
        *o++ = x;
        if (x == 'u') {
            static const char hx[] = "0123456789abcdef";
            *o++ = '0'; *o++ = '0'; *o++ = hx[*s >> 4]; *o++ = hx[*s & 15];
        }
        s++;
    }
#endif
    while (s < e) {
        const unsigned char *r = s;
        while (r < e && !JESC[*r]) r++;
        memcpy(o, s, r - s);
        o += r - s;
        if (r == e) break;
        char x = JESC[*r];
        *o++ = '\\';
        *o++ = x;
        if (x == 'u') {
            static const char hx[] = "0123456789abcdef";
            *o++ = '0'; *o++ = '0'; *o++ = hx[*r >> 4]; *o++ = hx[*r & 15];
        }
        s = r + 1;
    }
    return o;
}
