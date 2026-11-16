#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64
#define KEY_MAX 63

typedef struct {
    const unsigned char *p, *end;
    jfield *f;
    int nf;
} parser;

typedef struct {
    char *dst;
    size_t cap, len;
    int overflow;
} sink;

static void ws(parser *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

static void put_byte(sink *k, unsigned char b) {
    if (!k->dst) return;
    if (k->len + 1 < k->cap) k->dst[k->len++] = (char)b;
    else k->overflow = 1;
}

static void put_cp(sink *k, uint32_t cp) {
    if (cp < 0x80) {
        put_byte(k, (unsigned char)cp);
    } else if (cp < 0x800) {
        put_byte(k, (unsigned char)(0xC0 | (cp >> 6)));
        put_byte(k, (unsigned char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put_byte(k, (unsigned char)(0xE0 | (cp >> 12)));
        put_byte(k, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put_byte(k, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        put_byte(k, (unsigned char)(0xF0 | (cp >> 18)));
        put_byte(k, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
        put_byte(k, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        put_byte(k, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const unsigned char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Parses a string starting at the opening quote. */
static int parse_string(parser *s, sink *k) {
    if (s->p >= s->end || *s->p != '"') return 0;
    s->p++;
    for (;;) {
        if (s->p >= s->end) return 0;
        unsigned char c = *s->p++;
        if (c == '"') return 1;
        if (c < 0x20) return 0;
        if (c != '\\') { put_byte(k, c); continue; }
        if (s->p >= s->end) return 0;
        c = *s->p++;
        switch (c) {
        case '"': put_byte(k, '"'); break;
        case '\\': put_byte(k, '\\'); break;
        case '/': put_byte(k, '/'); break;
        case 'b': put_byte(k, '\b'); break;
        case 'f': put_byte(k, '\f'); break;
        case 'n': put_byte(k, '\n'); break;
        case 'r': put_byte(k, '\r'); break;
        case 't': put_byte(k, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (s->end - s->p < 4 || !hex4(s->p, &cp)) return 0;
            s->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00) {
                uint32_t lo;
                if (s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u' && hex4(s->p + 2, &lo) &&
                    lo >= 0xDC00 && lo < 0xE000) {
                    s->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                cp = 0xFFFD;
            }
            put_cp(k, cp);
            break;
        }
        default: return 0;
        }
    }
}

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static int parse_number(parser *s, double *out) {
    const unsigned char *start = s->p;
    if (s->p < s->end && *s->p == '-') s->p++;
    if (s->p >= s->end) return 0;
    if (*s->p == '0') {
        s->p++;
    } else if (*s->p >= '1' && *s->p <= '9') {
        while (s->p < s->end && is_digit(*s->p)) s->p++;
    } else {
        return 0;
    }
    if (s->p < s->end && *s->p == '.') {
        s->p++;
        if (s->p >= s->end || !is_digit(*s->p)) return 0;
        while (s->p < s->end && is_digit(*s->p)) s->p++;
    }
    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        s->p++;
        if (s->p < s->end && (*s->p == '+' || *s->p == '-')) s->p++;
        if (s->p >= s->end || !is_digit(*s->p)) return 0;
        while (s->p < s->end && is_digit(*s->p)) s->p++;
    }
    char tmp[64];
    size_t n = (size_t)(s->p - start);
    if (n >= sizeof tmp) { *out = 0; return 1; }
    memcpy(tmp, start, n);
    tmp[n] = 0;
    *out = strtod(tmp, NULL);
    return 1;
}

static int lit(parser *s, const char *w) {
    size_t n = strlen(w);
    if ((size_t)(s->end - s->p) < n || memcmp(s->p, w, n) != 0) return 0;
    s->p += n;
    return 1;
}

static int parse_value(parser *s, int depth, jfield *cap) {
    ws(s);
    if (s->p >= s->end) return 0;
    unsigned char c = *s->p;
    if (c == '"') {
        sink k = {0};
        if (cap && cap->str) { k.dst = cap->str; k.cap = cap->cap; }
        if (!parse_string(s, &k)) return 0;
        if (cap) {
            cap->type = JV_STRING;
            if (cap->str) {
                cap->str[k.len] = 0;
                cap->len = k.len;
                cap->overflow = k.overflow;
            }
        }
        return 1;
    }
    if (c == '-' || is_digit(c)) {
        double d;
        if (!parse_number(s, &d)) return 0;
        if (cap) { cap->type = JV_NUMBER; cap->num = d; }
        return 1;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        if (!(c == 't' ? lit(s, "true") : c == 'f' ? lit(s, "false") : lit(s, "null"))) return 0;
        if (cap) cap->type = JV_OTHER;
        return 1;
    }
    if (c == '[') {
        if (depth >= MAX_DEPTH) return 0;
        s->p++;
        ws(s);
        if (s->p < s->end && *s->p == ']') { s->p++; goto other; }
        for (;;) {
            if (!parse_value(s, depth + 1, NULL)) return 0;
            ws(s);
            if (s->p >= s->end) return 0;
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == ']') { s->p++; break; }
            return 0;
        }
        goto other;
    }
    if (c == '{') {
        if (depth >= MAX_DEPTH) return 0;
        s->p++;
        ws(s);
        if (s->p < s->end && *s->p == '}') { s->p++; goto other; }
        for (;;) {
            char kb[KEY_MAX + 1];
            sink ks = {kb, sizeof kb, 0, 0};
            ws(s);
            if (!parse_string(s, &ks)) return 0;
            kb[ks.len] = 0;
            ws(s);
            if (s->p >= s->end || *s->p != ':') return 0;
            s->p++;
            jfield *match = NULL;
            if (depth == 0 && !ks.overflow) {
                for (int i = 0; i < s->nf; i++) {
                    if (strcmp(s->f[i].key, kb) == 0) { match = &s->f[i]; break; }
                }
            }
            if (match) { match->type = JV_ABSENT; match->len = 0; match->overflow = 0; }
            if (!parse_value(s, depth + 1, match)) return 0;
            ws(s);
            if (s->p >= s->end) return 0;
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == '}') { s->p++; break; }
            return 0;
        }
        goto other;
    }
    return 0;
other:
    if (cap) cap->type = JV_OTHER;
    return 1;
}

int json_scan(const char *buf, size_t n, jfield *fields, int nfields) {
    parser s = {(const unsigned char *)buf, (const unsigned char *)buf + n, fields, nfields};
    for (int i = 0; i < nfields; i++) { fields[i].type = JV_ABSENT; fields[i].len = 0; fields[i].overflow = 0; }
    ws(&s);
    int is_obj = s.p < s.end && *s.p == '{';
    if (!parse_value(&s, 0, NULL)) return -1;
    ws(&s);
    if (s.p != s.end) return -1;
    return is_obj ? 1 : 0;
}
