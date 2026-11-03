#include "jwt.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

// ---- SHA-256 (FIPS 180-4) ----
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static void block(sha256_t *c, const uint8_t *p) {
  uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
    uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3]; e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
    uint32_t ch = (e & f) ^ (~e & g);
    t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
    uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
    t2 = S0 + mj;
    h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
  }
  c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
void sha256_init(sha256_t *c) {
  static const uint32_t H[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
  memcpy(c->h, H, sizeof H); c->len = 0; c->buflen = 0;
}
void sha256_update(sha256_t *c, const uint8_t *p, size_t n) {
  c->len += n;
  while (n) {
    size_t k = 64 - c->buflen;
    if (k > n) k = n;
    memcpy(c->buf + c->buflen, p, k);
    c->buflen += k; p += k; n -= k;
    if (c->buflen == 64) { block(c, c->buf); c->buflen = 0; }
  }
}
void sha256_final(sha256_t *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t z = 0;
  while (c->buflen != 56) sha256_update(c, &z, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8*i));
  // append length without disturbing c->len accounting side effects: write directly
  size_t save = c->buflen;
  (void)save;
  memcpy(c->buf + 56, lb, 8);
  block(c, c->buf);
  for (int i = 0; i < 8; i++) {
    out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16);
    out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i];
  }
}
void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
  uint8_t kb[64], ip[64], op[64];
  if (klen > 64) { sha256_t c; sha256_init(&c); sha256_update(&c, key, klen); sha256_final(&c, kb);
    memset(kb + 32, 0, 32); }
  else { memcpy(kb, key, klen); memset(kb + klen, 0, 64 - klen); }
  for (int i = 0; i < 64; i++) { ip[i] = kb[i] ^ 0x36; op[i] = kb[i] ^ 0x5c; }
  sha256_t c; uint8_t inner[32];
  sha256_init(&c); sha256_update(&c, ip, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
  sha256_init(&c); sha256_update(&c, op, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

// ---- base64url ----
static int b64v(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '-') return 62;
  if (ch == '_') return 63;
  return -1;
}
int b64url_dec(const char *in, size_t n, uint8_t *out, size_t cap) {
  if (n % 4 == 1) return -1;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    int a = b64v(in[i]), b = (i+1 < n) ? b64v(in[i+1]) : 0;
    int c = (i+2 < n) ? b64v(in[i+2]) : 0, d = (i+3 < n) ? b64v(in[i+3]) : 0;
    if (a < 0 || b < 0 || ((i+2 < n) && c < 0) || ((i+3 < n) && d < 0)) return -1;
    if (o + 3 > cap) return -1;
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
    if (i+2 < n) out[o++] = (uint8_t)((b << 4) | (c >> 2));
    if (i+3 < n) out[o++] = (uint8_t)(((c & 3) << 6) | d);
  }
  return (int)o;
}
size_t b64url_enc(const uint8_t *in, size_t n, char *out) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i+1 < n) v |= (uint32_t)in[i+1] << 8;
    if (i+2 < n) v |= in[i+2];
    out[o++] = A[(v >> 18) & 63]; out[o++] = A[(v >> 12) & 63];
    if (i+1 < n) out[o++] = A[(v >> 6) & 63];
    if (i+2 < n) out[o++] = A[v & 63];
  }
  return o;
}

