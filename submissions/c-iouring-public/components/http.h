/* parser/http.h (sha256 3db1fb0cf380), developed separately for this entry. */
// http.h - HTTP/1.1 request parser for the $12 server challenge API.
// Port of ideas/fastparse.h fast_parse3 to aarch64 NEON, x86-64 AVX2, AVX-512BW/VL (opt-in with
// -DFP_USE_AVX512; UNTESTED on real AVX-512 hardware) and a plain scalar fallback.
//
// Contract (as fast_parse3):
//   - the caller guarantees >= 64 readable bytes after buf[len] (contents ignored);
//   - fast_parse() returns >0 = bytes consumed (headers + body), FP_NEED_MORE (0) or
//     FP_FALLBACK (-1: unusual or invalid -> hand the request to a full parser);
//   - req_t.tok points into buf; body_off is relative to the buf pointer passed in.
//   - fast_parse_many(): pipelined batch, see the bottom of this file.
//   - fast_parse_learn(): same results, plus a fast path for the most common header layout per
//     route, learned at runtime into a caller-owned fp_learn_t (see "learned header layout").
//
// Backend is picked at compile time (FP_BACKEND, FP_BACKEND_NAME); -DFP_FORCE_SCALAR forces scalar,
// -DFP_WALK_ONLY makes SIMD backends use core 2 only (an A/B knob for x86, see RESULTS.md).
//
// Two cores, both always compiled and required to agree on every input:
//   fp_core_cand  (SIMD backends): per 64-byte block, one ISA-specific classify step yields two
//                 bitmasks: LF and "c/t" (case-folded). On GET routes only header lines starting with
//                 c/t (Content-Length, Transfer-Encoding) are visited, found straight from the masks:
//                 a k6 GET never touches a header line. POST routes (which need Authorization too)
//                 walk every line start, like core 2; fast_parse() sends requests starting with
//                 "POST" straight to core 2 (with the SIMD LF mask) since the c/t mask is wasted there.
//   fp_core_walk  (scalar backend / fast_parse_scalar): LF mask only, then a walk over every line
//                 start. Without SIMD the extra mask costs more than it saves.
// Everything except the classify step is shared portable code (64-bit masks, SWAR words).
//
// Stable internal API: explore/batch-parse/bp.h builds on these. Don't change their signatures or meaning
// without telling that session; `make test` here also runs its tests (make lockstep):
//   fp_quick_get, fp_reqline_canon, fp_route_auth, FP_CORE_L(b, len, r, l0), FP_BACKEND, FP_INLINE, FP_LIKELY,
//   fp_ctz, fp_ld64, fp_c8, fp_nondigit, fp_swar8 (the last three only in bp's SIMD request-line variants).
#pragma once
#include <stdint.h>
#include <string.h>

#if defined(FP_FORCE_SCALAR)
#define FP_BACKEND 0
#elif defined(FP_AVX512_EMU)    // test only: the AVX-512 code path on avx512_emu.h's C intrinsics
#define FP_BACKEND 3
#elif defined(FP_USE_AVX512) && defined(__AVX512BW__) && defined(__AVX512VL__)
#define FP_BACKEND 3
#elif defined(__AVX2__)
#define FP_BACKEND 2
#elif defined(__aarch64__) && defined(__ARM_NEON)
#define FP_BACKEND 1
#else
#define FP_BACKEND 0
#endif
#if defined(FP_USE_AVX512) && FP_BACKEND != 3 && !defined(FP_FORCE_SCALAR) && !defined(FP_AVX512_EMU)
#error "FP_USE_AVX512 needs -mavx512bw -mavx512vl (e.g. -march=cascadelake)"
#endif

#if FP_BACKEND == 1
#include <arm_neon.h>
#elif defined(FP_AVX512_EMU)
#include "avx512_emu.h"
#elif FP_BACKEND >= 2
#include <immintrin.h>
#endif

#if FP_BACKEND == 0
#define FP_BACKEND_NAME "scalar"
#elif FP_BACKEND == 1
#define FP_BACKEND_NAME "neon"
#elif FP_BACKEND == 2
#define FP_BACKEND_NAME "avx2"
#elif defined(FP_USE_ZMM)
#define FP_BACKEND_NAME "avx512bw-zmm" FP_EMU_TAG
#else
#define FP_BACKEND_NAME "avx512bw-ymm" FP_EMU_TAG
#endif
#ifdef FP_AVX512_EMU
#define FP_EMU_TAG " (C-emulated intrinsics)"
#else
#define FP_EMU_TAG ""
#endif

enum { FP_NEED_MORE = 0, FP_FALLBACK = -1, FP_FULL = 1 };   // FP_FULL: fast_parse_many status only
#define FP_MAXHDR 8192
#define FP_NCH (FP_MAXHDR / 64)

typedef struct {
  int route;
  int id_ok; int64_t id;          // GETPOST / LIKE: id_ok = positive integer, <= 16 digits
  int auth; const char *tok; int tok_len;   // auth: 1 = "Bearer " header present (POST routes only)
  int body_off, body_len;
} req_t;

#define FP_INLINE static inline __attribute__((always_inline))
#define FP_LIKELY(x) __builtin_expect(!!(x), 1)
#define FP_UNLIKELY(x) __builtin_expect(!!(x), 0)

FP_INLINE uint64_t fp_ld64(const char *p) { uint64_t x; memcpy(&x, p, 8); return x; }
FP_INLINE uint32_t fp_ld32(const char *p) { uint32_t x; memcpy(&x, p, 4); return x; }
FP_INLINE int fp_ctz(uint64_t x) { return __builtin_ctzll(x); }
// little-endian constant from a string literal of n <= 8 chars (folds at -O1+)
FP_INLINE uint64_t fp_c8(const char *s, int n) { uint64_t x = 0; memcpy(&x, s, n); return x; }
FP_INLINE uint64_t fp_bytes(int n) { return n >= 8 ? ~0ull : (1ull << (n * 8)) - 1; }   // n in 0..8
#define FP_ONES 0x0101010101010101ull
#define FP_HI 0x8080808080808080ull

