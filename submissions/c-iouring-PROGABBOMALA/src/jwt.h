#ifndef JWT_H
#define JWT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct sha256_ctx {
  uint32_t state[8];
  uint64_t count;
  uint8_t buffer[64];
  unsigned buf_len;
};

static const uint32_t K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR256(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
  uint32_t W[64];
  for (int i = 0; i < 16; i++) {
    W[i] = ((uint32_t)data[i * 4] << 24) |
           ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) |
           ((uint32_t)data[i * 4 + 3]);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR256(W[i - 15], 7) ^ ROR256(W[i - 15], 18) ^ (W[i - 15] >> 3);
    uint32_t s1 = ROR256(W[i - 2], 17) ^ ROR256(W[i - 2], 19) ^ (W[i - 2] >> 10);
    W[i] = W[i - 16] + s0 + W[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
#pragma GCC unroll 1
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROR256(e, 6) ^ ROR256(e, 11) ^ ROR256(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + S1 + ch + K256[i] + W[i];
    uint32_t S0 = ROR256(a, 2) ^ ROR256(a, 13) ^ ROR256(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = S0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static inline void sha256_init(struct sha256_ctx *ctx) {
  static const uint32_t H256_IV[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };
  memcpy(ctx->state, H256_IV, sizeof(ctx->state));
  ctx->count = 0;
  ctx->buf_len = 0;
}

static inline void sha256_update(struct sha256_ctx *ctx, const uint8_t *p, size_t len) {
  ctx->count += len;
  if (ctx->buf_len > 0) {
    unsigned need = 64 - ctx->buf_len;
    if (need > len) need = (unsigned)len;
    memcpy(ctx->buffer + ctx->buf_len, p, need);
    ctx->buf_len += need;
    p += need;
    len -= need;
    if (ctx->buf_len == 64) {
      sha256_transform(ctx->state, ctx->buffer);
      ctx->buf_len = 0;
    }
  }
  while (len >= 64) {
    sha256_transform(ctx->state, p);
    p += 64;
    len -= 64;
  }
  if (len > 0) {
    memcpy(ctx->buffer, p, len);
    ctx->buf_len = (unsigned)len;
  }
}

static inline void sha256_final(struct sha256_ctx *ctx, uint8_t out[32]) {
  uint64_t total_bits = ctx->count * 8;
  uint8_t pad = 0x80;
  sha256_update(ctx, &pad, 1);
  uint8_t zero[64] = {0};
  if (ctx->buf_len > 56) {
    sha256_update(ctx, zero, 64 - ctx->buf_len);
  }
  sha256_update(ctx, zero, 56 - ctx->buf_len);
  uint8_t len_bytes[8];
  for (int i = 0; i < 8; i++) {
    len_bytes[i] = (uint8_t)(total_bits >> (56 - 8 * i));
  }
  sha256_update(ctx, len_bytes, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
  }
}

struct jwt_hmac_state {
  struct sha256_ctx inner;
  struct sha256_ctx outer;
};

static void jwt_hmac_init(struct jwt_hmac_state *h, const char *secret) {
  size_t klen = strlen(secret);
  uint8_t key[64] = {0};
  if (klen > 64) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)secret, klen);
    sha256_final(&c, key);
  } else {
    memcpy(key, secret, klen);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = key[i] ^ 0x36;
    opad[i] = key[i] ^ 0x5c;
  }
  sha256_init(&h->inner);
  sha256_update(&h->inner, ipad, 64);
  sha256_init(&h->outer);
  sha256_update(&h->outer, opad, 64);
}

static inline void jwt_hmac_compute(const struct jwt_hmac_state *h, const uint8_t *msg, size_t mlen, uint8_t digest[32]) {
  struct sha256_ctx c = h->inner;
  sha256_update(&c, msg, mlen);
  uint8_t inner_hash[32];
  sha256_final(&c, inner_hash);
  c = h->outer;
  sha256_update(&c, inner_hash, 32);
  sha256_final(&c, digest);
}

static const int8_t B64_LUT[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static int b64url_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
  if ((n & 3) == 1 || (n * 3) / 4 > cap) return -1;
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int8_t v = B64_LUT[(uint8_t)in[i]];
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  return (int)o;
}

static void b64url_encode_32(const uint8_t in[32], char out[43]) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (int i = 0; i < 30; i += 3) {
    uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
    out[o++] = A[(n >> 18) & 63];
    out[o++] = A[(n >> 12) & 63];
    out[o++] = A[(n >> 6) & 63];
    out[o++] = A[n & 63];
  }
  uint32_t n = ((uint32_t)in[30] << 16) | ((uint32_t)in[31] << 8);
  out[o++] = A[(n >> 18) & 63];
  out[o++] = A[(n >> 12) & 63];
  out[o++] = A[(n >> 6) & 63];
}

