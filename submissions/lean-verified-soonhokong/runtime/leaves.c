/* The C leaves: 21 of the 22 Lean definitions that carry `@[extern]` (the 22nd, Feed.Idle.pick, is
 * idle_pick in uring.c). Each function implements the Lean definition its comment names, which is
 * its specification, and `@[extern]` makes the compiled program call this C code instead: the byte
 * operations of Feed/Bytes.lean, HTTP framing (Feed.Http.parse), routing (Feed.route), the JSON of
 * bodies and tokens (Feed.Json.parseTop, decodeString), HMAC-SHA256 (Feed.Hmac.mac), base64url
 * (Feed.Jwt.b64Encode, b64Decode), one post object (Feed.postJsonAfter), the feed's body
 * (Feed.feedFromPage, Feed.Page.authored) and the responses of the feed and of a post
 * (Feed.respondFeed, Feed.respondPost). A proof unfolds the Lean definitions of all but seven,
 * whose results the theorems take as given: Http.parse, Bytes.findHeadEnd, Json.parseTop,
 * Json.decodeString, Hmac.mac, Jwt.b64Encode and Jwt.b64Decode. Five are not called while serving,
 * since their only callers outside the self-test are the Lean bodies of other leaves:
 * Bytes.appendLitEsc, appendLitNat, appendSlice, findHeadEnd and Feed.postJsonAfter.
 * `bin/server --selftest` (Feed/Selftest.lean, run by build.sh) compares each with its Lean
 * definition on random inputs. */
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

/* ---- HTTP/1.1 framing: Feed.Http.parse ----------------------------------------------------
 * Each function below is the one of the same name in Feed/Http.lean (and Feed/Bytes.lean), on
 * the raw bytes s[0, n) and machine integers; the self-test compares feed_http_parse with the
 * Lean definition. Indices stay below n + 5 here, far from overflow. */
/* `byte a i`: 0 past the end */
static inline u8_t h_byte(const u8_t *s, size_t n, size_t i) { return i < n ? s[i] : 0; }
static inline u8_t h_lower(u8_t b) { return b >= 0x41 && b <= 0x5a ? (u8_t)(b + 32) : b; }
/* Bytes.findByte: the first i in [lo, min hi n) with s[i] = b, else min hi n. The ranges are
 * short (a line, a header value), so 32 bytes are compared at a time inline while a load stays
 * inside the array's allocation (cap bytes); memchr does the rest. */
static inline size_t h_find(const u8_t *s, size_t n, size_t cap, u8_t b, size_t lo, size_t hi) {
  if (hi > n) hi = n;
  if (lo >= hi) return hi;
  const __m256i vb = _mm256_set1_epi8((char)b);
  size_t i = lo;
  while (i + 32 <= cap) {
    unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(s + i)), vb));
    if (hi - i < 32) m &= (1u << (hi - i)) - 1;
    if (m) return i + (size_t)__builtin_ctz(m);
    i += 32;
    if (i >= hi) return hi;
  }
  const u8_t *p = memchr(s + i, b, hi - i);
  return p ? (size_t)(p - s) : hi;
}
/* Bytes.findHeadEnd with hi = n: the first i >= lo with i + 4 <= n where s[i, i+4) is \r\n\r\n,
 * else n. 32 candidates at a time while the loads (s[i, i+35)) stay inside the allocation. */
static inline size_t h_head_end(const u8_t *s, size_t n, size_t cap, size_t lo) {
  size_t i = lo;
  const __m256i cr = _mm256_set1_epi8('\r'), lf = _mm256_set1_epi8('\n');
  while (i + 4 <= n && i + 35 <= cap) {
    __m256i x = _mm256_and_si256(
        _mm256_and_si256(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(s + i)), cr),
                         _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(s + i + 1)), lf)),
        _mm256_and_si256(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(s + i + 2)), cr),
                         _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(s + i + 3)), lf)));
    unsigned m = (unsigned)_mm256_movemask_epi8(x);
    size_t left = n - 3 - i;   /* the candidates i + k with i + k + 4 <= n */
    if (left < 32) m &= (1u << left) - 1;
    if (m) return i + (size_t)__builtin_ctz(m);
    if (left <= 32) return n;
    i += 32;
  }
  while (i + 4 <= n) {
    const u8_t *p = memchr(s + i, '\r', n - 3 - i);
    if (!p) break;
    size_t k = (size_t)(p - s);
    if (p[1] == '\n' && p[2] == '\r' && p[3] == '\n') return k;
    i = k + 1;
  }
  return n;
}
/* eqAt: s[lo, hi) is lit[0, m) */
static inline int h_eq(const u8_t *s, size_t n, size_t lo, size_t hi, const char *lit, size_t m) {
  if ((hi > lo ? hi - lo : 0) != m) return 0;
  if (LIKELY(lo + m <= n)) return memcmp(s + lo, lit, m) == 0;
  for (size_t k = 0; k < m; k++) if (h_byte(s, n, lo + k) != (u8_t)lit[k]) return 0;
  return 1;
}
/* eqCI: s[lo, hi) is lit[0, m) ignoring ASCII case (lit lowercase) */
static inline int h_eq_ci(const u8_t *s, size_t n, size_t lo, size_t hi, const char *lit, size_t m) {
  if ((hi > lo ? hi - lo : 0) != m) return 0;
  for (size_t k = 0; k < m; k++) if (h_lower(h_byte(s, n, lo + k)) != (u8_t)lit[k]) return 0;
  return 1;
}
static inline size_t h_skip_l(const u8_t *s, size_t n, size_t i, size_t e) {
  for (size_t f = e > i ? e - i : 0; f > 0; f--) {
    u8_t b = h_byte(s, n, i);
    if (b != 0x20 && b != 0x09) break;
    i++;
  }
  return i;
}
static inline size_t h_skip_r(const u8_t *s, size_t n, size_t lo, size_t e) {
  for (size_t f = e > lo ? e - lo : 0; f > 0; f--) {
    u8_t b = h_byte(s, n, e - 1);
    if (b != 0x20 && b != 0x09) break;
    e--;
  }
  return e;
}
/* parseLen: 1 and the value of the digits s[lo, hi) (at most 15 of them), else 0 */
static inline int h_parse_len(const u8_t *s, size_t n, size_t lo, size_t hi, u64_t *v) {
  if (lo >= hi || hi - lo > 15) return 0;
  u64_t acc = 0;
  for (size_t i = lo; i < hi; i++) {
    u8_t c = h_byte(s, n, i);
    if (c < 0x30 || c > 0x39) return 0;
    acc = acc * 10 + (u64_t)(c - 0x30);
  }
  *v = acc;
  return 1;
}
/* hasToken: the comma-separated value s[lo, hi) has the token tok (lowercase) */
static int h_has_token(const u8_t *s, size_t n, size_t cap, size_t lo, size_t hi, const char *tok, size_t m) {
  size_t i = lo;
  for (size_t f = (hi > lo ? hi - lo : 0) + 1; f > 0; f--) {
    if (i >= hi) return 0;
    size_t e = h_find(s, n, cap, 0x2c, i, hi), st = h_skip_l(s, n, i, e);
    if (h_eq_ci(s, n, st, h_skip_r(s, n, st, e), tok, m)) return 1;
    i = e + 1;
  }
  return 0;
}
typedef struct {
  u64_t len;
  size_t auth_lo, auth_hi;
  u8_t has_len, len_bad, has_auth, close, keep_alive, te, bad;
} heads_t;
/* scanHeads */
static void h_scan_heads(const u8_t *s, size_t n, size_t cap, size_t he, size_t i, heads_t *h, size_t fuel) {
  for (; fuel > 0; fuel--) {
    if (i >= he) return;
    size_t le = h_find(s, n, cap, 0x0d, i, he), colon = h_find(s, n, cap, 0x3a, i, le);
    if (colon >= le || colon == i) h->bad = 1;
    else {
      size_t vs = h_skip_l(s, n, colon + 1, le), ve = h_skip_r(s, n, vs, le);
      switch (colon - i) {   /* the names' lengths: only one can match */
      case 14:
        if (h_eq_ci(s, n, i, colon, "content-length", 14)) {
          u64_t v;
          if (!h_parse_len(s, n, vs, ve, &v)) h->len_bad = 1;
          else if (!h->has_len) { h->has_len = 1; h->len = v; }
          else if (h->len != v) h->len_bad = 1;
        }
        break;
      case 13:
        if (h_eq_ci(s, n, i, colon, "authorization", 13)) { h->has_auth = 1; h->auth_lo = vs; h->auth_hi = ve; }
        break;
      case 10:
        if (h_eq_ci(s, n, i, colon, "connection", 10)) {
          h->close = h->close || h_has_token(s, n, cap, vs, ve, "close", 5);
          h->keep_alive = h->keep_alive || h_has_token(s, n, cap, vs, ve, "keep-alive", 10);
        }
        break;
      case 17:
        if (h_eq_ci(s, n, i, colon, "transfer-encoding", 17)) h->te = 1;
        break;
      }
    }
    i = le + 2;
  }
}
static lean_obj_res h_bad(unsigned st) {
  lean_object *o = lean_alloc_ctor(1, 1, 0);
  lean_ctor_set(o, 0, lean_box(st));
  return o;
}
/* Feed.Http.parse: the request at pos of a: incomplete (box 0), bad status (tag 1) or ok (tag 2)
 * with the Req: its Nat fields in declaration order, then method, hasAuth, close as bytes. */