// ---------------- classify 64 bytes -> bitmasks (bit i = byte i) ----------------
// Scalar: SWAR exact byte-equality per 8-byte word, then a multiply-gather "movemask".
FP_INLINE uint64_t fp_swar_eq(uint64_t x, uint64_t pat) {   // 0x80 in every byte of x equal to pat
  uint64_t t = x ^ pat;
  return ~(((t & ~FP_HI) + ~FP_HI) | t) & FP_HI;
}
FP_INLINE uint64_t fp_swar_mm(uint64_t m) { return ((m >> 7) * 0x0102040810204080ull) >> 56; }
// Two classify steps (64 bytes -> bitmasks):
//   fp_lf64:  lf = x == '\n'                                                             (walk core)
//   fp_cls64: lf, and ct = (x|0x20) is 'c' or 't', or x <= 0x20 (line starts to look at: Content-Length,
//             Transfer-Encoding, and SP/HT/control = obs-fold or junk, which fall back)       (cand core)
// plus the bare-CR check (fp_crbad / fp_crany, below), kept in vector registers: no extra mask per block.
FP_INLINE uint64_t fp_lf64_scalar(const char *p) {
  uint64_t l = 0;
  for (int i = 0; i < 8; i++) l |= fp_swar_mm(fp_swar_eq(fp_ld64(p + 8 * i), '\n' * FP_ONES)) << (8 * i);
  return l;
}
FP_INLINE uint64_t fp_swar_le20(uint64_t w) {   // 0x80 in every byte <= 0x20
  return ~(((w & ~FP_HI) + (0x5F * FP_ONES)) | w) & FP_HI;   // byte + 0x5F reaches 0x80 iff byte >= 0x21 (top bit clear)
}
FP_INLINE void fp_cls64_scalar(const char *p, uint64_t *lf, uint64_t *ct) {
  uint64_t l = 0, t = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t w = fp_ld64(p + 8 * i), f = w | 0x2020202020202020ull;
    l |= fp_swar_mm(fp_swar_eq(w, '\n' * FP_ONES)) << (8 * i);
    t |= fp_swar_mm(fp_swar_eq(f, 'c' * FP_ONES) | fp_swar_eq(f, 't' * FP_ONES) | fp_swar_le20(w)) << (8 * i);
  }
  *lf = l; *ct = t;
}
// bare CR = a CR whose next byte is not LF. Some parsers end lines there, so a header block containing
// one is ambiguous: fall back. Checked per 64-byte block into an accumulator; only bytes [0, n) count
// (the block holding the end of the headers passes n = end + 1); the byte after p[n - 1] is read (padding).
FP_INLINE uint64_t fp_crbad_scalar(uint64_t acc, const char *p, int n) {
  for (int i = 0; i < 8; i++) {
    int m = n - 8 * i; if (m <= 0) break;
    uint64_t bad = fp_swar_eq(fp_ld64(p + 8 * i), '\r' * FP_ONES) & ~fp_swar_eq(fp_ld64(p + 8 * i + 1), '\n' * FP_ONES);
    acc |= bad & fp_bytes(m < 8 ? m : 8);
  }
  return acc;
}
static const unsigned char fp_iota[64] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
  22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};

#if FP_BACKEND == 1
// NEON has no movemask. vld4q de-interleaves (v.val[j] lane i = byte 4i+j), a vsri chain packs the
// four 0x00/0xFF compare vectors into one nibble per lane, and shrn #4 squeezes the 16 nibbles into
// a 64-bit mask in byte order: 4 vsri + shrn + fmov per predicate per 64 bytes.
FP_INLINE uint64_t fp_neon_pack4(uint8x16_t c0, uint8x16_t c1, uint8x16_t c2, uint8x16_t c3) {
  uint8x16_t t0 = vsriq_n_u8(c1, c0, 1), t1 = vsriq_n_u8(c3, c2, 1);
  uint8x16_t t2 = vsriq_n_u8(t1, t0, 2), t3 = vsriq_n_u8(t2, t2, 4);
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(t3), 4)), 0);
}
FP_INLINE uint64_t fp_lf64_simd(const char *p) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
  const uint8x16_t LF = vdupq_n_u8('\n');
  return fp_neon_pack4(vceqq_u8(v.val[0], LF), vceqq_u8(v.val[1], LF), vceqq_u8(v.val[2], LF), vceqq_u8(v.val[3], LF));
}
FP_INLINE void fp_cls64_simd(const char *p, uint64_t *lf, uint64_t *ct) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
  const uint8x16_t LF = vdupq_n_u8('\n'), F = vdupq_n_u8(0x20), C = vdupq_n_u8('c'), T = vdupq_n_u8('t');
  *lf = fp_neon_pack4(vceqq_u8(v.val[0], LF), vceqq_u8(v.val[1], LF), vceqq_u8(v.val[2], LF), vceqq_u8(v.val[3], LF));
#define FP_CT(x) vorrq_u8(vorrq_u8(vceqq_u8(vorrq_u8(x, F), C), vceqq_u8(vorrq_u8(x, F), T)), vcleq_u8(x, F))
  *ct = fp_neon_pack4(FP_CT(v.val[0]), FP_CT(v.val[1]), FP_CT(v.val[2]), FP_CT(v.val[3]));
#undef FP_CT
}
typedef uint8x16_t fp_crv_t;
FP_INLINE fp_crv_t fp_crv0(void) { return vdupq_n_u8(0); }
// Same de-interleaved vld4 as the classify step (the compiler merges the loads and the LF compares):
// v.val[j] lane i = byte 4i+j, so the byte after val[j] is val[j+1] (same lane), and after val[3] it is
// val[0] one lane up, lane 15 taking byte 64. The final block keeps lanes with 4i+j < n.
static const unsigned char fp_iota4[64] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60,
  1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 54, 58, 62,
  3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63};
