#include <sys/types.h>

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

struct sha256_ctx {
  uint32_t h[8];
  uint64_t bits;
  uint8_t buf[64];
  size_t buflen;
};
typedef struct sha256_ctx sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr32(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  int i;

  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) |
           (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                  (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                  (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (i = 0; i < 64; i++) {
      uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
      uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
}

static void sha256_init(sha256_ctx *ctx) {
  ctx->h[0] = 0x6a09e667u;
  ctx->h[1] = 0xbb67ae85u;
  ctx->h[2] = 0x3c6ef372u;
  ctx->h[3] = 0xa54ff53au;
  ctx->h[4] = 0x510e527fu;
  ctx->h[5] = 0x9b05688cu;
  ctx->h[6] = 0x1f83d9abu;
  ctx->h[7] = 0x5be0cd19u;
  ctx->bits = 0;
  ctx->buflen = 0;
}

static void sha256_init_midstate(sha256_ctx *ctx, const uint32_t state[8]) {
  memcpy(ctx->h, state, sizeof(ctx->h));
  ctx->bits = 512;
  ctx->buflen = 0;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
  while (len > 0) {
    if (ctx->buflen == 0 && len >= 64) {
      sha256_compress(ctx->h, data);
      ctx->bits += 512;
      data += 64;
      len -= 64;
    } else {
      size_t take = 64 - ctx->buflen;
      if (take > len) {
        take = len;
      }
      memcpy(ctx->buf + ctx->buflen, data, take);
      ctx->buflen += take;
      data += take;
      len -= take;
      if (ctx->buflen == 64) {
        sha256_compress(ctx->h, ctx->buf);
        ctx->bits += 512;
        ctx->buflen = 0;
      }
    }
  }
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[32]) {
  uint64_t bitlen = ctx->bits + (uint64_t)ctx->buflen * 8;
  size_t i;

  ctx->buf[ctx->buflen++] = 0x80;
  if (ctx->buflen > 56) {
    memset(ctx->buf + ctx->buflen, 0, 64 - ctx->buflen);
    sha256_compress(ctx->h, ctx->buf);
    ctx->bits += 512;
    ctx->buflen = 0;
  }
  memset(ctx->buf + ctx->buflen, 0, 56 - ctx->buflen);
  for (i = 0; i < 8; i++) {
    ctx->buf[56 + i] = (uint8_t)(bitlen >> (56 - 8 * i));
  }
  sha256_compress(ctx->h, ctx->buf);
  ctx->bits += 512;

  for (i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)ctx->h[i];
  }
}

static void sha256_digest(const uint8_t *data, size_t len, uint8_t out[32]) {
  sha256_ctx ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, out);
}

static uint32_t g_hmac_ipad[8];
static uint32_t g_hmac_opad[8];

void proto_init(const char *jwt_secret) {
  uint8_t key[64];
  uint8_t ipad[64];
  uint8_t opad[64];
  sha256_ctx ctx;
  size_t n = 0;
  size_t i;

  memset(key, 0, sizeof(key));
  if (jwt_secret != NULL) {
    n = strlen(jwt_secret);
    if (n > sizeof(key)) {
      sha256_digest((const uint8_t *)jwt_secret, n, key);
    } else if (n > 0) {
      memcpy(key, jwt_secret, n);
    }
  }

  for (i = 0; i < sizeof(key); i++) {
    ipad[i] = (uint8_t)(key[i] ^ 0x36u);
    opad[i] = (uint8_t)(key[i] ^ 0x5cu);
  }

  sha256_init(&ctx);
  sha256_update(&ctx, ipad, sizeof(ipad));
  memcpy(g_hmac_ipad, ctx.h, sizeof(g_hmac_ipad));

  sha256_init(&ctx);
  sha256_update(&ctx, opad, sizeof(opad));
  memcpy(g_hmac_opad, ctx.h, sizeof(g_hmac_opad));

  memset(key, 0, sizeof(key));
  memset(ipad, 0, sizeof(ipad));
  memset(opad, 0, sizeof(opad));
}

static void hmac_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  sha256_ctx ctx;
  uint8_t inner[32];

  sha256_init_midstate(&ctx, g_hmac_ipad);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, inner);

  sha256_init_midstate(&ctx, g_hmac_opad);
  sha256_update(&ctx, inner, sizeof(inner));
  sha256_final(&ctx, out);

  memset(inner, 0, sizeof(inner));
}

static int is_json_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static int is_http_ows(uint8_t c) { return c == ' ' || c == '\t'; }

