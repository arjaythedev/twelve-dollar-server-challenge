/* sha256.h: SHA-256 (FIPS 180-4), plain C. */
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <string.h>

struct sha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  unsigned n;
};

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_init(struct sha256 *s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv);
  s->len = 0;
  s->n = 0;
}

static void sha256_update(struct sha256 *s, const uint8_t *p, size_t n) {
  s->len += n;
  if (s->n) {
    size_t k = 64 - s->n;
    if (k > n) k = n;
    memcpy(s->buf + s->n, p, k);
    s->n += (unsigned)k;
    p += k;
    n -= k;
    if (s->n < 64) return;
    sha256_block(s->h, s->buf);
    s->n = 0;
  }
  while (n >= 64) {
    sha256_block(s->h, p);
    p += 64;
    n -= 64;
  }
  if (n) {
    memcpy(s->buf, p, n);
    s->n = (unsigned)n;
  }
}

static void sha256_final(struct sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8;
  s->buf[s->n++] = 0x80;
  if (s->n > 56) {
    memset(s->buf + s->n, 0, 64 - s->n);
    sha256_block(s->h, s->buf);
    s->n = 0;
  }
  memset(s->buf + s->n, 0, 56 - s->n);
  for (int i = 0; i < 8; i++) s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_block(s->h, s->buf);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(s->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)s->h[i];
  }
}

/* HMAC-SHA256 with the key's inner and outer states computed once (the key schedule, not a
 * verification result): each message costs its own blocks plus one. */
struct hmac_key {
  struct sha256 inner, outer; /* after absorbing the padded key block */
};

static void hmac_key_init(struct hmac_key *k, const uint8_t *key, size_t n) {
  uint8_t kb[64];
  memset(kb, 0, sizeof kb);
  if (n > 64) {
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, key, n);
    sha256_final(&s, kb);
  } else {
    memcpy(kb, key, n);
  }
  uint8_t pad[64];
  for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
  sha256_init(&k->inner);
  sha256_update(&k->inner, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
  sha256_init(&k->outer);
  sha256_update(&k->outer, pad, 64);
}

static void hmac_sha256(const struct hmac_key *k, const uint8_t *msg, size_t n, uint8_t out[32]) {
  struct sha256 s = k->inner;
  sha256_update(&s, msg, n);
  uint8_t ih[32];
  sha256_final(&s, ih);
  s = k->outer;
  sha256_update(&s, ih, 32);
  sha256_final(&s, out);
}

#endif