FP_INLINE fp_crv_t fp_crbad_simd(fp_crv_t acc, const char *p, int n) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
  const uint8x16_t CR = vdupq_n_u8('\r'), LF = vdupq_n_u8('\n');
  uint8x16_t l0 = vceqq_u8(v.val[0], LF), l1 = vceqq_u8(v.val[1], LF), l2 = vceqq_u8(v.val[2], LF), l3 = vceqq_u8(v.val[3], LF);
  uint8x16_t l4 = vextq_u8(l0, vceqq_u8(vld1q_dup_u8((const uint8_t *)p + 64), LF), 1);
  uint8x16_t b0 = vbicq_u8(vceqq_u8(v.val[0], CR), l1), b1 = vbicq_u8(vceqq_u8(v.val[1], CR), l2);
  uint8x16_t b2 = vbicq_u8(vceqq_u8(v.val[2], CR), l3), b3 = vbicq_u8(vceqq_u8(v.val[3], CR), l4);
  if (n < 64) {
    const uint8x16_t N = vdupq_n_u8((uint8_t)n);
    b0 = vandq_u8(b0, vcltq_u8(vld1q_u8(fp_iota4), N)); b1 = vandq_u8(b1, vcltq_u8(vld1q_u8(fp_iota4 + 16), N));
    b2 = vandq_u8(b2, vcltq_u8(vld1q_u8(fp_iota4 + 32), N)); b3 = vandq_u8(b3, vcltq_u8(vld1q_u8(fp_iota4 + 48), N));
  }
  return vorrq_u8(acc, vorrq_u8(vorrq_u8(b0, b1), vorrq_u8(b2, b3)));
}
FP_INLINE int fp_crany_simd(fp_crv_t acc) { return vmaxvq_u8(acc) != 0; }
#elif FP_BACKEND >= 2
// AVX2 (and, compiled the same way, the AVX-512 builds: there the compares go to mask registers anyway)
#if FP_BACKEND == 2
FP_INLINE uint64_t fp_mm2(__m256i x, __m256i y) {
  return (uint32_t)_mm256_movemask_epi8(x) | ((uint64_t)(uint32_t)_mm256_movemask_epi8(y) << 32);
}
FP_INLINE uint64_t fp_lf64_simd(const char *p) {
  const __m256i LF = _mm256_set1_epi8('\n');
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  return fp_mm2(_mm256_cmpeq_epi8(v0, LF), _mm256_cmpeq_epi8(v1, LF));
}
// x <= 0x20 (unsigned) <=> min(x, 0x20) == x
#define FP_LE20(v) _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8(0x20)), v)
FP_INLINE void fp_cls64_simd(const char *p, uint64_t *lf, uint64_t *ct) {
  const __m256i LF = _mm256_set1_epi8('\n'), F = _mm256_set1_epi8(0x20), C = _mm256_set1_epi8('c'), T = _mm256_set1_epi8('t');
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  __m256i f0 = _mm256_or_si256(v0, F), f1 = _mm256_or_si256(v1, F);
  *lf = fp_mm2(_mm256_cmpeq_epi8(v0, LF), _mm256_cmpeq_epi8(v1, LF));
  *ct = fp_mm2(_mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(f0, C), _mm256_cmpeq_epi8(f0, T)), FP_LE20(v0)),
               _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(f1, C), _mm256_cmpeq_epi8(f1, T)), FP_LE20(v1)));
}
#undef FP_LE20
#else
// Same predicates as the AVX2 code, but compares go straight into mask registers (no movemask).
// Default: AVX-512BW+VL on ymm (no zmm -> no AVX-512 frequency license drop on Cascade Lake).
// -DFP_USE_ZMM: one 512-bit load + compare per predicate.
#if defined(FP_USE_ZMM)
#define FP_EQ64(v, c) ((uint64_t)_mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(c)))
FP_INLINE uint64_t fp_lf64_simd(const char *p) { return FP_EQ64(_mm512_loadu_si512((const void *)p), '\n'); }
FP_INLINE void fp_cls64_simd(const char *p, uint64_t *lf, uint64_t *ct) {
  __m512i v = _mm512_loadu_si512((const void *)p), f = _mm512_or_si512(v, _mm512_set1_epi8(0x20));
  *lf = FP_EQ64(v, '\n');
  *ct = FP_EQ64(f, 'c') | FP_EQ64(f, 't') | (uint64_t)_mm512_cmple_epu8_mask(v, _mm512_set1_epi8(0x20));
}
#undef FP_EQ64
#else
#define FP_EQ32(v, c) ((uint64_t)(uint32_t)_mm256_cmpeq_epi8_mask(v, _mm256_set1_epi8(c)))
#define FP_LE32(v) ((uint64_t)(uint32_t)_mm256_cmple_epu8_mask(v, _mm256_set1_epi8(0x20)))
FP_INLINE uint64_t fp_lf64_simd(const char *p) {
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  return FP_EQ32(v0, '\n') | FP_EQ32(v1, '\n') << 32;
}
FP_INLINE void fp_cls64_simd(const char *p, uint64_t *lf, uint64_t *ct) {
  const __m256i F = _mm256_set1_epi8(0x20);
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  __m256i f0 = _mm256_or_si256(v0, F), f1 = _mm256_or_si256(v1, F);
  *lf = FP_EQ32(v0, '\n') | FP_EQ32(v1, '\n') << 32;
  *ct = (FP_EQ32(f0, 'c') | FP_EQ32(f0, 't') | FP_LE32(v0)) | (FP_EQ32(f1, 'c') | FP_EQ32(f1, 't') | FP_LE32(v1)) << 32;
}
#undef FP_EQ32
#undef FP_LE32
#endif
#endif
typedef __m256i fp_crv_t;   // the bare-CR accumulator: plain AVX2 vector ops on every x86 backend
FP_INLINE fp_crv_t fp_crv0(void) { return _mm256_setzero_si256(); }
FP_INLINE fp_crv_t fp_crbad_simd(fp_crv_t acc, const char *p, int n) {
  const __m256i CR = _mm256_set1_epi8('\r'), LF = _mm256_set1_epi8('\n'), N = _mm256_set1_epi8((char)n);
  for (int i = 0; i < 2; i++) {
    __m256i bad = _mm256_andnot_si256(_mm256_cmpeq_epi8(_mm256_loadu_si256((const void *)(p + 32 * i + 1)), LF),
                                      _mm256_cmpeq_epi8(_mm256_loadu_si256((const void *)(p + 32 * i)), CR));
    acc = _mm256_or_si256(acc, n >= 64 ? bad : _mm256_and_si256(bad, _mm256_cmpgt_epi8(N, _mm256_loadu_si256((const void *)(fp_iota + 32 * i)))));
  }
  return acc;
}
FP_INLINE int fp_crany_simd(fp_crv_t acc) { return !_mm256_testz_si256(acc, acc); }
#endif

// simd is a compile-time constant at every call site; the untaken side folds away.
FP_INLINE uint64_t fp_lf64(int simd, const char *p) {
#if FP_BACKEND != 0
  if (simd) return fp_lf64_simd(p);
#endif
  (void)simd; return fp_lf64_scalar(p);
}
FP_INLINE void fp_cls64(int simd, const char *p, uint64_t *lf, uint64_t *ct) {
#if FP_BACKEND != 0
  if (simd) { fp_cls64_simd(p, lf, ct); return; }
#endif
  (void)simd; fp_cls64_scalar(p, lf, ct);
}
// bare-CR accumulator for either path: the vector one on SIMD backends, a SWAR word otherwise
typedef struct {
#if FP_BACKEND != 0
  fp_crv_t v;
#endif
  uint64_t s;
} fp_cracc_t;
FP_INLINE void fp_cr_init(int simd, fp_cracc_t *a) {
#if FP_BACKEND != 0
  if (simd) { a->v = fp_crv0(); return; }
#endif
  (void)simd; a->s = 0;
}
FP_INLINE void fp_cr_add(int simd, fp_cracc_t *a, const char *p, int n) {   // n = 64: whole block
#if FP_BACKEND != 0
  if (simd) { a->v = fp_crbad_simd(a->v, p, n); return; }
#endif
  (void)simd; a->s = fp_crbad_scalar(a->s, p, n);
}
FP_INLINE int fp_cr_any(int simd, const fp_cracc_t *a) {
#if FP_BACKEND != 0
  if (simd) return fp_crany_simd(a->v);
#endif
  (void)simd; return a->s != 0;
}

