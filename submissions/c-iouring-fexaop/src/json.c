#include "json.h"
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <stdint.h>
#include <string.h>

#define JSON_MAX_DEPTH 64
#define BODY_MAX_CP 500

/* ------------------------------------------------------------------------- */
/* UTF-8 helpers                                                             */

static int utf8_encode(uint32_t cp, unsigned char *b)
{
    if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    b[0] = (unsigned char)(0xF0 | (cp >> 18));
    b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Strict decoding of one raw UTF-8 sequence. Returns 0 on malformed input. */
static int utf8_decode(const unsigned char *s, size_t n, uint32_t *cp,
                       size_t *consumed)
{
    if (n == 0)
        return 0;
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        *consumed = 1;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        if (n < 2 || (s[1] & 0xC0) != 0x80)
            return 0;
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *consumed = 2;
        return 1;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
            return 0;
        if (c == 0xE0 && s[1] < 0xA0)
            return 0;
        if (c == 0xED && s[1] >= 0xA0) /* surrogates */
            return 0;
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
              (uint32_t)(s[2] & 0x3F);
        *consumed = 3;
        return 1;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80)
            return 0;
        if (c == 0xF0 && s[1] < 0x90)
            return 0;
        if (c == 0xF4 && s[1] >= 0x90)
            return 0;
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *consumed = 4;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* JavaScript String.prototype.trim whitespace set:                          */
/* Unicode White_Space minus U+0085, plus U+FEFF.                            */

static int is_js_ws(uint32_t cp)
{
    switch (cp) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x0020:
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return 1;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

/* ------------------------------------------------------------------------- */
/* Parser                                                                    */

typedef struct {
    const unsigned char *s;
    size_t len;
    size_t i;
    int depth;
} parser;

/* Streaming string output. In trim mode (body values) leading whitespace is
 * never stored and the trimmed code-point count is tracked; trailing
 * whitespace may be stored (end_byte marks the byte after the last non-ws). */
typedef struct {
    char *dst;
    size_t cap;
    size_t written; /* total bytes produced (may exceed cap) */
    int trim_stream;
    int started;
    size_t trimmed_cp;
    size_t pending_ws;
    size_t end_byte;
} strout;

typedef struct {
    strout body;
    int present;
    int is_string;
} body_info;

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void so_write(strout *so, const unsigned char *b, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (so->written < so->cap)
            so->dst[so->written] = (char)b[k];
        so->written++;
    }
}

static void so_feed(strout *so, uint32_t cp, const unsigned char *b, size_t n)
{
    if (!so->trim_stream) {
        so_write(so, b, n);
        return;
    }
    int ws = is_js_ws(cp);
    if (!so->started) {
        if (ws)
            return; /* leading whitespace is trimmed away */
        so->started = 1;
        so->trimmed_cp = 1;
        so->pending_ws = 0;
    } else if (ws) {
        so->pending_ws++;
    } else {
        so->trimmed_cp += so->pending_ws + 1;
        so->pending_ws = 0;
    }
    so_write(so, b, n);
    if (!ws)
        so->end_byte = so->written;
}

static void skip_ws(parser *p)
{
    while (p->i < p->len) {
        unsigned char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->i++;
        else
            break;
    }
}

/* Parses and validates a JSON string; when so != NULL the decoded UTF-8 is
 * streamed into it. Returns 1 on success. */
static int parse_string(parser *p, strout *so)
{
    if (p->i >= p->len || p->s[p->i] != '"')
        return 0;
    p->i++;
    while (p->i < p->len) {
        unsigned char c = p->s[p->i];
        if (c == '"') {
            p->i++;
            return 1;
        }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->len)
                return 0;
            unsigned char e = p->s[p->i++];
            uint32_t cp;
            switch (e) {
            case '"':
                cp = '"';
                break;
            case '\\':
                cp = '\\';
                break;
            case '/':
                cp = '/';
                break;
            case 'b':
                cp = 0x08;
                break;
            case 'f':
                cp = 0x0C;
                break;
            case 'n':
                cp = 0x0A;
                break;
            case 'r':
                cp = 0x0D;
                break;
            case 't':
                cp = 0x09;
                break;
            case 'u': {
                if (p->i + 4 > p->len)
                    return 0;
                uint32_t u = 0;
                for (int k = 0; k < 4; k++) {
                    int h = hexval(p->s[p->i + (size_t)k]);
                    if (h < 0)
                        return 0;
                    u = (u << 4) | (uint32_t)h;
                }
                p->i += 4;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    if (p->i + 2 > p->len || p->s[p->i] != '\\' ||
                        p->s[p->i + 1] != 'u')
                        return 0;
                    p->i += 2;
                    if (p->i + 4 > p->len)
                        return 0;
                    uint32_t lo = 0;
                    for (int k = 0; k < 4; k++) {
                        int h = hexval(p->s[p->i + (size_t)k]);
                        if (h < 0)
                            return 0;
                        lo = (lo << 4) | (uint32_t)h;
                    }
                    p->i += 4;
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        return 0;
                    cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                } else if (u >= 0xDC00 && u <= 0xDFFF) {
                    return 0;
                } else {
                    cp = u;
                }
                break;
            }
            default:
                return 0;
            }
            if (so) {
                unsigned char b[4];
                int n = utf8_encode(cp, b);
                so_feed(so, cp, b, (size_t)n);
            }
        } else if (c < 0x20) {
            return 0;
        } else if (c < 0x80) {
            if (so) {
                unsigned char b = c;
                so_feed(so, c, &b, 1);
            }
            p->i++;
        } else {
            size_t n = 0;
            uint32_t cp = 0;
            if (utf8_decode(p->s + p->i, p->len - p->i, &cp, &n)) {
                if (so)
                    so_feed(so, cp, p->s + p->i, n);
                p->i += n;
            } else {
                if (so) {
                    unsigned char b = c;
                    so_feed(so, c, &b, 1);
                }
                p->i++;
            }
        }
    }
    return 0;
}

