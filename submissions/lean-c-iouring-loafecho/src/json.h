/* json.h — a small strict JSON (RFC 8259) validator with just enough
 * extraction for this API: the members of a top-level object, and string
 * decoding (escapes, \uXXXX -> UTF-8, surrogate pairs). No allocation.
 * Single translation unit helper: everything is static. */
#ifndef TWELVE_JSON_H
#define TWELVE_JSON_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define JSON_MAX_DEPTH 256

enum { J_NONE = 0, J_STR, J_NUM, J_OBJ, J_ARR, J_TRUE, J_FALSE, J_NULL };

static inline const char *json_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static inline int json_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* p points at the opening quote. Returns the byte after the closing quote,
 * or NULL if the string is invalid. */
static const char *json_skip_string(const char *p, const char *e) {
    p++;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20) return NULL;
        if (c == '\\') {
            if (++p >= e) return NULL;
            switch (*p) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                p++; break;
            case 'u':
                if (e - p < 5) return NULL;
                for (int i = 1; i <= 4; i++) if (json_hex(p[i]) < 0) return NULL;
                p += 5; break;
            default: return NULL;
            }
        } else p++;
    }
    return NULL;
}

static const char *json_skip_number(const char *p, const char *e) {
    if (p < e && *p == '-') p++;
    if (p >= e) return NULL;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (p < e && *p >= '0' && *p <= '9') p++; }
    else return NULL;
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

static const char *json_skip_lit(const char *p, const char *e, const char *lit, size_t n) {
    if ((size_t)(e - p) < n || memcmp(p, lit, n) != 0) return NULL;
    return p + n;
}

/* p points at the first byte of a value (no leading whitespace).
 * Returns the byte after the value or NULL; *type gets the value kind. */
static const char *json_skip_value(const char *p, const char *e, int depth, int *type) {
    int t_dummy; if (!type) type = &t_dummy;
    if (p >= e) return NULL;
    switch (*p) {
    case '"': *type = J_STR; return json_skip_string(p, e);
    case 't': *type = J_TRUE; return json_skip_lit(p, e, "true", 4);
    case 'f': *type = J_FALSE; return json_skip_lit(p, e, "false", 5);
    case 'n': *type = J_NULL; return json_skip_lit(p, e, "null", 4);
    case '[':
        *type = J_ARR;
        if (depth >= JSON_MAX_DEPTH) return NULL;
        p = json_ws(p + 1, e);
        if (p < e && *p == ']') return p + 1;
        for (;;) {
            p = json_skip_value(p, e, depth + 1, NULL);
            if (!p) return NULL;
            p = json_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ']') return p + 1;
            if (*p != ',') return NULL;
            p = json_ws(p + 1, e);
        }
    case '{':
        *type = J_OBJ;
        if (depth >= JSON_MAX_DEPTH) return NULL;
        p = json_ws(p + 1, e);
        if (p < e && *p == '}') return p + 1;
        for (;;) {
            if (p >= e || *p != '"') return NULL;
            p = json_skip_string(p, e);
            if (!p) return NULL;
            p = json_ws(p, e);
            if (p >= e || *p != ':') return NULL;
            p = json_ws(p + 1, e);
            p = json_skip_value(p, e, depth + 1, NULL);
            if (!p) return NULL;
            p = json_ws(p, e);
            if (p >= e) return NULL;
            if (*p == '}') return p + 1;
            if (*p != ',') return NULL;
            p = json_ws(p + 1, e);
        }
    default:
        if (*p == '-' || (*p >= '0' && *p <= '9')) { *type = J_NUM; return json_skip_number(p, e); }
        return NULL;
    }
}

static inline size_t json_put_utf8(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static inline uint32_t json_u4(const char *p) {
    return (uint32_t)(json_hex(p[0]) << 12 | json_hex(p[1]) << 8 | json_hex(p[2]) << 4 | json_hex(p[3]));
}

/* Decode an already-validated string (s points at the opening quote, e is
 * the byte after the closing quote) into out. The decoded form is never
 * longer than the raw form, so cap >= (e - s) always suffices.
 * Returns the decoded length or -1 if it does not fit. */
static long json_decode_string(const char *s, const char *e, char *out, size_t cap) {
    const char *p = s + 1, *end = e - 1;
    size_t n = 0;
    while (p < end) {
        if (*p != '\\') {
            const char *q = p;
            while (q < end && *q != '\\') q++;
            size_t run = (size_t)(q - p);
            if (n + run > cap) return -1;
            memcpy(out + n, p, run); n += run; p = q;
            continue;
        }
        p++;
        char c = *p++;
        char one;
        switch (c) {
        case '"': one = '"'; break;
        case '\\': one = '\\'; break;
        case '/': one = '/'; break;
        case 'b': one = '\b'; break;
        case 'f': one = '\f'; break;
        case 'n': one = '\n'; break;
        case 'r': one = '\r'; break;
        case 't': one = '\t'; break;
        default: { /* 'u' */
            uint32_t cp = json_u4(p); p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                uint32_t lo = json_u4(p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (n + 4 > cap) return -1;
            n += json_put_utf8(out + n, cp);
            continue;
        }
        }
        if (n + 1 > cap) return -1;
        out[n++] = one;
    }
    return (long)n;
}

/* One wanted member of a top-level object. Fill name/nlen; after
 * json_object_fields() type is J_NONE if absent, else the value kind and
 * [vs, ve) the raw value (for strings including the quotes). The last
 * occurrence of a duplicated key wins (as in JSON.parse). */
typedef struct {
    const char *name; size_t nlen;
    int type; const char *vs, *ve;
} json_field;

/* Validate the whole document [p, e). Returns -1 if it is not valid JSON,
 * 0 if it is valid but not an object, 1 if it is an object (fields filled). */
static int json_object_fields(const char *p, const char *e, json_field *f, int nf) {
    for (int i = 0; i < nf; i++) { f[i].type = J_NONE; f[i].vs = f[i].ve = NULL; }
    p = json_ws(p, e);
    if (p >= e) return -1;
    if (*p != '{') {
        int t;
        const char *q = json_skip_value(p, e, 0, &t);
        if (!q) return -1;
        q = json_ws(q, e);
        return q == e ? 0 : -1;
    }
    p = json_ws(p + 1, e);
    if (p < e && *p == '}') { p++; goto tail; }
    for (;;) {
        if (p >= e || *p != '"') return -1;
        const char *ks = p;
        p = json_skip_string(p, e);
        if (!p) return -1;
        const char *ke = p;
        char kbuf[64];
        long klen = -1;
        if ((size_t)(ke - ks) <= sizeof kbuf) klen = json_decode_string(ks, ke, kbuf, sizeof kbuf);
        p = json_ws(p, e);
        if (p >= e || *p != ':') return -1;
        p = json_ws(p + 1, e);
        const char *vs = p;
        int t;
        p = json_skip_value(p, e, 1, &t);
        if (!p) return -1;
        if (klen >= 0)
            for (int i = 0; i < nf; i++)
                if ((size_t)klen == f[i].nlen && memcmp(kbuf, f[i].name, f[i].nlen) == 0) {
                    f[i].type = t; f[i].vs = vs; f[i].ve = p;
                }
        p = json_ws(p, e);
        if (p >= e) return -1;
        if (*p == '}') { p++; break; }
        if (*p != ',') return -1;
        p = json_ws(p + 1, e);
    }
tail:
    p = json_ws(p, e);
    return p == e ? 1 : -1;
}

#endif