// ---- minimal JSON field scanner (flat object values) ----
// Find top-level "name" in {...}. Sets *val/*vlen to the raw value and *vt to
// '"', 'n' (number), 't', 'f', 'u' (null), '{', '['. Returns 0 or -1.
static const char *skip_ws(const char *p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}
static const char *skip_str(const char *p, const char *e) { // p at opening quote
  p++;
  while (p < e) {
    if (*p == '\\') { p += 2; continue; }
    if (*p == '"') return p + 1;
    if ((unsigned char)*p < 0x20) return NULL;
    p++;
  }
  return NULL;
}
static const char *skip_val(const char *p, const char *e) {
  p = skip_ws(p, e);
  if (p >= e) return NULL;
  if (*p == '"') return skip_str(p, e);
  if (*p == '{' || *p == '[') {
    char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 0;
    int instr = 0;
    while (p < e) {
      char ch = *p;
      if (instr) { if (ch == '\\') p++; else if (ch == '"') instr = 0; }
      else if (ch == '"') instr = 1;
      else if (ch == open) depth++;
      else if (ch == close) { if (--depth == 0) return p + 1; }
      p++;
    }
    return NULL;
  }
  // literal: run to , } ]
  while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
  return p;
}
static int cmp_key(const char *p, const char *e, const char *name) { // p at opening quote
  size_t n = strlen(name);
  if ((size_t)(e - p) < n + 2) return -1;
  if (p[0] != '"') return -1;
  if (memcmp(p + 1, name, n) != 0 || p[1+n] != '"') return -1;
  return 0;
}
static int find_field(const char *p, size_t n, const char *name,
                      const char **val, size_t *vlen, char *vt) {
  const char *e = p + n;
  p = skip_ws(p, e);
  if (p >= e || *p != '{') return -1;
  p++;
  for (;;) {
    p = skip_ws(p, e);
    if (p >= e) return -1;
    if (*p == '}') return -1; // not found
    if (*p != '"') return -1;
    int iskey = (cmp_key(p, e, name) == 0);
    p = skip_str(p, e);
    if (!p) return -1;
    p = skip_ws(p, e);
    if (p >= e || *p != ':') return -1;
    p++;
    const char *vs = skip_ws(p, e);
    if (vs >= e) return -1;
    const char *ve = skip_val(vs, e);
    if (!ve) return -1;
    if (iskey) {
      char t = *vs; // 't'/'f' pass through; null maps to 'u', anything else is a number
      if (t == 'n') t = 'u';
      else if (t != '"' && t != '{' && t != '[' && t != 't' && t != 'f') t = 'n';
      *val = vs; *vlen = (size_t)(ve - vs); *vt = t;
      return 0;
    }
    p = skip_ws(ve, e);
    if (p >= e) return -1;
    if (*p == ',') { p++; continue; }
    return -1;
  }
}

static int consttime_eq(const uint8_t *a, const uint8_t *b, size_t n) {
  unsigned d = 0;
  for (size_t i = 0; i < n; i++) d |= (unsigned)(a[i] ^ b[i]);
  return d == 0;
}

