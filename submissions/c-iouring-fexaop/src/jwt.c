/* HS256 JWT verification with a strict, allocation-free JSON parser. */
#include "jwt.h"
#include <string.h>
#include <stdlib.h> /* strtod */

/* The two sha256.h entry points this module uses.  Each module includes only its own header;
 * sha256.h is a frozen public interface, so the declarations are repeated verbatim. */
void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[32]);
int b64url_decode(const char *s, size_t n, uint8_t *out, size_t cap);

#define HDR_MAX 1024
#define PAY_MAX 4096

/* ------------------------------------------------------------------ */
/* Minimal strict JSON parser                                          */

typedef struct {
    const uint8_t *s;
    size_t n;
    size_t i;
} jp;

static void jp_ws(jp *p)
{
    while (p->i < p->n) {
        uint8_t c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int jp_hex4(jp *p, uint32_t *v)
{
    if (p->n - p->i < 4) return 0;
    uint32_t x = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(p->s[p->i + k]);
        if (h < 0) return 0;
        x = (x << 4) | (uint32_t)h;
    }
    p->i += 4;
    *v = x;
    return 1;
}

static void jp_put(uint8_t *out, size_t cap, size_t *o, uint8_t b)
{
    if (out && *o < cap) out[*o] = b;
    (*o)++;
}

/* Parses a JSON string at p->i.  If out is non-NULL, decodes up to cap bytes into it, but
 * *olen always receives the full decoded length (so callers can detect overflow). */
static int jp_string(jp *p, uint8_t *out, size_t cap, size_t *olen)
{
    if (p->i >= p->n || p->s[p->i] != '"') return 0;
    p->i++;

    size_t o = 0;
    for (;;) {
        if (p->i >= p->n) return 0;
        uint8_t c = p->s[p->i++];
        if (c == '"') break;

        if (c == '\\') {
            if (p->i >= p->n) return 0;
            uint8_t e = p->s[p->i++];
            uint32_t cp;
            switch (e) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u': {
                if (!jp_hex4(p, &cp)) return 0;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (p->n - p->i < 6) return 0;
                    if (p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') return 0;
                    p->i += 2;
                    if (!jp_hex4(p, &lo)) return 0;
                    if (lo < 0xDC00 || lo > 0xDFFF) return 0;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return 0; /* lone low surrogate */
                }
                break;
            }
            default:
                return 0;
            }

            if (cp < 0x80) {
                jp_put(out, cap, &o, (uint8_t)cp);
            } else if (cp < 0x800) {
                jp_put(out, cap, &o, (uint8_t)(0xC0 | (cp >> 6)));
                jp_put(out, cap, &o, (uint8_t)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                jp_put(out, cap, &o, (uint8_t)(0xE0 | (cp >> 12)));
                jp_put(out, cap, &o, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                jp_put(out, cap, &o, (uint8_t)(0x80 | (cp & 0x3F)));
            } else {
                jp_put(out, cap, &o, (uint8_t)(0xF0 | (cp >> 18)));
                jp_put(out, cap, &o, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
                jp_put(out, cap, &o, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                jp_put(out, cap, &o, (uint8_t)(0x80 | (cp & 0x3F)));
            }
        } else if (c < 0x20) {
            return 0; /* raw control character */
        } else {
            jp_put(out, cap, &o, c);
        }
    }

    if (olen) *olen = o;
    return 1;
}

static int jp_number(jp *p)
{
    size_t i = p->i, n = p->n;
    const uint8_t *s = p->s;

    if (i < n && s[i] == '-') i++;
    if (i >= n) return 0;
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    } else {
        return 0;
    }
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || s[i] < '0' || s[i] > '9') return 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= n || s[i] < '0' || s[i] > '9') return 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    p->i = i;
    return 1;
}

static int jp_lit(jp *p, const char *lit)
{
    size_t l = strlen(lit);
    if (p->n - p->i < l) return 0;
    if (memcmp(p->s + p->i, lit, l) != 0) return 0;
    p->i += l;
    return 1;
}

static int jp_skip_value(jp *p, int depth)
{
    if (depth > 64) return 0;
    jp_ws(p);
    if (p->i >= p->n) return 0;

    uint8_t c = p->s[p->i];
    if (c == '"') return jp_string(p, NULL, 0, NULL);

    if (c == '{') {
        p->i++;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; return 1; }
        for (;;) {
            size_t dummy;
            jp_ws(p);
            if (!jp_string(p, NULL, 0, &dummy)) return 0;
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return 0;
            p->i++;
            if (!jp_skip_value(p, depth + 1)) return 0;
            jp_ws(p);
            if (p->i >= p->n) return 0;
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == '}') { p->i++; return 1; }
            return 0;
        }
    }

    if (c == '[') {
        p->i++;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; return 1; }
        for (;;) {
            if (!jp_skip_value(p, depth + 1)) return 0;
            jp_ws(p);
            if (p->i >= p->n) return 0;
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == ']') { p->i++; return 1; }
            return 0;
        }
    }

    if (c == 't') return jp_lit(p, "true");
    if (c == 'f') return jp_lit(p, "false");
    if (c == 'n') return jp_lit(p, "null");
    if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Claim evaluation                                                    */