// ---------------- SWAR helpers (replace the SSE fp_digits / fp_hname) ----------------
// nonzero in each byte of w that is not '0'..'9': digit iff the high nibble is 3 and adding 6 keeps
// it 3 (a carry out of a byte only happens for bytes >= 0xFA, which already failed, and only spills
// into the next = later byte)
FP_INLINE uint64_t fp_nondigit(uint64_t w) {
  return ((w & 0xF0F0F0F0F0F0F0F0ull) ^ 0x3030303030303030ull) | (((w + 0x0606060606060606ull) & 0xF0F0F0F0F0F0F0F0ull) ^ 0x3030303030303030ull);
}
// value of the first k (0..8) ASCII digits in the 8-byte word x (first digit = lowest byte)
FP_INLINE uint64_t fp_swar8(uint64_t x, int k) {
  x &= 0x0F0F0F0F0F0F0F0Full; x = k ? x << ((8 - k) * 8) : 0;
  x = (x * 2561) >> 8; x &= 0x00FF00FF00FF00FFull; x = (x * 6553601) >> 16; x &= 0x0000FFFF0000FFFFull;
  return (x * 42949672960001ull) >> 32;
}
// n bytes at p (16 readable) -> value; *ok = all digits, 1..16 of them, value > 0.
FP_INLINE int64_t fp_digits(const char *p, int n, int *ok) {
  uint32_t nn = (unsigned)n <= 16 ? (uint32_t)n : 0;
  int nhi = nn > 8 ? (int)nn - 8 : 0, nlo = (int)nn - nhi;
  uint64_t bad = (fp_nondigit(fp_ld64(p)) & fp_bytes(nn > 8 ? 8 : (int)nn)) | (fp_nondigit(fp_ld64(p + 8)) & fp_bytes(nhi));
  // two 8-digit SWAR halves: hi = first nhi digits, lo = last nlo digits
  int64_t val = (int64_t)fp_swar8(fp_ld64(p), nhi) * 100000000 + (int64_t)fp_swar8(fp_ld64(p + nhi), nlo);
  *ok = (nn != 0) & (bad == 0) & (val > 0);
  return val;
}

// Header-name constants as integer constant expressions (every compiler folds these; gcc did not fold
// the same computation done in an inline function): FP_S8 = 8 chars of a literal, little-endian;
// FP_LC = 0x20 in every byte that is a lowercase letter (x + 0x1F has its top bit set iff x >= 'a',
// x + 0x05 iff x > 'z'; ASCII only, no carries).
#define FP_S8(s) ((uint64_t)(unsigned char)(s)[0] | (uint64_t)(unsigned char)(s)[1] << 8 | (uint64_t)(unsigned char)(s)[2] << 16 | \
  (uint64_t)(unsigned char)(s)[3] << 24 | (uint64_t)(unsigned char)(s)[4] << 32 | (uint64_t)(unsigned char)(s)[5] << 40 | \
  (uint64_t)(unsigned char)(s)[6] << 48 | (uint64_t)(unsigned char)(s)[7] << 56)
#define FP_LC(c) ((((c) + 0x1F1F1F1F1F1F1F1Full) & ~((c) + 0x0505050505050505ull) & FP_HI) >> 2)
// case-insensitive prefix match of a lowercase name: the first 8 bytes against a0 (8 chars), then n1 <= 8
// bytes against a1 (an 8-char literal, '\0'-padded). Only letters are case-folded: '-' and ':' must match
// exactly (|0x20 would also turn CR into '-' and 0x1A into ':').
#define FP_HNAME(s, a0, a1, n1) fp_hname(s, FP_S8(a0), FP_LC(FP_S8(a0)), FP_S8(a1), FP_LC(FP_S8(a1)), n1)
FP_INLINE int fp_hname(const char *s, uint64_t c0, uint64_t m0, uint64_t c1, uint64_t m1, int n1) {
  return (((fp_ld64(s) | m0) ^ c0) | (((fp_ld64(s + 8) | m1) ^ c1) & fp_bytes(n1))) == 0;
}

FP_INLINE int fp_eq(const char *a, const char *b, int n) { return memcmp(a, b, n) == 0; }

// ==== BEGIN API SPEC ====
// Everything specific to the API lives between these markers: route ids, the general router, the
// canonical request-line templates and three policy hooks. gen/httpgen.py regenerates exactly this
// region from an API spec (+ optional traffic profile); the rest of the file is generic.
enum { R_NOTFOUND, R_FEED, R_HEALTH, R_GETPOST, R_CREATE, R_LIKE, FP_NROUTES };
#define FP_SPEC_NAME "hand-written (challenge API)"
// route needs the Authorization bearer token
FP_INLINE int fp_route_auth(int route) { return route == R_CREATE || route == R_LIKE; }
// before scanning: 1 = use the walk core (requests that will need Authorization: "POST")
FP_INLINE int fp_spec_walk(const char *b) { return fp_ld32(b) == 0x54534f50u; }
// learned-layout fast path worth attempting for this route
FP_INLINE int fp_spec_learn(int route) { (void)route; return 1; }

// Routing (same semantics as fastparse.h fp_route).
FP_INLINE void fp_route(const char *m, int ml, const char *p, int pl, req_t *r) {
  r->route = R_NOTFOUND; r->id_ok = 0; r->id = 0;
  if (ml == 3 && fp_eq(m, "GET", 3)) {
    if (pl == 5 && fp_eq(p, "/feed", 5)) r->route = R_FEED;
    else if (pl == 7 && fp_eq(p, "/health", 7)) r->route = R_HEALTH;
    else if (pl >= 7 && fp_eq(p, "/posts/", 7)) {
      r->id = fp_digits(p + 7, pl - 7, &r->id_ok);
      r->route = (r->id_ok || !memchr(p + 7, '/', pl - 7)) ? R_GETPOST : R_NOTFOUND; }
  } else if (ml == 4 && fp_eq(m, "POST", 4)) {
    if (pl == 6 && fp_eq(p, "/posts", 6)) r->route = R_CREATE;
    else if (pl >= 12 && fp_eq(p, "/posts/", 7) && fp_eq(p + pl - 5, "/like", 5)) {
      r->id = fp_digits(p + 7, pl - 12, &r->id_ok);
      r->route = (r->id_ok || !memchr(p + 7, '/', pl - 12)) ? R_LIKE : R_NOTFOUND; }
  }
}

// Canonical request lines of the API ("GET /feed", "GET /posts/<1..8 digits>", "POST /posts",
// "POST /posts/<1..8 digits>/like", single SPs, HTTP/1.1, CRLF) matched from the first bytes alone:
// no dependency on the LF scan, so the route is known (and its branch resolved) early.
// Returns l0 (the request line's LF) with r filled exactly as fp_reqline would, or 0 = not canonical.
FP_INLINE int fp_reqline_canon(const char *b, req_t *r) {
  uint64_t w0 = fp_ld64(b), w1 = fp_ld64(b + 8);
  int route, p, l0;
  if (w0 == fp_c8("GET /fee", 8)) {
    if (w1 != fp_c8("d HTTP/1", 8) || fp_ld32(b + 16) != 0x0a0d312eu) return 0;   // ".1\r\n"
    r->route = R_FEED; r->id_ok = 0; r->id = 0; l0 = 19; goto out;
  } else if (w0 == fp_c8("GET /pos", 8) && (w1 & 0xFFFFFF) == fp_c8("ts/", 3)) { route = R_GETPOST; p = 11;
  } else if (w0 == fp_c8("POST /po", 8)) {
    if (w1 == fp_c8("sts HTTP", 8)) {
      if (fp_ld64(b + 14) != fp_c8("TP/1.1\r\n", 8)) return 0;
      r->route = R_CREATE; r->id_ok = 0; r->id = 0; l0 = 21; goto out;
    }
    if ((uint32_t)w1 != fp_ld32("sts/")) return 0;
    route = R_LIKE; p = 12;
  } else return 0;
  {
    uint64_t d = fp_ld64(b + p), nd = fp_nondigit(d);
    if (!nd) return 0;                                  // 8+ digits: general path
    int n = fp_ctz(nd) >> 3;
    if (!n) return 0;
    const char *q = b + p + n;
    if (route == R_GETPOST) {
      if (fp_ld64(q) != fp_c8(" HTTP/1.", 8) || fp_ld32(q + 7) != 0x0a0d312eu) return 0;
      l0 = p + n + 10;
    } else {
      if (fp_ld64(q) != fp_c8("/like HT", 8) || fp_ld64(q + 8) != fp_c8("TP/1.1\r\n", 8)) return 0;
      l0 = p + n + 15;
    }
    r->route = route; r->id = (int64_t)fp_swar8(d, n); r->id_ok = r->id > 0;
  }
out:
  r->auth = 0; r->tok = 0; r->tok_len = 0;
  return l0;
}
// ==== END API SPEC ====