LEAN_EXPORT lean_obj_res feed_http_parse(b_lean_obj_arg a, b_lean_obj_arg posn) {
  const u8_t *s = lean_sarray_cptr(a);
  size_t n = lean_sarray_size(a), cap = lean_sarray_capacity(a);
  if (UNLIKELY(!lean_is_scalar(posn))) return lean_box(0);   /* past the end: n - pos = 0 */
  size_t pos = lean_unbox(posn);
  size_t he = h_head_end(s, n, cap, pos);
  if (he >= n) return n > pos && n - pos > 16384 ? h_bad(431) : lean_box(0);
  if (he - pos > 16384) return h_bad(431);
  size_t le = h_find(s, n, cap, 0x0d, pos, he);
  size_t sp1 = h_find(s, n, cap, 0x20, pos, le), sp2 = h_find(s, n, cap, 0x20, sp1 + 1, le);
  if (sp1 >= le || sp2 >= le || sp1 == pos) return h_bad(400);
  int v11 = h_eq(s, n, sp2 + 1, le, "HTTP/1.1", 8);
  if (!v11 && !h_eq(s, n, sp2 + 1, le, "HTTP/1.0", 8)) return h_bad(400);
  u8_t method = h_eq(s, n, pos, sp1, "GET", 3) ? 0 : h_eq(s, n, pos, sp1, "POST", 4) ? 1 : 2;
  size_t q = h_find(s, n, cap, 0x3f, sp1 + 1, sp2);
  heads_t h;
  memset(&h, 0, sizeof h);
  if (le != he) h_scan_heads(s, n, cap, he, le + 2, &h, he - le);
  if (h.bad || h.len_bad || h.te) return h_bad(400);
  u64_t len = h.has_len ? h.len : 0;
  if (len > 1048576) return h_bad(413);
  size_t body_lo = he + 4;
  if (body_lo + len > n) return lean_box(0);
  u8_t close = v11 ? h.close : !h.keep_alive;
  lean_object *r = lean_alloc_ctor(0, 7, 3);
  lean_ctor_set(r, 0, lean_box(sp1 + 1));
  lean_ctor_set(r, 1, lean_box(q));
  lean_ctor_set(r, 2, lean_box(h.auth_lo));
  lean_ctor_set(r, 3, lean_box(h.auth_hi));
  lean_ctor_set(r, 4, lean_box(body_lo));
  lean_ctor_set(r, 5, lean_box(body_lo + len));
  lean_ctor_set(r, 6, lean_box(body_lo + len));
  lean_ctor_set_uint8(r, sizeof(void *) * 7, method);
  lean_ctor_set_uint8(r, sizeof(void *) * 7 + 1, h.has_auth);
  lean_ctor_set_uint8(r, sizeof(void *) * 7 + 2, close);
  lean_object *o = lean_alloc_ctor(2, 1, 0);
  lean_ctor_set(o, 0, r);
  return o;
}

/* ---- JSON input: Feed.Json.decodeString and Feed.Json.parseTop ---------------------------
 * The functions of Feed/Json.lean on raw bytes. The Lean scanners carry fuel (hi - lo + 1);
 * it never runs out (a value of n bytes is scanned with any fuel >= n: each level of nesting
 * costs two and each earlier element or member one, and each of those has at least as many
 * bytes), so the C versions have none, and nesting is kept on an explicit stack instead of the
 * C stack. */
