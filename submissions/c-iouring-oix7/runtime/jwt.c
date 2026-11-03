/* FIPS 180-4 SHA-256, HMAC-SHA256 with precomputed ipad/opad states,
 * strict unpadded base64url decoding, and HS256 JWT claim verification. */

static const u32 K256[64] = {
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
  0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static const u32 SHA256_IV[8] = {
  0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

typedef struct sha256_state {
  u32 h[8];
} sha256_state;

static inline u32 ror32(u32 x, u32 n) {
  return (x >> n) | (x << (32u - n));
}

#define SHA256_S1(x)      (ror32(x,  6) ^ ror32(x, 11) ^ ror32(x, 25))
#define SHA256_S0(x)      (ror32(x,  2) ^ ror32(x, 13) ^ ror32(x, 22))
#define SHA256_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define SHA256_RND(A,B,C,D,E,F,G,H,ki) do {                             \
    u32 t1_ = (H) + SHA256_S1(E) + SHA256_CH(E,F,G) + K256[ki] + w[ki]; \
    u32 t2_ = SHA256_S0(A) + SHA256_MAJ(A,B,C);                         \
    (D) += t1_;                                                         \
    (H)  = t1_ + t2_;                                                   \
  } while (0)

#define SHA256_RND8(A,B,C,D,E,F,G,H,ki) do { \
    SHA256_RND(A,B,C,D,E,F,G,H,(ki)+0);      \
    SHA256_RND(H,A,B,C,D,E,F,G,(ki)+1);      \
    SHA256_RND(G,H,A,B,C,D,E,F,(ki)+2);      \
    SHA256_RND(F,G,H,A,B,C,D,E,(ki)+3);      \
    SHA256_RND(E,F,G,H,A,B,C,D,(ki)+4);      \
    SHA256_RND(D,E,F,G,H,A,B,C,(ki)+5);      \
    SHA256_RND(C,D,E,F,G,H,A,B,(ki)+6);      \
    SHA256_RND(B,C,D,E,F,G,H,A,(ki)+7);      \
  } while (0)

