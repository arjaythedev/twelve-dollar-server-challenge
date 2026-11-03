/* json.c - scanner: one validating pass that keeps only named fields */
#include <string.h>

#include "json.h"

typedef struct {
    const char *p, *e;
    int bad;
} jp_t;

static void ws(jp_t *j)
{
    while (j->p < j->e) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
        else break;
    }
}

static void value(jp_t *j, jt_t *t, const char **v, const char **ve);

/* "..." with strict escape validation; leaves v/ve around the raw contents */
static void string(jp_t *j, const char **v, const char **ve)
{
    *v = j->p; /* past the opening quote */
    for (;;) {
        if (j->p >= j->e) { j->bad = 1; return; }
        unsigned char c = (unsigned char)*j->p;
        if (c == '"') { *ve = j->p; j->p++; return; }
        if (c == '\\') {
            if (j->e - j->p < 2) { j->bad = 1; return; }
            char e = j->p[1];
            j->p += 2;
            if (e == 'u') {
                if (j->e - j->p < 4) { j->bad = 1; return; }
                for (int i = 0; i < 4; i++) {
                    char h = j->p[i];
                    int ok = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                    if (!ok) { j->bad = 1; return; }
                }
                j->p += 4;
            } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
                       e == 'n' || e == 'r' || e == 't') {
                /* ok */
            } else {
                j->bad = 1;
                return;
            }
            continue;
        }
        if (c < 0x20) { j->bad = 1; return; } /* raw control chars are invalid */
        j->p++;
    }
}

static void number(jp_t *j, const char **v, const char **ve)
{
    *v = j->p;
    const char *p = j->p;
    if (p < j->e && *p == '-') p++;
    if (p >= j->e || *p < '0' || *p > '9') { j->bad = 1; return; }
    if (*p == '0') p++;
    else while (p < j->e && *p >= '0' && *p <= '9') p++;
    if (p < j->e && *p == '.') {
        p++;
        if (p >= j->e || *p < '0' || *p > '9') { j->bad = 1; return; }
        while (p < j->e && *p >= '0' && *p <= '9') p++;
    }
    if (p < j->e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < j->e && (*p == '+' || *p == '-')) p++;
        if (p >= j->e || *p < '0' || *p > '9') { j->bad = 1; return; }
        while (p < j->e && *p >= '0' && *p <= '9') p++;
    }
    j->p = p;
    *ve = p;
}

static void skip_container(jp_t *j, char open)
{
    char close = open == '{' ? '}' : ']';
    j->p++; /* the opening bracket */
    int depth = 1;
    while (depth > 0) {
        if (j->p >= j->e) { j->bad = 1; return; }
        char c = *j->p;
        if (c == '"') {
            const char *v, *ve;
            j->p++; /* opening quote */
            string(j, &v, &ve);
            if (j->bad) return;
            continue;
        }
        j->p++;
        if (c == open) depth++;
        else if (c == close) depth--;
    }
}

static void value(jp_t *j, jt_t *t, const char **v, const char **ve)
{
    ws(j);
    if (j->p >= j->e) { j->bad = 1; return; }
    char c = *j->p;
    if (c == '"') {
        j->p++;
        string(j, v, ve);
        *t = JT_STRING;
        return;
    }
    if (c == '{' || c == '[') {
        *t = JT_OTHER;
        skip_container(j, c);
        return;
    }
    if (c == 't') {
        if (j->e - j->p < 4 || memcmp(j->p, "true", 4)) { j->bad = 1; return; }
        j->p += 4; *t = JT_OTHER; return;
    }
    if (c == 'f') {
        if (j->e - j->p < 5 || memcmp(j->p, "false", 5)) { j->bad = 1; return; }
        j->p += 5; *t = JT_OTHER; return;
    }
    if (c == 'n') {
        if (j->e - j->p < 4 || memcmp(j->p, "null", 4)) { j->bad = 1; return; }
        j->p += 4; *t = JT_OTHER; return;
    }
    number(j, v, ve);
    *t = JT_NUMBER;
}