#define J_NONE SIZE_MAX
static inline int j_ws(u8_t b) { return b == 0x20 || b == 0x09 || b == 0x0a || b == 0x0d; }
static inline int j_digit(u8_t b) { return b >= 0x30 && b <= 0x39; }
static inline int j_hex(u8_t b) { return j_digit(b) || (b >= 0x61 && b <= 0x66) || (b >= 0x41 && b <= 0x46); }
static inline size_t j_skip_ws(const u8_t *s, size_t hi, size_t i) {
  while (i < hi && j_ws(s[i])) i++;
  return i;
}
/* scanString: i just after the opening quote; the index after the closing quote */
static size_t j_string(const u8_t *s, size_t hi, size_t i) {
  while (i < hi) {
    /* 32 bytes at a time while none is a quote, a backslash or a control */
    while (i + 32 <= hi && !esc_mask32(_mm256_loadu_si256((const __m256i *)(s + i)))) i += 32;
    if (i >= hi) break;
    u8_t b = s[i];
    if (b == 0x22) return i + 1;
    if (b < 0x20) return J_NONE;
    if (b == 0x5c) {
      if (i + 1 >= hi) return J_NONE;
      u8_t e = s[i + 1];
      if (e == 0x22 || e == 0x5c || e == 0x2f || e == 0x62 || e == 0x66 || e == 0x6e || e == 0x72 || e == 0x74) i += 2;
      else if (e == 0x75 && i + 6 <= hi && j_hex(s[i + 2]) && j_hex(s[i + 3]) && j_hex(s[i + 4]) && j_hex(s[i + 5])) i += 6;
      else return J_NONE;
    } else i++;
  }
  return J_NONE;
}
static inline size_t j_digits(const u8_t *s, size_t hi, size_t i) {
  while (i < hi && j_digit(s[i])) i++;
  return i;
}
/* scanNumber */
static size_t j_number(const u8_t *s, size_t hi, size_t i) {
  if (i < hi && s[i] == 0x2d) i++;
  if (i >= hi) return J_NONE;
  if (s[i] == 0x30) i++;
  else if (j_digit(s[i])) i = j_digits(s, hi, i);
  else return J_NONE;
  if (i < hi && s[i] == 0x2e) {
    size_t j = j_digits(s, hi, i + 1);
    if (j == i + 1) return J_NONE;
    i = j;
  }
  if (i < hi && (s[i] == 0x65 || s[i] == 0x45)) {
    size_t k = i + 1 < hi && (s[i + 1] == 0x2b || s[i + 1] == 0x2d) ? i + 2 : i + 1;
    size_t j = j_digits(s, hi, k);
    if (j == k) return J_NONE;
    return j;
  }
  return i;
}
static inline size_t j_lit(const u8_t *s, size_t hi, size_t i, const char *lit, size_t m) {
  return i + m <= hi && !memcmp(s + i, lit, m) ? i + m : J_NONE;
}
/* scanValue at i (whitespace skipped): the index after the value */
static size_t j_value(const u8_t *s, size_t hi, size_t i) {
  u8_t small[64], *stk = small;   /* the open containers: '{' or '[' */
  size_t depth = 0, cap = sizeof small;
  for (;;) {
    /* a value at i */
    if (i >= hi) goto fail;
    u8_t b = s[i];
    if (b == 0x7b || b == 0x5b) {
      if (depth == cap) {
        u8_t *n2 = malloc(2 * cap);
        memcpy(n2, stk, cap);
        if (stk != small) free(stk);
        stk = n2; cap *= 2;
      }
      stk[depth++] = b;
      i = j_skip_ws(s, hi, i + 1);
      if (i >= hi) goto fail;
      if (s[i] == b + 2) { i++; depth--; goto after; }   /* '}' is '{' + 2, ']' is '[' + 2 */
      if (b == 0x7b) goto key;
      continue;
    }
    if (b == 0x22) i = j_string(s, hi, i + 1);
    else if (b == 0x74) i = j_lit(s, hi, i, "true", 4);
    else if (b == 0x66) i = j_lit(s, hi, i, "false", 5);
    else if (b == 0x6e) i = j_lit(s, hi, i, "null", 4);
    else i = j_number(s, hi, i);
    if (i == J_NONE) goto fail;
  after:
    /* a value ended at i */
    if (depth == 0) { if (stk != small) free(stk); return i; }
    i = j_skip_ws(s, hi, i);
    if (i >= hi) goto fail;
    if (s[i] == stk[depth - 1] + 2) { i++; depth--; goto after; }
    if (s[i] != 0x2c) goto fail;
    i = j_skip_ws(s, hi, i + 1);
    if (stk[depth - 1] == 0x5b) continue;
  key:
    /* a member at i: key, colon, then its value */
    if (i >= hi || s[i] != 0x22) goto fail;
    i = j_string(s, hi, i + 1);
    if (i == J_NONE) goto fail;
    i = j_skip_ws(s, hi, i);
    if (i >= hi || s[i] != 0x3a) goto fail;
    i = j_skip_ws(s, hi, i + 1);
  }
fail:
  if (stk != small) free(stk);
  return J_NONE;
}
static inline u8_t j_byte(const u8_t *s, size_t n, size_t i) { return i < n ? s[i] : 0; }
/* hexVal, with the wraparound of UInt8 for a byte that is not a hex digit */
static inline unsigned j_hex_val(u8_t b) {
  if (j_digit(b)) return (unsigned)(b - 0x30);
  if (b >= 0x61) return (u8_t)(b - 0x61 + 10);
  return (u8_t)(b - 0x41 + 10);
}
static inline u64_t j_hex4(const u8_t *s, size_t n, size_t i) {
  return (((u64_t)j_hex_val(j_byte(s, n, i)) * 16 + j_hex_val(j_byte(s, n, i + 1))) * 16 + j_hex_val(j_byte(s, n, i + 2))) * 16 +
         j_hex_val(j_byte(s, n, i + 3));
}
/* pushUtf8 at d: the end */
static inline u8_t *j_utf8(u8_t *d, u64_t c) {
  if (c < 0x80) *d++ = (u8_t)c;
  else if (c < 0x800) { *d++ = (u8_t)(0xc0 + c / 64); *d++ = (u8_t)(0x80 + c % 64); }
  else if (c < 0x10000) { *d++ = (u8_t)(0xe0 + c / 4096); *d++ = (u8_t)(0x80 + c / 64 % 64); *d++ = (u8_t)(0x80 + c % 64); }
  else { *d++ = (u8_t)(0xf0 + c / 262144); *d++ = (u8_t)(0x80 + c / 4096 % 64); *d++ = (u8_t)(0x80 + c / 64 % 64); *d++ = (u8_t)(0x80 + c % 64); }
  return d;
}
/* decodeString.go from i to hi into d, which has room for 3 (hi - i) + 4 bytes (no step writes
 * more than three bytes per byte it consumes, a surrogate pair four for twelve): the end */