static int is_json_ws(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_js_ws_cp(uint32_t cp) {
  if (cp == 0x09u || cp == 0x0Au || cp == 0x0Bu || cp == 0x0Cu ||
      cp == 0x0Du || cp == 0x20u) {
    return 1;
  }
  if (cp == 0x00A0u || cp == 0x1680u || cp == 0x2028u || cp == 0x2029u ||
      cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u || cp == 0xFEFFu) {
    return 1;
  }
  if (cp >= 0x2000u && cp <= 0x200Au) {
    return 1;
  }
  return 0;
}

static void json_skip_ws(const uint8_t *p, size_t n, size_t *pos) {
  while (*pos < n && is_json_ws(p[*pos])) {
    (*pos)++;
  }
}

static int json_hex4(const uint8_t *p, uint32_t *out) {
  uint32_t v = 0;
  int i;
  for (i = 0; i < 4; i++) {
    uint8_t c = p[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = (uint32_t)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      d = (uint32_t)(c - 'A') + 10u;
    } else {
      return 0;
    }
    v = (v << 4) | d;
  }
  *out = v;
  return 1;
}

static int utf8_decode_cp(const uint8_t *p, size_t n, uint32_t *cp,
                          size_t *used) {
  uint8_t c;
  if (n == 0) {
    return 0;
  }
  c = p[0];
  if (c < 0x80u) {
    *cp = c;
    *used = 1;
    return 1;
  }
  if (c >= 0xC2u && c <= 0xDFu) {
    if (n < 2 || (p[1] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
    *used = 2;
    return 1;
  }
  if (c == 0xE0u) {
    if (n < 3 || p[1] < 0xA0u || p[1] > 0xBFu ||
        (p[2] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x0Fu) << 12) |
          ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
    *used = 3;
    return 1;
  }
  if (c >= 0xE1u && c <= 0xECu) {
    if (n < 3 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x0Fu) << 12) |
          ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
    *used = 3;
    return 1;
  }
  if (c == 0xEDu) {
    if (n < 3 || p[1] < 0x80u || p[1] > 0x9Fu ||
        (p[2] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x0Fu) << 12) |
          ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
    *used = 3;
    return 1;
  }
  if (c >= 0xEEu && c <= 0xEFu) {
    if (n < 3 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x0Fu) << 12) |
          ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
    *used = 3;
    return 1;
  }
  if (c == 0xF0u) {
    if (n < 4 || p[1] < 0x90u || p[1] > 0xBFu ||
        (p[2] & 0xC0u) != 0x80u || (p[3] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x07u) << 18) |
          ((uint32_t)(p[1] & 0x3Fu) << 12) |
          ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
    *used = 4;
    return 1;
  }
  if (c >= 0xF1u && c <= 0xF3u) {
    if (n < 4 || (p[1] & 0xC0u) != 0x80u ||
        (p[2] & 0xC0u) != 0x80u || (p[3] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x07u) << 18) |
          ((uint32_t)(p[1] & 0x3Fu) << 12) |
          ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
    *used = 4;
    return 1;
  }
  if (c == 0xF4u) {
    if (n < 4 || p[1] < 0x80u || p[1] > 0x8Fu ||
        (p[2] & 0xC0u) != 0x80u || (p[3] & 0xC0u) != 0x80u) {
      return 0;
    }
    *cp = ((uint32_t)(c & 0x07u) << 18) |
          ((uint32_t)(p[1] & 0x3Fu) << 12) |
          ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
    *used = 4;
    return 1;
  }
  return 0;
}

static size_t utf8_encode_cp(uint32_t cp, uint8_t out[4]) {
  if (cp <= 0x7Fu) {
    out[0] = (uint8_t)cp;
    return 1;
  }
  if (cp <= 0x7FFu) {
    out[0] = (uint8_t)(0xC0u | (cp >> 6));
    out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp <= 0xFFFFu) {
    out[0] = (uint8_t)(0xE0u | (cp >> 12));
    out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 3;
  }
  out[0] = (uint8_t)(0xF0u | (cp >> 18));
  out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
  out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
  out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
  return 4;
}

struct jstr_sink {
  uint8_t *buf;
  size_t cap;
  size_t len;
  size_t cp;
  size_t last_len;
  size_t last_cp;
  int overflow;
  int trim;
  int started;
};

static void jstr_reset(struct jstr_sink *s) {
  s->len = 0;
  s->cp = 0;
  s->last_len = 0;
  s->last_cp = 0;
  s->overflow = 0;
  s->started = 0;
}

static void jstr_init(struct jstr_sink *s, uint8_t *buf, size_t cap, int trim) {
  s->buf = buf;
  s->cap = cap;
  s->trim = trim;
  jstr_reset(s);
}

static void jstr_put_cp(struct jstr_sink *s, uint32_t cp) {
  uint8_t tmp[4];
  size_t n;
  int ws = 0;

  if (s->trim) {
    ws = is_js_ws_cp(cp);
    if (!s->started && ws) {
      return;
    }
    if (!ws) {
      s->started = 1;
    }
    s->cp++;
  }

  n = utf8_encode_cp(cp, tmp);
  if (s->buf != NULL) {
    if (n <= s->cap - s->len) {
      memcpy(s->buf + s->len, tmp, n);
      s->len += n;
    } else {
      s->overflow = 1;
    }
  } else {
    s->len += n;
  }

  if (s->trim && !ws) {
    s->last_len = s->len;
    s->last_cp = s->cp;
  }
}

static int json_parse_string(const uint8_t *p, size_t n, size_t *pos,
                             struct jstr_sink *sink) {
  size_t i = *pos;
  if (i >= n || p[i] != '"') {
    return 0;
  }
  i++;

  while (i < n) {
    uint8_t c = p[i];
    if (c == '"') {
      *pos = i + 1;
      return 1;
    }
    if (c == '\\') {
      uint32_t cp;
      i++;
      if (i >= n) {
        return 0;
      }
      switch (p[i]) {
        case '"':
          cp = '"';
          i++;
          break;
        case '\\':
          cp = '\\';
          i++;
          break;
        case '/':
          cp = '/';
          i++;
          break;
        case 'b':
          cp = 0x08u;
          i++;
          break;
        case 'f':
          cp = 0x0Cu;
          i++;
          break;
        case 'n':
          cp = 0x0Au;
          i++;
          break;
        case 'r':
          cp = 0x0Du;
          i++;
          break;
        case 't':
          cp = 0x09u;
          i++;
          break;
        case 'u': {
          uint32_t v;
          i++;
          if (i + 4 > n || !json_hex4(p + i, &v)) {
            return 0;
          }
          i += 4;
          if (v >= 0xD800u && v <= 0xDBFFu) {
            uint32_t lo;
            if (i + 6 > n || p[i] != '\\' || p[i + 1] != 'u') {
              return 0;
            }
            if (!json_hex4(p + i + 2, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
              return 0;
            }
            i += 6;
            cp = 0x10000u + ((v - 0xD800u) << 10) + (lo - 0xDC00u);
          } else if (v >= 0xDC00u && v <= 0xDFFFu) {
            return 0;
          } else {
            cp = v;
          }
          break;
        }
        default:
          return 0;
      }
      jstr_put_cp(sink, cp);
      continue;
    }
    if (c < 0x20u) {
      return 0;
    }
    if (c < 0x80u) {
      jstr_put_cp(sink, (uint32_t)c);
      i++;
    } else {
      uint32_t cp;
      size_t used;
      if (!utf8_decode_cp(p + i, n - i, &cp, &used)) {
        return 0;
      }
      jstr_put_cp(sink, cp);
      i += used;
    }
  }
  return 0;
}

static int json_scan_number(const uint8_t *p, size_t n, size_t *pos) {
  size_t i = *pos;
  if (i < n && p[i] == '-') {
    i++;
  }
  if (i >= n) {
    return 0;
  }
  if (p[i] == '0') {
    i++;
    if (i < n && is_json_digit(p[i])) {
      return 0;
    }
  } else if (p[i] >= '1' && p[i] <= '9') {
    do {
      i++;
    } while (i < n && is_json_digit(p[i]));
  } else {
    return 0;
  }

  if (i < n && p[i] == '.') {
    i++;
    if (i >= n || !is_json_digit(p[i])) {
      return 0;
    }
    do {
      i++;
    } while (i < n && is_json_digit(p[i]));
  }

  if (i < n && (p[i] == 'e' || p[i] == 'E')) {
    i++;
    if (i < n && (p[i] == '+' || p[i] == '-')) {
      i++;
    }
    if (i >= n || !is_json_digit(p[i])) {
      return 0;
    }
    do {
      i++;
    } while (i < n && is_json_digit(p[i]));
  }

  *pos = i;
  return 1;
}

static int json_skip_value(const uint8_t *p, size_t n, size_t *pos,
                           int depth);

static int json_skip_object(const uint8_t *p, size_t n, size_t *pos,
                            int depth) {
  (*pos)++;
  json_skip_ws(p, n, pos);
  if (*pos < n && p[*pos] == '}') {
    (*pos)++;
    return 1;
  }
  for (;;) {
    struct jstr_sink sink;
    json_skip_ws(p, n, pos);
    if (*pos >= n || p[*pos] != '"') {
      return 0;
    }
    jstr_init(&sink, NULL, 0, 0);
    if (!json_parse_string(p, n, pos, &sink)) {
      return 0;
    }
    json_skip_ws(p, n, pos);
    if (*pos >= n || p[*pos] != ':') {
      return 0;
    }
    (*pos)++;
    if (!json_skip_value(p, n, pos, depth + 1)) {
      return 0;
    }
    json_skip_ws(p, n, pos);
    if (*pos < n && p[*pos] == ',') {
      (*pos)++;
      continue;
    }
    if (*pos < n && p[*pos] == '}') {
      (*pos)++;
      return 1;
    }
    return 0;
  }
}

static int json_skip_array(const uint8_t *p, size_t n, size_t *pos,
                           int depth) {
  (*pos)++;
  json_skip_ws(p, n, pos);
  if (*pos < n && p[*pos] == ']') {
    (*pos)++;
    return 1;
  }
  for (;;) {
    if (!json_skip_value(p, n, pos, depth + 1)) {
      return 0;
    }
    json_skip_ws(p, n, pos);
    if (*pos < n && p[*pos] == ',') {
      (*pos)++;
      continue;
    }
    if (*pos < n && p[*pos] == ']') {
      (*pos)++;
      return 1;
    }
    return 0;
  }
}

static int json_skip_value(const uint8_t *p, size_t n, size_t *pos,
                           int depth) {
  uint8_t c;
  if (depth > 128) {
    return 0;
  }
  json_skip_ws(p, n, pos);
  if (*pos >= n) {
    return 0;
  }
  c = p[*pos];

  if (c == '"') {
    struct jstr_sink sink;
    jstr_init(&sink, NULL, 0, 0);
    return json_parse_string(p, n, pos, &sink);
  }
  if (c == '{') {
    return json_skip_object(p, n, pos, depth);
  }
  if (c == '[') {
    return json_skip_array(p, n, pos, depth);
  }
  if (c == 't') {
    if (n - *pos >= 4 && memcmp(p + *pos, "true", 4) == 0) {
      *pos += 4;
      return 1;
    }
    return 0;
  }
  if (c == 'f') {
    if (n - *pos >= 5 && memcmp(p + *pos, "false", 5) == 0) {
      *pos += 5;
      return 1;
    }
    return 0;
  }
  if (c == 'n') {
    if (n - *pos >= 4 && memcmp(p + *pos, "null", 4) == 0) {
      *pos += 4;
      return 1;
    }
    return 0;
  }
  if (c == '-' || is_json_digit(c)) {
    return json_scan_number(p, n, pos);
  }
  return 0;
}

enum json_body_rc json_parse_post_body(const uint8_t *body, size_t len,
                                       uint8_t *trimmed_out, size_t trimmed_cap,
                                       size_t *trimmed_len_out) {
  size_t pos = 0;
  struct jstr_sink body_sink;
  int body_kind = 0;

  if (body == NULL || len == 0) {
    return JSON_BODY_MALFORMED;
  }

  jstr_init(&body_sink, trimmed_out, trimmed_cap, 1);

  json_skip_ws(body, len, &pos);
  if (pos >= len || body[pos] != '{') {
    return JSON_BODY_MALFORMED;
  }
  pos++;
  json_skip_ws(body, len, &pos);

  if (pos < len && body[pos] == '}') {
    pos++;
    goto finished;
  }

  for (;;) {
    uint8_t keybuf[8];
    struct jstr_sink key_sink;
    int is_body_key;

    json_skip_ws(body, len, &pos);
    if (pos >= len || body[pos] != '"') {
      return JSON_BODY_MALFORMED;
    }
    jstr_init(&key_sink, keybuf, sizeof(keybuf), 0);
    if (!json_parse_string(body, len, &pos, &key_sink)) {
      return JSON_BODY_MALFORMED;
    }
    is_body_key = (!key_sink.overflow && key_sink.len == 4 &&
                   memcmp(keybuf, "body", 4) == 0);

    json_skip_ws(body, len, &pos);
    if (pos >= len || body[pos] != ':') {
      return JSON_BODY_MALFORMED;
    }
    pos++;
    json_skip_ws(body, len, &pos);

    if (is_body_key) {
      body_kind = 0;
      jstr_reset(&body_sink);
      if (pos < len && body[pos] == '"') {
        body_kind = 1;
        if (!json_parse_string(body, len, &pos, &body_sink)) {
          return JSON_BODY_MALFORMED;
        }
      } else {
        body_kind = 2;
        if (!json_skip_value(body, len, &pos, 0)) {
          return JSON_BODY_MALFORMED;
        }
      }
    } else {
      if (!json_skip_value(body, len, &pos, 0)) {
        return JSON_BODY_MALFORMED;
      }
    }

    json_skip_ws(body, len, &pos);
    if (pos < len && body[pos] == ',') {
      pos++;
      continue;
    }
    if (pos < len && body[pos] == '}') {
      pos++;
      break;
    }
    return JSON_BODY_MALFORMED;
  }

finished:
  json_skip_ws(body, len, &pos);
  if (pos != len) {
    return JSON_BODY_MALFORMED;
  }
  if (body_kind != 1) {
    return JSON_BODY_REQUIRED;
  }
  if (body_sink.last_cp == 0) {
    return JSON_BODY_REQUIRED;
  }
  if (body_sink.last_cp > 500) {
    return JSON_BODY_TOO_LONG;
  }
  if (trimmed_len_out != NULL) {
    *trimmed_len_out = body_sink.last_len;
  }
  return JSON_BODY_OK;
}

#define SWAR_ONES UINT64_C(0x0101010101010101)
#define SWAR_HIGHS UINT64_C(0x8080808080808080)
#define SWAR_REP(x) (SWAR_ONES * (uint64_t)(x))

static uint64_t swar_zero_byte(uint64_t x) {
  return (x - SWAR_ONES) & ~x & SWAR_HIGHS;
}

static int json_escape_one(uint8_t *dst, size_t *off, size_t cap, uint8_t c) {
  static const char hex[] = "0123456789abcdef";
  size_t need;
  uint8_t tmp[6];
  const uint8_t *out = tmp;

  if (dst == NULL || *off > cap) {
    return -1;
  }

  if (c == '"' || c == '\\') {
    tmp[0] = '\\';
    tmp[1] = c;
    need = 2;
  } else if (c == 0x08u) {
    tmp[0] = '\\';
    tmp[1] = 'b';
    need = 2;
  } else if (c == 0x0Cu) {
    tmp[0] = '\\';
    tmp[1] = 'f';
    need = 2;
  } else if (c == 0x0Au) {
    tmp[0] = '\\';
    tmp[1] = 'n';
    need = 2;
  } else if (c == 0x0Du) {
    tmp[0] = '\\';
    tmp[1] = 'r';
    need = 2;
  } else if (c == 0x09u) {
    tmp[0] = '\\';
    tmp[1] = 't';
    need = 2;
  } else if (c < 0x20u) {
    tmp[0] = '\\';
    tmp[1] = 'u';
    tmp[2] = '0';
    tmp[3] = '0';
    tmp[4] = (uint8_t)hex[c >> 4];
    tmp[5] = (uint8_t)hex[c & 0x0Fu];
    need = 6;
  } else {
    tmp[0] = c;
    need = 1;
  }

  if (need > cap - *off) {
    return -1;
  }
  memcpy(dst + *off, out, need);
  *off += need;
  return 0;
}

size_t json_escape_append(uint8_t *dst, size_t off, size_t cap,
                          const uint8_t *src, size_t len) {
  if (src == NULL) {
    return off;
  }

  while (len >= 8) {
    uint64_t v;
    uint64_t special;
    memcpy(&v, src, 8);
    special = swar_zero_byte(v ^ SWAR_REP(0x22)) |
              swar_zero_byte(v ^ SWAR_REP(0x5C)) |
              ((v - SWAR_REP(0x20)) & ~v & SWAR_HIGHS);

    if (special == 0) {
      if (off > cap || cap - off < 8) {
        return cap + 1;
      }
      memcpy(dst + off, src, 8);
      off += 8;
    } else {
      int i;
      for (i = 0; i < 8; i++) {
        if (json_escape_one(dst, &off, cap, src[i]) != 0) {
          return cap + 1;
        }
      }
    }
    src += 8;
    len -= 8;
  }

  while (len > 0) {
    if (json_escape_one(dst, &off, cap, *src) != 0) {
      return cap + 1;
    }
    src++;
    len--;
  }
  return off;
}

static size_t json_escape_len(const uint8_t *src, size_t len) {
  size_t i;
  size_t total = 0;
  if (src == NULL) {
    return 0;
  }
  for (i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c == '"' || c == '\\' || c == 0x08u || c == 0x0Cu || c == 0x0Au ||
        c == 0x0Du || c == 0x09u) {
      total += 2;
    } else if (c < 0x20u) {
      total += 6;
    } else {
      total++;
    }
  }
  return total;
}

static int b64url_val(uint8_t c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '-') {
    return 62;
  }
  if (c == '_') {
    return 63;
  }
  return -1;
}

