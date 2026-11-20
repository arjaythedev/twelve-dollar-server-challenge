/* HS256 JWT verification: SHA-256, HMAC with precomputed key pads, base64url.
 *
 * Every token is fully verified on every request (signature, alg, exp); the
 * only thing computed once is the HMAC key schedule (the SHA-256 states after
 * absorbing key^ipad and key^opad), which depends only on JWT_SECRET. */
#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "json.h"

typedef struct { uint32_t h[8]; } sha256_state;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_state *s, const unsigned char *blk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)blk[i * 4] << 24 | (uint32_t)blk[i * 4 + 1] << 16 |
               (uint32_t)blk[i * 4 + 2] << 8 | (uint32_t)blk[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static const sha256_state SHA256_IV = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};

/* Finishes a hash whose state already absorbed `prefix_bytes` bytes (a multiple
 * of 64), continuing with msg[0..n). Writes the 32-byte digest. */
static void sha256_finish(sha256_state s, uint64_t prefix_bytes, const unsigned char *msg, size_t n,
                          unsigned char out[32]) {
    uint64_t total = prefix_bytes + n;
    while (n >= 64) {
        sha256_block(&s, msg);
        msg += 64;
        n -= 64;
    }
    unsigned char tail[128];
    memcpy(tail, msg, n);
    tail[n] = 0x80;
    size_t padlen = (n < 56) ? 64 : 128;
    memset(tail + n + 1, 0, padlen - n - 1);
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++) tail[padlen - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha256_block(&s, tail);
    if (padlen == 128) sha256_block(&s, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(s.h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s.h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s.h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s.h[i];
    }
}

typedef struct { sha256_state inner, outer; } hmac_key;

static void hmac_key_init(hmac_key *k, const unsigned char *key, size_t keylen) {
    unsigned char kb[64] = {0}, pad[64];
    if (keylen > 64) sha256_finish(SHA256_IV, 0, key, keylen, kb);
    else memcpy(kb, key, keylen);
    k->inner = SHA256_IV;
    k->outer = SHA256_IV;
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    sha256_block(&k->inner, pad);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
    sha256_block(&k->outer, pad);
}

static void hmac_sha256(const hmac_key *k, const unsigned char *msg, size_t n, unsigned char out[32]) {
    unsigned char ih[32];
    sha256_finish(k->inner, 64, msg, n, ih);
    sha256_finish(k->outer, 64, ih, 32, out);
}

/* base64url without padding. Returns decoded length or -1. */
static long b64url_decode(const char *s, size_t n, unsigned char *out) {
    static signed char T[256];
    static int init;
    if (!init) {
        memset(T, -1, sizeof T);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; i++) T[(unsigned char)a[i]] = (signed char)i;
        init = 1;
    }
    if (n % 4 == 1) return -1;
    size_t o = 0, i = 0;
    for (; i + 4 <= n; i += 4) {
        int a = T[(unsigned char)s[i]], b = T[(unsigned char)s[i + 1]];
        int c = T[(unsigned char)s[i + 2]], d = T[(unsigned char)s[i + 3]];
        if ((a | b | c | d) < 0) return -1;
        uint32_t v = (uint32_t)(a << 18 | b << 12 | c << 6 | d);
        out[o++] = (unsigned char)(v >> 16);
        out[o++] = (unsigned char)(v >> 8);
        out[o++] = (unsigned char)v;
    }
    size_t rem = n - i;
    if (rem >= 2) {
        int a = T[(unsigned char)s[i]], b = T[(unsigned char)s[i + 1]];
        int c = rem == 3 ? T[(unsigned char)s[i + 2]] : 0;
        if ((a | b | c) < 0) return -1;
        uint32_t v = (uint32_t)(a << 18 | b << 12 | c << 6);
        out[o++] = (unsigned char)(v >> 16);
        if (rem == 3) out[o++] = (unsigned char)(v >> 8);
    }
    return (long)o;
}

enum { JWT_OK = 0, JWT_INVALID = 1, JWT_BAD_PAYLOAD = 2 };

#define JWT_MAX_USERNAME 1024

typedef struct {
    int64_t uid;
    size_t uname_len;
    char uname[JWT_MAX_USERNAME];
} jwt_claims;

struct jwt_hdr_ctx { int alg_ok; int alg_seen; };
static void jwt_hdr_cb(void *c, const char *k, size_t kl, const char *v, const char *ve) {
    struct jwt_hdr_ctx *x = c;
    if (kl == 3 && memcmp(k, "alg", 3) == 0) {
        x->alg_seen = 1;
        char buf[16];
        size_t n;
        x->alg_ok = (ve - v) <= (long)sizeof buf && json_string(v, ve, buf, &n) && n == 5 &&
                    memcmp(buf, "HS256", 5) == 0;
    }
}