static u8_t *j_decode(const u8_t *s, size_t n, size_t i, size_t hi, u8_t *d) {
  while (i + 1 < hi) {
    u8_t b = j_byte(s, n, i);
    if (b == 0x5c) {
      u8_t e = j_byte(s, n, i + 1);
      if (e == 0x75) {
        u64_t c = j_hex4(s, n, i + 2);
        if (c >= 0xd800 && c < 0xdc00 && i + 12 <= hi - 1 && j_byte(s, n, i + 6) == 0x5c && j_byte(s, n, i + 7) == 0x75) {
          u64_t c2 = j_hex4(s, n, i + 8);
          if (c2 >= 0xdc00 && c2 < 0xe000) { d = j_utf8(d, 0x10000 + (c - 0xd800) * 1024 + (c2 - 0xdc00)); i += 12; }
          else { d = j_utf8(d, 0xfffd); i += 6; }
        } else if (c >= 0xd800 && c < 0xe000) { d = j_utf8(d, 0xfffd); i += 6; }
        else { d = j_utf8(d, c); i += 6; }
      } else {
        *d++ = e == 0x62 ? 0x08 : e == 0x66 ? 0x0c : e == 0x6e ? 0x0a : e == 0x72 ? 0x0d : e == 0x74 ? 0x09 : e;
        i += 2;
      }
    } else if (b < 0x80) { *d++ = b; i++; }
    else {
      unsigned k = (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
      int ok = k >= 2;
      for (unsigned j = 1; ok && j < k; j++) ok = i + j < hi - 1 && (j_byte(s, n, i + j) & 0xc0) == 0x80;
      if (ok) {
        u64_t cp = k == 2 ? (u64_t)(b & 0x1f) * 64 + (j_byte(s, n, i + 1) & 0x3f)
                 : k == 3 ? ((u64_t)(b & 0x0f) * 64 + (j_byte(s, n, i + 1) & 0x3f)) * 64 + (j_byte(s, n, i + 2) & 0x3f)
                 : (((u64_t)(b & 0x07) * 64 + (j_byte(s, n, i + 1) & 0x3f)) * 64 + (j_byte(s, n, i + 2) & 0x3f)) * 64 +
                       (j_byte(s, n, i + 3) & 0x3f);
        u64_t min = k == 2 ? 0x80 : k == 3 ? 0x800 : 0x10000;
        if (cp < min || (cp >= 0xd800 && cp < 0xe000) || cp > 0x10ffff) { d = j_utf8(d, 0xfffd); i++; }
        else { for (unsigned j = 0; j < k; j++) *d++ = j_byte(s, n, i + j); i += k; }
      } else { d = j_utf8(d, 0xfffd); i++; }
    }
  }
  return d;
}
/* Feed.Json.decodeString a lo hi */
LEAN_EXPORT lean_obj_res feed_json_decode_string(b_lean_obj_arg a, b_lean_obj_arg lon, b_lean_obj_arg hin) {
  u64_t lo = nat_u64(lon), hi = nat_u64(hin);
  /* ranges that do not fit in memory never occur (the Lean version would allocate hi - lo bytes) */
  size_t len = hi > lo && hi != UINT64_MAX ? (size_t)(hi - lo) : 0;
  lean_object *r = lean_alloc_sarray(1, 0, 3 * len + 8);
  if (len) {
    u8_t *d = j_decode(lean_sarray_cptr(a), lean_sarray_size(a), (size_t)lo + 1, (size_t)hi, lean_sarray_cptr(r));
    lean_to_sarray(r)->m_size = (size_t)(d - lean_sarray_cptr(r));
  }
  return r;
}
/* keyEq: the key whose text is s[klo, khi) is `key` (decoded first when written with escapes) */
static int j_key_eq(const u8_t *s, size_t n, size_t klo, size_t khi, b_lean_obj_arg key) {
  size_t m = lean_sarray_size(key);
  const u8_t *k = lean_sarray_cptr(key);
  if (!memchr(s + klo, 0x5c, khi - klo)) return khi - klo == m && !memcmp(s + klo, k, m);
  size_t room = 3 * (khi - klo + 2) + 8;
  u8_t small[256], *buf = room <= sizeof small ? small : malloc(room);
  size_t dl = (size_t)(j_decode(s, n, klo, khi + 1, buf) - buf);
  int eq = dl == m && !memcmp(buf, k, m);
  if (buf != small) free(buf);
  return eq;
}
static lean_obj_res j_field(u8_t kind, size_t lo, size_t hi) {
  lean_object *f = lean_alloc_ctor(0, 2, 1);
  lean_ctor_set(f, 0, lean_box(lo));
  lean_ctor_set(f, 1, lean_box(hi));
  lean_ctor_set_uint8(f, sizeof(void *) * 2, kind);
  return f;
}
/* Feed.Json.parseTop: malformed (box 0), notObject (box 1), or object (tag 2) with the last value
 * of each wanted key: Field kind (absent 0, str 1, num 2, other 3), lo, hi. */
LEAN_EXPORT lean_obj_res feed_json_parse_top(b_lean_obj_arg a, b_lean_obj_arg lon, b_lean_obj_arg hin, b_lean_obj_arg keys) {
  const u8_t *s = lean_sarray_cptr(a);
  size_t n = lean_sarray_size(a);
  size_t hi = lean_is_scalar(hin) && lean_unbox(hin) < n ? lean_unbox(hin) : n;
  if (!lean_is_scalar(lon) || lean_unbox(lon) >= hi) return lean_box(0);   /* nothing to scan */
  size_t i = j_skip_ws(s, hi, lean_unbox(lon));
  if (!(i < hi && s[i] == 0x7b)) {
    size_t j = j_value(s, hi, i);
    return j != J_NONE && j_skip_ws(s, hi, j) == hi ? lean_box(1) : lean_box(0);
  }
  size_t nk = lean_array_size(keys);
  /* the last value of each key: kind, lo, hi (the handler asks for at most four keys) */
  size_t small[3 * 8], *rec = nk <= 8 ? small : malloc(3 * nk * sizeof(size_t));
  for (size_t k = 0; k < 3 * nk; k++) rec[k] = 0;
  i = j_skip_ws(s, hi, i + 1);
  int first = 1, ok = 0;
  size_t j = 0;
  for (;;) {
    if (i >= hi) break;
    if (first && s[i] == 0x7d) { j = i + 1; ok = 1; break; }
    if (s[i] != 0x22) break;
    size_t klo = i + 1;
    j = j_string(s, hi, i + 1);
    if (j == J_NONE) break;
    size_t khi = j - 1;
    j = j_skip_ws(s, hi, j);
    if (j >= hi || s[j] != 0x3a) break;
    size_t vlo = j_skip_ws(s, hi, j + 1);
    size_t k = j_value(s, hi, vlo);
    if (k == J_NONE) break;
    for (size_t q = 0; q < nk; q++)
      if (j_key_eq(s, n, klo, khi, lean_array_get_core(keys, q))) {
        u8_t b = s[vlo];
        rec[3 * q] = b == 0x22 ? 1 : (b == 0x2d || j_digit(b)) ? 2 : 3;
        rec[3 * q + 1] = vlo; rec[3 * q + 2] = k;
        break;
      }
    k = j_skip_ws(s, hi, k);
    if (k >= hi) break;
    if (s[k] == 0x7d) { j = k + 1; ok = 1; break; }
    if (s[k] != 0x2c) break;
    i = j_skip_ws(s, hi, k + 1);
    first = 0;
  }
  lean_object *res = lean_box(0);
  if (ok && j_skip_ws(s, hi, j) == hi) {
    lean_object *fs = lean_alloc_array(nk, nk);
    for (size_t k = 0; k < nk; k++) lean_array_set_core(fs, k, j_field((u8_t)rec[3 * k], rec[3 * k + 1], rec[3 * k + 2]));
    res = lean_alloc_ctor(2, 1, 0);
    lean_ctor_set(res, 0, fs);
  }
  if (rec != small) free(rec);
  return res;
}

/* ---- Routing: Feed.route ------------------------------------------------------------------ */
/* Feed.parseId: PostId.ok v (tag 0) when s[lo, hi) is a non-empty run of digits of value v > 0,
 * else PostId.invalid (box 1); lo <= hi <= the size. */
static lean_obj_res r_post_id(const u8_t *s, size_t lo, size_t hi) {
  if (lo >= hi) return lean_box(1);
  u64_t v = 0;
  for (size_t i = lo; i < hi; i++) {
    if (s[i] < 0x30 || s[i] > 0x39) return lean_box(1);
    if (i - lo < 19) v = v * 10 + (u64_t)(s[i] - 0x30);
  }
  lean_object *nat;
  if (hi - lo <= 19) {
    if (v == 0) return lean_box(1);
    nat = u64_nat(v);
  } else {
    /* more than 19 digits: Lean's own conversion (a value this large is never a post) */
    char *buf = malloc(hi - lo + 1);
    memcpy(buf, s + lo, hi - lo);
    buf[hi - lo] = 0;
    nat = lean_cstr_to_nat(buf);
    free(buf);
    if (lean_is_scalar(nat) && lean_unbox(nat) == 0) return lean_box(1);
  }
  lean_object *o = lean_alloc_ctor(0, 1, 0);
  lean_ctor_set(o, 0, nat);
  return o;
}
static lean_obj_res r_with_id(unsigned tag, lean_obj_arg id) {
  lean_object *o = lean_alloc_ctor(tag, 1, 0);
  lean_ctor_set(o, 0, id);
  return o;
}
/* Feed.route: the route of a request from its method and the path a[pathLo, pathHi) (both ends
 * clamped): health 0, feed 1, post id (tag 2), create 3, like id (tag 4), notFound 5. */
LEAN_EXPORT lean_obj_res feed_route(b_lean_obj_arg a, b_lean_obj_arg r) {
  const u8_t *s = lean_sarray_cptr(a);
  size_t n = lean_sarray_size(a);
  lean_object *plo = lean_ctor_get(r, 0), *phi = lean_ctor_get(r, 1);
  size_t hi = lean_is_scalar(phi) && lean_unbox(phi) < n ? lean_unbox(phi) : n;
  size_t lo = lean_is_scalar(plo) && lean_unbox(plo) < hi ? lean_unbox(plo) : hi;
  size_t len = hi - lo;
  const u8_t *p = s + lo;
  switch (lean_ctor_get_uint8(r, sizeof(void *) * 7)) {
  case 0:   /* GET */
    if (len == 5 && !memcmp(p, "/feed", 5)) return lean_box(1);
    if (len == 7 && !memcmp(p, "/health", 7)) return lean_box(0);
    if (len > 7 && !memcmp(p, "/posts/", 7) && !memchr(p + 7, '/', len - 7)) return r_with_id(2, r_post_id(s, lo + 7, hi));
    return lean_box(5);
  case 1:   /* POST */
    if (len == 6 && !memcmp(p, "/posts", 6)) return lean_box(3);
    if (len > 12 && !memcmp(p, "/posts/", 7) && !memcmp(s + hi - 5, "/like", 5) && !memchr(p + 7, '/', len - 12))
      return r_with_id(4, r_post_id(s, lo + 7, hi - 5));
    return lean_box(5);
  default:
    return lean_box(5);
  }
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
#ifdef __SHA__
/* The compression function on the sixteen words w[0, 16) (big-endian words of a block), with
 * the SHA extensions (built when the CPU has them, as -march=native tells): four rounds per pair
 * of sha256rnds2, the message schedule by sha256msg1 and sha256msg2. The state is kept as ABEF
 * and CDGH, the order these instructions use. The SHA instructions have only legacy SSE encodings,
 * which are very slow while the upper halves of the vector registers are in use (here, hundreds
 * of cycles each): so this function clears them first and asks gcc for 128-bit vectors only. */
__attribute__((target("prefer-vector-width=128"), noinline)) static void sha_compress(u32_t h[8], const u32_t *win) {
  _mm256_zeroupper();
  __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[0]), 0xB1);   /* CDAB */
  __m128i st1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[4]), 0x1B);   /* EFGH */
  __m128i st0 = _mm_alignr_epi8(tmp, st1, 8);                                       /* ABEF */
  st1 = _mm_blend_epi16(st1, tmp, 0xF0);                                            /* CDGH */
  const __m128i abef = st0, cdgh = st1;
  __m128i x[4];
  for (int g = 0; g < 16; g++) {
    __m128i w;
    if (g < 4) w = x[g] = _mm_loadu_si128((const __m128i *)(win + 4 * g));
    else {
      /* W[i] = W[i-16] + s0(W[i-15]) + W[i-7] + s1(W[i-2]) for the four i of group g */
      w = _mm_add_epi32(_mm_sha256msg1_epu32(x[g & 3], x[(g + 1) & 3]), _mm_alignr_epi8(x[(g + 3) & 3], x[(g + 2) & 3], 4));
      w = x[g & 3] = _mm_sha256msg2_epu32(w, x[(g + 3) & 3]);
    }
    __m128i m = _mm_add_epi32(w, _mm_loadu_si128((const __m128i *)(sha_k + 4 * g)));
    st1 = _mm_sha256rnds2_epu32(st1, st0, m);
    st0 = _mm_sha256rnds2_epu32(st0, st1, _mm_shuffle_epi32(m, 0x0E));
  }
  st0 = _mm_add_epi32(st0, abef);
  st1 = _mm_add_epi32(st1, cdgh);
  tmp = _mm_shuffle_epi32(st0, 0x1B);                                               /* FEBA */
  st1 = _mm_shuffle_epi32(st1, 0xB1);                                               /* DCHG */
  _mm_storeu_si128((__m128i *)&h[0], _mm_blend_epi16(tmp, st1, 0xF0));              /* DCBA */
  _mm_storeu_si128((__m128i *)&h[4], _mm_alignr_epi8(st1, tmp, 8));                 /* HGFE */
}
#else
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
#endif
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
 * Feed/Handler.lean, which the self-test checks this against).
 *
 * The object is written with one reservation: room for every byte escaped to six, so the
 * copies below check no capacity. */
