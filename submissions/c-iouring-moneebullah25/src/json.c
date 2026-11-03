#include "json.h"

#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define JSON_MAX_DEPTH 32

struct cur {
    const char *p, *end;
};

static void skip_ws(struct cur *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* Parses a string starting at the opening quote. When `f` is non-NULL the
 * unescaped bytes go to f->buf. Returns 0 on success. */
static int parse_string(struct cur *c, struct json_field *f) {
    if (c->p >= c->end || *c->p != '"') return -1;
    c->p++;
    size_t o = 0;
    for (;;) {
        if (c->p >= c->end) return -1;
        unsigned char ch = (unsigned char)*c->p++;
        char tmp[4];
        size_t n;
        if (ch == '"') break;
        if (ch < 0x20) return -1; /* raw control characters are not allowed */
        if (ch != '\\') {
            tmp[0] = (char)ch;
            n = 1;
        } else {
            if (c->p >= c->end) return -1;
            char e = *c->p++;
            switch (e) {
            case '"':
                tmp[0] = '"';
                n = 1;
                break;
            case '\\':
                tmp[0] = '\\';
                n = 1;
                break;
            case '/':
                tmp[0] = '/';
                n = 1;
                break;
            case 'b':
                tmp[0] = '\b';
                n = 1;
                break;
            case 'f':
                tmp[0] = '\f';
                n = 1;
                break;
            case 'n':
                tmp[0] = '\n';
                n = 1;
                break;
            case 'r':
                tmp[0] = '\r';
                n = 1;
                break;
            case 't':
                tmp[0] = '\t';
                n = 1;
                break;
            case 'u': {
                if (c->end - c->p < 4) return -1;
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hexval(c->p[i]);
                    if (h < 0) return -1;
                    cp = cp << 4 | (uint32_t)h;
                }
                c->p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    /* high surrogate: a low surrogate must follow */
                    if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u') return -1;
                    uint32_t lo = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hexval(c->p[2 + i]);
                        if (h < 0) return -1;
                        lo = lo << 4 | (uint32_t)h;
                    }
                    if (lo < 0xdc00 || lo > 0xdfff) return -1;
                    c->p += 6;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    return -1; /* lone low surrogate */
                }
                n = utf8_encode(cp, tmp);
                break;
            }
            default: return -1;
            }
        }
        if (f) {
            if (o + n <= f->cap) {
                memcpy(f->buf + o, tmp, n);
                o += n;
            } else {
                f->truncated = 1;
            }
        }
    }
    if (f) f->len = o;
    return 0;
}

static int parse_number(struct cur *c, struct json_field *f) {
    const char *start = c->p;
    if (c->p < c->end && *c->p == '-') c->p++;
    if (c->p >= c->end) return -1;
    if (*c->p == '0') {
        c->p++;
    } else if (*c->p >= '1' && *c->p <= '9') {
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') c->p++;
    } else {
        return -1;
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9') return -1;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9') return -1;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') c->p++;
    }
    if (f) {
        size_t n = (size_t)(c->p - start);
        if (n > f->cap) {
            n = f->cap;
            f->truncated = 1;
        }
        memcpy(f->buf, start, n);
        f->len = n;
    }
    return 0;
}

static int parse_value(struct cur *c, int depth, struct json_field *f, enum json_kind *kind);

static int parse_literal(struct cur *c, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) return -1;
    c->p += n;
    return 0;
}

static int parse_object(struct cur *c, int depth, struct json_field *fields, size_t nfields) {
    c->p++; /* '{' */
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return 0;
    }
    for (;;) {
        skip_ws(c);
        /* key: scan it raw first so we can match it against the wanted names */
        char key[64];
        struct json_field kf = {.buf = key, .cap = sizeof key};
        if (parse_string(c, &kf)) return -1;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') return -1;
        c->p++;
        skip_ws(c);
        struct json_field *want = NULL;
        if (!kf.truncated) {
            for (size_t i = 0; i < nfields; i++) {
                if (strlen(fields[i].name) == kf.len && memcmp(fields[i].name, key, kf.len) == 0) {
                    want = &fields[i];
                    break;
                }
            }
        }
        enum json_kind kind;
        if (want) {
            want->len = 0;
            want->truncated = 0;
        }
        if (parse_value(c, depth, want, &kind)) return -1;
        if (want) want->kind = kind;
        skip_ws(c);
        if (c->p >= c->end) return -1;
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == '}') {
            c->p++;
            return 0;
        }
        return -1;
    }
}

