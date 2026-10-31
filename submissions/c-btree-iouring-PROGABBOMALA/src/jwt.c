#include "rt.h"
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <stdlib.h>

static SHA256_CTX g_hmac_ipad_ctx;
static SHA256_CTX g_hmac_opad_ctx;
static uint8_t g_b64url_lut[256];

void jwt_fast_init(const char *jwt_secret, size_t secret_len) {
  memset(g_b64url_lut, 0xFF, sizeof g_b64url_lut);
  for (int c = 'A'; c <= 'Z'; c++) g_b64url_lut[c] = (uint8_t)(c - 'A');
  for (int c = 'a'; c <= 'z'; c++) g_b64url_lut[c] = (uint8_t)(c - 'a' + 26);
  for (int c = '0'; c <= '9'; c++) g_b64url_lut[c] = (uint8_t)(c - '0' + 52);
  g_b64url_lut[(uint8_t)'-'] = 62;
  g_b64url_lut[(uint8_t)'_'] = 63;

  uint8_t k_norm[64], k_ipad[64], k_opad[64];
  memset(k_norm, 0, 64);
  if (secret_len > 64) SHA256((const uint8_t *)jwt_secret, secret_len, k_norm);
  else memcpy(k_norm, jwt_secret, secret_len);
  for (int i = 0; i < 64; i++) {
    k_ipad[i] = (uint8_t)(k_norm[i] ^ 0x36);
    k_opad[i] = (uint8_t)(k_norm[i] ^ 0x5c);
  }
  SHA256_Init(&g_hmac_ipad_ctx);
  SHA256_Update(&g_hmac_ipad_ctx, k_ipad, 64);
  SHA256_Init(&g_hmac_opad_ctx);
  SHA256_Update(&g_hmac_opad_ctx, k_opad, 64);
  OPENSSL_cleanse(k_norm, sizeof k_norm);
  OPENSSL_cleanse(k_ipad, sizeof k_ipad);
  OPENSSL_cleanse(k_opad, sizeof k_opad);
}

static int b64url_decode(const uint8_t *in, size_t in_len, uint8_t *out) {
  size_t i = 0, o = 0;
  while (i + 4 <= in_len) {
    uint8_t a = g_b64url_lut[in[i]], b = g_b64url_lut[in[i + 1]];
    uint8_t c = g_b64url_lut[in[i + 2]], d = g_b64url_lut[in[i + 3]];
    if ((a | b | c | d) & 0x80) return -1;
    out[o] = (uint8_t)((a << 2) | (b >> 4));
    out[o + 1] = (uint8_t)((b << 4) | (c >> 2));
    out[o + 2] = (uint8_t)((c << 6) | d);
    i += 4;
    o += 3;
  }
  size_t rem = in_len - i;
  if (rem == 1) return -1;
  if (rem == 2) {
    uint8_t a = g_b64url_lut[in[i]], b = g_b64url_lut[in[i + 1]];
    if ((a | b) & 0x80) return -1;
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
  } else if (rem == 3) {
    uint8_t a = g_b64url_lut[in[i]], b = g_b64url_lut[in[i + 1]], c = g_b64url_lut[in[i + 2]];
    if ((a | b | c) & 0x80) return -1;
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
    out[o++] = (uint8_t)((b << 4) | (c >> 2));
  }
  return (int)o;
}

/* Parse a JSON number span. Returns 0 and sets *ok when the value is a finite second-count. */
static int parse_time_num(const uint8_t *p, uint32_t n, int64_t now, int future_ok, int *reject) {
  if (!n) return -1;
  char buf[64];
  if (n >= sizeof buf) return -1;
  memcpy(buf, p, n);
  buf[n] = 0;
  char *end = 0;
  double dv = strtod(buf, &end);
  if (end == buf || *end) return -1;
  if (future_ok) {
    if (!(dv > (double)now)) *reject = 1;
  } else {
    if (dv > (double)now) *reject = 1;
  }
  return 0;
}