// ---------------- shared pieces of both cores ----------------
// request line [0, l0]: METHOD SP+ path SP* "HTTP/1.1" CR LF -> route. 0 or FP_FALLBACK.
FP_INLINE int fp_reqline(const char *b, int l0, req_t *r) {
  if (l0 < 16 || b[l0 - 1] != '\r' || b[l0 - 10] != ' ' || fp_ld64(b + l0 - 9) != fp_c8("HTTP/1.1", 8)) return FP_FALLBACK;
  uint32_t m4 = fp_ld32(b); int ml;
  if (m4 == 0x20544547u) ml = 3;                         // "GET "
  else if (m4 == 0x54534f50u && b[4] == ' ') ml = 4;     // "POST "
  else { const char *sp = memchr(b, ' ', l0 - 10); if (!sp) return FP_FALLBACK; ml = (int)(sp - b); }
  int ps = ml + 1, pe = l0 - 10;
  while (FP_UNLIKELY(ps < pe && b[ps] == ' ')) ps++;     // SP+ after the method (as pico)
  while (FP_UNLIKELY(pe > ps && b[pe - 1] == ' ')) pe--; // SP* before the version (as pico)
  if (pe <= ps) return FP_FALLBACK;
  fp_route(b, ml, b + ps, pe - ps, r);
  r->auth = 0; r->tok = 0; r->tok_len = 0;
  return 0;
}
FP_INLINE int fp_wants_auth(const req_t *r) { return fp_route_auth(r->route); }


// header line [s, end) (end excludes CR/LF); value [*v, *ve) trimmed of SP/HT on both sides (as pico)
FP_INLINE void fp_trim(const char *b, int *v, int *ve) {
  while (*v < *ve && (b[*v] == ' ' || b[*v] == '\t')) ++*v;
  while (*ve > *v && (b[*ve - 1] == ' ' || b[*ve - 1] == '\t')) --*ve;
}
// Header line [s, end) starting with c/C, t/T or a byte <= 0x20. -> *cl, 0, or FP_FALLBACK.
// Anything that could make another parser frame the request differently falls back:
//   a line starting with SP/HT/control (obs-fold or junk; the final empty line is fine),
//   a c/t line without ':' (a header name split across lines, "Transfer-\r\nEncoding"),
//   "content-length" not directly followed by ':' ("Content-Length : 5"), odd or duplicate CL,
//   and any header named transfer* (Transfer-Encoding and lookalikes).
FP_INLINE int fp_has_colon(const char *b, int s, int end) {   // first 16 bytes in SWAR, then memchr
  uint64_t m = (fp_swar_eq(fp_ld64(b + s), ':' * FP_ONES) & fp_bytes(end - s < 8 ? end - s : 8))
             | (fp_swar_eq(fp_ld64(b + s + 8), ':' * FP_ONES) & fp_bytes(end - s < 8 ? 0 : end - s < 16 ? end - s - 8 : 8));
  return m != 0 || (end - s > 16 && memchr(b + s + 16, ':', end - s - 16));
}
FP_INLINE int fp_hdr_ct(const char *b, int s, int end, int *cl, int *have_cl) {
  unsigned char c0 = (unsigned char)b[s];
  if (c0 <= 0x20) return c0 == '\r' && end == s ? 0 : FP_FALLBACK;
  if ((c0 | 0x20) == 'c') {
    if (end - s >= 14 && FP_HNAME(b + s, "content-", "length\0\0", 6)) {
      if (b[s + 14] != ':') return FP_FALLBACK;
      int v = s + 15, ve = end; fp_trim(b, &v, &ve);
      int ok; int64_t x = fp_digits(b + v, ve - v, &ok);
      if (ve - v == 1 && b[v] == '0') { ok = 1; x = 0; }
      if (!ok || ve - v > 9 || *have_cl) return FP_FALLBACK;   // odd or duplicate CL -> slow path
      *cl = (int)x; *have_cl = 1;
      return 0;
    }
  } else if (end - s >= 8 && FP_HNAME(b + s, "transfer", "\0\0\0\0\0\0\0\0", 0)) return FP_FALLBACK;
  return FP_LIKELY(fp_has_colon(b, s, end)) ? 0 : FP_FALLBACK;
}
// line starting with a/A: 1 if it is Authorization (then r->auth/tok set if "Bearer ")
FP_INLINE int fp_hdr_auth(const char *b, int s, int end, req_t *r) {
  if (!(end - s >= 14 && FP_HNAME(b + s, "authoriz", "ation:\0\0", 6))) return 0;
  int v = s + 14, ve = end; fp_trim(b, &v, &ve);
  if (ve - v >= 7 && (fp_ld64(b + v) & 0x00FFFFFFFFFFFFFFull) == FP_S8("Bearer \0")) {   // one load, not memcmp
    r->auth = 1; r->tok = b + v + 7; r->tok_len = ve - v - 7; }
  return 1;
}

// End of headers in one 64-byte block, from the LF mask alone (no CR mask): the first empty line,
// i.e. "\n\n" (c1) or "\n\r\n" (c2, LFs 2 apart, CR checked with a load). Returns its bit or 0;
// *bare = 1 if it is not a full "\r\n\r\n" (bare-LF line ends: pico accepts them, we fall back
// instead of waiting forever). In practice c1|c2 is just the real end.
FP_INLINE uint64_t fp_end(const char *b, int off, uint64_t lf, uint64_t lfp, int *bare) {
  uint64_t c1 = lf & ((lf << 1) | (lfp >> 63)), c2 = lf & ((lf << 2) | (lfp >> 62));
  for (uint64_t c = c1 | c2; c; c &= c - 1) {
    uint64_t bit = c & -c; int p = off + fp_ctz(c);
    if (FP_LIKELY(b[p - 1] == '\r')) { *bare = !(p >= 3 && b[p - 3] == '\r'); return bit; }
    if (bit & c1) { *bare = 1; return bit; }
  }
  return 0;
}
FP_INLINE int fp_next_lf(const uint64_t *L, int pos) {   // first LF at index >= pos (must exist)
  int k = pos >> 6; uint64_t m = L[k] >> (pos & 63) << (pos & 63);
  while (!m) m = L[++k];
  return (k << 6) + fp_ctz(m);
}