static int b64url_decoded_len(size_t n, size_t *out) {
  size_t q;
  size_t r;
  size_t need;
  if ((n & 3u) == 1u) {
    return 0;
  }
  q = n / 4;
  r = n & 3u;
  if (q > (SIZE_MAX - 3) / 3) {
    return 0;
  }
  need = q * 3;
  if (r == 2) {
    need += 1;
  } else if (r == 3) {
    need += 2;
  }
  *out = need;
  return 1;
}

static int b64url_decode(const uint8_t *src, size_t n, uint8_t *out,
                         size_t cap, size_t *outlen) {
  size_t i = 0;
  size_t o = 0;

  while (i + 4 <= n) {
    int a = b64url_val(src[i]);
    int b = b64url_val(src[i + 1]);
    int c = b64url_val(src[i + 2]);
    int d = b64url_val(src[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) {
      return 0;
    }
    if (o + 3 > cap) {
      return 0;
    }
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
    out[o++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
    out[o++] = (uint8_t)(((c & 3) << 6) | d);
    i += 4;
  }

  if (n - i == 2) {
    int a = b64url_val(src[i]);
    int b = b64url_val(src[i + 1]);
    if (a < 0 || b < 0 || (b & 15) != 0 || o + 1 > cap) {
      return 0;
    }
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
  } else if (n - i == 3) {
    int a = b64url_val(src[i]);
    int b = b64url_val(src[i + 1]);
    int c = b64url_val(src[i + 2]);
    if (a < 0 || b < 0 || c < 0 || (c & 3) != 0 || o + 2 > cap) {
      return 0;
    }
    out[o++] = (uint8_t)((a << 2) | (b >> 4));
    out[o++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
  } else if (n - i != 0) {
    return 0;
  }

  *outlen = o;
  return 1;
}

static int ct_equal32(const uint8_t *a, const uint8_t *b) {
  uint8_t d = 0;
  int i;
  for (i = 0; i < 32; i++) {
    d |= (uint8_t)(a[i] ^ b[i]);
  }
  return d == 0;
}

static int jwt_key_is(const uint8_t *key, size_t len, const char *name) {
  size_t n = strlen(name);
  return len == n && memcmp(key, name, n) == 0;
}

static int jwt_header_alg_hs256(const uint8_t *p, size_t n) {
  size_t pos = 0;
  int seen_alg = 0;
  int alg_ok = 0;

  json_skip_ws(p, n, &pos);
  if (pos >= n || p[pos] != '{') {
    return 0;
  }
  pos++;
  json_skip_ws(p, n, &pos);
  if (pos < n && p[pos] == '}') {
    pos++;
    goto finished;
  }

  for (;;) {
    uint8_t keybuf[8];
    struct jstr_sink key_sink;
    int is_alg;

    json_skip_ws(p, n, &pos);
    if (pos >= n || p[pos] != '"') {
      return 0;
    }
    jstr_init(&key_sink, keybuf, sizeof(keybuf), 0);
    if (!json_parse_string(p, n, &pos, &key_sink)) {
      return 0;
    }
    is_alg = (!key_sink.overflow && jwt_key_is(keybuf, key_sink.len, "alg"));

    json_skip_ws(p, n, &pos);
    if (pos >= n || p[pos] != ':') {
      return 0;
    }
    pos++;
    json_skip_ws(p, n, &pos);

    if (is_alg) {
      uint8_t valbuf[8];
      struct jstr_sink val_sink;
      jstr_init(&val_sink, valbuf, sizeof(valbuf), 0);
      if (pos < n && p[pos] == '"') {
        if (!json_parse_string(p, n, &pos, &val_sink)) {
          return 0;
        }
        alg_ok = (!val_sink.overflow && val_sink.len == 5 &&
                  memcmp(valbuf, "HS256", 5) == 0);
      } else {
        if (!json_skip_value(p, n, &pos, 0)) {
          return 0;
        }
        alg_ok = 0;
      }
      seen_alg = 1;
    } else {
      if (!json_skip_value(p, n, &pos, 0)) {
        return 0;
      }
    }

    json_skip_ws(p, n, &pos);
    if (pos < n && p[pos] == ',') {
      pos++;
      continue;
    }
    if (pos < n && p[pos] == '}') {
      pos++;
      break;
    }
    return 0;
  }

finished:
  json_skip_ws(p, n, &pos);
  return pos == n && seen_alg && alg_ok;
}

static int jwt_parse_uid_string(const uint8_t *p, size_t n, int64_t *uid_out) {
  uint64_t v = 0;
  size_t i;
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    uint64_t d;
    if (!is_json_digit(p[i])) {
      return 0;
    }
    d = (uint64_t)(p[i] - '0');
    if (v > (UINT64_C(9007199254740991) - d) / 10) {
      return 0;
    }
    v = v * 10 + d;
  }
  if (v == 0 || v > UINT64_C(9007199254740991)) {
    return 0;
  }
  *uid_out = (int64_t)v;
  return 1;
}

static enum jwt_rc jwt_parse_payload(const uint8_t *p, size_t n,
                                     int64_t now_sec, int64_t *uid_out,
                                     uint8_t *uname_buf, size_t uname_cap,
                                     size_t *uname_len_out) {
  size_t pos = 0;
  int exp_seen = 0;
  int exp_ok = 0;
  int sub_seen = 0;
  int sub_ok = 0;
  int uname_seen = 0;
  int uname_ok = 0;
  int64_t sub_id = 0;
  size_t uname_len = 0;

  json_skip_ws(p, n, &pos);
  if (pos >= n || p[pos] != '{') {
    return JWT_ERR_INVALID;
  }
  pos++;
  json_skip_ws(p, n, &pos);
  if (pos < n && p[pos] == '}') {
    pos++;
    goto finished;
  }

  for (;;) {
    uint8_t keybuf[8];
    struct jstr_sink key_sink;
    int is_exp;
    int is_sub;
    int is_uname;

    json_skip_ws(p, n, &pos);
    if (pos >= n || p[pos] != '"') {
      return JWT_ERR_INVALID;
    }
    jstr_init(&key_sink, keybuf, sizeof(keybuf), 0);
    if (!json_parse_string(p, n, &pos, &key_sink)) {
      return JWT_ERR_INVALID;
    }
    is_exp = (!key_sink.overflow && jwt_key_is(keybuf, key_sink.len, "exp"));
    is_sub = (!key_sink.overflow && jwt_key_is(keybuf, key_sink.len, "sub"));
    is_uname =
        (!key_sink.overflow && jwt_key_is(keybuf, key_sink.len, "username"));

    json_skip_ws(p, n, &pos);
    if (pos >= n || p[pos] != ':') {
      return JWT_ERR_INVALID;
    }
    pos++;
    json_skip_ws(p, n, &pos);

    if (is_exp) {
      exp_seen = 1;
      exp_ok = 0;
      if (pos < n && (p[pos] == '-' || is_json_digit(p[pos]))) {
        size_t num_start = pos;
        char *endptr = NULL;
        double value;
        if (!json_scan_number(p, n, &pos)) {
          return JWT_ERR_INVALID;
        }
        value = strtod((const char *)(p + num_start), &endptr);
        if (endptr == (char *)(p + pos)) {
          exp_ok = (value > (double)now_sec);
        }
      } else {
        if (!json_skip_value(p, n, &pos, 0)) {
          return JWT_ERR_INVALID;
        }
      }
    } else if (is_sub) {
      uint8_t subbuf[32];
      struct jstr_sink sub_sink;
      sub_seen = 1;
      sub_ok = 0;
      jstr_init(&sub_sink, subbuf, sizeof(subbuf), 0);
      if (pos < n && p[pos] == '"') {
        if (!json_parse_string(p, n, &pos, &sub_sink)) {
          return JWT_ERR_INVALID;
        }
        if (!sub_sink.overflow) {
          sub_ok = jwt_parse_uid_string(subbuf, sub_sink.len, &sub_id);
        }
      } else {
        if (!json_skip_value(p, n, &pos, 0)) {
          return JWT_ERR_INVALID;
        }
      }
    } else if (is_uname) {
      struct jstr_sink uname_sink;
      uname_seen = 1;
      uname_ok = 0;
      uname_len = 0;
      jstr_init(&uname_sink, uname_buf, uname_cap, 0);
      if (pos < n && p[pos] == '"') {
        if (!json_parse_string(p, n, &pos, &uname_sink)) {
          return JWT_ERR_INVALID;
        }
        uname_len = uname_sink.len;
        uname_ok = !uname_sink.overflow;
      } else {
        if (!json_skip_value(p, n, &pos, 0)) {
          return JWT_ERR_INVALID;
        }
      }
    } else {
      if (!json_skip_value(p, n, &pos, 0)) {
        return JWT_ERR_INVALID;
      }
    }

    json_skip_ws(p, n, &pos);
    if (pos < n && p[pos] == ',') {
      pos++;
      continue;
    }
    if (pos < n && p[pos] == '}') {
      pos++;
      break;
    }
    return JWT_ERR_INVALID;
  }

finished:
  json_skip_ws(p, n, &pos);
  if (pos != n) {
    return JWT_ERR_INVALID;
  }
  if (!exp_seen || !exp_ok) {
    return JWT_ERR_INVALID;
  }
  if (!sub_seen || !sub_ok || !uname_seen || !uname_ok) {
    return JWT_ERR_PAYLOAD;
  }
  if (uid_out != NULL) {
    *uid_out = sub_id;
  }
  if (uname_len_out != NULL) {
    *uname_len_out = uname_len;
  }
  return JWT_OK;
}

enum jwt_rc jwt_verify_hs256(const uint8_t *auth, size_t auth_len,
                             int64_t now_sec, int64_t *uid_out,
                             uint8_t *uname_buf, size_t uname_cap,
                             size_t *uname_len_out) {
  const uint8_t *tok;
  size_t tok_len;
  const uint8_t *dot1 = NULL;
  const uint8_t *dot2 = NULL;
  size_t h_len;
  size_t p_len;
  size_t s_len;
  uint8_t mac[32];
  uint8_t sig[32];
  size_t sig_len = 0;
  size_t h_need;
  size_t p_need;
  uint8_t h_local[512];
  uint8_t p_local[4096];
  uint8_t *h_buf = h_local;
  uint8_t *p_buf = p_local;
  uint8_t *heap = NULL;
  enum jwt_rc rc;
  size_t i;

  if (uname_len_out != NULL) {
    *uname_len_out = 0;
  }

  if (auth == NULL || auth_len < 7 ||
      memcmp(auth, "Bearer ", 7) != 0) {
    return JWT_ERR_MISSING;
  }

  tok = auth + 7;
  tok_len = auth_len - 7;

  for (i = 0; i < tok_len; i++) {
    if (tok[i] == '.') {
      if (dot1 == NULL) {
        dot1 = tok + i;
      } else if (dot2 == NULL) {
        dot2 = tok + i;
      } else {
        return JWT_ERR_INVALID;
      }
    }
  }
  if (dot1 == NULL || dot2 == NULL || dot1 == tok || dot1 + 1 == dot2 ||
      dot2 + 1 == tok + tok_len) {
    return JWT_ERR_INVALID;
  }

  h_len = (size_t)(dot1 - tok);
  p_len = (size_t)(dot2 - dot1 - 1);
  s_len = (size_t)(tok + tok_len - dot2 - 1);

  hmac_sha256(tok, (size_t)(dot2 - tok), mac);
  if (!b64url_decode(dot2 + 1, s_len, sig, sizeof(sig), &sig_len) ||
      sig_len != 32 || !ct_equal32(mac, sig)) {
    return JWT_ERR_INVALID;
  }

  if (!b64url_decoded_len(h_len, &h_need) ||
      !b64url_decoded_len(p_len, &p_need) || h_need == 0 || p_need == 0) {
    return JWT_ERR_INVALID;
  }

  if (h_need >= sizeof(h_local) || p_need >= sizeof(p_local)) {
    if (h_need > SIZE_MAX - p_need - 2) {
      return JWT_ERR_INVALID;
    }
    heap = (uint8_t *)malloc(h_need + 1 + p_need + 1);
    if (heap == NULL) {
      return JWT_ERR_INVALID;
    }
    h_buf = heap;
    p_buf = heap + h_need + 1;
  }

  {
    size_t got = 0;
    if (!b64url_decode(dot1 + 1, p_len, p_buf, p_need, &got) ||
        got != p_need) {
      free(heap);
      return JWT_ERR_INVALID;
    }
  }
  {
    size_t got = 0;
    if (!b64url_decode(tok, h_len, h_buf, h_need, &got) || got != h_need) {
      free(heap);
      return JWT_ERR_INVALID;
    }
  }

  h_buf[h_need] = 0;
  p_buf[p_need] = 0;

  if (!jwt_header_alg_hs256(h_buf, h_need)) {
    free(heap);
    return JWT_ERR_INVALID;
  }

  rc = jwt_parse_payload(p_buf, p_need, now_sec, uid_out, uname_buf,
                         uname_cap, uname_len_out);
  free(heap);
  return rc;
}

#define HTTP_MAX_HEAD 16384u
#define HTTP_MAX_BODY 1048576u

static int ascii_case_eq(const uint8_t *s, size_t n, const char *lit) {
  size_t i;
  size_t m = strlen(lit);
  if (n != m) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (uint8_t)(c + ('a' - 'A'));
    }
    if (c != (uint8_t)lit[i]) {
      return 0;
    }
  }
  return 1;
}

