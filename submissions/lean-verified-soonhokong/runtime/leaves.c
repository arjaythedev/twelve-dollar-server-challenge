/* Byte-level leaf functions. Each one implements a Lean definition in Feed/Bytes.lean, which is
 * its specification: the Lean body is what the proofs reason about, and `@[extern]` makes the
 * compiled program call this C code instead. They know nothing about the API; they copy, scan
 * and escape bytes. `test/leaves.lean` checks each against its Lean definition on random inputs. */
#include "rt.h"
#include <immintrin.h>
#include <stdlib.h>

/* Bytes that JSON requires escaping inside a string: '"', '\\' and the controls 0x00-0x1f. */
static inline unsigned esc_mask32(__m256i v) {
  __m256i q = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'));
  __m256i b = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'));
  __m256i c = _mm256_cmpeq_epi8(_mm256_max_epu8(v, _mm256_set1_epi8(0x1f)), _mm256_set1_epi8(0x1f));
  return (unsigned)_mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(q, b), c));
}

/* Load 32 bytes at s[i..], of which only `rem` (< 32 allowed) are meaningful. Reads past the
 * array's size stay inside its allocation when the capacity allows; otherwise copy. */
static inline __m256i load_tail(const u8_t *s, size_t i, size_t rem, size_t cap) {
  if (rem >= 32 || i + 32 <= cap) return _mm256_loadu_si256((const __m256i *)(s + i));
  u8_t tmp[32] = {0};
  memcpy(tmp, s + i, rem);
  return _mm256_loadu_si256((const __m256i *)tmp);
}

/* escape(src[lo, hi)) appended to out, where room for the plain copy is already reserved;
 * escapes (rare) grow the reservation for the rest of the input. */
static lean_object *esc_append(lean_object *out, b_lean_obj_arg src, u64_t lo, u64_t hi) {
  const u8_t *s = lean_sarray_cptr(src);
  size_t scap = lean_sarray_capacity(src);
  u8_t *d = ba_end(out);
  u8_t *lim = lean_sarray_cptr(out) + lean_sarray_capacity(out);
  size_t i = (size_t)lo;
  while (i < hi) {
    size_t rem = hi - i;
    __m256i v = load_tail(s, i, rem, scap);
    unsigned m = esc_mask32(v);
    if (rem < 32) m &= (1u << rem) - 1;
    _mm256_storeu_si256((__m256i *)d, v);
    if (LIKELY(!m)) {
      size_t k = rem < 32 ? rem : 32;
      d += k; i += k;
      continue;
    }
    unsigned k = (unsigned)__builtin_ctz(m);
    d += k; i += k;
    if ((size_t)(lim - d) < (hi - i) * 6 + 64) {
      lean_to_sarray(out)->m_size = (size_t)(d - lean_sarray_cptr(out));
      out = ba_reserve(out, (hi - i) * 6 + 64);
      d = ba_end(out);
      lim = lean_sarray_cptr(out) + lean_sarray_capacity(out);
    }
    u8_t ch = s[i++];
    *d++ = '\\';
    switch (ch) {
    case '"': *d++ = '"'; break;
    case '\\': *d++ = '\\'; break;
    case '\b': *d++ = 'b'; break;
    case '\f': *d++ = 'f'; break;
    case '\n': *d++ = 'n'; break;
    case '\r': *d++ = 'r'; break;
    case '\t': *d++ = 't'; break;
    default: {
      static const char hx[] = "0123456789abcdef";
      *d++ = 'u'; *d++ = '0'; *d++ = '0'; *d++ = (u8_t)hx[ch >> 4]; *d++ = (u8_t)hx[ch & 15];
    }
    }
  }
  lean_to_sarray(out)->m_size = (size_t)(d - lean_sarray_cptr(out));
  return out;
}
static inline void clamp_range(b_lean_obj_arg src, b_lean_obj_arg lon, b_lean_obj_arg hin, u64_t *lo, u64_t *hi) {
  size_t n = lean_sarray_size(src);
  *lo = nat_u64(lon); *hi = nat_u64(hin);
  if (*hi > n) *hi = n;
  if (*lo > *hi) *lo = *hi;
}
/* bytes of lit appended, room reserved by the caller */
static inline void lit_append(lean_object *out, b_lean_obj_arg lit) {
  size_t len = lean_sarray_size(lit);
  copy_small(ba_end(out), lean_sarray_cptr(lit), len);
  ba_grow(out, len);
}

