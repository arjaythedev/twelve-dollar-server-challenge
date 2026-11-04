/* Byte-level helpers the Swift side calls: the JSON string escaper (SSE2 baseline, AVX2 when built
 * with it, scalar elsewhere), a strict UTF-8 check, and HMAC-SHA256 with pre-hashed key pads. */
#include "shim.h"
#include <stdint.h>
#include <string.h>
#if defined(__SSE2__)
#include <immintrin.h>
#endif

typedef unsigned char u8;

/* ---- escaping ---- */

static const u8 ESC[256] = {
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  [0x22] = 1, [0x5c] = 1,
};

static inline u8 *esc_byte(u8 *o, u8 c) {
  static const char H[] = "0123456789abcdef";
  if (!ESC[c]) { *o++ = c; return o; }
  *o++ = '\\';
  switch (c) {
  case '"': *o++ = '"'; break;
  case '\\': *o++ = '\\'; break;
  case '\n': *o++ = 'n'; break;
  case '\r': *o++ = 'r'; break;
  case '\t': *o++ = 't'; break;
  case 8: *o++ = 'b'; break;
  case 12: *o++ = 'f'; break;
  default: *o++ = 'u'; *o++ = '0'; *o++ = '0'; *o++ = H[c >> 4]; *o++ = H[c & 15];
  }
  return o;
}

size_t td_esc(const u8 *s, size_t n, u8 *d) {
  u8 *o = d;
  size_t i = 0;
#if defined(__AVX2__)
  while (i + 32 <= n) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
    _mm256_storeu_si256((__m256i *)o, v);
    __m256i ctl = _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8(0x1f)), v);
    __m256i q = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'));
    __m256i b = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'));
    unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_or_si256(ctl, _mm256_or_si256(q, b)));
    if (m == 0) { o += 32; i += 32; continue; }
    unsigned k = (unsigned)__builtin_ctz(m);
    o += k;
    for (size_t j = i + k; j < i + 32; j++) o = esc_byte(o, s[j]);
    i += 32;
  }
#endif
#if defined(__SSE2__)
  while (i + 16 <= n) {
    __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
    _mm_storeu_si128((__m128i *)o, v);
    __m128i ctl = _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1f)), v);
    __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
    __m128i b = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
    unsigned m = (unsigned)_mm_movemask_epi8(_mm_or_si128(ctl, _mm_or_si128(q, b)));
    if (m == 0) { o += 16; i += 16; continue; }
    unsigned k = (unsigned)__builtin_ctz(m);
    o += k;
    for (size_t j = i + k; j < i + 16; j++) o = esc_byte(o, s[j]);
    i += 16;
  }
#endif
  /* Eight bytes at a time while nothing needs escaping, then byte by byte. */
  while (i + 8 <= n) {
    uint64_t w;
    memcpy(&w, s + i, 8);
    const uint64_t ONES = 0x0101010101010101ull, HIGH = 0x8080808080808080ull;
    uint64_t ctl = (w - ONES * 0x20) & ~w;
    uint64_t q = w ^ (ONES * '"');
    q = (q - ONES) & ~q;
    uint64_t b = w ^ (ONES * '\\');
    b = (b - ONES) & ~b;
    if ((ctl | q | b) & HIGH) {
      for (size_t j = i; j < i + 8; j++) o = esc_byte(o, s[j]);
    } else {
      memcpy(o, &w, 8);
      o += 8;
    }
    i += 8;
  }
  for (; i < n; i++) o = esc_byte(o, s[i]);
  return (size_t)(o - d);
}

/* ---- UTF-8 ---- */

int td_utf8_valid(const u8 *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    u8 c = s[i];
    if (c < 0x80) { i++; continue; }
    size_t k;
    uint32_t cp;
    if (c >= 0xc2 && c <= 0xdf) { k = 1; cp = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { k = 2; cp = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { k = 3; cp = c & 0x07; }
    else return 0;
    if (i + k >= n) return 0;
    for (size_t j = 1; j <= k; j++) {
      if ((s[i + j] & 0xc0) != 0x80) return 0;
      cp = cp << 6 | (s[i + j] & 0x3f);
    }
    if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff))) return 0;
    if (cp >= 0xd800 && cp <= 0xdfff) return 0;
    i += k + 1;
  }
  return 1;
}

/* ---- SHA-256 / HMAC ---- */

typedef struct {
  uint32_t h[8];
  u8 buf[64];
  uint64_t total;
  unsigned fill;
} sha_t;

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

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t *h, const u8 *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K[i] + w[i];
    uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha_init(sha_t *s) {
  static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, IV, sizeof IV);
  s->total = 0;
  s->fill = 0;
}

static void sha_update(sha_t *s, const u8 *p, size_t n) {
  s->total += n;
  if (s->fill) {
    size_t take = 64 - s->fill < n ? 64 - s->fill : n;
    memcpy(s->buf + s->fill, p, take);
    s->fill += (unsigned)take;
    p += take;
    n -= take;
    if (s->fill < 64) return;
    sha_block(s->h, s->buf);
    s->fill = 0;
  }
  for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
  if (n) { memcpy(s->buf, p, n); s->fill = (unsigned)n; }
}

static void sha_final(sha_t *s, u8 out[32]) {
  uint64_t bits = s->total * 8;
  u8 pad[72] = {0x80};
  size_t padn = (s->fill < 56 ? 56 : 120) - s->fill;
  for (int i = 0; i < 8; i++) pad[padn + i] = (u8)(bits >> (56 - 8 * i));
  sha_update(s, pad, padn + 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (u8)(s->h[i] >> 24);
    out[4 * i + 1] = (u8)(s->h[i] >> 16);
    out[4 * i + 2] = (u8)(s->h[i] >> 8);
    out[4 * i + 3] = (u8)s->h[i];
  }
}

static sha_t inner0, outer0;

void td_hmac_init(const u8 *secret, size_t n) {
  u8 key[64] = {0};
  if (n > 64) {
    sha_t s;
    sha_init(&s);
    sha_update(&s, secret, n);
    sha_final(&s, key);
  } else {
    memcpy(key, secret, n);
  }
  u8 ipad[64], opad[64];
  for (int i = 0; i < 64; i++) { ipad[i] = key[i] ^ 0x36; opad[i] = key[i] ^ 0x5c; }
  sha_init(&inner0);
  sha_update(&inner0, ipad, 64);
  sha_init(&outer0);
  sha_update(&outer0, opad, 64);
}

void td_hmac(const u8 *msg, size_t n, u8 out[32]) {
  sha_t s = inner0;
  u8 ih[32];
  sha_update(&s, msg, n);
  sha_final(&s, ih);
  s = outer0;
  sha_update(&s, ih, 32);
  sha_final(&s, out);
}