/* The number span is lexically valid JSON.  Returns 1 iff it is > now. */
static int number_gt_now(const uint8_t *s, size_t n, int64_t now)
{
    if (n == 0) return 0;
    if (s[0] == '-') return 0; /* now_seconds is a non-negative epoch */

    int integral = 1;
    for (size_t k = 0; k < n; k++) {
        if (s[k] == '.' || s[k] == 'e' || s[k] == 'E') { integral = 0; break; }
    }

    if (integral) {
        uint64_t v = 0;
        for (size_t k = 0; k < n; k++) {
            uint64_t d = (uint64_t)(s[k] - '0');
            if (v > (UINT64_MAX - d) / 10) return 1; /* huge positive */
            v = v * 10 + d;
        }
        if (v > (uint64_t)INT64_MAX) return 1;
        return (int64_t)v > now;
    }

    char tmp[64];
    if (n >= sizeof tmp) return 0;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    double d = strtod(tmp, NULL);
    return d > (double)now;
}

/* Parses the decoded payload object and applies exp / sub / username rules. */
static enum jwt_result eval_payload(const uint8_t *buf, size_t n, int64_t now,
                                    struct jwt_claims *out)
{
    jp p = { buf, n, 0 };

    jp_ws(&p);
    if (p.i >= p.n || p.s[p.i] != '{') return JWT_INVALID;
    p.i++;

    int has_exp = 0, exp_is_num = 0;
    const uint8_t *exp_s = NULL;
    size_t exp_len = 0;

    int has_sub = 0, sub_bad = 0;
    uint8_t sub[32];
    size_t sub_len = 0;

    int has_name = 0, name_bad = 0;
    uint8_t name[256];
    size_t name_len = 0;

    jp_ws(&p);
    if (p.i < p.n && p.s[p.i] == '}') {
        p.i++;
    } else {
        for (;;) {
            jp_ws(&p);
            uint8_t key[16];
            size_t klen = 0;
            if (!jp_string(&p, key, sizeof key, &klen)) return JWT_INVALID;

            jp_ws(&p);
            if (p.i >= p.n || p.s[p.i] != ':') return JWT_INVALID;
            p.i++;
            jp_ws(&p);

            if (klen == 3 && memcmp(key, "sub", 3) == 0) {
                has_sub = 1;
                sub_bad = 0;
                sub_len = 0;
                if (p.i < p.n && p.s[p.i] == '"') {
                    if (!jp_string(&p, sub, sizeof sub, &sub_len)) return JWT_INVALID;
                    if (sub_len > sizeof sub) sub_bad = 1;
                } else {
                    sub_bad = 1;
                    if (!jp_skip_value(&p, 0)) return JWT_INVALID;
                }
            } else if (klen == 8 && memcmp(key, "username", 8) == 0) {
                has_name = 1;
                name_bad = 0;
                name_len = 0;
                if (p.i < p.n && p.s[p.i] == '"') {
                    if (!jp_string(&p, name, sizeof name, &name_len)) return JWT_INVALID;
                    if (name_len > sizeof name) name_bad = 1;
                } else {
                    name_bad = 1;
                    if (!jp_skip_value(&p, 0)) return JWT_INVALID;
                }
            } else if (klen == 3 && memcmp(key, "exp", 3) == 0) {
                has_exp = 1;
                exp_is_num = 0;
                exp_s = NULL;
                exp_len = 0;
                if (p.i < p.n && (p.s[p.i] == '-' ||
                                  (p.s[p.i] >= '0' && p.s[p.i] <= '9'))) {
                    size_t start = p.i;
                    if (!jp_number(&p)) return JWT_INVALID;
                    exp_is_num = 1;
                    exp_s = p.s + start;
                    exp_len = p.i - start;
                } else if (!jp_skip_value(&p, 0)) {
                    return JWT_INVALID;
                }
            } else {
                if (!jp_skip_value(&p, 0)) return JWT_INVALID;
            }

            jp_ws(&p);
            if (p.i >= p.n) return JWT_INVALID;
            if (p.s[p.i] == ',') { p.i++; continue; }
            if (p.s[p.i] == '}') { p.i++; break; }
            return JWT_INVALID;
        }
    }

    jp_ws(&p);
    if (p.i != p.n) return JWT_INVALID;

    /* Signature is already known good: bad exp is INVALID, bad claims are PAYLOAD. */
    if (!has_exp || !exp_is_num || !number_gt_now(exp_s, exp_len, now))
        return JWT_INVALID;

