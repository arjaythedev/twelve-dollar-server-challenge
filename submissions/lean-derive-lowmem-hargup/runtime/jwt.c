#include "rt.h"

/* HS256 JWT verification: SHA-256 (FIPS 180-4), HMAC with the key pads absorbed once at
 * startup (the key is fixed for the process; every token is still fully verified), base64url,
 * and claim checks. */

typedef struct { u32 h[8]; } sha256_state;

static const u32 K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline u32 ror32(u32 x, int n) { return x >> n | x << (32 - n); }

static void sha256_block(sha256_state *s, const u8 *p) {
  u32 w[64];
  for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    u32 t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    u32 t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static const u32 SHA256_IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

/* Hash `prefix_blocks` 64-byte blocks already absorbed into `s`, followed by msg. */
static void sha256_finish(sha256_state *s, u64 prefix_bytes, const u8 *msg, size_t n, u8 out[32]) {
  size_t full = n & ~(size_t)63;
  for (size_t i = 0; i < full; i += 64) sha256_block(s, msg + i);
  u8 tail[128];
  size_t r = n - full;
  memcpy(tail, msg + full, r);
  tail[r] = 0x80;
  size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
  memset(tail + r + 1, 0, tl - r - 1);
  u64 bits = (prefix_bytes + n) * 8;
  for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (u8)(bits >> (8 * i));
  sha256_block(s, tail);
  if (tl == 128) sha256_block(s, tail + 64);
  for (int i = 0; i < 8; i++) { out[4 * i] = (u8)(s->h[i] >> 24); out[4 * i + 1] = (u8)(s->h[i] >> 16); out[4 * i + 2] = (u8)(s->h[i] >> 8); out[4 * i + 3] = (u8)s->h[i]; }
}

static sha256_state hmac_inner, hmac_outer;   /* states after absorbing key^ipad / key^opad */

static void hmac_init(const u8 *key, size_t n) {
  u8 k[64] = {0};
  if (n > 64) { sha256_state s; memcpy(s.h, SHA256_IV, sizeof s.h); sha256_finish(&s, 0, key, n, k); }
  else memcpy(k, key, n);
  u8 pad[64];
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  memcpy(hmac_inner.h, SHA256_IV, sizeof SHA256_IV);
  sha256_block(&hmac_inner, pad);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  memcpy(hmac_outer.h, SHA256_IV, sizeof SHA256_IV);
  sha256_block(&hmac_outer, pad);
}
static void hmac_sha256(const u8 *msg, size_t n, u8 out[32]) {
  u8 ih[32];
  sha256_state s = hmac_inner;
  sha256_finish(&s, 64, msg, n, ih);
  s = hmac_outer;
  sha256_finish(&s, 64, ih, 32, out);
}

/* base64url without padding. Returns decoded length or -1. */
static int b64url_decode(const u8 *s, u32 n, u8 *out, u32 cap) {
  static i64 tab[256];
  static int ready;
  if (!ready) {
    for (int i = 0; i < 256; i++) tab[i] = -1;
    const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; i++) tab[(u8)a[i]] = i;
    ready = 1;
  }
  if (n % 4 == 1) return -1;
  u32 o = 0, acc = 0, bits = 0;
  for (u32 i = 0; i < n; i++) {
    i64 v = tab[s[i]];
    if (v < 0) return -1;
    acc = acc << 6 | (u32)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (u8)(acc >> bits);
    }
  }
  if (bits && (acc & ((1u << bits) - 1))) return -1;   /* non-canonical trailing bits */
  return (int)o;
}

enum { JWT_OK = 0, JWT_INVALID = 1, JWT_PAYLOAD = 2 };

typedef struct {
  i64 uid;
  u8 decoded[2048];           /* verified JSON scratch, valid only during request decoding */
  const u8 *raw_name; u32 raw_name_len;
  u8 name_escaped;
} claims_t;