// Visit every line after the request line (l0 = its LF); dispatch on the first byte. -> *cl or FP_FALLBACK.
FP_INLINE int fp_walk(const char *b, const uint64_t *L, int k0, int nch, int l0, req_t *r, int *cl) {
  int have_cl = 0, have_auth = !fp_wants_auth(r), s = l0 + 1;
  uint64_t m = L[k0] & (L[k0] - 1);
  for (int kk = k0;;) {
    while (!m) { if (++kk >= nch) return 0; m = L[kk]; }
    int end = kk * 64 + fp_ctz(m), ls = s; m &= m - 1; s = end + 1;
    unsigned char c0 = (unsigned char)b[ls]; int c = c0 | 0x20;
    if (FP_LIKELY(c0 > 0x20 && c != 'a' && c != 'c' && c != 't')) continue;
    if (b[end - 1] == '\r') end--;
    if (c != 'a') { if (fp_hdr_ct(b, ls, end, cl, &have_cl)) return FP_FALLBACK; }
    else if (!have_auth) have_auth = fp_hdr_auth(b, ls, end, r);   // first Authorization wins (as ref)
  }
}

// ---------------- core 1: candidate masks (SIMD backends) ----------------
// l0c: -1 = unknown; else the result of fp_reqline_canon(b, r) already run on this r (0 = not canonical)
FP_INLINE int fp_core_cand_l(const char *b, int len, req_t *r, const int simd, int l0c) {
  // C[k]: line starts (bit after an LF) whose byte is c/t or <= 0x20, up to the final LF
  uint64_t L[FP_NCH], C[FP_NCH], lfp = 0, lf, ct, e, anyc = 0;
  fp_cracc_t cra; fp_cr_init(simd, &cra);
  int lim = len < FP_MAXHDR ? len : FP_MAXHDR, k = 0, off = 0;
  for (;; off += 64, k++) {
    if (off >= lim) return len >= FP_MAXHDR ? FP_FALLBACK : FP_NEED_MORE;
    fp_cls64(simd, b + off, &lf, &ct);
    int rem = lim - off;
    if (rem < 64) lf &= (1ull << rem) - 1;
    int bare = 0;
    e = fp_end(b, off, lf, lfp, &bare);
    if (FP_UNLIKELY(bare)) return FP_FALLBACK;
    uint64_t keep = e ? e ^ (e - 1) : ~0ull;     // LFs/line starts up to and including the final LF
    L[k] = lf & keep; C[k] = ((lf << 1) | (lfp >> 63)) & ct & keep & ~(e >> 1);   // minus the final empty line
    fp_cr_add(simd, &cra, b + off, e ? fp_ctz(e) + 1 : 64);   // a partial block without e: NEED_MORE anyway
    if (e) {
      if (FP_UNLIKELY(e == 1) && k) C[k - 1] &= ~(1ull << 63), anyc = 0;   // its CR ended the previous block
      for (int j = anyc ? k : 0; j <= k; j++) anyc |= C[j];
      break;
    }
    anyc |= C[k];
    lfp = lf;
  }
  if (FP_UNLIKELY(fp_cr_any(simd, &cra))) return FP_FALLBACK;
  const int nch = k + 1, hend = off + fp_ctz(e) + 1;
  int l0 = l0c >= 0 ? l0c : fp_reqline_canon(b, r), k0;
  if (FP_LIKELY(l0)) k0 = l0 >> 6;
  else {
    k0 = 0; while (!L[k0]) k0++;                  // terminates: the final LF is in L[k]
    l0 = k0 * 64 + fp_ctz(L[k0]);
    if (fp_reqline(b, l0, r)) return FP_FALLBACK;
  }
  int cl = 0;
  if (fp_wants_auth(r)) { if (fp_walk(b, L, k0, nch, l0, r, &cl)) return FP_FALLBACK; }
  else if (anyc) {
    int have_cl = 0;
    for (int kk = 0; kk < nch; kk++) for (uint64_t m = C[kk]; m; m &= m - 1) {
      int s = (kk << 6) + fp_ctz(m), end = fp_next_lf(L, s); if (b[end - 1] == '\r') end--;
      if (fp_hdr_ct(b, s, end, &cl, &have_cl)) return FP_FALLBACK;
    }
  }
  if (hend + cl > len) return FP_NEED_MORE;
  r->body_off = hend; r->body_len = cl;
  return hend + cl;
}

// ---------------- core 2: LF mask + line walk (scalar backend) ----------------
FP_INLINE int fp_core_walk_l(const char *b, int len, req_t *r, const int simd, int l0c) {
  uint64_t L[FP_NCH], lfp = 0, lf, e;
  fp_cracc_t cra; fp_cr_init(simd, &cra);
  int lim = len < FP_MAXHDR ? len : FP_MAXHDR, k = 0, off = 0;
  for (;; off += 64, k++) {
    if (off >= lim) return len >= FP_MAXHDR ? FP_FALLBACK : FP_NEED_MORE;
    lf = fp_lf64(simd, b + off);
    int rem = lim - off;
    if (rem < 64) lf &= (1ull << rem) - 1;
    int bare = 0;
    if ((e = fp_end(b, off, lf, lfp, &bare))) { if (FP_UNLIKELY(bare)) return FP_FALLBACK; break; }
    fp_cr_add(simd, &cra, b + off, 64);
    L[k] = lf; lfp = lf;
  }
  fp_cr_add(simd, &cra, b + off, fp_ctz(e) + 1);
  if (FP_UNLIKELY(fp_cr_any(simd, &cra))) return FP_FALLBACK;
  L[k] = lf & (e ^ (e - 1));
  const int nch = k + 1, hend = off + fp_ctz(e) + 1;
  int l0 = l0c >= 0 ? l0c : fp_reqline_canon(b, r), k0;
  if (FP_LIKELY(l0)) k0 = l0 >> 6;
  else {
    k0 = 0; while (!L[k0]) k0++;
    l0 = k0 * 64 + fp_ctz(L[k0]);
    if (fp_reqline(b, l0, r)) return FP_FALLBACK;
  }
  int cl = 0;
  if (fp_walk(b, L, k0, nch, l0, r, &cl)) return FP_FALLBACK;
  if (hend + cl > len) return FP_NEED_MORE;
  r->body_off = hend; r->body_len = cl;
  return hend + cl;
}