    if (!has_sub || sub_bad || sub_len == 0) return JWT_PAYLOAD;
    if (sub[0] == '0') return JWT_PAYLOAD; /* "0" is not positive; no leading zeros */
    uint64_t uid = 0;
    for (size_t k = 0; k < sub_len; k++) {
        uint8_t c = sub[k];
        if (c < '0' || c > '9') return JWT_PAYLOAD;
        uint64_t d = (uint64_t)(c - '0');
        if (uid > (UINT64_MAX - d) / 10) return JWT_PAYLOAD;
        uid = uid * 10 + d;
    }
    if (uid == 0 || uid > (uint64_t)INT64_MAX) return JWT_PAYLOAD;

    if (!has_name || name_bad || name_len > 255) return JWT_PAYLOAD;

    out->user_id = (int64_t)uid;
    memcpy(out->username, name, name_len);
    out->username[name_len] = '\0';
    out->username_len = (int)name_len;
    return JWT_OK;
}

static int header_is_hs256(const uint8_t *buf, size_t n)
{
    jp p = { buf, n, 0 };

    jp_ws(&p);
    if (p.i >= p.n || p.s[p.i] != '{') return 0;
    p.i++;

    int alg_seen = 0, alg_ok = 0;

    jp_ws(&p);
    if (p.i < p.n && p.s[p.i] == '}') {
        p.i++;
    } else {
        for (;;) {
            jp_ws(&p);
            uint8_t key[16];
            size_t klen = 0;
            if (!jp_string(&p, key, sizeof key, &klen)) return 0;

            jp_ws(&p);
            if (p.i >= p.n || p.s[p.i] != ':') return 0;
            p.i++;
            jp_ws(&p);

            if (klen == 3 && memcmp(key, "alg", 3) == 0) {
                uint8_t alg[8];
                size_t alen = 0;
                if (!jp_string(&p, alg, sizeof alg, &alen)) return 0;
                alg_seen = 1;
                alg_ok = (alen == 5 && memcmp(alg, "HS256", 5) == 0);
            } else if (klen == 3 && memcmp(key, "typ", 3) == 0) {
                if (!jp_string(&p, NULL, 0, NULL)) return 0; /* must be a string */
            } else {
                if (!jp_skip_value(&p, 0)) return 0;
            }

            jp_ws(&p);
            if (p.i >= p.n) return 0;
            if (p.s[p.i] == ',') { p.i++; continue; }
            if (p.s[p.i] == '}') { p.i++; break; }
            return 0;
        }
    }

    jp_ws(&p);
    if (p.i != p.n) return 0;
    return alg_seen && alg_ok;
}

/* ------------------------------------------------------------------ */

enum jwt_result jwt_verify(const char *auth, size_t auth_len,
                           const uint8_t *secret, size_t secret_len,
                           int64_t now_seconds, struct jwt_claims *out)
{
    uint8_t hdr[HDR_MAX];
    uint8_t pay[PAY_MAX];
    uint8_t sig[32];
    uint8_t want[32];

    if (!auth || auth_len < 8) return JWT_MISSING;
    if (memcmp(auth, "Bearer ", 7) != 0) return JWT_MISSING;

    const char *t = auth + 7;
    size_t tl = auth_len - 7;

    size_t d1 = 0;
    while (d1 < tl && t[d1] != '.') d1++;
    if (d1 == tl) return JWT_INVALID;      /* no signature separator */
    if (d1 == 0) return JWT_INVALID;       /* empty header segment */

    size_t d2 = d1 + 1;
    while (d2 < tl && t[d2] != '.') d2++;
    if (d2 == tl) return JWT_INVALID;      /* only two segments */
    if (d2 == d1 + 1) return JWT_INVALID;  /* empty payload segment */

    size_t d3 = d2 + 1;
    while (d3 < tl && t[d3] != '.') d3++;
    if (d3 != tl) return JWT_INVALID;      /* a fourth segment */
    if (tl - d2 - 1 == 0) return JWT_INVALID; /* empty signature segment */

    int hl = b64url_decode(t, d1, hdr, sizeof hdr);
    if (hl < 0) return JWT_INVALID;
    if (!header_is_hs256(hdr, (size_t)hl)) return JWT_INVALID;

    const char *sig_b64 = t + d2 + 1;
    size_t sig_b64_len = tl - d2 - 1;

    int sl = b64url_decode(sig_b64, sig_b64_len, sig, sizeof sig);
    if (sl != 32) return JWT_INVALID;

    hmac_sha256(secret, secret_len, t, d2, want);

    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof sig; i++) diff |= (uint8_t)(sig[i] ^ want[i]);
    if (diff != 0) return JWT_INVALID;

    int pl = b64url_decode(t + d1 + 1, d2 - d1 - 1, pay, sizeof pay);
    if (pl < 0) return JWT_INVALID;

    return eval_payload(pay, (size_t)pl, now_seconds, out);
}