struct jwt_pl_ctx {
    const char *sub, *sube;   /* raw value spans */
    const char *name, *namee;
    const char *exp, *expe;
};
static void jwt_pl_cb(void *c, const char *k, size_t kl, const char *v, const char *ve) {
    struct jwt_pl_ctx *x = c;
    if (kl == 3 && memcmp(k, "sub", 3) == 0) { x->sub = v; x->sube = ve; }
    else if (kl == 8 && memcmp(k, "username", 8) == 0) { x->name = v; x->namee = ve; }
    else if (kl == 3 && memcmp(k, "exp", 3) == 0) { x->exp = v; x->expe = ve; }
}

/* Verifies tok[0..n) at time `now` (unix seconds). */
static int jwt_verify(const hmac_key *key, const char *tok, size_t n, int64_t now, jwt_claims *out) {
    const char *d1 = memchr(tok, '.', n);
    if (!d1) return JWT_INVALID;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(tok + n - d1 - 1));
    if (!d2 || memchr(d2 + 1, '.', (size_t)(tok + n - d2 - 1))) return JWT_INVALID;
    size_t hl = (size_t)(d1 - tok), pl = (size_t)(d2 - d1 - 1), sl = (size_t)(tok + n - d2 - 1);
    if (hl == 0 || pl == 0 || sl != 43 || hl > 512 || pl > 4096) return JWT_INVALID;

    unsigned char sig[33], mac[32];
    if (b64url_decode(d2 + 1, sl, sig) != 32) return JWT_INVALID;

    unsigned char hdr[384];
    long hn = b64url_decode(tok, hl, hdr);
    if (hn <= 0) return JWT_INVALID;
    struct jwt_hdr_ctx hc = {0, 0};
    if (json_object_each((const char *)hdr, (const char *)hdr + hn, jwt_hdr_cb, &hc) != 0 || !hc.alg_ok)
        return JWT_INVALID;

    hmac_sha256(key, (const unsigned char *)tok, (size_t)(d2 - tok), mac);
    unsigned diff = 0;
    for (int i = 0; i < 32; i++) diff |= (unsigned)(mac[i] ^ sig[i]);
    if (diff) return JWT_INVALID;

    unsigned char pld[3072];
    long pn = b64url_decode(d1 + 1, pl, pld);
    if (pn <= 0) return JWT_INVALID;
    struct jwt_pl_ctx pc = {0};
    if (json_object_each((const char *)pld, (const char *)pld + pn, jwt_pl_cb, &pc) != 0)
        return JWT_INVALID;

    /* exp: required, a JSON number, strictly in the future */
    if (!pc.exp || !json_number(pc.exp, pc.expe) || json_number(pc.exp, pc.expe) != pc.expe)
        return JWT_INVALID;
    char numbuf[64];
    size_t el = (size_t)(pc.expe - pc.exp);
    if (el >= sizeof numbuf) return JWT_INVALID;
    memcpy(numbuf, pc.exp, el);
    numbuf[el] = 0;
    double exp = strtod(numbuf, NULL);
    if (!(exp > (double)now)) return JWT_INVALID;

    /* sub: a string holding a positive integer */
    if (!pc.sub || *pc.sub != '"') return JWT_BAD_PAYLOAD;
    char subbuf[32];
    size_t subl;
    if (pc.sube - pc.sub > (long)sizeof subbuf || !json_string(pc.sub, pc.sube, subbuf, &subl))
        return JWT_BAD_PAYLOAD;
    if (subl == 0 || subl > 18) return JWT_BAD_PAYLOAD;
    int64_t uid = 0;
    for (size_t i = 0; i < subl; i++) {
        if (subbuf[i] < '0' || subbuf[i] > '9') return JWT_BAD_PAYLOAD;
        uid = uid * 10 + (subbuf[i] - '0');
    }
    if (uid <= 0) return JWT_BAD_PAYLOAD;

    /* username: a string, used as-is */
    if (!pc.name || *pc.name != '"') return JWT_BAD_PAYLOAD;
    if (pc.namee - pc.name > JWT_MAX_USERNAME) return JWT_BAD_PAYLOAD;
    if (!json_string(pc.name, pc.namee, out->uname, &out->uname_len)) return JWT_BAD_PAYLOAD;
    out->uid = uid;
    return JWT_OK;
}

#endif