/* Feed.Bytes.appendEsc: out ++ escape(src[lo, hi)) */
LEAN_EXPORT lean_obj_res feed_append_esc(lean_obj_arg out, b_lean_obj_arg src, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  u64_t lo, hi;
  clamp_range(src, lon, hin, &lo, &hi);
  out = ba_reserve(out, (size_t)(hi - lo) + 64);
  return esc_append(out, src, lo, hi);
}

/* Feed.Bytes.appendLitEsc: out ++ lit ++ escape(src[lo, hi)) */
LEAN_EXPORT lean_obj_res feed_append_lit_esc(lean_obj_arg out, b_lean_obj_arg lit, b_lean_obj_arg src, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  u64_t lo, hi;
  clamp_range(src, lon, hin, &lo, &hi);
  out = ba_reserve(out, lean_sarray_size(lit) + (size_t)(hi - lo) + 64);
  lit_append(out, lit);
  return esc_append(out, src, lo, hi);
}

/* Feed.Bytes.appendNat: out ++ decimal digits of n */
static const char dig2[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
LEAN_EXPORT lean_obj_res feed_append_nat(lean_obj_arg out, b_lean_obj_arg nn) {
  if (UNLIKELY(!lean_is_scalar(nn))) {
    /* numbers this large never occur in responses; render them with Lean's Nat arithmetic */
    size_t cap = 64, k = 0;
    char *buf = malloc(cap);
    lean_inc(nn);
    lean_object *v = nn;
    do {
      lean_object *q = lean_nat_div(v, lean_box(10)), *r = lean_nat_mod(v, lean_box(10));
      if (k == cap) { cap *= 2; buf = realloc(buf, cap); }
      buf[k++] = (char)('0' + lean_unbox(r));
      lean_dec(v);
      v = q;
    } while (!(lean_is_scalar(v) && lean_unbox(v) == 0));
    out = ba_reserve(out, k);
    for (size_t j = 0; j < k; j++) ba_end(out)[j] = (u8_t)buf[k - 1 - j];
    ba_grow(out, k);
    free(buf);
    return out;
  }
  u64_t v = (u64_t)lean_unbox(nn);
  out = ba_reserve(out, 24);
  size_t k = 1;
  for (u64_t t = v; t >= 10; t /= 10) k++;
  u8_t *p = ba_end(out) + k;
  while (v >= 100) { unsigned r = (unsigned)(v % 100); v /= 100; p -= 2; memcpy(p, dig2 + 2 * r, 2); }
  if (v >= 10) { p -= 2; memcpy(p, dig2 + 2 * v, 2); } else *--p = (u8_t)('0' + v);
  ba_grow(out, k);
  return out;
}

/* Feed.Bytes.appendLitNat: out ++ lit ++ decimal digits of n */
LEAN_EXPORT lean_obj_res feed_append_lit_nat(lean_obj_arg out, b_lean_obj_arg lit, b_lean_obj_arg nn) {
  out = ba_reserve(out, lean_sarray_size(lit) + 64);
  lit_append(out, lit);
  return feed_append_nat(out, nn);
}

/* Feed.Bytes.appendSlice: out ++ src[lo, hi) */
LEAN_EXPORT lean_obj_res feed_append_slice(lean_obj_arg out, b_lean_obj_arg src, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  size_t n = lean_sarray_size(src);
  u64_t lo = nat_u64(lon), hi = nat_u64(hin);
  if (hi > n) hi = n;
  if (lo > hi) lo = hi;
  size_t len = (size_t)(hi - lo);
  out = ba_reserve(out, len + 32);
  copy_small(ba_end(out), lean_sarray_cptr(src) + lo, len);
  ba_grow(out, len);
  return out;
}

/* Feed.Bytes.appendAll: out ++ src */
LEAN_EXPORT lean_obj_res feed_append_all(lean_obj_arg out, b_lean_obj_arg src) {
  size_t len = lean_sarray_size(src);
  out = ba_reserve(out, len + 32);
  copy_small(ba_end(out), lean_sarray_cptr(src), len);
  ba_grow(out, len);
  return out;
}

/* Feed.Bytes.findByte: the first i in [lo, hi) with a[i] = b, else hi (clamped to the size) */
LEAN_EXPORT lean_obj_res feed_find_byte(b_lean_obj_arg a, uint8_t b, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  size_t n = lean_sarray_size(a);
  u64_t lo = nat_u64(lon), hi = nat_u64(hin);
  if (hi > n) hi = n;
  if (lo >= hi) return lean_box((size_t)(lo < hi ? lo : hi));
  const u8_t *p = memchr(lean_sarray_cptr(a) + lo, b, (size_t)(hi - lo));
  return lean_box(p ? (size_t)(p - lean_sarray_cptr(a)) : (size_t)hi);
}

/* Feed.Bytes.findHeadEnd: the first i in [lo, hi - 3) with a[i..i+4] = "\r\n\r\n", else hi */
LEAN_EXPORT lean_obj_res feed_find_head_end(b_lean_obj_arg a, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  size_t n = lean_sarray_size(a);
  u64_t lo = nat_u64(lon), hi = nat_u64(hin);
  if (hi > n) hi = n;
  const u8_t *s = lean_sarray_cptr(a);
  size_t i = (size_t)lo;
  while (i + 4 <= hi) {
    const u8_t *p = memchr(s + i, '\r', hi - 3 - i);
    if (!p) break;
    size_t k = (size_t)(p - s);
    if (p[1] == '\n' && p[2] == '\r' && p[3] == '\n') return lean_box(k);
    i = k + 1;
  }
  return lean_box((size_t)hi);
}

/* Feed.Bytes.clear: the empty array (the C version keeps the allocation when it can be reused) */
LEAN_EXPORT lean_obj_res feed_bytes_clear(lean_obj_arg a) {
  if (lean_is_exclusive(a)) { lean_to_sarray(a)->m_size = 0; return a; }
  lean_dec(a);
  return lean_alloc_sarray(1, 0, 4096);
}

/* ---- SHA-256 (FIPS 180-4), HMAC-SHA256 and base64url, for JWT verification -------------- */
static const u32_t sha_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
/* The compression function on the sixteen words w[0, 16) (big-endian words of a block). */
static void sha_compress(u32_t h[8], const u32_t *win) {
  u32_t w[64];
  memcpy(w, win, 64);
  for (int i = 16; i < 64; i++) {
    u32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
#define SHA_ROUND(a, b, c, d, e, f, g, h, i)                                                      \
  do {                                                                                            \
    u32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i]; \
    u32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));      \
    d += t1;                                                                                      \
    h = t1 + t2;                                                                                  \
  } while (0)
  for (int i = 0; i < 64; i += 8) {
    SHA_ROUND(a, b, c, d, e, f, g, hh, i);
    SHA_ROUND(hh, a, b, c, d, e, f, g, i + 1);
    SHA_ROUND(g, hh, a, b, c, d, e, f, i + 2);
    SHA_ROUND(f, g, hh, a, b, c, d, e, i + 3);
    SHA_ROUND(e, f, g, hh, a, b, c, d, i + 4);
    SHA_ROUND(d, e, f, g, hh, a, b, c, i + 5);
    SHA_ROUND(c, d, e, f, g, hh, a, b, i + 6);
    SHA_ROUND(b, c, d, e, f, g, hh, a, i + 7);
  }