static int header_has_token(const uint8_t *v, size_t n, const char *token) {
  size_t i = 0;
  while (i < n) {
    size_t start;
    size_t end;
    while (i < n && is_http_ows(v[i])) {
      i++;
    }
    start = i;
    while (i < n && v[i] != ',') {
      i++;
    }
    end = i;
    while (end > start && is_http_ows(v[end - 1])) {
      end--;
    }
    if (end > start && ascii_case_eq(v + start, end - start, token)) {
      return 1;
    }
    if (i < n && v[i] == ',') {
      i++;
    }
  }
  return 0;
}

static int64_t parse_path_id(const uint8_t *p, size_t n) {
  uint64_t v = 0;
  size_t i;
  if (n == 0) {
    return -1;
  }
  if (n > 1 && p[0] == '0') {
    return -1;
  }
  for (i = 0; i < n; i++) {
    uint64_t d;
    if (!is_json_digit(p[i])) {
      return -1;
    }
    d = (uint64_t)(p[i] - '0');
    if (v > (UINT64_C(9007199254740991) - d) / 10) {
      return -1;
    }
    v = v * 10 + d;
  }
  if (v == 0 || v > UINT64_C(9007199254740991)) {
    return -1;
  }
  return (int64_t)v;
}

static void set_route(struct http_req *out, int method, const uint8_t *path,
                      size_t path_len) {
  size_t n = path_len;
  size_t i;
  for (i = 0; i < n; i++) {
    if (path[i] == '?') {
      n = i;
      break;
    }
  }

  out->route = ROUTE_NOT_FOUND;
  out->post_id = 0;

  if (method == 1) {
    if (n == 7 && memcmp(path, "/health", 7) == 0) {
      out->route = ROUTE_HEALTH;
      return;
    }
    if (n == 5 && memcmp(path, "/feed", 5) == 0) {
      out->route = ROUTE_FEED;
      return;
    }
    if (n > 7 && memcmp(path, "/posts/", 7) == 0) {
      size_t rest_len = n - 7;
      const uint8_t *rest = path + 7;
      if (memchr(rest, '/', rest_len) == NULL) {
        out->route = ROUTE_POST_GET;
        out->post_id = parse_path_id(rest, rest_len);
      }
    }
    return;
  }

  if (method == 2) {
    if (n == 6 && memcmp(path, "/posts", 6) == 0) {
      out->route = ROUTE_POST_CREATE;
      return;
    }
    if (n > 7 && memcmp(path, "/posts/", 7) == 0) {
      const uint8_t *rest = path + 7;
      size_t rest_len = n - 7;
      if (rest_len > 5 && memcmp(rest + rest_len - 5, "/like", 5) == 0) {
        size_t id_len = rest_len - 5;
        if (memchr(rest, '/', id_len) == NULL) {
          out->route = ROUTE_POST_LIKE;
          out->post_id = parse_path_id(rest, id_len);
        }
      }
    }
    return;
  }
}