/* Verify "Bearer <jwt>" token bytes (without the "Bearer " prefix). */
static int jwt_verify(const u8 *t, u32 n, i64 now, claims_t *c) {
  const u8 *d1 = memchr(t, '.', n);
  if (!d1) return JWT_INVALID;
  const u8 *d2 = memchr(d1 + 1, '.', (size_t)(t + n - d1 - 1));
  if (!d2 || memchr(d2 + 1, '.', (size_t)(t + n - d2 - 1))) return JWT_INVALID;
  u32 hl = (u32)(d1 - t), pl = (u32)(d2 - d1 - 1), sl = (u32)(t + n - d2 - 1);
  if (!hl || !pl || sl != 43 || hl > 512 || pl > 2048) return JWT_INVALID;
  u8 sig[33], mac[32];
  if (b64url_decode(d2 + 1, sl, sig, sizeof sig) != 32) return JWT_INVALID;
  hmac_sha256(t, (size_t)(d2 - t), mac);
  u8 diff = 0;
  for (int i = 0; i < 32; i++) diff |= (u8)(sig[i] ^ mac[i]);
  if (diff) return JWT_INVALID;
  u8 *buf = c->decoded;
  int hn = b64url_decode(t, hl, buf, sizeof c->decoded);
  if (hn < 0) return JWT_INVALID;
  jmember hm[1] = {{"alg", 3, 0, 0, 0, 0}};
  if (json_members(buf, (u32)hn, hm, 1) != 0) return JWT_INVALID;
  if (hm[0].type != JT_STRING || hm[0].escaped || hm[0].v_len != 5 || memcmp(hm[0].v, "HS256", 5)) return JWT_INVALID;
  int pn = b64url_decode(d1 + 1, pl, buf, sizeof c->decoded);
  if (pn < 0) return JWT_INVALID;
  jmember pm[3] = {{"exp", 3, 0, 0, 0, 0}, {"sub", 3, 0, 0, 0, 0}, {"username", 8, 0, 0, 0, 0}};
  if (json_members(buf, (u32)pn, pm, 3) != 0) return JWT_INVALID;
  /* exp: a JSON number; the token is expired when exp <= now (seconds) */
  if (pm[0].type != JT_NUMBER) return JWT_INVALID;
  {
    const u8 *p = pm[0].v, *e = p + pm[0].v_len;
    if (*p == '-') return JWT_INVALID;
    i64 v = 0;
    while (p < e && *p >= '0' && *p <= '9') { if (v > (i64)1e15) return JWT_INVALID; v = v * 10 + (*p++ - '0'); }
    int frac = 0;
    if (p < e && *p == '.') { p++; while (p < e && *p >= '0' && *p <= '9') { if (*p != '0') frac = 1; p++; } }
    if (p < e) {   /* exponent form: evaluate conservatively with strtod */
      char tmp[64]; u32 l = pm[0].v_len < 63 ? pm[0].v_len : 63;
      memcpy(tmp, pm[0].v, l); tmp[l] = 0;
      double dv = strtod(tmp, 0);
      if (!(dv > (double)now)) return JWT_INVALID;
    } else if (!(v > now || (v == now && frac))) return JWT_INVALID;
  }
  /* sub: a positive integer string */
  if (pm[1].type != JT_STRING || pm[1].escaped || pm[1].v_len == 0 || pm[1].v_len > 15) return JWT_PAYLOAD;
  i64 uid = 0;
  for (u32 i = 0; i < pm[1].v_len; i++) {
    u8 ch = pm[1].v[i];
    if (ch < '0' || ch > '9') return JWT_PAYLOAD;
    uid = uid * 10 + (ch - '0');
  }
  if (uid <= 0) return JWT_PAYLOAD;
  if (pm[2].type != JT_STRING || pm[2].v_len > 512) return JWT_PAYLOAD;
  c->uid = uid;
  c->raw_name = pm[2].v; c->raw_name_len = pm[2].v_len; c->name_escaped = pm[2].escaped;
  return JWT_OK;
}