#undef SHA_ROUND
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static inline u32_t be32(const u8_t *p) { return (u32_t)p[0] << 24 | (u32_t)p[1] << 16 | (u32_t)p[2] << 8 | p[3]; }
static void sha_block(u32_t h[8], const u8_t *p) {
  u32_t w[16];
  for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
  sha_compress(h, w);
}
static void state_get(b_lean_obj_arg s, u32_t h[8]) { for (int i = 0; i < 8; i++) h[i] = lean_ctor_get_uint32(s, (unsigned)(4 * i)); }

/* Feed.Hmac.mac: HMAC-SHA256 of m[lo, hi) under the key's inner and outer states, as
 * Sha256.finish (from the inner state, 64 bytes already hashed) then one outer block. */
LEAN_EXPORT lean_obj_res feed_hmac_mac(b_lean_obj_arg key, b_lean_obj_arg m, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  u32_t h[8];
  state_get(lean_ctor_get(key, 0), h);
  size_t size = lean_sarray_size(m);
  u64_t lo = nat_u64(lon), hi = nat_u64(hin);
  if (hi > size) hi = size;
  const u8_t *s = lean_sarray_cptr(m);
  u8_t tail[128];
  size_t len;
  if (lo <= hi) {
    u64_t n = hi - lo, full = n / 64;
    for (u64_t k = 0; k < full; k++) sha_block(h, s + lo + 64 * k);
    u64_t tl = lo + 64 * full, rem = hi - tl, bits = (64 + n) * 8;
    len = rem < 56 ? 64 : 128;
    memset(tail, 0, len);
    memcpy(tail, s + tl, (size_t)rem);
    tail[rem] = 0x80;
    for (int j = 0; j < 8; j++) tail[len - 1 - (size_t)j] = (u8_t)(bits >> (8 * j));
  } else {
    /* an empty range given backwards: the padded tail of the specification has no 0x80 */
    len = 64;
    memset(tail, 0, len);
    u64_t bits = 64 * 8;
    for (int j = 0; j < 8; j++) tail[len - 1 - (size_t)j] = (u8_t)(bits >> (8 * j));
  }
  sha_block(h, tail);
  if (len == 128) sha_block(h, tail + 64);
  u32_t o[8], w[16] = {0};
  state_get(lean_ctor_get(key, 1), o);
  memcpy(w, h, 32);
  w[8] = 0x80000000u;
  w[15] = 768;
  sha_compress(o, w);
  lean_object *r = lean_alloc_sarray(1, 32, 32);
  u8_t *d = lean_sarray_cptr(r);
  for (int i = 0; i < 8; i++) { d[4 * i] = (u8_t)(o[i] >> 24); d[4 * i + 1] = (u8_t)(o[i] >> 16); d[4 * i + 2] = (u8_t)(o[i] >> 8); d[4 * i + 3] = (u8_t)o[i]; }
  return r;
}