// Stable entry for callers that do their own request-line work (explore/batch-parse): the common GET
// shape in one shot. Precondition: the request line is canonical with a route that needs no Authorization
// (fp_reqline_canon returned l0 > 0 and !fp_route_auth). Returns hend > 0 iff the headers end within the
// first 128 bytes, the whole block is CRLF-clean (no bare CR, no bare-LF end) and no header line is one the
// candidate core would look at (c/t/<= 0x20 start); fast_parse() then returns exactly hend with body_len 0.
// Returns 0 otherwise: use fast_parse / FP_CORE_L. This keeps the header rules in http.h only.
FP_INLINE int fp_quick_get(const char *b, int len, const int simd) {
  const int lim = len < FP_MAXHDR ? len : FP_MAXHDR;
  if (lim > 128) return 0;
  uint64_t lf0, ct0, lf1 = 0, ct1 = 0, e;
  fp_cls64(simd, b, &lf0, &ct0);
  if (lim > 64) fp_cls64(simd, b + 64, &lf1, &ct1);
  if (lim < 64) lf0 &= (1ull << lim) - 1;
  if (lim > 64 && lim < 128) lf1 &= (1ull << (lim - 64)) - 1;
  int bare = 0, hend;
  uint64_t C;
  fp_cracc_t cra; fp_cr_init(simd, &cra);
  if ((e = fp_end(b, 0, lf0, 0, &bare))) {
    C = (lf0 << 1) & ct0 & (e ^ (e - 1)) & ~(e >> 1);
    hend = fp_ctz(e) + 1; fp_cr_add(simd, &cra, b, hend);
  } else if (lim > 64 && (e = fp_end(b, 64, lf1, lf0, &bare))) {
    C = ((lf0 << 1) & ct0 & (e == 1 ? ~(1ull << 63) : ~0ull)) | (((lf1 << 1) | (lf0 >> 63)) & ct1 & (e ^ (e - 1)) & ~(e >> 1));
    hend = 64 + fp_ctz(e) + 1; fp_cr_add(simd, &cra, b, 64); fp_cr_add(simd, &cra, b + 64, hend - 64);
  } else return 0;
  if (FP_UNLIKELY(bare | (C != 0) | fp_cr_any(simd, &cra))) return 0;
  return hend;
}

FP_INLINE int fp_core_cand(const char *b, int len, req_t *r, const int simd) { return fp_core_cand_l(b, len, r, simd, -1); }
FP_INLINE int fp_core_walk(const char *b, int len, req_t *r, const int simd) { return fp_core_walk_l(b, len, r, simd, -1); }

#if FP_BACKEND != 0 && defined(FP_WALK_ONLY)
// A/B switch for x86 (where only static estimates exist): walk core + SIMD LF mask for everything
#define FP_CORE_L(b, len, r, l0c) fp_core_walk_l(b, len, r, 1, l0c)
#elif FP_BACKEND != 0
// Requests that need Authorization (POST here) are long and must walk every line anyway: the c/t
// mask would cost ~1 ns per 64-byte block for nothing, so the spec picks the core from the first bytes.
FP_INLINE int fp_core_simd(const char *b, int len, req_t *r, int l0c) {
  if (fp_spec_walk(b)) return fp_core_walk_l(b, len, r, 1, l0c);
  return fp_core_cand_l(b, len, r, 1, l0c);
}
#define FP_CORE_L(b, len, r, l0c) fp_core_simd(b, len, r, l0c)
#else
#define FP_CORE_L(b, len, r, l0c) fp_core_walk_l(b, len, r, 0, l0c)
#endif
#define FP_CORE(b, len, r) FP_CORE_L(b, len, r, -1)
static inline int fast_parse(const char *b, int len, req_t *r) { return FP_CORE(b, len, r); }
static inline int fast_parse_scalar(const char *b, int len, req_t *r) { return fp_core_walk(b, len, r, 0); }
static inline int fast_parse3(const char *b, int len, req_t *r) { return fast_parse(b, len, r); }   // old name

// ---------------- learned header layout (opt-in: fast_parse_learn) ----------------
// Per route, remember the most common header layout seen at runtime (majority vote): the LF positions
// relative to the first header byte, the first <= 16 bytes of every header line, and which line is
// Content-Length / the first Authorization. A request with a canonical request line is then checked
// against that layout alone: LF masks compared with the expected ones (no end search, no c/t mask,
// no line walk), one 16-byte key compare + CR check per line (independent, no loop-carried chain),
// then only the CL / Authorization lines are parsed. Any mismatch -> the general parser.
// Nothing is built in: an empty table learns whatever traffic it sees.
//
// Why the result is identical to the general parser when the check passes:
//   - LFs in [0, hend) are exactly the learned ones and every line ends in CRLF with >= 1 byte of
//     content, so there is no "\n\n" or "\n?\n" before hend: the general scan finds the same end;
//   - the request line is canonical (same route/id/l0 by construction);
//   - per line the general parser's decision depends only on the line length (equal: same LFs, same
//     CR) and its first min(16, length) bytes (equal: exact compare), so the same lines are CL /
//     Authorization / ignored, and those two are then parsed by the very same functions.
#define FP_LAY_WORDS 8                 // header block (after the request line) up to 512 bytes
#define FP_LAY_LINES 24
typedef struct {
  uint64_t rel[FP_LAY_WORDS + 2];      // bit t: LF at l0 + 1 + t (last two words always 0)
  uint64_t k0[FP_LAY_LINES], k1[FP_LAY_LINES], m0[FP_LAY_LINES], m1[FP_LAY_LINES];   // line keys
  uint16_t off[FP_LAY_LINES + 1];      // line starts relative to l0 + 1; off[n] = the empty line
  int n, hrel;                         // header lines; bytes from l0 + 1 to the end of headers (0 = empty slot)
  int cl_i, auth_i;                    // line index of Content-Length / first Authorization, or -1
  int votes, cool, backoff, peak;      // majority vote; relearn every (backoff+1)-th lost vote
} fp_layout_t;
typedef struct { fp_layout_t lay[FP_NROUTES]; uint64_t hits, misses, learned; } fp_learn_t;   // zero-init

// -2 = layout doesn't match (use the general parser), else exactly what the general parser returns
FP_INLINE int fp_try_layout(const char *b, int len, req_t *r, int l0, const fp_layout_t *L, const int simd) {
  const int base = l0 + 1, hend = base + L->hrel;         // base <= 36: always inside block 0
  if (hend > len || fp_ld32(b + hend - 4) != 0x0a0d0a0du) return -2;
  // LFs in [0, hend) exactly the learned ones, each line CRLF-terminated (per-line CR check; the request
  // line's and the final CRLF are in the template / the end check), and no bare CR anywhere before hend
  uint64_t diff = 0;
  fp_cracc_t cra; fp_cr_init(simd, &cra);
  for (int k = 0, nk = (hend + 63) >> 6; k < nk; k++) {
    uint64_t exp = (L->rel[k] << base) | (k ? L->rel[k - 1] >> (64 - base) : 1ull << l0);
    uint64_t lf = fp_lf64(simd, b + 64 * k);
    int rem = hend - 64 * k; if (rem < 64) lf &= (1ull << rem) - 1;
    diff |= lf ^ exp;
    fp_cr_add(simd, &cra, b + 64 * k, rem < 64 ? rem : 64);
  }
  for (int i = 0; i < L->n; i++) {
    const char *p = b + base + L->off[i];
    diff |= ((fp_ld64(p) ^ L->k0[i]) & L->m0[i]) | ((fp_ld64(p + 8) ^ L->k1[i]) & L->m1[i]);
    diff |= (uint8_t)(b[base + L->off[i + 1] - 2] ^ '\r');
  }
  diff |= (uint64_t)fp_cr_any(simd, &cra);
  if (diff) return -2;
  int cl = 0, have_cl = 0;
  if (L->cl_i >= 0) {
    int s = base + L->off[L->cl_i];
    if (fp_hdr_ct(b, s, base + L->off[L->cl_i + 1] - 2, &cl, &have_cl)) return FP_FALLBACK;
  }
  if (L->auth_i >= 0 && fp_wants_auth(r)) {
    int s = base + L->off[L->auth_i];
    fp_hdr_auth(b, s, base + L->off[L->auth_i + 1] - 2, r);
  }
  if (hend + cl > len) return FP_NEED_MORE;
  r->body_off = hend; r->body_len = cl;
  return hend + cl;
}