int jwt_verify(const char *tok, size_t tlen,
               const uint8_t *secret, size_t slen,
               uint64_t now_s, auth_t *out, char *scratch, size_t scratch_cap) {
  const char *d1 = memchr(tok, '.', tlen);
  if (!d1) return JWT_BAD_TOKEN;
  const char *d2 = memchr(d1 + 1, '.', (size_t)(tok + tlen - (d1 + 1)));
  if (!d2) return JWT_BAD_TOKEN;
  size_t hlen = (size_t)(d1 - tok), plen = (size_t)(d2 - (d1 + 1));
  const char *sig = d2 + 1;
  size_t siglen = (size_t)(tok + tlen - sig);
  if (hlen == 0 || plen == 0 || siglen == 0) return JWT_BAD_TOKEN;
  if (scratch_cap < 64) return JWT_BAD_TOKEN;

  // 1. header: must be a JSON object with alg == "HS256"
  if (hlen > scratch_cap) return JWT_BAD_TOKEN;
  uint8_t *hraw = (uint8_t *)scratch;
  int hnb = b64url_dec(tok, hlen, hraw, scratch_cap);
  if (hnb < 0) return JWT_BAD_TOKEN;
  const char *hv; size_t hvl; char hvt;
  if (find_field((const char *)hraw, (size_t)hnb, "alg", &hv, &hvl, &hvt) != 0) return JWT_BAD_TOKEN;
  if (hvt != '"' || hvl != 7 || memcmp(hv, "\"HS256\"", 7) != 0) return JWT_BAD_TOKEN;

  // 2. signature over "h.p" (the original base64 text)
  size_t msglen = (size_t)(d2 - tok);
  uint8_t mac[32], sigraw[64];
  hmac_sha256(secret, slen, (const uint8_t *)tok, msglen, mac);
  int snb = b64url_dec(sig, siglen, sigraw, sizeof sigraw);
  if (snb != 32 || !consttime_eq(mac, sigraw, 32)) return JWT_BAD_TOKEN;

  // 3. payload claims
  char *praw = scratch; // reuse (header no longer needed)
  // payload b64 length can exceed scratch? cap: need plen*3/4+4 <= scratch_cap
  if (plen * 3 / 4 + 8 > scratch_cap) return JWT_BAD_TOKEN;
  int pnb = b64url_dec(d1 + 1, plen, (uint8_t *)praw, scratch_cap);
  if (pnb < 0) return JWT_BAD_TOKEN;

  const char *v; size_t vl; char vt;
  // sub: must be a string of a positive integer
  if (find_field(praw, (size_t)pnb, "sub", &v, &vl, &vt) != 0 || vt != '"') return JWT_BAD_PAYLOAD;
  // v includes quotes; parse digits strictly
  if (vl < 3) return JWT_BAD_PAYLOAD; // at least "x"
  uint64_t uid = 0;
  int digits = 0;
  for (size_t i = 1; i + 1 < vl; i++) {
    char ch = v[i];
    if (ch == '\\') return JWT_BAD_PAYLOAD; // no escapes allowed in sub
    if (ch < '0' || ch > '9') return JWT_BAD_PAYLOAD;
    if (++digits > 16) return JWT_BAD_PAYLOAD; // 2^53 fits in 16 digits; longer cannot be valid
    uid = uid * 10 + (uint64_t)(ch - '0');
    if (uid > 9007199254740991ULL) return JWT_BAD_PAYLOAD; // beyond 2^53: out of scope, reject
  }
  if (uid == 0) return JWT_BAD_PAYLOAD;

  // username: must be a string; decode escapes into scratch after payload
  if (find_field(praw, (size_t)pnb, "username", &v, &vl, &vt) != 0 || vt != '"') return JWT_BAD_PAYLOAD;
  size_t uname_off = (size_t)pnb;
  // ensure room: decoded <= vlen, fits if uname_off + vl <= scratch_cap
  if (uname_off + vl > scratch_cap) return JWT_BAD_TOKEN;
  int unb = json_unescape(v + 1, vl - 2, praw + uname_off, scratch_cap - uname_off);
  if (unb < 0) return JWT_BAD_PAYLOAD;

  // exp: must be a JSON number and > now
  if (find_field(praw, (size_t)pnb, "exp", &v, &vl, &vt) != 0 || vt != 'n') return JWT_BAD_TOKEN;
  // strict-ish number parse: optional -, digits, optional frac/exp; use strtod on a copy
  if (vl == 0 || vl >= 64) return JWT_BAD_TOKEN;
  char num[64];
  memcpy(num, v, vl); num[vl] = 0;
  char *end = NULL;
  double exp = strtod(num, &end);
  if (end == num || *end != 0) return JWT_BAD_TOKEN;
  if (!(exp > (double)now_s)) return JWT_BAD_TOKEN;

  // optional nbf: if present as number, must be <= now
  if (find_field(praw, (size_t)pnb, "nbf", &v, &vl, &vt) == 0 && vt == 'n' && vl > 0 && vl < 64) {
    memcpy(num, v, vl); num[vl] = 0;
    double nbf = strtod(num, &end);
    if (end != num && *end == 0 && nbf > (double)now_s) return JWT_BAD_TOKEN;
  }

  out->uid = uid;
  out->uname = praw + uname_off;
  out->ulen = (size_t)unb;
  return JWT_OK;
}