static inline void lit_bytes(lean_object *out, const char *s, size_t n) { memcpy(ba_end(out), s, n); ba_grow(out, n); }

/* The number of decimal digits of v (1 for 0). */
static inline unsigned dec_len(u64_t v) {
  static const u64_t p10[20] = {1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull,
                                100000000ull, 1000000000ull, 10000000000ull, 100000000000ull, 1000000000000ull,
                                10000000000000ull, 100000000000000ull, 1000000000000000ull, 10000000000000000ull,
                                100000000000000000ull, 1000000000000000000ull, 10000000000000000000ull};
  unsigned t = ((64u - (unsigned)__builtin_clzll(v | 1)) * 1233u) >> 12;   /* floor(log10 v) or one more */
  return t + 1 - ((v | 1) < p10[t]);
}
/* The decimal digits of v at d; the end. */
static inline u8_t *put_dec(u8_t *d, u64_t v) {
  u8_t *end = d + dec_len(v), *p = end;
  while (v >= 100) { unsigned r = (unsigned)(v % 100); v /= 100; p -= 2; memcpy(p, dig2 + 2 * r, 2); }
  if (v >= 10) { p -= 2; memcpy(p, dig2 + 2 * v, 2); } else *--p = (u8_t)('0' + v);
  return end;
}
/* escape(s[0, n)) at d, which has room for 6 n + 32 bytes; `sroom` bytes may be read from s
 * (n or more: the rest of the source's allocation). The end. */