static int parse_value(parser *p, body_info *bi);

static int parse_number(parser *p)
{
    size_t i = p->i;
    if (i < p->len && p->s[i] == '-')
        i++;
    if (i >= p->len)
        return 0;
    if (p->s[i] == '0') {
        i++;
    } else if (p->s[i] >= '1' && p->s[i] <= '9') {
        while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9')
            i++;
    } else {
        return 0;
    }
    if (i < p->len && p->s[i] == '.') {
        i++;
        if (i >= p->len || p->s[i] < '0' || p->s[i] > '9')
            return 0;
        while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9')
            i++;
    }
    if (i < p->len && (p->s[i] == 'e' || p->s[i] == 'E')) {
        i++;
        if (i < p->len && (p->s[i] == '+' || p->s[i] == '-'))
            i++;
        if (i >= p->len || p->s[i] < '0' || p->s[i] > '9')
            return 0;
        while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9')
            i++;
    }
    p->i = i;
    return 1;
}

static int parse_literal(parser *p, const char *lit)
{
    size_t n = strlen(lit);
    if (p->i + n > p->len || memcmp(p->s + p->i, lit, n) != 0)
        return 0;
    p->i += n;
    return 1;
}

static int parse_array(parser *p)
{
    if (++p->depth > JSON_MAX_DEPTH)
        return 0;
    p->i++; /* '[' */
    skip_ws(p);
    if (p->i < p->len && p->s[p->i] == ']') {
        p->i++;
        p->depth--;
        return 1;
    }
    for (;;) {
        skip_ws(p);
        if (!parse_value(p, NULL))
            return 0;
        skip_ws(p);
        if (p->i < p->len && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->len && p->s[p->i] == ']') {
            p->i++;
            p->depth--;
            return 1;
        }
        return 0;
    }
}

static int parse_object(parser *p, body_info *bi)
{
    if (++p->depth > JSON_MAX_DEPTH)
        return 0;
    p->i++; /* '{' */
    skip_ws(p);
    if (p->i < p->len && p->s[p->i] == '}') {
        p->i++;
        p->depth--;
        return 1;
    }
    for (;;) {
        skip_ws(p);
        char keybuf[8];
        strout ks;
        memset(&ks, 0, sizeof ks);
        ks.dst = keybuf;
        ks.cap = sizeof keybuf;
        if (!parse_string(p, &ks))
            return 0;
        skip_ws(p);
        if (p->i >= p->len || p->s[p->i] != ':')
            return 0;
        p->i++;
        skip_ws(p);
        if (bi && ks.written == 4 && memcmp(keybuf, "body", 4) == 0) {
            bi->present = 1;
            if (p->i < p->len && p->s[p->i] == '"') {
                strout *bo = &bi->body;
                bo->written = 0;
                bo->started = 0;
                bo->trimmed_cp = 0;
                bo->pending_ws = 0;
                bo->end_byte = 0;
                bi->is_string = 1;
                if (!parse_string(p, bo))
                    return 0;
            } else {
                bi->is_string = 0;
                if (!parse_value(p, NULL))
                    return 0;
            }
        } else {
            if (!parse_value(p, NULL))
                return 0;
        }
        skip_ws(p);
        if (p->i < p->len && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->len && p->s[p->i] == '}') {
            p->i++;
            p->depth--;
            return 1;
        }
        return 0;
    }
}

static int parse_value(parser *p, body_info *bi)
{
    if (p->i >= p->len)
        return 0;
    unsigned char c = p->s[p->i];
    if (c == '{')
        return parse_object(p, bi);
    if (c == '[')
        return parse_array(p);
    if (c == '"')
        return parse_string(p, NULL);
    if (c == '-' || (c >= '0' && c <= '9'))
        return parse_number(p);
    if (c == 't')
        return parse_literal(p, "true");
    if (c == 'f')
        return parse_literal(p, "false");
    if (c == 'n')
        return parse_literal(p, "null");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */

enum json_body_result json_extract_body(const char *buf, size_t len, char *out,
                                        size_t cap, size_t *out_len)
{
    parser p;
    p.s = (const unsigned char *)buf;
    p.len = len;
    p.i = 0;
    p.depth = 0;

    body_info bi;
    memset(&bi, 0, sizeof bi);
    bi.body.dst = out;
    bi.body.cap = cap;
    bi.body.trim_stream = 1;

    skip_ws(&p);
    int ok;
    if (p.i < p.len && p.s[p.i] == '{')
        ok = parse_object(&p, &bi);
    else
        ok = parse_value(&p, NULL);
    if (!ok)
        return JSON_BODY_MALFORMED;

    skip_ws(&p);
    if (p.i != p.len)
        return JSON_BODY_MALFORMED; /* trailing garbage */

    if (!bi.present || !bi.is_string)
        return JSON_BODY_MISSING;
    if (bi.body.trimmed_cp == 0)
        return JSON_BODY_MISSING;
    if (bi.body.trimmed_cp > BODY_MAX_CP)
        return JSON_BODY_TOO_LONG;
    if (bi.body.end_byte >= cap)
        return JSON_BODY_TOO_LONG; /* does not fit / no room for NUL */
    out[bi.body.end_byte] = '\0';
    if (out_len)
        *out_len = bi.body.end_byte;
    return JSON_BODY_OK;
}


size_t json_escape(char *dst, const char *s, size_t n)
{
    return json_escape_inline(dst, s, n);
}