static int parse_content_length(const uint8_t *p, size_t n, size_t *out) {
  uint64_t v = 0;
  size_t i;
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    uint64_t d;
    if (!is_json_digit(p[i])) {
      return 0;
    }
    d = (uint64_t)(p[i] - '0');
    if (v > (UINT64_C(1048576) - d) / 10) {
      return 0;
    }
    v = v * 10 + d;
  }
  *out = (size_t)v;
  return 1;
}

ssize_t http_parse_req(const uint8_t *buf, size_t len, struct http_req *out) {
  const uint8_t *cr;
  size_t request_line_len;
  size_t sp1 = SIZE_MAX;
  size_t sp2 = SIZE_MAX;
  size_t i;
  int method;
  int http11;
  size_t pos;
  size_t head_end = 0;
  size_t content_length = 0;
  int have_content_length = 0;
  int conn_close = 0;
  int conn_keep = 0;
  size_t path_off;
  size_t path_len;

  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  if (len == 0) {
    return 0;
  }
  if (buf == NULL) {
    return -1;
  }

  cr = (const uint8_t *)memchr(buf, '\r', len);
  if (cr == NULL) {
    return len > HTTP_MAX_HEAD ? -1 : 0;
  }
  if ((size_t)(cr - buf) + 1 >= len) {
    return len > HTTP_MAX_HEAD ? -1 : 0;
  }
  if (cr[1] != '\n') {
    return -1;
  }

  request_line_len = (size_t)(cr - buf);
  if (request_line_len + 2 > HTTP_MAX_HEAD) {
    return -1;
  }

  for (i = 0; i < request_line_len; i++) {
    if (buf[i] == ' ') {
      if (sp1 == SIZE_MAX) {
        sp1 = i;
      } else if (sp2 == SIZE_MAX) {
        sp2 = i;
      } else {
        return -1;
      }
    }
  }
  if (sp1 == SIZE_MAX || sp2 == SIZE_MAX || sp1 == 0 || sp2 == sp1 + 1 ||
      sp2 + 1 >= request_line_len) {
    return -1;
  }

  if (sp1 == 3 && memcmp(buf, "GET", 3) == 0) {
    method = 1;
  } else if (sp1 == 4 && memcmp(buf, "POST", 4) == 0) {
    method = 2;
  } else {
    return -1;
  }

  if (request_line_len - sp2 - 1 != 8 ||
      memcmp(buf + sp2 + 1, "HTTP/1.1", 8) != 0) {
    if (request_line_len - sp2 - 1 != 8 ||
        memcmp(buf + sp2 + 1, "HTTP/1.0", 8) != 0) {
      return -1;
    }
    http11 = 0;
  } else {
    http11 = 1;
  }

  path_off = sp1 + 1;
  path_len = sp2 - sp1 - 1;

  pos = request_line_len + 2;
  for (;;) {
    const uint8_t *line_cr;
    size_t line_end;
    size_t line_len;

    if (pos >= len) {
      return len > HTTP_MAX_HEAD ? -1 : 0;
    }
    line_cr = (const uint8_t *)memchr(buf + pos, '\r', len - pos);
    if (line_cr == NULL) {
      return len > HTTP_MAX_HEAD ? -1 : 0;
    }
    line_end = (size_t)(line_cr - buf);
    if (line_end + 1 >= len) {
      return len > HTTP_MAX_HEAD ? -1 : 0;
    }
    if (buf[line_end + 1] != '\n') {
      return -1;
    }
    if (line_end + 2 > HTTP_MAX_HEAD) {
      return -1;
    }
    line_len = line_end - pos;
    if (line_len == 0) {
      head_end = line_end + 2;
      break;
    }

    {
      const uint8_t *colon = (const uint8_t *)memchr(buf + pos, ':', line_len);
      size_t name_len;
      const uint8_t *value;
      size_t value_len;
      size_t value_start;
      size_t value_end;

      if (colon == NULL) {
        return -1;
      }
      name_len = (size_t)(colon - (buf + pos));
      if (name_len == 0) {
        return -1;
      }

      value = colon + 1;
      value_len = line_end - (size_t)(colon - buf) - 1;
      value_start = 0;
      value_end = value_len;
      while (value_start < value_end && is_http_ows(value[value_start])) {
        value_start++;
      }
      while (value_end > value_start && is_http_ows(value[value_end - 1])) {
        value_end--;
      }

      if (ascii_case_eq(buf + pos, name_len, "content-length")) {
        size_t parsed;
        if (!parse_content_length(value + value_start,
                                  value_end - value_start, &parsed)) {
          return -1;
        }
        if (have_content_length && content_length != parsed) {
          return -1;
        }
        content_length = parsed;
        have_content_length = 1;
      } else if (ascii_case_eq(buf + pos, name_len, "transfer-encoding")) {
        return -1;
      } else if (ascii_case_eq(buf + pos, name_len, "connection")) {
        if (header_has_token(value + value_start,
                             value_end - value_start, "close")) {
          conn_close = 1;
        }
        if (header_has_token(value + value_start,
                             value_end - value_start, "keep-alive")) {
          conn_keep = 1;
        }
      } else if (ascii_case_eq(buf + pos, name_len, "authorization")) {
        out->auth = value + value_start;
        out->auth_len = value_end - value_start;
      }
    }

    pos = line_end + 2;
  }

  if (content_length > HTTP_MAX_BODY) {
    return -1;
  }
  if (head_end > HTTP_MAX_HEAD) {
    return -1;
  }
  if (len - head_end < content_length) {
    return 0;
  }

  out->body = buf + head_end;
  out->body_len = content_length;
  out->consumed = head_end + content_length;
  out->keep_alive = http11 ? 1 : 0;
  if (conn_close) {
    out->keep_alive = 0;
  } else if (!http11 && conn_keep) {
    out->keep_alive = 1;
  }

  set_route(out, method, buf + path_off, path_len);
  return (ssize_t)out->consumed;
}

