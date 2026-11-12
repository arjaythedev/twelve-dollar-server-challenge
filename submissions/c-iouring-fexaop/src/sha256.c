/* SHA-256, HMAC-SHA256 and a strict canonical base64url decoder. */
#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct sha256_ctx {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t buflen;
};

static void sha256_transform(struct sha256_ctx *c, const uint8_t *blk)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[4 * i] << 24) | ((uint32_t)blk[4 * i + 1] << 16) |
               ((uint32_t)blk[4 * i + 2] << 8) | (uint32_t)blk[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], hh = c->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}

static void sha256_init(struct sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu; c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->total = 0;
    c->buflen = 0;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += (uint64_t)n;

    if (c->buflen) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sha256_transform(c, c->buf);
            c->buflen = 0;
        }
    }
    while (n >= 64) {
        sha256_transform(c, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n;
    }
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total << 3;
    uint8_t pad[72];
    size_t rem = c->buflen;
    size_t padlen = (rem < 56) ? (56 - rem) : (120 - rem);

    memset(pad, 0, padlen);
    pad[0] = 0x80;
    sha256_update(c, pad, padlen);

    uint8_t lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lb, 8);

    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[32])
{
    uint8_t k[64];
    uint8_t pad[64];
    uint8_t inner[32];
    struct sha256_ctx c;

    memset(k, 0, sizeof k);
    if (keylen > sizeof k) {
        sha256(key, keylen, k);
    } else if (keylen) {
        memcpy(k, key, keylen);
    }

    sha256_init(&c);
    for (size_t i = 0; i < sizeof k; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    sha256_update(&c, pad, sizeof pad);
    sha256_update(&c, msg, msglen);
    sha256_final(&c, inner);

    sha256_init(&c);
    for (size_t i = 0; i < sizeof k; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    sha256_update(&c, pad, sizeof pad);
    sha256_update(&c, inner, sizeof inner);
    sha256_final(&c, out);
}

static int b64val(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int b64url_decode(const char *s, size_t n, uint8_t *out, size_t cap)
{
    size_t rem = n & 3u;
    if (rem == 1) return -1;

    size_t outlen = (n >> 2) * 3 + (rem == 2 ? 1u : rem == 3 ? 2u : 0u);
    if (outlen > cap) return -1;

    size_t o = 0, i = 0;
    while (i + 4 <= n) {
        int v0 = b64val((uint8_t)s[i]);
        int v1 = b64val((uint8_t)s[i + 1]);
        int v2 = b64val((uint8_t)s[i + 2]);
        int v3 = b64val((uint8_t)s[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return -1;
        uint32_t x = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                     ((uint32_t)v2 << 6) | (uint32_t)v3;
        out[o++] = (uint8_t)(x >> 16);
        out[o++] = (uint8_t)(x >> 8);
        out[o++] = (uint8_t)x;
        i += 4;
    }

    if (rem == 2) {
        int v0 = b64val((uint8_t)s[i]);
        int v1 = b64val((uint8_t)s[i + 1]);
        if (v0 < 0 || v1 < 0) return -1;
        if (v1 & 0x0f) return -1; /* non-zero trailing bits */
        out[o++] = (uint8_t)(((uint32_t)v0 << 2) | ((uint32_t)v1 >> 4));
    } else if (rem == 3) {
        int v0 = b64val((uint8_t)s[i]);
        int v1 = b64val((uint8_t)s[i + 1]);
        int v2 = b64val((uint8_t)s[i + 2]);
        if (v0 < 0 || v1 < 0 || v2 < 0) return -1;
        if (v2 & 0x03) return -1; /* non-zero trailing bits */
        out[o++] = (uint8_t)(((uint32_t)v0 << 2) | ((uint32_t)v1 >> 4));
        out[o++] = (uint8_t)(((uint32_t)(v1 & 0x0f) << 4) | ((uint32_t)v2 >> 2));
    }

    return (int)outlen;
}