static void sha256_block(sha256_state *s, const u8 b[64]) {
  u32 w[64];
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  memcpy(w, b, 64);
  for (u32 i = 0; i < 16; i++) w[i] = __builtin_bswap32(w[i]);
#else
  for (u32 i = 0; i < 16; i++) {
    w[i] = ((u32)b[4*i]   << 24) | ((u32)b[4*i+1] << 16) |
           ((u32)b[4*i+2] <<  8) |  (u32)b[4*i+3];
  }
#endif
  for (u32 i = 16; i < 64; i++) {
    u32 s0 = ror32(w[i-15], 7)  ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
    u32 s1 = ror32(w[i-2],  17) ^ ror32(w[i-2],  19) ^ (w[i-2]  >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  u32 a = s->h[0], b0 = s->h[1], c = s->h[2], d = s->h[3];
  u32 e = s->h[4], f  = s->h[5], g = s->h[6], h = s->h[7];
  SHA256_RND8(a, b0, c, d, e, f, g, h,  0);
  SHA256_RND8(a, b0, c, d, e, f, g, h,  8);
  SHA256_RND8(a, b0, c, d, e, f, g, h, 16);
  SHA256_RND8(a, b0, c, d, e, f, g, h, 24);
  SHA256_RND8(a, b0, c, d, e, f, g, h, 32);
  SHA256_RND8(a, b0, c, d, e, f, g, h, 40);
  SHA256_RND8(a, b0, c, d, e, f, g, h, 48);
  SHA256_RND8(a, b0, c, d, e, f, g, h, 56);
  s->h[0] += a;  s->h[1] += b0; s->h[2] += c; s->h[3] += d;
  s->h[4] += e;  s->h[5] += f;  s->h[6] += g; s->h[7] += h;
}

#undef SHA256_RND8
#undef SHA256_RND
#undef SHA256_MAJ
#undef SHA256_CH
#undef SHA256_S0
#undef SHA256_S1

static void sha256_finish(sha256_state *s, const u8 *msg, size_t n, u64 total_bytes, u8 out[32]) {
  while (n >= 64) {
    sha256_block(s, msg);
    msg += 64;
    n -= 64;
  }
  u8 pad[64];
  memset(pad, 0, 64);
  if (n > 0) memcpy(pad, msg, n);
  pad[n] = 0x80;
  if (n >= 56) {
    sha256_block(s, pad);
    memset(pad, 0, 64);
  }
  u64 bits = total_bytes * 8;
  for (u32 i = 0; i < 8; i++)
    pad[56 + i] = (u8)(bits >> (56 - 8*i));
  sha256_block(s, pad);
  for (u32 i = 0; i < 8; i++) {
    out[4*i]   = (u8)(s->h[i] >> 24);
    out[4*i+1] = (u8)(s->h[i] >> 16);
    out[4*i+2] = (u8)(s->h[i] >>  8);
    out[4*i+3] = (u8)(s->h[i]);
  }
}

static sha256_state hmac_inner, hmac_outer;

static void hmac_init(const u8 *key, size_t n) {
  u8 k[64], block[64];
  memset(k, 0, 64);
  if (n > 64) {
    sha256_state ks;
    memcpy(ks.h, SHA256_IV, 32);
    sha256_finish(&ks, key, n, (u64)n, k);
  } else {
    memcpy(k, key, n);
  }
  memcpy(hmac_inner.h, SHA256_IV, 32);
  for (int i = 0; i < 64; i++) block[i] = (u8)(k[i] ^ 0x36);
  sha256_block(&hmac_inner, block);

  memcpy(hmac_outer.h, SHA256_IV, 32);
  for (int i = 0; i < 64; i++) block[i] = (u8)(k[i] ^ 0x5c);
  sha256_block(&hmac_outer, block);
}

static void hmac_sha256(const u8 *msg, size_t n, u8 out[32]) {
  u8 inner_hash[32];
  sha256_state s = hmac_inner;
  sha256_finish(&s, msg, n, 64 + (u64)n, inner_hash);
  s = hmac_outer;
  sha256_finish(&s, inner_hash, 32, 64 + 32, out);
}

static const i8 b64_tab[256] = {
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0x00..0x0f */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0x10..0x1f */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,  /* 0x20..0x2f */
  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,  /* 0x30..0x3f */
  -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,  /* 0x40..0x4f */
  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,  /* 0x50..0x5f */
  -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,  /* 0x60..0x6f */
  41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,  /* 0x70..0x7f */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0x80..0x8f */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0x90..0x9f */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0xa0..0xaf */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0xb0..0xbf */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0xc0..0xcf */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0xd0..0xdf */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  /* 0xe0..0xef */
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1   /* 0xf0..0xff */
};

static int b64url_decode(const u8 *s, u32 n, u8 *out, u32 cap) {
  if ((n & 3u) == 1u) return -1;
  u32 nq = n & ~3u;
  u32 olen = 0;
  u32 i = 0;
  while (i < nq) {
    i8 v0 = b64_tab[s[i]];
    i8 v1 = b64_tab[s[i + 1]];
    i8 v2 = b64_tab[s[i + 2]];
    i8 v3 = b64_tab[s[i + 3]];
    if (UNLIKELY(((v0 | v1 | v2 | v3) < 0) | (cap - olen < 3u))) break;
    u32 o = olen;
    out[o]     = (u8)(((u32)v0 << 2) | ((u32)v1 >> 4));
    out[o + 1] = (u8)(((u32)v1 << 4) | ((u32)v2 >> 2));
    out[o + 2] = (u8)(((u32)v2 << 6) | (u32)v3);
    olen = o + 3;
    i += 4;
  }
  u32 acc = 0, bits = 0;
  for (; i < n; i++) {
    i8 v = b64_tab[s[i]];
    if (UNLIKELY(v < 0)) return -1;
    acc = (acc << 6) | (u32)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (olen >= cap) return -1;
      out[olen++] = (u8)(acc >> bits);
      acc &= (1u << bits) - 1u;
    }
  }
  if (bits && acc) return -1;
  return (int)olen;
}

static int jwt_exp_after(const u8 *s, u32 n, i64 now) {
  u32 i = 0;
  int neg = 0, digits = 0, e = 0;
  u64 mant = 0;

  if (i < n && s[i] == '-') { neg = 1; i++; }
  if (i >= n) return 0;
  if (s[i] == '0') {
    i++;
    if (i < n && s[i] >= '0' && s[i] <= '9') return 0;
  } else if (s[i] >= '1' && s[i] <= '9') {
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (digits < 19) { mant = mant * 10 + (u64)(s[i] - '0'); digits++; }
      else e++;
      i++;
    }
  } else {
    return 0;
  }
  if (i < n && s[i] == '.') {
    i++;
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (digits < 19) { mant = mant * 10 + (u64)(s[i] - '0'); digits++; e--; }
      i++;
    }
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    int eneg = 0, ev = 0;
    i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) { eneg = (s[i] == '-'); i++; }
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (ev < 4096) ev = ev * 10 + (s[i] - '0');
      i++;
    }
    e += eneg ? -ev : ev;
  }
  if (i != n) return 0;
  if (neg) return 0;
  if (now < 0) return 1;
  if (mant == 0) return 0;
  if (e >= 19) return 1;
  if (e >= 0) {
    u64 p = 1;
    for (int k = 0; k < e; k++) p *= 10;
    if (mant > (u64)-1 / p) return 1;
    return mant * p > (u64)now;
  }
  int d = -e;
  if (d >= 19) return 0;
  u64 p = 1;
  for (int k = 0; k < d; k++) p *= 10;
  if ((u64)now > (u64)-1 / p) return 0;
  return mant > (u64)now * p;
}

