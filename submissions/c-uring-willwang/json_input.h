/* A bounded JSON projection. Strings are unescaped in their input buffer; the
 * caller owns it until the returned slices have been consumed. Unknown fields
 * still go through the full grammar. Only top-level members are projected. */
#ifndef JSON_INPUT_H
#define JSON_INPUT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { const char *p; size_t n; } Slice;
typedef enum { J_MISSING, J_OBJECT, J_ARRAY, J_STRING, J_NUMBER, J_LITERAL } JsonKind;
typedef struct { JsonKind kind; Slice text; } JsonValue;
typedef struct { Slice name; JsonValue value; } JsonField;
typedef struct { char *at, *end; } JsonParser;

#define LIT(s) ((Slice){(s), sizeof(s) - 1})
#define FIELD(s) { { (s), sizeof(s) - 1 }, { J_MISSING, { NULL, 0 } } }

static int same(Slice a, Slice b) {
    return a.n == b.n && memcmp(a.p, b.p, a.n) == 0;
}

static int digit(unsigned char c) { return c >= '0' && c <= '9'; }

static void json_space(JsonParser *p) {
    while (p->at < p->end) {
        switch (*p->at) {
        case ' ': case '\t': case '\n': case '\r': p->at++; continue;
        default: return;
        }
    }
}

static int hex_digit(unsigned char c) {
    if (digit(c)) return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int hex_unit(const char *s) {
    int u = 0;
    for (int i = 0; i < 4; ++i) {
        int h = hex_digit((unsigned char)s[i]);
        if (h < 0) return -1;
        u = (u << 4) | h;
    }
    return u;
}

/* Reject overlong sequences, surrogates, truncation and code points above
 * U+10FFFF. Used for unescaped wire bytes and for measuring trimmed text. */
static int utf8_read(const char *s, const char *end, uint32_t *value) {
    unsigned a = (unsigned char)s[0];
    if (a < 128) { *value = a; return 1; }
    if (a < 0xc2 || a > 0xf4) return 0;
    int n = a < 0xe0 ? 2 : a < 0xf0 ? 3 : 4;
    if (end - s < n) return 0;
    unsigned v = a & (0x7fu >> n);
    for (int i = 1; i < n; ++i) {
        unsigned b = (unsigned char)s[i];
        if ((b & 0xc0) != 0x80) return 0;
        v = (v << 6) | (b & 0x3f);
    }
    if ((n == 3 && v < 0x800) || (n == 4 && v < 0x10000) ||
        (v >= 0xd800 && v <= 0xdfff) || v > 0x10ffff) return 0;
    *value = v;
    return n;
}

static char *utf8_put(char *d, unsigned cp) {
    if (cp < 0x80) { *d++ = (char)cp; return d; }
    if (cp < 0x800) {
        *d++ = (char)(0xc0 | (cp >> 6));
    } else {
        if (cp >= 0x10000) {
            *d++ = (char)(0xf0 | (cp >> 18));
            *d++ = (char)(0x80 | ((cp >> 12) & 63));
        } else {
            *d++ = (char)(0xe0 | (cp >> 12));
        }
        *d++ = (char)(0x80 | ((cp >> 6) & 63));
    }
    *d++ = (char)(0x80 | (cp & 63));
    return d;
}

static int json_text(JsonParser *p, Slice *out) {
    if (p->at == p->end || *p->at++ != '"') return 0;
    char *start = p->at, *d = start;
    while (p->at < p->end) {
        unsigned char c = (unsigned char)*p->at++;
        if (c == '"') { *out = (Slice){start, (size_t)(d - start)}; return 1; }
        if (c < 0x20) return 0;
        if (c < 0x80 && c != '\\') { *d++ = (char)c; continue; }
        if (c >= 0x80) {
            char *src = p->at - 1;
            uint32_t cp;
            int n = utf8_read(src, p->end, &cp);
            if (!n) return 0;
            if (d != src) memmove(d, src, (size_t)n);
            d += n;
            p->at = src + n;
            continue;
        }
        if (p->at == p->end) return 0;
        c = (unsigned char)*p->at++;
        switch (c) {
        case '"': case '/': case '\\': *d++ = (char)c; break;
        case 'b': *d++ = '\b'; break;
        case 'f': *d++ = '\f'; break;
        case 'n': *d++ = '\n'; break;
        case 'r': *d++ = '\r'; break;
        case 't': *d++ = '\t'; break;
        case 'u': {
            if (p->end - p->at < 4) return 0;
            int cp = hex_unit(p->at);
            if (cp < 0) return 0;
            p->at += 4;
            if (cp >= 0xd800 && cp <= 0xdbff && p->end - p->at >= 6 &&
                p->at[0] == '\\' && p->at[1] == 'u') {
                int low = hex_unit(p->at + 2);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    p->at += 6;
                }
            }
            /* JSON permits isolated UTF-16 halves. Emit a Unicode scalar for
             * the UTF-8 SQLite and HTTP interfaces. */
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
            d = utf8_put(d, (unsigned)cp);
            break;
        }
        default: return 0;
        }
    }
    return 0;
}