/* Feed.Jwt.b64Encode: base64url without padding */
LEAN_EXPORT lean_obj_res feed_b64_encode(b_lean_obj_arg dv) {
  static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t n = lean_sarray_size(dv);
  const u8_t *s = lean_sarray_cptr(dv);
  lean_object *r = lean_alloc_sarray(1, 0, n * 4 / 3 + 4);
  u8_t *d = lean_sarray_cptr(r);
  size_t i = 0, k = 0;
  for (; i + 3 <= n; i += 3) {
    u32_t v = (u32_t)s[i] << 16 | (u32_t)s[i + 1] << 8 | s[i + 2];
    d[k++] = (u8_t)al[v >> 18]; d[k++] = (u8_t)al[(v >> 12) & 63]; d[k++] = (u8_t)al[(v >> 6) & 63]; d[k++] = (u8_t)al[v & 63];
  }
  if (i + 2 == n) {
    u32_t v = (u32_t)s[i] << 8 | s[i + 1];
    d[k++] = (u8_t)al[v >> 10]; d[k++] = (u8_t)al[(v >> 4) & 63]; d[k++] = (u8_t)al[(v & 15) << 2];
  } else if (i + 1 == n) {
    u32_t v = s[i];
    d[k++] = (u8_t)al[v >> 2]; d[k++] = (u8_t)al[(v & 3) << 4];
  }
  lean_to_sarray(r)->m_size = k;
  return r;
}

/* Feed.Jwt.b64Decode: base64url text a[lo, hi) (up to two '=' of padding stripped); none if a
 * character is not base64url or the length is 1 mod 4. Trailing bits that do not make a byte
 * are dropped. */
