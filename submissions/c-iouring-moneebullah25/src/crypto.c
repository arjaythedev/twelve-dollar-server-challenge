#include "crypto.h"

#include <string.h>

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

static inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static inline uint32_t load_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static inline void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load_be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void sha256_init(struct sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->buf_len = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->len += len;
    if (s->buf_len) {
        size_t take = SHA256_BLOCK - s->buf_len;
        if (take > len) take = len;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += (uint32_t)take;
        p += take;
        len -= take;
        if (s->buf_len < SHA256_BLOCK) return;
        sha256_block(s->h, s->buf);
        s->buf_len = 0;
    }
    while (len >= SHA256_BLOCK) {
        sha256_block(s->h, p);
        p += SHA256_BLOCK;
        len -= SHA256_BLOCK;
    }
    if (len) {
        memcpy(s->buf, p, len);
        s->buf_len = (uint32_t)len;
    }
}

void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]) {
    uint64_t bits = s->len * 8;
    uint8_t pad[SHA256_BLOCK + 8] = {0x80};
    /* pad to 56 mod 64, then the 64-bit big-endian bit length */
    size_t pad_len = (s->buf_len < 56) ? 56 - s->buf_len : 120 - s->buf_len;
    for (int i = 0; i < 8; i++) pad[pad_len + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, pad, pad_len + 8);
    for (int i = 0; i < 8; i++) store_be32(out + 4 * i, s->h[i]);
}

void hmac_key_init(struct hmac_key *k, const void *key, size_t key_len) {
    uint8_t kb[SHA256_BLOCK] = {0};
    if (key_len > SHA256_BLOCK) {
        struct sha256 s;
        sha256_init(&s);
        sha256_update(&s, key, key_len);
        sha256_final(&s, kb);
    } else {
        memcpy(kb, key, key_len);
    }
    uint8_t pad[SHA256_BLOCK];
    for (int i = 0; i < SHA256_BLOCK; i++) pad[i] = kb[i] ^ 0x36;
    sha256_init(&k->inner);
    sha256_update(&k->inner, pad, SHA256_BLOCK);
    for (int i = 0; i < SHA256_BLOCK; i++) pad[i] = kb[i] ^ 0x5c;
    sha256_init(&k->outer);
    sha256_update(&k->outer, pad, SHA256_BLOCK);
}

void hmac_sha256(const struct hmac_key *k, const void *msg, size_t msg_len, uint8_t out[SHA256_DIGEST]) {
    struct sha256 s = k->inner;
    uint8_t ih[SHA256_DIGEST];
    sha256_update(&s, msg, msg_len);
    sha256_final(&s, ih);
    s = k->outer;
    sha256_update(&s, ih, SHA256_DIGEST);
    sha256_final(&s, out);
}

int crypto_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int base64url_decode(const char *in, size_t len, uint8_t *out, size_t out_cap) {
    if (len % 4 == 1) return -1; /* no valid encoding has this length */
    size_t need = (len / 4) * 3 + ((len % 4) ? (len % 4) - 1 : 0);
    if (need > out_cap) return -1;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64val((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    /* only canonical encodings: the leftover bits must be zero */
    if (bits && (acc & ((1u << bits) - 1))) return -1;
    return (int)o;
}

size_t base64url_encode(const uint8_t *in, size_t len, char *out) {
    size_t o = 0, i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
    }
    return o;
}