int json_fields(const char *s, size_t n, jfield_t *f, int nf)
{
    jp_t j = { s, s + n, 0 };
    for (int i = 0; i < nf; i++) { f[i].type = JT_NONE; f[i].v = f[i].ve = NULL; }

    ws(&j);
    if (j.p >= j.e || *j.p != '{') {
        if (j.p >= j.e) return JT_NONE;
        jt_t t; const char *v, *ve;
        value(&j, &t, &v, &ve);
        ws(&j);
        if (j.bad || j.p != j.e) return JT_NONE;
        return t; /* string/number/array/bool/null: not an object, so no fields */
    }
    j.p++; /* '{' */

    ws(&j);
    if (j.p < j.e && *j.p == '}') { j.p++; goto done; }
    for (;;) {
        if (j.p >= j.e || *j.p != '"') { return JT_NONE; }
        j.p++;
        const char *kv, *kve;
        string(&j, &kv, &kve);
        if (j.bad) return JT_NONE;
        ws(&j);
        if (j.p >= j.e || *j.p != ':') return JT_NONE;
        j.p++;
        jt_t t; const char *v, *ve;
        value(&j, &t, &v, &ve);
        if (j.bad) return JT_NONE;
        size_t kl = (size_t)(kve - kv);
        for (int i = 0; i < nf; i++) {
            if (strlen(f[i].name) == kl && !memcmp(f[i].name, kv, kl)) {
                f[i].type = t;
                f[i].v = v;
                f[i].ve = ve;
                break;
            }
        }
        ws(&j);
        if (j.p >= j.e) return JT_NONE;
        if (*j.p == ',') { j.p++; ws(&j); continue; }
        if (*j.p == '}') { j.p++; break; }
        return JT_NONE;
    }
done:
    ws(&j);
    if (j.bad || j.p != j.e) return JT_NONE;
    return JT_OTHER; /* valid object root */
}

size_t j_unescape(const char *src, const char *se, char *dst)
{
    char *o = dst;
    const char *p = src;
    while (p < se) {
        char c = *p;
        if (c != '\\') { *o++ = c; p++; continue; }
        if (++p >= se) return (size_t)-1;
        char e = *p++;
        switch (e) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            if (se - p < 4) return (size_t)-1;
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                char h = p[i];
                int d = h >= '0' && h <= '9' ? h - '0'
                      : h >= 'a' && h <= 'f' ? h - 'a' + 10
                      : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                if (d < 0) return (size_t)-1;
                cp = cp << 4 | (unsigned)d;
            }
            p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) { /* high surrogate: needs a low one */
                if (se - p < 6 || p[0] != '\\' || p[1] != 'u') return (size_t)-1;
                unsigned lo = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p[2 + i];
                    int d = h >= '0' && h <= '9' ? h - '0'
                          : h >= 'a' && h <= 'f' ? h - 'a' + 10
                          : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (d < 0) return (size_t)-1;
                    lo = lo << 4 | (unsigned)d;
                }
                if (lo < 0xDC00 || lo > 0xDFFF) return (size_t)-1;
                p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return (size_t)-1; /* stray low surrogate */
            }
            if (cp < 0x80) {
                *o++ = (char)cp;
            } else if (cp < 0x800) {
                *o++ = (char)(0xC0 | cp >> 6);
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                *o++ = (char)(0xE0 | cp >> 12);
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *o++ = (char)(0xF0 | cp >> 18);
                *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            return (size_t)-1;
        }
    }
    return (size_t)(o - dst);
}

char *j_escape(char *o, const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t run = i;
        while (i < n) {
            unsigned char c = s[i];
            if (c == '"' || c == '\\' || c < 0x20) break;
            i++;
        }
        if (i > run) {
            memcpy(o, s + run, i - run);
            o += i - run;
        }
        if (i >= n) break;
        unsigned char c = s[i++];
        *o++ = '\\';
        switch (c) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '\b': *o++ = 'b'; break;
        case '\f': *o++ = 'f'; break;
        case '\n': *o++ = 'n'; break;
        case '\r': *o++ = 'r'; break;
        case '\t': *o++ = 't'; break;
        default: {
            static const char hex[] = "0123456789abcdef";
            *o++ = 'u';
            *o++ = '0'; *o++ = '0';
            *o++ = hex[c >> 4];
            *o++ = hex[c & 15];
        }
        }
    }
    return o;
}