struct wbuf {
  uint8_t *dst;
  size_t cap;
  size_t len;
  int bad;
};

static void w_mem(struct wbuf *w, const void *src, size_t n) {
  if (w->bad) {
    return;
  }
  if (w->len > w->cap || n > w->cap - w->len) {
    w->bad = 1;
    return;
  }
  if (n > 0) {
    memcpy(w->dst + w->len, src, n);
  }
  w->len += n;
}

static void w_str(struct wbuf *w, const char *s) {
  w_mem(w, s, strlen(s));
}

static void w_ch(struct wbuf *w, uint8_t c) {
  if (w->bad) {
    return;
  }
  if (w->len >= w->cap) {
    w->bad = 1;
    return;
  }
  w->dst[w->len++] = c;
}

static void w_u64(struct wbuf *w, uint64_t v) {
  uint8_t tmp[20];
  size_t n = 0;
  if (v == 0) {
    w_ch(w, '0');
    return;
  }
  while (v > 0) {
    tmp[n++] = (uint8_t)('0' + (v % 10));
    v /= 10;
  }
  while (n > 0) {
    w_ch(w, tmp[--n]);
  }
}

static void w_i64(struct wbuf *w, int64_t v) {
  uint64_t mag;
  if (v < 0) {
    w_ch(w, '-');
    mag = (uint64_t)(-(v + 1)) + 1;
  } else {
    mag = (uint64_t)v;
  }
  w_u64(w, mag);
}