static const u8_t b64v[256] = {
#define X64 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
  X64, X64,
  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64,
  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
  64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 63,
  64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
  41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
  X64, X64, X64, X64, X64, X64, X64, X64
#undef X64
};
LEAN_EXPORT lean_obj_res feed_b64_decode(b_lean_obj_arg a, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  size_t size = lean_sarray_size(a);
  const u8_t *s = lean_sarray_cptr(a);
  u64_t lo = nat_u64(lon), hi = nat_u64(hin);
  if (hi > size) hi = size;
  if (hi > lo && s[hi - 1] == '=') hi--;
  if (hi > lo && s[hi - 1] == '=') hi--;
  u64_t n = hi > lo ? hi - lo : 0;
  if (n % 4 == 1) return lean_box(0);
  lean_object *r = lean_alloc_sarray(1, 0, n * 3 / 4 + 3);
  u8_t *d = lean_sarray_cptr(r);
  size_t k = 0;
  u32_t acc = 0, nbits = 0;
  for (u64_t i = 0; i < n; i++) {
    u8_t v = b64v[s[lo + i]];
    if (v == 64) { lean_dec(r); return lean_box(0); }
    acc = acc << 6 | v;
    nbits += 6;
    if (nbits >= 8) { nbits -= 8; d[k++] = (u8_t)(acc >> nbits); acc &= (1u << nbits) - 1; }
  }
  lean_to_sarray(r)->m_size = k;
  lean_object *o = lean_alloc_ctor(1, 1, 0);
  lean_ctor_set(o, 0, r);
  return o;
}

/* Feed.postJsonAfter: out ++ start ++ digits id ++ ",\"body\":\"" ++ escape(src[b, c)) ++
 * "\",\"created_at\":\"" ++ escape(src[c, e)) ++ "\",\"author\":\"" ++ escape(names[s, t)) ++
 * "\",\"like_count\":" ++ digits likes ++ "}" (one post object; the literals are those of
 * Feed/Handler.lean, which the self-test checks this against) */
static inline void lit_bytes(lean_object *out, const char *s, size_t n) { memcpy(ba_end(out), s, n); ba_grow(out, n); }
LEAN_EXPORT lean_obj_res feed_append_post(lean_obj_arg out, b_lean_obj_arg start, b_lean_obj_arg idn, b_lean_obj_arg src,
                                          b_lean_obj_arg bn, b_lean_obj_arg cn, b_lean_obj_arg en, b_lean_obj_arg names,
                                          b_lean_obj_arg sn, b_lean_obj_arg tn, b_lean_obj_arg likesn) {
  static const char kBody[] = ",\"body\":\"", kCreated[] = "\",\"created_at\":\"", kAuthor[] = "\",\"author\":\"",
                    kLikes[] = "\",\"like_count\":";
  u64_t b, c, c2, e, s, t;
  clamp_range(src, bn, cn, &b, &c);
  clamp_range(src, cn, en, &c2, &e);
  clamp_range(names, sn, tn, &s, &t);
  out = ba_reserve(out, lean_sarray_size(start) + 160 + (size_t)(c - b) + (size_t)(e - c2) + (size_t)(t - s));
  lit_append(out, start);
  out = feed_append_nat(out, idn);
  lit_bytes(out, kBody, sizeof kBody - 1);
  out = esc_append(out, src, b, c);
  out = ba_reserve(out, 64 + (size_t)(e - c2) + (size_t)(t - s));
  lit_bytes(out, kCreated, sizeof kCreated - 1);
  out = esc_append(out, src, c2, e);
  out = ba_reserve(out, 64 + (size_t)(t - s));
  lit_bytes(out, kAuthor, sizeof kAuthor - 1);
  out = esc_append(out, names, s, t);
  out = ba_reserve(out, 64);
  lit_bytes(out, kLikes, sizeof kLikes - 1);
  out = feed_append_nat(out, likesn);
  out = ba_reserve(out, 1);
  ba_end(out)[0] = '}';
  ba_grow(out, 1);
  return out;
}
