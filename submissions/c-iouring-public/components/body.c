/* db/body.c (sha256 5d006de72a0f), developed separately for this entry. */
/* Strict POST /posts body parser. See body.h for the exact semantics. Plain C11, no allocation, no recursion. */
#include "body.h"
#include <string.h>

#define MAX_DEPTH 32768

typedef struct {
    unsigned char *dst; /* may be NULL: validate only */
    size_t cap, len;    /* len = bytes produced so far (only those < cap are stored) */
} sink;

static void put(sink *s, unsigned char b) {
    if (s->dst && s->len < s->cap) s->dst[s->len] = b;
    s->len++;
}

static void put_cp(sink *s, unsigned cp) {
    if (cp < 0x80) put(s, (unsigned char)cp);
    else if (cp < 0x800) { put(s, (unsigned char)(0xC0 | (cp >> 6))); put(s, (unsigned char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        put(s, (unsigned char)(0xE0 | (cp >> 12)));
        put(s, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put(s, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        put(s, (unsigned char)(0xF0 | (cp >> 18)));
        put(s, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
        put(s, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put(s, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Reads 4 hex digits at p (needs p+4 <= end). Returns value or -1. */
static long hex4(const unsigned char *p, const unsigned char *end) {
    if (end - p < 4) return -1;
    long v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(p[i]);
        if (h < 0) return -1;
        v = (v << 4) | h;
    }
    return v;
}

/* Length of a valid UTF-8 sequence starting at p (lead byte >= 0x80), or 0 if invalid/truncated. */
static int utf8_seq(const unsigned char *p, const unsigned char *end) {
    unsigned char c = p[0];
    ptrdiff_t avail = end - p;
    unsigned char lo = 0x80, hi = 0xBF;
    int len;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;      /* overlong */
        else if (c == 0xED) hi = 0x9F; /* surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;      /* overlong */
        else if (c == 0xF4) hi = 0x8F; /* > U+10FFFF */
    } else return 0;
    if (avail < len) return 0;
    if (p[1] < lo || p[1] > hi) return 0;
    for (int i = 2; i < len; i++)
        if (p[i] < 0x80 || p[i] > 0xBF) return 0;
    return len;
}

/* Scans a JSON string; *pp points just after the opening quote. On success *pp points after the closing
 * quote and the decoded UTF-8 went to s. Returns 0 or -1 (malformed). */
static int scan_string(const unsigned char **pp, const unsigned char *end, sink *s) {
    const unsigned char *p = *pp;
    for (;;) {
        if (p >= end) return -1;
        unsigned char c = *p;
        if (c == '"') { *pp = p + 1; return 0; }
        if (c < 0x20) return -1;
        if (c < 0x80 && c != '\\') { put(s, c); p++; continue; }
        if (c >= 0x80) {
            int l = utf8_seq(p, end);
            if (!l) return -1;
            for (int i = 0; i < l; i++) put(s, p[i]);
            p += l;
            continue;
        }
        /* backslash */
        if (end - p < 2) return -1;
        unsigned char e = p[1];
        unsigned char o;
        switch (e) {
        case '"': o = '"'; break;
        case '\\': o = '\\'; break;
        case '/': o = '/'; break;
        case 'b': o = '\b'; break;
        case 'f': o = '\f'; break;
        case 'n': o = '\n'; break;
        case 'r': o = '\r'; break;
        case 't': o = '\t'; break;
        case 'u': {
            long v = hex4(p + 2, end);
            if (v < 0) return -1;
            p += 6;
            if (v >= 0xD800 && v <= 0xDBFF) {
                /* try to pair with an immediately following \uDC00-\uDFFF; otherwise lone -> U+FFFD and the
                 * following text is processed normally on the next iteration */
                if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    long w = hex4(p + 2, end);
                    if (w >= 0xDC00 && w <= 0xDFFF) {
                        put_cp(s, 0x10000 + (((unsigned)v - 0xD800) << 10) + ((unsigned)w - 0xDC00));
                        p += 6;
                        continue;
                    }
                }
                put_cp(s, 0xFFFD);
            } else if (v >= 0xDC00 && v <= 0xDFFF) {
                put_cp(s, 0xFFFD);
            } else {
                put_cp(s, (unsigned)v);
            }
            continue;
        }
        default: return -1;
        }
        put(s, o);
        p += 2;
    }
}

static const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static int scan_number(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *p = *pp;
    if (p < end && *p == '-') p++;
    if (p >= end) return -1;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (p < end && is_digit(*p)) p++; }
    else return -1;
    if (p < end && *p == '.') {
        p++;
        if (p >= end || !is_digit(*p)) return -1;
        while (p < end && is_digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || !is_digit(*p)) return -1;
        while (p < end && is_digit(*p)) p++;
    }
    *pp = p;
    return 0;
}

static int scan_lit(const unsigned char **pp, const unsigned char *end, const char *lit) {
    size_t l = strlen(lit);
    if ((size_t)(end - *pp) < l || memcmp(*pp, lit, l) != 0) return -1;
    *pp += l;
    return 0;
}

/* Decodes one code point from valid UTF-8 at p; sets *len. */
static unsigned dec_cp(const unsigned char *p, int *len) {
    unsigned char c = p[0];
    if (c < 0x80) { *len = 1; return c; }
    if (c < 0xE0) { *len = 2; return ((unsigned)(c & 0x1F) << 6) | (p[1] & 0x3F); }
    if (c < 0xF0) { *len = 3; return ((unsigned)(c & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    *len = 4;
    return ((unsigned)(c & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12) | ((unsigned)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

static int js_ws(unsigned cp) {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

int eng_parse_post_body(const char *json, size_t n, char *out, size_t cap, size_t *outn, int len_utf16) {
    if (cap < n) return -1;
    const unsigned char *p = (const unsigned char *)json, *end = p + n;
    unsigned char stack[MAX_DEPTH / 8]; /* bit set = object, clear = array */
    size_t depth = 0;
    int top_is_object = 0;
    int have_body = 0;   /* last top-level "body" value was a string, decoded into out[0..body_len) */
    size_t body_len = 0;
    int key_is_body = 0; /* the value about to be parsed belongs to top-level key "body" */

#define TOP_OBJ() ((stack[(depth - 1) >> 3] >> ((depth - 1) & 7)) & 1)

    p = skip_ws(p, end);
    if (p >= end) return ENG_BODY_MALFORMED;
    top_is_object = (*p == '{');

    for (;;) {
        /* ---- expect a value ---- */
        p = skip_ws(p, end);
        if (p >= end) return ENG_BODY_MALFORMED;
        int this_is_body = key_is_body; /* have_body was already cleared at the ':' */
        key_is_body = 0;
        unsigned char c = *p;
        if (c == '{' || c == '[') {
            if (depth >= MAX_DEPTH) return ENG_BODY_MALFORMED;
            size_t i = depth >> 3;
            unsigned char bit = (unsigned char)(1u << (depth & 7));
            if (c == '{') stack[i] |= bit; else stack[i] &= (unsigned char)~bit;
            depth++;
            p = skip_ws(p + 1, end);
            if (p >= end) return ENG_BODY_MALFORMED;
            if (c == '{') {
                if (*p == '}') { p++; depth--; goto after_value; }
                goto expect_key;
            }
            if (*p == ']') { p++; depth--; goto after_value; }
            continue; /* first array element */
        } else if (c == '"') {
            p++;
            if (this_is_body) {
                sink s = { (unsigned char *)out, cap, 0 };
                if (scan_string(&p, end, &s)) return ENG_BODY_MALFORMED;
                have_body = 1;
                body_len = s.len;
            } else {
                sink s = { NULL, 0, 0 };
                if (scan_string(&p, end, &s)) return ENG_BODY_MALFORMED;
            }
            goto after_value;
        } else if (c == '-' || is_digit(c)) {
            if (scan_number(&p, end)) return ENG_BODY_MALFORMED;
        } else if (c == 't') { if (scan_lit(&p, end, "true")) return ENG_BODY_MALFORMED; }
        else if (c == 'f') { if (scan_lit(&p, end, "false")) return ENG_BODY_MALFORMED; }
        else if (c == 'n') { if (scan_lit(&p, end, "null")) return ENG_BODY_MALFORMED; }
        else return ENG_BODY_MALFORMED;
        goto after_value;

    after_value:
        p = skip_ws(p, end);
        if (depth == 0) break;
        if (p >= end) return ENG_BODY_MALFORMED;
        c = *p;
        if (c == ',') {
            p = skip_ws(p + 1, end);
            if (TOP_OBJ()) goto expect_key;
            continue;
        }
        if (c == '}' && TOP_OBJ()) { p++; depth--; goto after_value; }
        if (c == ']' && !TOP_OBJ()) { p++; depth--; goto after_value; }
        return ENG_BODY_MALFORMED;

    expect_key:
        if (p >= end || *p != '"') return ENG_BODY_MALFORMED;
        p++;
        {
            unsigned char kb[8];
            sink s = { kb, sizeof kb, 0 };
            if (scan_string(&p, end, &s)) return ENG_BODY_MALFORMED;
            key_is_body = (depth == 1 && top_is_object && s.len == 4 && memcmp(kb, "body", 4) == 0);
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return ENG_BODY_MALFORMED;
        p++;
        if (key_is_body) have_body = 0; /* this later duplicate replaces any earlier value; set to 1 only if it is a string */
        continue;
    }
#undef TOP_OBJ
    if (p != end) return ENG_BODY_MALFORMED;
    if (!top_is_object || !have_body) return ENG_BODY_REQUIRED;

    /* trim (JS String.prototype.trim) on the decoded UTF-8 in out[0..body_len) */
    unsigned char *b = (unsigned char *)out;
    size_t s0 = 0, e0 = body_len;
    while (s0 < e0) {
        int l;
        unsigned cp = dec_cp(b + s0, &l);
        if (!js_ws(cp)) break;
        s0 += (size_t)l;
    }
    while (e0 > s0) {
        size_t k = e0 - 1;
        while (k > s0 && (b[k] & 0xC0) == 0x80) k--;
        int l;
        unsigned cp = dec_cp(b + k, &l);
        if (!js_ws(cp)) break;
        e0 = k;
    }
    if (s0 == e0) return ENG_BODY_REQUIRED;
    size_t units = 0;
    for (size_t k = s0; k < e0; k++) {
        if ((b[k] & 0xC0) != 0x80) units++;
        if (len_utf16 && b[k] >= 0xF0) units++;
    }
    if (units > 500) return ENG_BODY_TOO_LONG;
    if (s0) memmove(b, b + s0, e0 - s0);
    *outn = e0 - s0;
    return ENG_BODY_OK;
}
