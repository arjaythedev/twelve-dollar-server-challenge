/* jwt.c - SHA-256, HMAC with precomputed pads, base64url, HS256 token check */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "jwt.h"

/* ---------------------------------------------------------------- SHA-256 */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t buf[64];
    unsigned n;
} sha256_ctx;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define S0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define S1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define s0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define s1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_compress(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + S1(e) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = S0(a) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, H0, sizeof H0);
    c->bits = 0;
    c->n = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->bits += (uint64_t)n * 8;
    if (c->n) {
        unsigned take = 64 - c->n;
        if (take > n) take = (unsigned)n;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        n -= take;
        if (c->n == 64) { sha256_compress(c->h, c->buf); c->n = 0; }
    }
    while (n >= 64) { sha256_compress(c->h, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->n = (unsigned)n; }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->bits;
    uint8_t one = 0x80;
    c->buf[c->n++] = one;
    if (c->n > 56) {
        memset(c->buf + c->n, 0, 64 - c->n);
        c->n = 64;
        sha256_compress(c->h, c->buf);
        c->n = 0;
    }
    memset(c->buf + c->n, 0, 56 - c->n);
    for (int i = 0; i < 8; i++) c->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_compress(c->h, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

/* ---------------------------------------------------------------- HMAC */

/* Pads hashed once at startup: a request then costs two short continuations. */
typedef struct {
    uint32_t istate[8]; /* after the ipad block */
    uint32_t ostate[8]; /* after the opad block */
} hmac_key_t;

static hmac_key_t g_key;

void jwt_init_raw(const uint8_t *secret, size_t klen);

void jwt_init(const char *secret, size_t secret_len)
{
    jwt_init_raw((const uint8_t *)secret, secret_len);
}

void jwt_init_raw(const uint8_t *secret, size_t klen)
{
    uint8_t k[64];
    memset(k, 0, sizeof k);
    if (klen > 64) {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, secret, klen);
        sha256_final(&c, k);
    } else {
        memcpy(k, secret, klen);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    memcpy(g_key.istate, c.h, sizeof c.h);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    memcpy(g_key.ostate, c.h, sizeof c.h);
}

void hmac_sha256_str(const char *msg, size_t mlen, uint8_t out[32])
{
    sha256_ctx c;
    memcpy(c.h, g_key.istate, sizeof c.h);
    c.bits = 64 * 8;
    c.n = 0;
    sha256_update(&c, msg, mlen);
    uint8_t inner[32];
    sha256_final(&c, inner);

    memcpy(c.h, g_key.ostate, sizeof c.h);
    c.bits = 64 * 8;
    c.n = 0;
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* ---------------------------------------------------------------- base64url */

static signed char B64U[256];

void jwt_b64url_init(void)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    memset(B64U, -1, sizeof B64U);
    for (int i = 0; i < 64; i++) B64U[(unsigned char)A[i]] = (signed char)i;
}

static long b64url_decode(const char *s, size_t n, char *out)
{
    if (n % 4 == 1) return -1;
    char *o = out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = B64U[(unsigned char)s[i]];
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; *o++ = (char)(acc >> bits); }
    }
    if (acc & ((1u << bits) - 1)) return -1; /* non-canonical trailing bits */
    return o - out;
}

static void b64url_encode32(const uint8_t in[32], char out[43])
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int j = 0;
    for (int i = 0; i < 30; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[j++] = A[v >> 18]; out[j++] = A[v >> 12 & 63]; out[j++] = A[v >> 6 & 63]; out[j++] = A[v & 63];
    }
    uint32_t v = (uint32_t)in[30] << 16 | (uint32_t)in[31] << 8;
    out[j++] = A[v >> 18]; out[j++] = A[v >> 12 & 63]; out[j++] = A[v >> 6 & 63];
}

/* ---------------------------------------------------------------- token check */

static int json_number(const jfield_t *f, double *out)
{
    char tmp[64];
    size_t n = (size_t)(f->ve - f->v);
    if (n == 0 || n >= sizeof tmp) return 0;
    memcpy(tmp, f->v, n);
    tmp[n] = 0;
    char *end = NULL;
    *out = strtod(tmp, &end);
    return end && *end == 0 && isfinite(*out);
}

int check_auth(const char *h, size_t hn, double now, auth_t *a)
{
    if (!h || hn < 7 || memcmp(h, "Bearer ", 7)) return 1;
    const char *t = h + 7, *te = h + hn;

    const char *d1 = memchr(t, '.', (size_t)(te - t));
    if (!d1) return 2;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(te - d1 - 1));
    if (!d2 || memchr(d2 + 1, '.', (size_t)(te - d2 - 1))) return 2;

    /* signature first, constant time, always exactly 43 base64url chars */
    if (te - d2 - 1 != 43) return 2;
    uint8_t mac[32];
    char enc[43];
    hmac_sha256_str(t, (size_t)(d2 - t), mac);
    b64url_encode32(mac, enc);
    unsigned diff = 0;
    for (int i = 0; i < 43; i++) diff |= (unsigned char)enc[i] ^ (unsigned char)d2[1 + i];
    if (diff) return 2;

    char hb[256], pb[1536];
    if ((size_t)(d1 - t) > sizeof hb * 3 / 4) return 2;
    if ((size_t)(d2 - d1 - 1) > sizeof pb * 3 / 4) return 2;
    long hl = b64url_decode(t, (size_t)(d1 - t), hb);
    if (hl < 0) return 2;
    jfield_t hf[1] = { { .name = "alg" } };
    if (json_fields(hb, (size_t)hl, hf, 1) != JT_OTHER || hf[0].type != JT_STRING ||
        hf[0].ve - hf[0].v != 5 || memcmp(hf[0].v, "HS256", 5))
        return 2;

    long pl = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), pb);
    if (pl < 0) return 2;
    jfield_t pf[4] = { { .name = "sub" }, { .name = "username" }, { .name = "exp" }, { .name = "nbf" } };
    if (json_fields(pb, (size_t)pl, pf, 4) != JT_OTHER) return 2;

    double exp, nbf;
    if (pf[2].type != JT_NUMBER || !json_number(&pf[2], &exp) || exp <= now) return 2;
    if (pf[3].type != JT_NONE && (pf[3].type != JT_NUMBER || !json_number(&pf[3], &nbf) || nbf > now))
        return 2;
    if (pf[0].type != JT_STRING || pf[1].type != JT_STRING) return 3;

    const char *s = pf[0].v, *se = pf[0].ve;
    if (s == se) return 3;
    int64_t uid = 0;
    for (; s < se; s++) {
        if (*s < '0' || *s > '9' || uid > (INT64_MAX - (*s - '0')) / 10) return 3;
        uid = uid * 10 + (*s - '0');
    }
    if (uid <= 0) return 3;
    a->uid = uid;

    size_t raw = (size_t)(pf[1].ve - pf[1].v);
    if (raw > sizeof a->name) return 3;
    size_t nl = j_unescape(pf[1].v, pf[1].ve, a->name);
    if (nl == (size_t)-1) return 2;
    a->name_len = nl;
    return 0;
}
