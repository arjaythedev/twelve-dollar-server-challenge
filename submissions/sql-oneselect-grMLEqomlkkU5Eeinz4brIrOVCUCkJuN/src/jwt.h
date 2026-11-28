/* HS256 JWT verification: SHA-256, HMAC with the key pads precomputed once, base64url, and a small
 * reader for the flat JSON objects inside a token. Every token is verified in full on every call. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- SHA-256 (FIPS 180-4) ---- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha256;

static const uint32_t K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(sha256 *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha256_init(sha256 *s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv);
  s->len = 0; s->n = 0;
}
static void sha256_update(sha256 *s, const void *data, size_t len) {
  const uint8_t *p = data;
  s->len += len;
  if (s->n) {
    while (len && s->n < 64) { s->buf[s->n++] = *p++; len--; }
    if (s->n < 64) return;
    sha256_block(s, s->buf); s->n = 0;
  }
  for (; len >= 64; p += 64, len -= 64) sha256_block(s, p);
  memcpy(s->buf, p, len); s->n = (uint32_t)len;
}
static void sha256_final(sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8;
  uint8_t pad = 0x80, zero = 0;
  sha256_update(s, &pad, 1);
  while (s->n != 56) sha256_update(s, &zero, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(s, l, 8);
  for (int i = 0; i < 8; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
}

/* HMAC key schedule: the hash states after the inner and outer key pads (this is the key, not a token cache). */
static sha256 HMAC_INNER, HMAC_OUTER;
static void hmac_init(const char *key) {
  uint8_t k[64] = {0}, pad[64];
  size_t kl = strlen(key);
  if (kl > 64) { sha256 s; sha256_init(&s); sha256_update(&s, key, kl); sha256_final(&s, k); }
  else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha256_init(&HMAC_INNER); sha256_update(&HMAC_INNER, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha256_init(&HMAC_OUTER); sha256_update(&HMAC_OUTER, pad, 64);
}
static void hmac(const void *msg, size_t len, uint8_t out[32]) {
  sha256 s = HMAC_INNER;
  sha256_update(&s, msg, len);
  uint8_t inner[32];
  sha256_final(&s, inner);
  s = HMAC_OUTER;
  sha256_update(&s, inner, 32);
  sha256_final(&s, out);
}

/* ---- base64url (no padding) ---- */
static int b64url_val(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}
/* Decodes into out (room for len*3/4 bytes); returns the length, or -1 on a bad character or length. */
static int b64url_decode(const char *in, int len, uint8_t *out) {
  if (len % 4 == 1) return -1;
  int o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (int i = 0; i < len; i++) {
    int v = b64url_val((uint8_t)in[i]);
    if (v < 0) return -1;
    acc = acc << 6 | (uint32_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
  }
  return o;
}
static void b64url_encode32(const uint8_t in[32], char out[43]) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  int o = 0, i = 0;
  for (; i + 3 <= 30; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = A[v >> 18]; out[o++] = A[v >> 12 & 63]; out[o++] = A[v >> 6 & 63]; out[o++] = A[v & 63];
  }
  uint32_t v = (uint32_t)in[30] << 16 | (uint32_t)in[31] << 8;  /* last 2 bytes -> 3 chars */
  out[o++] = A[v >> 18]; out[o++] = A[v >> 12 & 63]; out[o++] = A[v >> 6 & 63];
}

/* ---- a reader for one flat JSON object: finds a top-level key, skips everything else ---- */
typedef struct { const char *p, *end; } jcur;
static void jws(jcur *c) { while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++; }
/* Skips one JSON string starting at '"'; returns 0 on malformed input. */
static int jskip_string(jcur *c) {
  if (c->p >= c->end || *c->p != '"') return 0;
  for (c->p++; c->p < c->end; c->p++) {
    if (*c->p == '\\') { if (++c->p >= c->end) return 0; }
    else if (*c->p == '"') { c->p++; return 1; }
    else if ((uint8_t)*c->p < 0x20) return 0;
  }
  return 0;
}
static int jskip_value(jcur *c, int depth) {
  jws(c);
  if (c->p >= c->end || depth > 32) return 0;
  char ch = *c->p;
  if (ch == '"') return jskip_string(c);
  if (ch == '{' || ch == '[') {
    char close = ch == '{' ? '}' : ']';
    c->p++; jws(c);
    if (c->p < c->end && *c->p == close) { c->p++; return 1; }
    for (;;) {
      if (ch == '{') { jws(c); if (!jskip_string(c)) return 0; jws(c); if (c->p >= c->end || *c->p++ != ':') return 0; }
      if (!jskip_value(c, depth + 1)) return 0;
      jws(c);
      if (c->p >= c->end) return 0;
      if (*c->p == ',') { c->p++; continue; }
      if (*c->p == close) { c->p++; return 1; }
      return 0;
    }
  }
  const char *s = c->p;
  while (c->p < c->end && (strchr("+-.0123456789eE", *c->p) || (*c->p >= 'a' && *c->p <= 'z'))) c->p++;
  return c->p > s;
}
/* Finds key in the object [p, end): *vs, *ve = the raw value (strings keep their quotes). 1 found, 0 absent, -1 malformed. */
static int jget(const char *p, const char *end, const char *key, const char **vs, const char **ve) {
  jcur c = {p, end};
  size_t kl = strlen(key);
  int found = 0;
  jws(&c);
  if (c.p >= c.end || *c.p++ != '{') return -1;
  jws(&c);
  if (c.p < c.end && *c.p == '}') return 0;
  for (;;) {
    jws(&c);
    const char *ks = c.p;
    if (!jskip_string(&c)) return -1;
    int match = (size_t)(c.p - ks) == kl + 2 && memcmp(ks + 1, key, kl) == 0;
    jws(&c);
    if (c.p >= c.end || *c.p++ != ':') return -1;
    jws(&c);
    const char *s = c.p;
    if (!jskip_value(&c, 0)) return -1;
    if (match) { *vs = s; *ve = c.p; found = 1; }
    jws(&c);
    if (c.p >= c.end) return -1;
    if (*c.p == ',') { c.p++; continue; }
    if (*c.p == '}') return found;
    return -1;
  }
}

/* ---- the check ----
 * Writes the result into out: "missing" | "invalid" | "payload" | {"id":<sub>,"name":<username JSON string>}.
 * Returns its length. */
static int jwt_check(const char *auth, int alen, char *out, int outcap) {
#define RET(s) do { memcpy(out, s, sizeof s - 1); return (int)sizeof s - 1; } while (0)
  if (!auth || alen < 7 || memcmp(auth, "Bearer ", 7) != 0) RET("missing");
  const char *t = auth + 7, *end = auth + alen;
  const char *d1 = memchr(t, '.', (size_t)(end - t));
  if (!d1) RET("invalid");
  const char *d2 = memchr(d1 + 1, '.', (size_t)(end - d1 - 1));
  if (!d2 || memchr(d2 + 1, '.', (size_t)(end - d2 - 1))) RET("invalid");
  /* signature first */
  uint8_t mac[32];
  char want[43];
  hmac(t, (size_t)(d2 - t), mac);
  b64url_encode32(mac, want);
  if (end - (d2 + 1) != 43) RET("invalid");
  uint8_t diff = 0;
  for (int i = 0; i < 43; i++) diff |= (uint8_t)(want[i] ^ d2[1 + i]);
  if (diff) RET("invalid");
  /* header: alg must be HS256 */
  uint8_t hdr[512], pl[2048];
  int hl = (int)(d1 - t), pll = (int)(d2 - d1 - 1);
  if (hl > 680 || pll > 2700) RET("invalid");
  hl = b64url_decode(t, hl, hdr);
  pll = b64url_decode(d1 + 1, pll, pl);
  if (hl < 0 || pll < 0) RET("invalid");
  const char *vs, *ve;
  if (jget((char *)hdr, (char *)hdr + hl, "alg", &vs, &ve) != 1 || ve - vs != 7 || memcmp(vs, "\"HS256\"", 7) != 0) RET("invalid");
  /* exp: required, a number, in the future */
  const char *pe = (char *)pl + pll;
  if (jget((char *)pl, pe, "exp", &vs, &ve) != 1 || vs >= ve || !(*vs == '-' || (*vs >= '0' && *vs <= '9'))) RET("invalid");
  char num[32];
  int nl = (int)(ve - vs) < 31 ? (int)(ve - vs) : 31;
  memcpy(num, vs, (size_t)nl); num[nl] = 0;
  if (strtod(num, 0) <= (double)time(0)) RET("invalid");
  /* payload: sub a positive integer string, username a string */
  const char *ss, *se, *us, *ue;
  if (jget((char *)pl, pe, "sub", &ss, &se) != 1 || se - ss < 3 || *ss != '"' || se - ss > 2 + 15) RET("payload");
  for (const char *q = ss + 1; q < se - 1; q++) if (*q < '0' || *q > '9') RET("payload");
  if (ss[1] == '0') RET("payload");
  if (jget((char *)pl, pe, "username", &us, &ue) != 1 || *us != '"') RET("payload");
  int n = (int)(se - ss - 2) + (int)(ue - us) + 20;
  if (n > outcap) RET("payload");
  char *o = out;
  memcpy(o, "{\"id\":", 6); o += 6;
  memcpy(o, ss + 1, (size_t)(se - ss - 2)); o += se - ss - 2;
  memcpy(o, ",\"name\":", 8); o += 8;
  memcpy(o, us, (size_t)(ue - us)); o += ue - us;
  *o++ = '}';
  return (int)(o - out);
#undef RET
}