// Build the layout of a request the general parser accepted (rare: only when a vote is lost).
// Line keys: lines the general parser ignores because of their first byte (not a/c/t; or a/A on
// routes without auth) are keyed on that byte only, so their values (Host: ...) may vary; a/c/t
// lines are keyed on min(16, length) bytes, which must include their ':' (the general parser falls
// back on a c/t line without one); with equal line lengths that is all its decision looks at.
static inline int fp_layout_build(const char *b, int l0, int hend, int route, fp_layout_t *L) {
  const int base = l0 + 1, hrel = hend - base;
  if (hrel > 64 * FP_LAY_WORDS) return 0;
  fp_layout_t t; memset(&t, 0, sizeof t); t.cl_i = t.auth_i = -1;
  int s = base;
  for (int p = base; p < hend; p++) {
    if (b[p] != '\n') continue;
    t.rel[(p - base) >> 6] |= 1ull << ((p - base) & 63);
    if (p == hend - 1) break;                                  // LF of the empty line
    int e = p - 1;                                             // line [s, e) + CR LF
    if (b[e] != '\r' || e <= s || t.n == FP_LAY_LINES) return 0;
    int kl = e - s < 16 ? e - s : 16, c = b[s] | 0x20;
    if ((unsigned char)b[s] <= 0x20) return 0;                 // the general parser falls back on these
    if (!(c == 'c' || c == 't' || (c == 'a' && fp_route_auth(route)))) kl = 1;
    else if ((c == 'c' || c == 't') && !memchr(b + s, ':', kl)) return 0;   // key must cover the colon
    t.off[t.n] = (uint16_t)(s - base);
    t.k0[t.n] = fp_ld64(b + s); t.k1[t.n] = fp_ld64(b + s + 8);
    t.m0[t.n] = fp_bytes(kl < 8 ? kl : 8); t.m1[t.n] = fp_bytes(kl > 8 ? kl - 8 : 0);
    t.k0[t.n] &= t.m0[t.n]; t.k1[t.n] &= t.m1[t.n];
    if (c == 'c' && e - s >= 14 && FP_HNAME(b + s, "content-", "length\0\0", 6)) {   // as fp_hdr_ct
      if (t.cl_i >= 0) return 0;
      t.cl_i = t.n;
    }
    if (c == 't' && e - s >= 8 && FP_HNAME(b + s, "transfer", "\0\0\0\0\0\0\0\0", 0)) return 0;
    if (c == 'a' && t.auth_i < 0 && e - s >= 14 && FP_HNAME(b + s, "authoriz", "ation:\0\0", 6)) t.auth_i = t.n;
    t.n++; s = p + 1;
  }
  if (s != hend - 2 || b[s] != '\r') return 0;
  t.off[t.n] = (uint16_t)(s - base); t.hrel = hrel; t.votes = 1;
  t.cool = 1; t.backoff = L->backoff; t.peak = 0;
  *L = t;
  return 1;
}

FP_INLINE int fp_core_learn(const char *b, int len, req_t *r, fp_learn_t *T, const int simd) {
  int l0 = fp_reqline_canon(b, r);
  if (!l0) return simd ? FP_CORE_L(b, len, r, 0) : fp_core_walk_l(b, len, r, 0, 0);
  if (!fp_spec_learn(r->route)) return simd ? FP_CORE_L(b, len, r, l0) : fp_core_walk_l(b, len, r, 0, l0);
  fp_layout_t *L = &T->lay[r->route];
  if (L->votes > 0) {                                        // only while the layout wins its vote
    int c = fp_try_layout(b, len, r, l0, L, simd);
    if (c != -2) { L->votes += L->votes < 1024; L->peak += L->peak < 64; T->hits++; return c; }
  }
  int c = simd ? FP_CORE_L(b, len, r, l0) : fp_core_walk_l(b, len, r, 0, l0);   // r is still canon's
  if (c > 0) {                                               // a complete request with another layout
    T->misses++;
    if (L->votes > 0) L->votes--;
    else if (--L->cool <= 0) {
      // the last layout died young (< 64 hits): this route has no stable majority, relearn 2x rarer
      L->backoff = !L->hrel ? 0 : L->peak >= 64 ? 15 : L->backoff < 4096 ? L->backoff * 2 + 1 : 8191;
      req_t t; if (fp_layout_build(b, fp_reqline_canon(b, &t), r->body_off, r->route, L)) T->learned++;
      L->cool = L->backoff + 1;
    }
  }
  return c;
}
static inline int fast_parse_learn(const char *b, int len, req_t *r, fp_learn_t *T) { return fp_core_learn(b, len, r, T, FP_BACKEND != 0); }
static inline int fast_parse_learn_scalar(const char *b, int len, req_t *r, fp_learn_t *T) { return fp_core_learn(b, len, r, T, 0); }

// ---------------- pipelined batch ----------------
// Parses consecutive complete requests from buf[0..len) into out[0..max) (same padding contract).
//   n      = requests parsed; request i ends at out[i].body_off + out[i].body_len (offsets relative to buf)
//   used   = bytes consumed by those n requests
//   status = about the bytes at buf + used: FP_NEED_MORE (incomplete request, or nothing left),
//            FP_FALLBACK (that request needs the slow path), FP_FULL (out[] full, more data follows).
typedef struct { int n, used, status; } fp_batch_t;
#define FP_MANY(core_call) \
  fp_batch_t res = {0, 0, FP_FULL}; \
  while (res.n < max) { \
    const char *b = buf + res.used; int l = len - res.used; req_t *r = &out[res.n]; \
    int c = core_call; \
    if (c <= 0) { res.status = c; return res; } \
    r->body_off += res.used; res.used += c; res.n++; \
  } \
  if (res.used == len) res.status = FP_NEED_MORE; \
  return res;
static inline fp_batch_t fast_parse_many(const char *buf, int len, req_t *out, int max) { FP_MANY(FP_CORE(b, l, r)) }
static inline fp_batch_t fast_parse_many_scalar(const char *buf, int len, req_t *out, int max) { FP_MANY(fp_core_walk(b, l, r, 0)) }
static inline fp_batch_t fast_parse_many_learn(const char *buf, int len, req_t *out, int max, fp_learn_t *T) { FP_MANY(fast_parse_learn(b, l, r, T)) }
#undef FP_MANY