static size_t u64_dec_len(uint64_t v) {
  size_t n = 1;
  while (v >= 10) {
    v /= 10;
    n++;
  }
  return n;
}

static size_t i64_dec_len(int64_t v) {
  if (v < 0) {
    return 1 + u64_dec_len((uint64_t)(-(v + 1)) + 1);
  }
  return u64_dec_len((uint64_t)v);
}

static void w_escape(struct wbuf *w, const uint8_t *src, size_t len) {
  size_t off;
  if (w->bad) {
    return;
  }
  off = json_escape_append(w->dst, w->len, w->cap, src, len);
  if (off > w->cap) {
    w->bad = 1;
  } else {
    w->len = off;
  }
}

static const char *reason_for(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 422:
      return "Unprocessable Entity";
    case 429:
      return "Too Many Requests";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "Error";
  }
}

static void w_headers(struct wbuf *w, int status, const char *reason,
                      size_t body_len, int keep_alive) {
  if (reason == NULL) {
    reason = reason_for(status);
  }
  w_str(w, "HTTP/1.1 ");
  w_i64(w, (int64_t)status);
  w_ch(w, ' ');
  w_str(w, reason);
  w_str(w, "\r\nContent-Type: application/json\r\nContent-Length: ");
  w_u64(w, (uint64_t)body_len);
  w_str(w, "\r\nConnection: ");
  w_str(w, keep_alive ? "keep-alive" : "close");
  w_str(w, "\r\n\r\n");
}