static int parse_array(struct cur *c, int depth) {
    c->p++; /* '[' */
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return 0;
    }
    for (;;) {
        skip_ws(c);
        enum json_kind kind;
        if (parse_value(c, depth, NULL, &kind)) return -1;
        skip_ws(c);
        if (c->p >= c->end) return -1;
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == ']') {
            c->p++;
            return 0;
        }
        return -1;
    }
}

static int parse_value(struct cur *c, int depth, struct json_field *f, enum json_kind *kind) {
    if (depth > JSON_MAX_DEPTH) return -1;
    skip_ws(c);
    if (c->p >= c->end) return -1;
    switch (*c->p) {
    case '"': *kind = JSON_STRING; return parse_string(c, f);
    case '{': *kind = JSON_OTHER; return parse_object(c, depth + 1, NULL, 0);
    case '[': *kind = JSON_OTHER; return parse_array(c, depth + 1);
    case 't': *kind = JSON_OTHER; return parse_literal(c, "true");
    case 'f': *kind = JSON_OTHER; return parse_literal(c, "false");
    case 'n': *kind = JSON_OTHER; return parse_literal(c, "null");
    default: *kind = JSON_NUMBER; return parse_number(c, f);
    }
}

enum json_status json_parse_object(const char *doc, size_t len, struct json_field *fields, size_t nfields) {
    struct cur c = {doc, doc + len};
    for (size_t i = 0; i < nfields; i++) {
        fields[i].kind = JSON_MISSING;
        fields[i].len = 0;
        fields[i].truncated = 0;
    }
    skip_ws(&c);
    if (c.p >= c.end) return JSON_MALFORMED;
    if (*c.p != '{') {
        enum json_kind kind;
        if (parse_value(&c, 0, NULL, &kind)) return JSON_MALFORMED;
        skip_ws(&c);
        return c.p == c.end ? JSON_NOT_OBJECT : JSON_MALFORMED;
    }
    if (parse_object(&c, 1, fields, nfields)) return JSON_MALFORMED;
    skip_ws(&c);
    return c.p == c.end ? JSON_OK : JSON_MALFORMED;
}

/* Length of the prefix of s that needs no escaping: bytes >= 0x20 other than
 * '"' and '\\'. 16 bytes per step with SSE2; the tail and other targets go
 * byte by byte. */
static size_t clean_run(const char *s, size_t len) {
    size_t i = 0;
#if defined(__SSE2__)
    const __m128i quote = _mm_set1_epi8('"'), bslash = _mm_set1_epi8('\\'), ctl = _mm_set1_epi8(0x1f);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
        /* max(v, 0x1f) == 0x1f is an unsigned v <= 0x1f, i.e. a control byte */
        __m128i bad = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, bslash)),
                                   _mm_cmpeq_epi8(_mm_max_epu8(v, ctl), ctl));
        int m = _mm_movemask_epi8(bad);
        if (m) return i + (size_t)__builtin_ctz((unsigned)m);
    }
#endif
    for (; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == '"' || ch == '\\') break;
    }
    return i;
}

size_t json_write_string(char *out, size_t cap, const char *s, size_t len) {
    static const char HEX[] = "0123456789abcdef";
    size_t o = 0;
    if (cap < 2) return 0;
    out[o++] = '"';
    for (size_t i = 0; i < len; i++) {
        size_t run = clean_run(s + i, len - i);
        if (run) {
            if (o + run >= cap) return 0;
            memcpy(out + o, s + i, run);
            o += run;
            i += run;
            if (i >= len) break;
        }
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 0x20 && ch != '"' && ch != '\\') {
            if (o + 1 >= cap) return 0;
            out[o++] = (char)ch;
            continue;
        }
        if (o + 6 >= cap) return 0;
        out[o++] = '\\';
        switch (ch) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '\n': out[o++] = 'n'; break;
        case '\r': out[o++] = 'r'; break;
        case '\t': out[o++] = 't'; break;
        case '\b': out[o++] = 'b'; break;
        case '\f': out[o++] = 'f'; break;
        default:
            out[o++] = 'u';
            out[o++] = '0';
            out[o++] = '0';
            out[o++] = HEX[ch >> 4];
            out[o++] = HEX[ch & 15];
        }
    }
    if (o + 1 > cap) return 0;
    out[o++] = '"';
    return o;
}