#define JWT_WORK 4096 /* most caller memory a verification ever needs */

enum { JWT_OK = 0, JWT_MISSING = -1, JWT_INVALID = -2, JWT_PAYLOAD = -3 };

struct jwt_claims {
  int64_t uid;
  const uint8_t *name; /* decoded username, inside the caller's work buffer */
  uint32_t name_len;
};

/* All digits, at most 18 of them, value above zero. */
static int jwt_positive_int(const uint8_t *s, uint32_t n, int64_t *out) {
  if (n == 0 || n > 18) return 0;
  int64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    unsigned d = (unsigned)(s[i] - '0');
    if (d > 9) return 0;
    v = v * 10 + (int64_t)d;
  }
  if (v <= 0) return 0;
  *out = v;
  return 1;
}

static int jwt_verify(const struct jwt_hmac_state *h, const char *auth, size_t auth_len,
                      int64_t now_s, struct jwt_claims *out, uint8_t *work, uint32_t cap) {
  if (!auth || auth_len < 7 || memcmp(auth, "Bearer ", 7) != 0) return JWT_MISSING;
  const char *tok = auth + 7;
  size_t n = auth_len - 7;

  const char *d1 = memchr(tok, '.', n);
  if (!d1) return JWT_INVALID;
  const char *d2 = memchr(d1 + 1, '.', (size_t)(tok + n - d1 - 1));
  if (!d2) return JWT_INVALID;
  const char *sig = d2 + 1;
  size_t sig_len = (size_t)(tok + n - sig);
  if (sig_len != 43) return JWT_INVALID; /* also rejects a fourth segment and alg=none */

  uint8_t mac[32];
  char want[43];
  jwt_hmac_compute(h, (const uint8_t *)tok, (size_t)(d2 - tok), mac);
  b64url_encode_32(mac, want);
  if (memcmp(sig, want, 43) != 0) return JWT_INVALID;

  /* header: {"alg":"HS256", ...} */
  int hn = b64url_decode(tok, (size_t)(d1 - tok), work, cap);
  if (hn <= 0) return JWT_INVALID;
  struct jmember hm[1] = {{"alg", 3, 0, 0, 0, 0}};
  if (json_object(work, (uint32_t)hn, hm, 1) != 0) return JWT_INVALID;
  if (hm[0].type != JV_STRING || hm[0].vlen != 5 || memcmp(hm[0].v, "HS256", 5) != 0) return JWT_INVALID;

  int pn = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), work, cap);
  if (pn <= 0) return JWT_INVALID;
  struct jmember pm[3] = {{"exp", 3, 0, 0, 0, 0}, {"sub", 3, 0, 0, 0, 0}, {"username", 8, 0, 0, 0, 0}};
  if (json_object(work, (uint32_t)pn, pm, 3) != 0) return JWT_INVALID;

  /* A token without a numeric exp is rejected. */
  if (pm[0].type != JV_NUMBER || pm[0].vlen >= 40) return JWT_INVALID;
  char num[40];
  memcpy(num, pm[0].v, pm[0].vlen);
  num[pm[0].vlen] = 0;
  if (!(strtod(num, 0) > (double)now_s)) return JWT_INVALID;

  if (pm[1].type == JV_STRING) {
    if (pm[1].esc || !jwt_positive_int(pm[1].v, pm[1].vlen, &out->uid)) return JWT_PAYLOAD;
  } else if (pm[1].type == JV_NUMBER) {
    if (!jwt_positive_int(pm[1].v, pm[1].vlen, &out->uid)) return JWT_PAYLOAD;
  } else {
    return JWT_PAYLOAD;
  }

  if (pm[2].type != JV_STRING) return JWT_PAYLOAD;
  uint8_t *name = (uint8_t *)(uintptr_t)pm[2].v;
  out->name = name;
  out->name_len = pm[2].esc ? json_unescape(name, pm[2].vlen, name) : pm[2].vlen;
  return JWT_OK;
}

#endif