static size_t w_finish(const struct wbuf *w) {
  return w->bad ? 0 : w->len;
}

size_t resp_write_error(uint8_t *dst, size_t cap, int status, const char *msg,
                        int keep_alive) {
  static const char prefix[] = "{\"error\":\"";
  static const char suffix[] = "\"}";
  size_t msg_len = msg != NULL ? strlen(msg) : 0;
  size_t escaped_len =
      json_escape_len((const uint8_t *)msg, msg_len);
  size_t body_len = (sizeof(prefix) - 1) + escaped_len + (sizeof(suffix) - 1);
  struct wbuf w;
  w.dst = dst;
  w.cap = cap;
  w.len = 0;
  w.bad = 0;

  w_headers(&w, status, reason_for(status), body_len, keep_alive);
  w_str(&w, prefix);
  w_escape(&w, (const uint8_t *)msg, msg_len);
  w_str(&w, suffix);
  return w_finish(&w);
}

size_t resp_write_bad_request(uint8_t *dst, size_t cap) {
  return resp_write_error(dst, cap, 400, "bad request", 0);
}

size_t resp_write_health(uint8_t *dst, size_t cap, int ok, int64_t uptime_s,
                         const char *err, int keep_alive) {
  struct wbuf w;
  w.dst = dst;
  w.cap = cap;
  w.len = 0;
  w.bad = 0;

  if (ok) {
    static const char prefix[] = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":";
    size_t body_len = (sizeof(prefix) - 1) + i64_dec_len(uptime_s) + 1;
    w_headers(&w, 200, "OK", body_len, keep_alive);
    w_str(&w, prefix);
    w_i64(&w, uptime_s);
    w_ch(&w, '}');
  } else {
    static const char prefix[] =
        "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"";
    static const char suffix[] = "\"}";
    size_t err_len = err != NULL ? strlen(err) : 0;
    size_t escaped_len = json_escape_len((const uint8_t *)err, err_len);
    size_t body_len =
        (sizeof(prefix) - 1) + escaped_len + (sizeof(suffix) - 1);
    w_headers(&w, 503, "Service Unavailable", body_len, keep_alive);
    w_str(&w, prefix);
    w_escape(&w, (const uint8_t *)err, err_len);
    w_str(&w, suffix);
  }
  return w_finish(&w);
}

size_t resp_write_json_body(uint8_t *dst, size_t cap, int status,
                            const char *reason, const uint8_t *json_body,
                            size_t json_len, int keep_alive) {
  struct wbuf w;
  w.dst = dst;
  w.cap = cap;
  w.len = 0;
  w.bad = 0;

  w_headers(&w, status, reason != NULL ? reason : reason_for(status), json_len,
            keep_alive);
  w_mem(&w, json_body, json_len);
  return w_finish(&w);
}

size_t resp_write_post_created(uint8_t *dst, size_t cap, int64_t id,
                               const uint8_t *body, size_t body_len,
                               const char *created_at, size_t created_len,
                               const uint8_t *author, size_t author_len,
                               int keep_alive) {
  static const char p1[] = "{\"post\":{\"id\":";
  static const char p2[] = ",\"body\":\"";
  static const char p3[] = "\",\"created_at\":\"";
  static const char p4[] = "\",\"author\":\"";
  static const char p5[] = "\",\"like_count\":0}}";
  size_t body_esc = json_escape_len(body, body_len);
  size_t created_esc =
      json_escape_len((const uint8_t *)created_at, created_len);
  size_t author_esc = json_escape_len(author, author_len);
  size_t total = (sizeof(p1) - 1) + i64_dec_len(id) + (sizeof(p2) - 1) +
                 body_esc + (sizeof(p3) - 1) + created_esc +
                 (sizeof(p4) - 1) + author_esc + (sizeof(p5) - 1);
  struct wbuf w;
  w.dst = dst;
  w.cap = cap;
  w.len = 0;
  w.bad = 0;

  w_headers(&w, 201, "Created", total, keep_alive);
  w_str(&w, p1);
  w_i64(&w, id);
  w_str(&w, p2);
  w_escape(&w, body, body_len);
  w_str(&w, p3);
  w_escape(&w, (const uint8_t *)created_at, created_len);
  w_str(&w, p4);
  w_escape(&w, author, author_len);
  w_str(&w, p5);
  return w_finish(&w);
}

size_t resp_write_like(uint8_t *dst, size_t cap, int already_liked,
                       int64_t post_id, int keep_alive) {
  static const char prefix[] = "{\"liked\":true,\"already_liked\":";
  static const char middle[] = ",\"post_id\":";
  const char *flag = already_liked ? "true" : "false";
  size_t flag_len = already_liked ? 4 : 5;
  size_t total = (sizeof(prefix) - 1) + flag_len + (sizeof(middle) - 1) +
                 i64_dec_len(post_id) + 1;
  int status = already_liked ? 200 : 201;
  struct wbuf w;
  w.dst = dst;
  w.cap = cap;
  w.len = 0;
  w.bad = 0;

  w_headers(&w, status, status == 201 ? "Created" : "OK", total, keep_alive);
  w_str(&w, prefix);
  w_mem(&w, flag, flag_len);
  w_str(&w, middle);
  w_i64(&w, post_id);
  w_ch(&w, '}');
  return w_finish(&w);
}