static int json_number(JsonParser *p, Slice *out) {
    char *start = p->at;
    if (*p->at == '-') ++p->at;
    if (p->at == p->end || !digit(*p->at)) return 0;
    if (*p->at++ != '0') while (p->at < p->end && digit(*p->at)) ++p->at;
    if (p->at < p->end && *p->at == '.') {
        ++p->at;
        if (p->at == p->end || !digit(*p->at)) return 0;
        while (p->at < p->end && digit(*p->at)) ++p->at;
    }
    if (p->at < p->end && (*p->at == 'e' || *p->at == 'E')) {
        ++p->at;
        if (p->at < p->end && (*p->at == '+' || *p->at == '-')) ++p->at;
        if (p->at == p->end || !digit(*p->at)) return 0;
        while (p->at < p->end && digit(*p->at)) ++p->at;
    }
    *out = (Slice){start, (size_t)(p->at - start)};
    return 1;
}

/* Return 2 for an open container, 1 for a complete scalar and 0 on error. */
static int json_start(JsonParser *p, JsonValue *v) {
    json_space(p);
    if (p->at == p->end) return 0;
    unsigned char c = (unsigned char)*p->at;
    if (c == '"') { v->kind = J_STRING; return json_text(p, &v->text); }
    if (c == '-' || digit(c)) { v->kind = J_NUMBER; return json_number(p, &v->text); }
    if (c == '{' || c == '[') {
        ++p->at;
        v->kind = c == '{' ? J_OBJECT : J_ARRAY;
        return 2;
    }
    Slice literal;
    if (c == 't') literal = LIT("true");
    else if (c == 'f') literal = LIT("false");
    else if (c == 'n') literal = LIT("null");
    else return 0;
    if ((size_t)(p->end - p->at) < literal.n || memcmp(p->at, literal.p, literal.n)) return 0;
    p->at += literal.n;
    v->kind = J_LITERAL;
    return 1;
}

static int json_key(JsonParser *p, Slice *key) {
    if (!json_text(p, key)) return 0;
    json_space(p);
    return p->at < p->end && *p->at++ == ':';
}

/* An explicit stack also accepts deeply nested auxiliary metadata. A valid
 * 16 KiB document can have at most 8192 pairs of delimiters. */
static int json_children(JsonParser *p, JsonKind kind) {
    unsigned char stack[8192];
    size_t depth = 0;
    enum { ENTER, MEMBER, DELIMITER } state = ENTER;
    for (;;) {
        if (state == ENTER) {
            if (depth == sizeof(stack)) return 0;
            stack[depth++] = kind == J_OBJECT ? '}' : ']';
            json_space(p);
            if (p->at < p->end && *p->at == stack[depth - 1]) {
                ++p->at;
                --depth;
                state = DELIMITER;
            } else {
                state = MEMBER;
            }
        } else if (state == MEMBER) {
            Slice key;
            JsonValue member;
            if (stack[depth - 1] == '}' && !json_key(p, &key)) return 0;
            int rc = json_start(p, &member);
            if (!rc) return 0;
            kind = member.kind;
            state = rc == 2 ? ENTER : DELIMITER;
        } else {
            if (!depth) return 1;
            json_space(p);
            if (p->at == p->end) return 0;
            char c = *p->at++;
            if (c == stack[depth - 1]) {
                --depth;
            } else if (c == ',') {
                json_space(p);
                state = MEMBER;
            } else {
                return 0;
            }
        }
    }
}

static int json_value(JsonParser *p, JsonValue *v) {
    int rc = json_start(p, v);
    return rc == 1 || (rc == 2 && json_children(p, v->kind));
}

static int json_project(char *s, size_t n, JsonField *fields, size_t count, JsonKind *kind) {
    JsonParser p = {s, s + n};
    json_space(&p);
    if (p.at < p.end && *p.at == '{') {
        *kind = J_OBJECT;
        ++p.at;
        json_space(&p);
        if (p.at < p.end && *p.at == '}') {
            ++p.at;
        } else {
            for (;;) {
                Slice key;
                JsonValue member = {0};
                if (!json_key(&p, &key) || !json_value(&p, &member)) return 0;
                for (size_t i = 0; i < count; ++i)
                    if (same(key, fields[i].name)) fields[i].value = member;
                json_space(&p);
                if (p.at == p.end) return 0;
                char c = *p.at++;
                if (c == '}') break;
                if (c != ',') return 0;
                json_space(&p);
            }
        }
    } else {
        JsonValue root;
        if (!json_value(&p, &root)) return 0;
        *kind = root.kind;
    }
    json_space(&p);
    return p.at == p.end;
}

#endif
