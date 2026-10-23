// Root class, byte buffers and a small validating JSON reader.
#include "td.h"

@implementation TDObject
+ (instancetype)make {
    TDObject *o = class_createInstance(self, 0);
    return [o init];
}
- (instancetype)init { return self; }
@end

void td_buf_grow(TDBuf *b, uint32_t need) {
    uint32_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) { perror("realloc"); abort(); }
    b->p = p;
    b->cap = cap;
}

// 1 = needs escaping
static const uint8_t esc_tab[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
};

void td_buf_json_str(TDBuf *b, const char *s, uint32_t n) {
    td_buf_reserve(b, n);
    uint32_t i = 0, start = 0;
    for (; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (TD_LIKELY(!esc_tab[c])) continue;
        td_buf_put(b, s + start, i - start);
        start = i + 1;
        td_buf_reserve(b, 6);
        char *o = b->p + b->len;
        switch (c) {
        case '"': o[0] = '\\'; o[1] = '"'; b->len += 2; break;
        case '\\': o[0] = '\\'; o[1] = '\\'; b->len += 2; break;
        case '\n': o[0] = '\\'; o[1] = 'n'; b->len += 2; break;
        case '\r': o[0] = '\\'; o[1] = 'r'; b->len += 2; break;
        case '\t': o[0] = '\\'; o[1] = 't'; b->len += 2; break;
        case '\b': o[0] = '\\'; o[1] = 'b'; b->len += 2; break;
        case '\f': o[0] = '\\'; o[1] = 'f'; b->len += 2; break;
        default: {
            static const char hex[] = "0123456789abcdef";
            o[0] = '\\'; o[1] = 'u'; o[2] = '0'; o[3] = '0';
            o[4] = hex[c >> 4]; o[5] = hex[c & 15];
            b->len += 6;
        }
        }
    }
    td_buf_put(b, s + start, i - start);
}

// ---------------------------------------------------------------- JSON reader
typedef struct { const char *p, *e; int depth; } JP;

TD_INLINE void jws(JP *j) {
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}
static int jvalue(JP *j);

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// at opening quote; on success p is past the closing quote. *esc set if escapes seen.
static int jstring(JP *j, bool *esc) {
    j->p++;
    while (j->p < j->e) {
        uint8_t c = (uint8_t)*j->p;
        if (c == '"') { j->p++; return 0; }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (esc) *esc = true;
            j->p++;
            if (j->p >= j->e) return -1;
            char d = *j->p;
            if (d == 'u') {
                if (j->e - j->p < 5) return -1;
                for (int k = 1; k <= 4; k++) if (hexv(j->p[k]) < 0) return -1;
                j->p += 5;
                continue;
            }
            if (!strchr("\"\\/bfnrt", d) || d == 0) return -1;
        }
        j->p++;
    }
    return -1;
}

static int jnumber(JP *j) {
    const char *p = j->p;
    if (p < j->e && *p == '-') p++;
    if (p >= j->e) return -1;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (p < j->e && *p >= '0' && *p <= '9') p++; }
    else return -1;
    if (p < j->e && *p == '.') {
        p++;
        if (p >= j->e || *p < '0' || *p > '9') return -1;
        while (p < j->e && *p >= '0' && *p <= '9') p++;
    }
    if (p < j->e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < j->e && (*p == '+' || *p == '-')) p++;
        if (p >= j->e || *p < '0' || *p > '9') return -1;
        while (p < j->e && *p >= '0' && *p <= '9') p++;
    }
    j->p = p;
    return 0;
}

static int jlit(JP *j, const char *s, size_t n) {
    if ((size_t)(j->e - j->p) < n || memcmp(j->p, s, n)) return -1;
    j->p += n;
    return 0;
}

static int jcontainer(JP *j, char close) {
    if (++j->depth > 64) return -1;
    j->p++;
    jws(j);
    if (j->p < j->e && *j->p == close) { j->p++; j->depth--; return 0; }
    for (;;) {
        jws(j);
        if (close == '}') {
            if (j->p >= j->e || *j->p != '"' || jstring(j, NULL)) return -1;
            jws(j);
            if (j->p >= j->e || *j->p != ':') return -1;
            j->p++;
            jws(j);
        }
        if (jvalue(j)) return -1;
        jws(j);
        if (j->p >= j->e) return -1;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == close) { j->p++; j->depth--; return 0; }
        return -1;
    }
}

static int jvalue(JP *j) {
    if (j->p >= j->e) return -1;
    switch (*j->p) {
    case '"': return jstring(j, NULL);
    case '{': return jcontainer(j, '}');
    case '[': return jcontainer(j, ']');
    case 't': return jlit(j, "true", 4);
    case 'f': return jlit(j, "false", 5);
    case 'n': return jlit(j, "null", 4);
    default: return jnumber(j);
    }
}

int td_json_object(const char *p, uint32_t n, TDJField *f, int nf) {
    JP j = { p, p + n, 0 };
    for (int i = 0; i < nf; i++) f[i].type = TDJ_NONE;
    jws(&j);
    if (j.p >= j.e) return -1;
    if (*j.p != '{') {
        if (jvalue(&j)) return -1;
        jws(&j);
        return j.p == j.e ? 0 : -1;
    }
    j.p++;
    j.depth = 1;
    jws(&j);
    if (j.p < j.e && *j.p == '}') { j.p++; goto done; }
    for (;;) {
        jws(&j);
        if (j.p >= j.e || *j.p != '"') return -1;
        const char *ks = j.p + 1;
        bool kesc = false;
        if (jstring(&j, &kesc)) return -1;
        uint32_t klen = (uint32_t)(j.p - 1 - ks);
        jws(&j);
        if (j.p >= j.e || *j.p != ':') return -1;
        j.p++;
        jws(&j);
        if (j.p >= j.e) return -1;
        TDJField *hit = NULL;
        if (!kesc)
            for (int i = 0; i < nf; i++)
                if (f[i].keylen == klen && !memcmp(f[i].key, ks, klen)) { hit = &f[i]; break; }
        const char *vs = j.p;
        char c = *j.p;
        if (c == '"') {
            bool esc = false;
            if (jstring(&j, &esc)) return -1;
            if (hit) { hit->type = TDJ_STRING; hit->v = vs + 1; hit->vlen = (uint32_t)(j.p - vs - 2); hit->escaped = esc; }
        } else {
            if (jvalue(&j)) return -1;
            if (hit) {
                hit->type = (c == '-' || (c >= '0' && c <= '9')) ? TDJ_NUMBER : TDJ_OTHER;
                hit->v = vs; hit->vlen = (uint32_t)(j.p - vs); hit->escaped = false;
            }
        }
        jws(&j);
        if (j.p >= j.e) return -1;
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p == '}') { j.p++; break; }
        return -1;
    }
done:
    jws(&j);
    return j.p == j.e ? 1 : -1;
}

static int put_utf8(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static uint32_t hex4(const char *s) {
    return (uint32_t)(hexv(s[0]) << 12 | hexv(s[1]) << 8 | hexv(s[2]) << 4 | hexv(s[3]));
}

int td_json_unescape(const char *s, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n;) {
        if (o + 4 > cap) return -1;
        char c = s[i];
        if (c != '\\') { out[o++] = c; i++; continue; }
        char d = s[i + 1];
        i += 2;
        switch (d) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            uint32_t cp = hex4(s + i);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') {
                uint32_t lo = hex4(s + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; // lone surrogate
            o += (uint32_t)put_utf8(out + o, cp);
            break;
        }
        default: return -1;
        }
    }
    return (int)o;
}