static inline u8_t *esc_put(u8_t *d, const u8_t *s, size_t n, size_t sroom) {
  size_t i = 0;
  while (i < n) {
    size_t rem = n - i;
    __m256i v;
    if (LIKELY(rem >= 32 || sroom - i >= 32)) v = _mm256_loadu_si256((const __m256i *)(s + i));
    else {
      u8_t tmp[32] = {0};
      memcpy(tmp, s + i, rem);
      v = _mm256_loadu_si256((const __m256i *)tmp);
    }
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
  return d;
}
/* s[0, n) at d, which has room for n + 32 bytes, when no byte of it needs escaping (its escape
 * is itself); `sroom` bytes may be read from s. The end. */
static inline u8_t *raw_put(u8_t *d, const u8_t *s, size_t n, size_t sroom) {
  if (n < 32) {
    if (LIKELY(sroom >= 32)) _mm256_storeu_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
    else copy_small(d, s, n);
    return d + n;
  }
  size_t i = 0;
  for (; i + 32 < n; i += 32) _mm256_storeu_si256((__m256i *)(d + i), _mm256_loadu_si256((const __m256i *)(s + i)));
  _mm256_storeu_si256((__m256i *)(d + n - 32), _mm256_loadu_si256((const __m256i *)(s + n - 32)));
  return d + n;
}
/* Whether no byte of a needs escaping. */
static int ba_plain(b_lean_obj_arg a) {
  const u8_t *s = lean_sarray_cptr(a);
  size_t n = lean_sarray_size(a), i = 0;
  const __m256i q = _mm256_set1_epi8('"'), bs = _mm256_set1_epi8('\\'), c = _mm256_set1_epi8(0x1f);
#define ESC_V(v) _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs)), \
                                 _mm256_cmpeq_epi8(_mm256_max_epu8(v, c), c))
  for (; i + 128 <= n; i += 128) {
    __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + i)), v1 = _mm256_loadu_si256((const __m256i *)(s + i + 32));
    __m256i v2 = _mm256_loadu_si256((const __m256i *)(s + i + 64)), v3 = _mm256_loadu_si256((const __m256i *)(s + i + 96));
    __m256i x = _mm256_or_si256(_mm256_or_si256(ESC_V(v0), ESC_V(v1)), _mm256_or_si256(ESC_V(v2), ESC_V(v3)));
    if (!_mm256_testz_si256(x, x)) return 0;
  }
  for (; i + 32 <= n; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(s + i)), x = ESC_V(v);
    if (!_mm256_testz_si256(x, x)) return 0;
  }
#undef ESC_V
  for (; i < n; i++) if (s[i] == '"' || s[i] == '\\' || s[i] < 0x20) return 0;
  return 1;
}
/* A Nat as decimal digits at *d, the end of out's bytes (room for 20 digits reserved). A Nat
 * that is not small goes through feed_append_nat, and then `need` bytes are reserved again. */
static inline lean_object *obj_nat(lean_object *out, u8_t **d, b_lean_obj_arg n, size_t need) {
  if (LIKELY(lean_is_scalar(n))) { *d = put_dec(*d, (u64_t)lean_unbox(n)); return out; }
  lean_to_sarray(out)->m_size = (size_t)(*d - lean_sarray_cptr(out));
  out = feed_append_nat(out, n);
  out = ba_reserve(out, need);
  *d = ba_end(out);
  return out;
}
/* One post object after `start` (slen bytes), as feed_append_post. `plain`: no byte of src or
 * names needs escaping, so the strings are copied. */