int jwt_verify(const uint8_t *token, uint32_t token_len, int64_t now, claims_t *c) {
  const uint8_t *dot1 = memchr(token, '.', token_len);
  if (!dot1) return JWT_INVALID;
  uint32_t hdr_len = (uint32_t)(dot1 - token);
  uint32_t rem1 = token_len - hdr_len - 1;
  const uint8_t *dot2 = memchr(dot1 + 1, '.', rem1);
  if (!dot2) return JWT_INVALID;
  if (memchr(dot2 + 1, '.', (size_t)(token + token_len - (dot2 + 1)))) return JWT_INVALID;
  uint32_t sig_len = (uint32_t)(token + token_len - (dot2 + 1));
  if (sig_len != 43) return JWT_INVALID;

  uint8_t sig_bin[32], expect[32], inner[32];
  if (b64url_decode(dot2 + 1, 43, sig_bin) != 32) return JWT_INVALID;
  uint32_t signed_len = (uint32_t)(dot2 - token);
  SHA256_CTX ictx = g_hmac_ipad_ctx;
  SHA256_Update(&ictx, token, signed_len);
  SHA256_Final(inner, &ictx);
  SHA256_CTX octx = g_hmac_opad_ctx;
  SHA256_Update(&octx, inner, 32);
  SHA256_Final(expect, &octx);
  if (CRYPTO_memcmp(sig_bin, expect, 32) != 0) return JWT_INVALID;

  static const char CANON[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
  if (hdr_len != 36 || memcmp(token, CANON, 36) != 0) {
    if (hdr_len > 256) return JWT_INVALID;
    uint8_t hdr[256];
    int hn = b64url_decode(token, hdr_len, hdr);
    if (hn <= 0) return JWT_INVALID;
    jmember hm[1] = {{"alg", 3, 0, 0, 0, 0}};
    if (json_members(hdr, (uint32_t)hn, hm, 1) != 0) return JWT_INVALID;
    if (hm[0].type != JT_STRING || hm[0].v_len != 5 || memcmp(hm[0].v, "HS256", 5) != 0) return JWT_INVALID;
  }

  uint32_t pay_len = (uint32_t)(dot2 - (dot1 + 1));
  if (!pay_len || pay_len > 1024) return JWT_INVALID;
  uint8_t pay[768];
  int pn = b64url_decode(dot1 + 1, pay_len, pay);
  if (pn <= 0 || pn > (int)sizeof pay) return JWT_INVALID;

  jmember pm[4] = {
    {"exp", 3, 0, 0, 0, 0},
    {"nbf", 3, 0, 0, 0, 0},
    {"sub", 3, 0, 0, 0, 0},
    {"username", 8, 0, 0, 0, 0},
  };
  if (json_members(pay, (uint32_t)pn, pm, 4) != 0) return JWT_INVALID;
  if (pm[0].type != JT_NUMBER) return JWT_INVALID;
  int bad_exp = 0;
  if (parse_time_num(pm[0].v, pm[0].v_len, now, 1, &bad_exp) || bad_exp) return JWT_INVALID;
  if (pm[1].type == JT_NUMBER) {
    int bad_nbf = 0;
    if (parse_time_num(pm[1].v, pm[1].v_len, now, 0, &bad_nbf) || bad_nbf) return JWT_INVALID;
  } else if (pm[1].type != JT_NONE) return JWT_INVALID;

  if (pm[2].type != JT_STRING || pm[2].escaped || pm[2].v_len == 0 || pm[2].v_len > 18) return JWT_PAYLOAD;
  int64_t uid = 0;
  for (uint32_t i = 0; i < pm[2].v_len; i++) {
    if (pm[2].v[i] < '0' || pm[2].v[i] > '9') return JWT_PAYLOAD;
    uid = uid * 10 + (pm[2].v[i] - '0');
  }
  if (uid <= 0) return JWT_PAYLOAD;

  if (pm[3].type != JT_STRING) return JWT_PAYLOAD;
  uint8_t name[256];
  uint32_t nl;
  if (pm[3].escaped) {
    if (pm[3].v_len > 512) return JWT_PAYLOAD;
    uint8_t raw[512];
    memcpy(raw, pm[3].v, pm[3].v_len);
    nl = json_unescape(raw, pm[3].v_len, name);
  } else {
    if (pm[3].v_len > sizeof name) return JWT_PAYLOAD;
    memcpy(name, pm[3].v, pm[3].v_len);
    nl = pm[3].v_len;
  }
  if (nl == 0 || nl > sizeof c->name) return JWT_PAYLOAD;
  memcpy(c->name, name, nl);
  c->name_len = nl;
  c->uid = uid;
  return JWT_OK;
}