static int jwt_verify(const u8 *t, u32 n, i64 now, claims_t *c) {
  int std_hdr = (n > 80 && t[36] == '.' &&
                 memcmp(t, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.", 37) == 0);
  const u8 *d1 = std_hdr ? (t + 36) : (const u8 *)memchr(t, '.', n);
  if (!d1) return JWT_INVALID;
  u32 hl = std_hdr ? 36u : (u32)(d1 - t);
  u32 rem1 = n - hl - 1;
  const u8 *d2 = (const u8 *)memchr(d1 + 1, '.', rem1);
  if (!d2) return JWT_INVALID;
  u32 pl = (u32)(d2 - (d1 + 1));
  u32 sl = n - (u32)(d2 - t) - 1;
  if (hl == 0 || pl == 0 || sl != 43 || hl > 512 || pl > 2048) return JWT_INVALID;
  if (memchr(d2 + 1, '.', sl)) return JWT_INVALID;

  u8 sig[32], mac[32];
  if (b64url_decode(d2 + 1, sl, sig, sizeof sig) != 32) return JWT_INVALID;
  hmac_sha256(t, (size_t)(d2 - t), mac);
  u32 diff = 0;
  for (int i = 0; i < 32; i++) diff |= (u32)(sig[i] ^ mac[i]);
  if (diff != 0) return JWT_INVALID;

  u8 buf[2048];
  if (!std_hdr) {
    int hn = b64url_decode(t, hl, buf, sizeof buf);
    if (hn <= 0) return JWT_INVALID;
    jmember hm[1] = {{"alg", 3, 0, 0, 0, 0}};
    if (json_members(buf, (u32)hn, hm, 1) != 0) return JWT_INVALID;
    if (hm[0].type != JT_STRING || hm[0].escaped ||
        hm[0].v_len != 5 || memcmp(hm[0].v, "HS256", 5) != 0)
      return JWT_INVALID;
  }
  int pn = b64url_decode(d1 + 1, pl, buf, sizeof buf);
  if (pn <= 0) return JWT_INVALID;
  jmember pm[3] = {{"exp", 3, 0, 0, 0, 0}, {"sub", 3, 0, 0, 0, 0}, {"username", 8, 0, 0, 0, 0}};
  if (json_members(buf, (u32)pn, pm, 3) != 0) return JWT_INVALID;

  if (pm[0].type != JT_NUMBER || !jwt_exp_after(pm[0].v, pm[0].v_len, now))
    return JWT_INVALID;

  if (pm[1].type != JT_STRING || pm[1].escaped || pm[1].v_len < 1 || pm[1].v_len > 15)
    return JWT_PAYLOAD;
  i64 uid = 0;
  for (u32 i = 0; i < pm[1].v_len; i++) {
    u8 ch = pm[1].v[i];
    if (ch < '0' || ch > '9') return JWT_PAYLOAD;
    uid = uid * 10 + (ch - '0');
  }
  if (uid <= 0) return JWT_PAYLOAD;

  if (pm[2].type != JT_STRING || pm[2].v_len > sizeof c->name)
    return JWT_PAYLOAD;
  if (pm[2].escaped) {
    c->name_len = json_unescape(pm[2].v, pm[2].v_len, c->name);
  } else {
    memcpy(c->name, pm[2].v, pm[2].v_len);
    c->name_len = pm[2].v_len;
  }
  c->uid = uid;
  return JWT_OK;
}