static inline lean_object *post_obj(lean_object *out, const u8_t *start, size_t slen, b_lean_obj_arg idn, b_lean_obj_arg src,
                                    b_lean_obj_arg bn, b_lean_obj_arg cn, b_lean_obj_arg en, b_lean_obj_arg names,
                                    b_lean_obj_arg sn, b_lean_obj_arg tn, b_lean_obj_arg likesn, int plain) {
  static const char kBody[] = ",\"body\":\"", kCreated[] = "\",\"created_at\":\"", kAuthor[] = "\",\"author\":\"",
                    kLikes[] = "\",\"like_count\":";
  u64_t b, c, c2, e, s, t;
  clamp_range(src, bn, cn, &b, &c);
  clamp_range(src, cn, en, &c2, &e);
  clamp_range(names, sn, tn, &s, &t);
  size_t lb = (size_t)(c - b), lc = (size_t)(e - c2), la = (size_t)(t - s);
  /* at most 16 + 19 + 9 + 16 + 12 + 15 + 19 + 1 literal and digit bytes, six per escaped byte
   * (one when plain), and 32 past the end of the last string (strings are stored 32 bytes at a
   * time) */
  size_t need = (plain ? 1 : 6) * (lb + lc + la) + 256;
  out = ba_reserve(out, slen + need);
  u8_t *d = ba_end(out);
  copy_small(d, start, slen);
  d += slen;
  out = obj_nat(out, &d, idn, need);
  memcpy(d, kBody, sizeof kBody - 1);
  d += sizeof kBody - 1;
  const u8_t *sp = lean_sarray_cptr(src), *np = lean_sarray_cptr(names);
  size_t scap = lean_sarray_capacity(src), ncap = lean_sarray_capacity(names);
  if (plain) {
    d = raw_put(d, sp + b, lb, scap - (size_t)b);
    memcpy(d, kCreated, sizeof kCreated - 1);
    d += sizeof kCreated - 1;
    d = raw_put(d, sp + c2, lc, scap - (size_t)c2);
    memcpy(d, kAuthor, sizeof kAuthor - 1);
    d += sizeof kAuthor - 1;
    d = raw_put(d, np + s, la, ncap - (size_t)s);
  } else {
    d = esc_put(d, sp + b, lb, scap - (size_t)b);
    memcpy(d, kCreated, sizeof kCreated - 1);
    d += sizeof kCreated - 1;
    d = esc_put(d, sp + c2, lc, scap - (size_t)c2);
    memcpy(d, kAuthor, sizeof kAuthor - 1);
    d += sizeof kAuthor - 1;
    d = esc_put(d, np + s, la, ncap - (size_t)s);
  }
  memcpy(d, kLikes, sizeof kLikes - 1);
  d += sizeof kLikes - 1;
  out = obj_nat(out, &d, likesn, 64);
  *d++ = '}';
  lean_to_sarray(out)->m_size = (size_t)(d - lean_sarray_cptr(out));
  return out;
}
LEAN_EXPORT lean_obj_res feed_append_post(lean_obj_arg out, b_lean_obj_arg start, b_lean_obj_arg idn, b_lean_obj_arg src,
                                          b_lean_obj_arg bn, b_lean_obj_arg cn, b_lean_obj_arg en, b_lean_obj_arg names,
                                          b_lean_obj_arg sn, b_lean_obj_arg tn, b_lean_obj_arg likesn) {
  /* the start bytes are copied first; reserve them separately when they are long */
  size_t slen = lean_sarray_size(start);
  if (UNLIKELY(slen > 16)) {
    out = ba_reserve(out, slen + 32);
    lit_append(out, start);
    return post_obj(out, (const u8_t *)"", 0, idn, src, bn, cn, en, names, sn, tn, likesn, 0);
  }
  return post_obj(out, lean_sarray_cptr(start), slen, idn, src, bn, cn, en, names, sn, tn, likesn, 0);
}

/* Feed.feedFromPage: "{\"posts\":[" ++ the post objects of the rows whose author was found (at
 * most 20, separated by commas) ++ "]}" in the cleared body, with the number of objects and the
 * number of rows (renderPage and the literals of Feed/Handler.lean, which the self-test checks
 * this against). When no byte of the page's text needs escaping, which one pass over it tells,
 * its strings are copied. */
LEAN_EXPORT lean_obj_res feed_feed_from_page(lean_obj_arg body, b_lean_obj_arg pg) {
  lean_object *blob = lean_ctor_get(pg, 0), *cols = lean_ctor_get(pg, 1);
  size_t rows = lean_array_size(cols) / 8, n = 0;
  lean_object *out = feed_bytes_clear(body);
  out = ba_reserve(out, 64);
  lit_bytes(out, "{\"posts\":[", 10);
  int plain = ba_plain(blob);
  lean_object **m = lean_array_cptr(cols);
  for (size_t k = 0; k < rows && n < 20; k++, m += 8) {
    if (m[4] != lean_box(1)) continue;
    out = post_obj(out, (const u8_t *)",{\"id\":" + (n == 0), n == 0 ? 6 : 7, m[0], blob, m[1], m[2], m[3], blob, m[5],
                   m[6], m[7], plain);
    n++;
  }
  out = ba_reserve(out, 2);
  lit_bytes(out, "]}", 2);
  lean_object *counts = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(counts, 0, lean_box(n));
  lean_ctor_set(counts, 1, lean_box(rows));
  lean_object *r = lean_alloc_ctor(0, 2, 0);
  lean_ctor_set(r, 0, out);
  lean_ctor_set(r, 1, counts);
  return r;
}

/* ---- Responses written in place: Feed.respondFeed, Feed.Page.authored --------------------- */
/* Feed.Page.authored: the rows k < size / 8 with cols[8k + 4] = 1, counted up to limit */
LEAN_EXPORT lean_obj_res feed_page_authored(b_lean_obj_arg pg, b_lean_obj_arg limitn) {
  lean_object *cols = lean_ctor_get(pg, 1);
  size_t rows = lean_array_size(cols) / 8;
  u64_t limit = nat_u64(limitn), n = 0;
  lean_object **m = lean_array_cptr(cols);
  for (size_t k = 0; k < rows && n < limit; k++) n += m[8 * k + 4] == lean_box(1);
  return lean_box((size_t)n);
}

