/* jwt.h: HS256 bearer tokens. Every token's signature is verified on every request (constant-time
 * compare); the header must say HS256; exp is required and must be in the future. */
#ifndef JWT_H
#define JWT_H

#include "json.h"
#include "sha256.h"

enum { JWT_OK = 0, JWT_MISSING = 1, JWT_INVALID = 2, JWT_PAYLOAD = 3 };

struct claims {
  int64_t user_id;
  const uint8_t *username; /* decoded into the caller's buffer */
  uint32_t username_len;
};

static int b64url_val(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

/* Decodes unpadded base64url. Returns the output length or -1. */
static int b64url_decode(const uint8_t *s, uint32_t n, uint8_t *out, uint32_t cap) {
  uint32_t o = 0, acc = 0;
  int bits = 0;
  for (uint32_t i = 0; i < n; i++) {
    int v = b64url_val(s[i]);
    if (v < 0) return -1;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  if (bits >= 6) return -1;              /* a stray sextet */
  if (acc & ((1u << bits) - 1)) return -1; /* non-canonical trailing bits */
  return (int)o;
}

struct jwt_hdr_scan {
  int alg_ok;
};
static int jwt_hdr_member(void *ud, const uint8_t *k, uint32_t kn, int kesc, struct jv *v) {
  struct jwt_hdr_scan *s = ud;
  if (!kesc && kn == 3 && !memcmp(k, "alg", 3)) s->alg_ok = v->type == JV_STRING && !v->escaped && v->n == 5 && !memcmp(v->p, "HS256", 5);
  return 0;
}

struct jwt_payload_scan {
  struct jv sub, username, exp;
};
static int jwt_payload_member(void *ud, const uint8_t *k, uint32_t kn, int kesc, struct jv *v) {
  struct jwt_payload_scan *s = ud;
  if (kesc) return 0;
  if (kn == 3 && !memcmp(k, "sub", 3)) s->sub = *v;
  else if (kn == 3 && !memcmp(k, "exp", 3)) s->exp = *v;
  else if (kn == 8 && !memcmp(k, "username", 8)) s->username = *v;
  return 0;
}

/* Digits only, 1..18 of them, value >= 1 (leading zeros allowed). */
static inline int parse_positive(const uint8_t *s, uint32_t n, int64_t *out) {
  if (n == 0 || n > 18) return 0;
  int64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    v = v * 10 + (s[i] - '0');
  }
  if (v < 1) return 0;
  *out = v;
  return 1;
}

/* Verifies the Authorization header value. On JWT_OK fills c (username decoded into
 * namebuf, up to namecap bytes). */
static int jwt_verify(const struct hmac_key *key, const uint8_t *auth, uint32_t auth_len, int64_t now, struct claims *c,
                      uint8_t *namebuf, uint32_t namecap) {
  if (!auth || auth_len < 7 || memcmp(auth, "Bearer ", 7) != 0) return JWT_MISSING;
  const uint8_t *t = auth + 7;
  uint32_t n = auth_len - 7;
  while (n && *t == ' ') { t++; n--; }
  const uint8_t *d1 = memchr(t, '.', n);
  if (!d1) return JWT_INVALID;
  const uint8_t *d2 = memchr(d1 + 1, '.', (size_t)(t + n - d1 - 1));
  if (!d2) return JWT_INVALID;
  if (memchr(d2 + 1, '.', (size_t)(t + n - d2 - 1))) return JWT_INVALID;
  uint32_t hlen = (uint32_t)(d1 - t), plen = (uint32_t)(d2 - d1 - 1), slen = (uint32_t)(t + n - d2 - 1);
  if (hlen == 0 || plen == 0 || slen != 43 || plen > 4096 || hlen > 1024) return JWT_INVALID;
  /* signature first */
  uint8_t sig[32], want[32];
  if (b64url_decode(d2 + 1, slen, sig, sizeof sig) != 32) return JWT_INVALID;
  hmac_sha256(key, t, (size_t)(d2 - t), want);
  unsigned diff = 0;
  for (int i = 0; i < 32; i++) diff |= sig[i] ^ want[i];
  if (diff) return JWT_INVALID;
  /* header: alg must be HS256 */
  uint8_t hdr[1024];
  int hn = b64url_decode(t, hlen, hdr, sizeof hdr);
  if (hn < 0) return JWT_INVALID;
  struct jwt_hdr_scan hs = {0};
  if (json_parse_object(hdr, (uint32_t)hn, jwt_hdr_member, &hs) || !hs.alg_ok) return JWT_INVALID;
  /* payload */
  uint8_t pl[4096];
  int pn = b64url_decode(d1 + 1, plen, pl, sizeof pl);
  if (pn < 0) return JWT_INVALID;
  struct jwt_payload_scan ps;
  memset(&ps, 0, sizeof ps);
  if (json_parse_object(pl, (uint32_t)pn, jwt_payload_member, &ps)) return JWT_INVALID;
  if (ps.exp.type != JV_NUMBER) return JWT_INVALID;
  /* exp: an integer number of seconds (a fraction or exponent is read as JavaScript would, roughly) */
  {
    const uint8_t *e = ps.exp.p;
    uint32_t en = ps.exp.n;
    int neg = 0;
    uint32_t i = 0;
    if (en && e[0] == '-') { neg = 1; i = 1; }
    int64_t v = 0;
    for (; i < en && e[i] >= '0' && e[i] <= '9'; i++) {
      if (v < (int64_t)1e15) v = v * 10 + (e[i] - '0');
    }
    if (i < en && (e[i] == 'e' || e[i] == 'E')) v = (int64_t)1e15; /* exponent forms: treat as far future... */
    if (neg) v = -v;
    if (v <= now) return JWT_INVALID;
  }
  if (ps.sub.type != JV_STRING || ps.username.type != JV_STRING) return JWT_PAYLOAD;
  if (ps.sub.escaped || !parse_positive(ps.sub.p, ps.sub.n, &c->user_id)) return JWT_PAYLOAD;
  if (ps.username.escaped) {
    if (ps.username.n > namecap) return JWT_PAYLOAD;
    c->username_len = json_unescape(ps.username.p, ps.username.n, namebuf);
  } else {
    if (ps.username.n > namecap) return JWT_PAYLOAD;
    memcpy(namebuf, ps.username.p, ps.username.n);
    c->username_len = ps.username.n;
  }
  c->username = namebuf;
  return JWT_OK;
}

#endif