/* respond's status line for 200 and its two ends of the head (Feed/Handler.lean) */
static const char k_head200[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
static const char k_end_head[] = "\r\n\r\n", k_end_head_close[] = "\r\nConnection: close\r\n\r\n";
/* out ++ the head of a 200 response with a body of len bytes, with room reserved for the body
 * and 32 bytes more (the copies store 32 bytes at a time) */
static inline __attribute__((always_inline)) lean_object *head_200(lean_object *out, size_t len, int close) {
  size_t el = close ? sizeof k_end_head_close - 1 : sizeof k_end_head - 1;
  out = ba_reserve(out, sizeof k_head200 - 1 + 20 + el + len + 32);
  u8_t *d = ba_end(out);
  memcpy(d, k_head200, sizeof k_head200 - 1);
  d = put_dec(d + sizeof k_head200 - 1, len);
  memcpy(d, close ? k_end_head_close : k_end_head, el);
  lean_to_sarray(out)->m_size = (size_t)(d + el - lean_sarray_cptr(out));
  return out;
}
/* a rendered row of a feed page: id, like count, and the clamped ranges of its strings */
typedef struct { u64_t id, likes, b, lb, c, lc, s, la; } feed_row_t;

/* Feed.respondFeed: respond out 200 (feedFromPage empty pg).1 close. When no byte of the page's
 * text needs escaping and every rendered id and like count is small, the body's length is known
 * from the rows, so the head is written first and the body after it, in place; otherwise the body
 * is rendered by feed_feed_from_page and copied after the head. */
LEAN_EXPORT lean_obj_res feed_respond_feed(lean_obj_arg out, b_lean_obj_arg pg, uint8_t close) {
  lean_object *blob = lean_ctor_get(pg, 0), *cols = lean_ctor_get(pg, 1);
  size_t rows = lean_array_size(cols) / 8, n = 0, len = 12;   /* {"posts":[ and ]} */
  lean_object **m = lean_array_cptr(cols);
  feed_row_t r[20];
  int fast = ba_plain(blob);
  for (size_t k = 0; fast && k < rows && n < 20; k++, m += 8) {
    if (m[4] != lean_box(1)) continue;
    if (UNLIKELY(!lean_is_scalar(m[0]) || !lean_is_scalar(m[7]))) { fast = 0; break; }
    feed_row_t *q = &r[n];
    u64_t b, c, c2, e, s, t;
    clamp_range(blob, m[1], m[2], &b, &c);
    clamp_range(blob, m[2], m[3], &c2, &e);
    clamp_range(blob, m[5], m[6], &s, &t);
    q->id = (u64_t)lean_unbox(m[0]); q->likes = (u64_t)lean_unbox(m[7]);
    q->b = b; q->lb = c - b; q->c = c2; q->lc = e - c2; q->s = s; q->la = t - s;
    /* {"id": (after a comma but for the first) ,"body":" ","created_at":" ","author":" ","like_count": } */
    len += (n ? 7 : 6) + 9 + 16 + 12 + 15 + 1 + dec_len(q->id) + dec_len(q->likes) + q->lb + q->lc + q->la;
    n++;
  }
  if (UNLIKELY(!fast)) {
    lean_object *res = feed_feed_from_page(lean_alloc_sarray(1, 0, 4096), pg);
    lean_object *body = lean_ctor_get(res, 0);
    lean_inc(body);
    lean_dec(res);
    size_t bl = lean_sarray_size(body);
    out = head_200(out, bl, close);
    memcpy(ba_end(out), lean_sarray_cptr(body), bl);
    ba_grow(out, bl);
    lean_dec(body);
    return out;
  }
  out = head_200(out, len, close);
  u8_t *d = ba_end(out);
  const u8_t *sp = lean_sarray_cptr(blob);
  size_t scap = lean_sarray_capacity(blob);
  memcpy(d, "{\"posts\":[", 10);
  d += 10;
  for (size_t j = 0; j < n; j++) {
    const feed_row_t *q = &r[j];
    *d = ',';
    d += j != 0;
    memcpy(d, "{\"id\":", 6);
    d = put_dec(d + 6, q->id);
    memcpy(d, ",\"body\":\"", 9);
    d = raw_put(d + 9, sp + q->b, (size_t)q->lb, scap - (size_t)q->b);
    memcpy(d, "\",\"created_at\":\"", 16);
    d = raw_put(d + 16, sp + q->c, (size_t)q->lc, scap - (size_t)q->c);
    memcpy(d, "\",\"author\":\"", 12);
    d = raw_put(d + 12, sp + q->s, (size_t)q->la, scap - (size_t)q->s);
    memcpy(d, "\",\"like_count\":", 15);
    d = put_dec(d + 15, q->likes);
    *d++ = '}';
  }
  memcpy(d, "]}", 2);
  lean_to_sarray(out)->m_size = (size_t)(d + 2 - lean_sarray_cptr(out));
  return out;
}

/* Feed.respondPost: respond out 200 (postFromPage empty id pg).2 close: the body is empty when
 * the page's first row is missing or has no author; otherwise {"post":…} around the post object
 * of that row with the given id. Written in place after the head when no byte of the text needs
 * escaping and the numbers are small; otherwise rendered by post_obj and copied after the head. */
LEAN_EXPORT lean_obj_res feed_respond_post(lean_obj_arg out, b_lean_obj_arg idn, b_lean_obj_arg pg, uint8_t close) {
  lean_object *blob = lean_ctor_get(pg, 0), *cols = lean_ctor_get(pg, 1);
  lean_object **m = lean_array_cptr(cols);
  if (lean_array_size(cols) < 8 || m[4] != lean_box(1)) return head_200(out, 0, close);
  if (UNLIKELY(!lean_is_scalar(idn) || !lean_is_scalar(m[7]) || !ba_plain(blob))) {
    lean_object *body = lean_alloc_sarray(1, 0, 256);
    lit_bytes(body, "{\"post\":", 8);
    body = post_obj(body, (const u8_t *)"{\"id\":", 6, idn, blob, m[1], m[2], m[3], blob, m[5], m[6], m[7], 0);
    body = ba_reserve(body, 1);
    lit_bytes(body, "}", 1);
    size_t bl = lean_sarray_size(body);
    out = head_200(out, bl, close);
    memcpy(ba_end(out), lean_sarray_cptr(body), bl);
    ba_grow(out, bl);
    lean_dec(body);
    return out;
  }
  u64_t b, c, c2, e, s, t;
  clamp_range(blob, m[1], m[2], &b, &c);
  clamp_range(blob, m[2], m[3], &c2, &e);
  clamp_range(blob, m[5], m[6], &s, &t);
  u64_t id = (u64_t)lean_unbox(idn), likes = (u64_t)lean_unbox(m[7]);
  /* {"post":{"id": ,"body":" ","created_at":" ","author":" ","like_count": }} */
  size_t len = 14 + 9 + 16 + 12 + 15 + 2 + dec_len(id) + dec_len(likes) + (size_t)(c - b) + (size_t)(e - c2) + (size_t)(t - s);
  out = head_200(out, len, close);
  u8_t *d = ba_end(out);
  const u8_t *sp = lean_sarray_cptr(blob);
  size_t scap = lean_sarray_capacity(blob);
  memcpy(d, "{\"post\":{\"id\":", 14);
  d = put_dec(d + 14, id);
  memcpy(d, ",\"body\":\"", 9);
  d = raw_put(d + 9, sp + b, (size_t)(c - b), scap - (size_t)b);
  memcpy(d, "\",\"created_at\":\"", 16);
  d = raw_put(d + 16, sp + c2, (size_t)(e - c2), scap - (size_t)c2);
  memcpy(d, "\",\"author\":\"", 12);
  d = raw_put(d + 12, sp + s, (size_t)(t - s), scap - (size_t)s);
  memcpy(d, "\",\"like_count\":", 15);
  d = put_dec(d + 15, likes);
  memcpy(d, "}}", 2);
  lean_to_sarray(out)->m_size = (size_t)(d + 2 - lean_sarray_cptr(out));
  return out;
}
