// fbparse.h - general HTTP/1.x request parser, picohttpparser-exact, SIMD (the slow path behind fastparse.h).
// Part of cursed-http-parser. MIT licence, see LICENSE. The validation rules and the -1/-2 decisions follow
// picohttpparser's behaviour (MIT; credited in THIRD_PARTY.md).
//
// API (picohttpparser's phr_parse_request, same arguments and results):
//   int fb_parse_request(buf, len, &method, &method_len, &path, &path_len, &minor, headers, &num_headers, last_len);
//     > 0 bytes of the request head consumed, -1 invalid, -2 incomplete. num_headers: in = capacity, out = count.
//     Header names are not NUL-terminated; an obs-fold continuation line has name == NULL (as pico).
//   Validation is pico's, byte for byte (pinned commit 465a7ff): same token set for method and header names,
//   same control-character rules for path and values, LF or CRLF line ends, one leading empty line skipped,
//   HTTP/1.<digit>. Every accept/reject AND every -1 vs -2 decision is pico's; tests/test_fbparse.c checks that.
//
//   Adapter (include fastparse.h first): int fb_parse_req(buf, len, req_t *r) = exactly tests/testlib.h's
//   ref_parse (pico + fastparse.h's routing and its Authorization/Content-Length rules, 48 headers max):
//     > 0 head + body bytes, FP_NEED_MORE, FP_FALLBACK (HTTP/1.0 or other minor, Transfer-Encoding*,
//     duplicate/odd Content-Length), -2 invalid (send 400). fb_parse_req_ex() also says why (fb_info_t).
//
// Contract (as fastparse.h): >= 64 readable bytes after buf[len], any contents. No length limit on the head
// (headers over 8 KB are fine); the header count is bounded by the caller's array.
//
// How: the ISA-specific part classifies 64 bytes into a bitmask (bit i = byte i) for one of three predicates;
// everything else is shared C on those masks:
//   nt = not a token character (pico's token_char_map; nibble LUT: two pshufb/tbl and one AND per vector)
//   ct = ends a header value: a control byte other than HT, or DEL (pico's get_token_to_eol set)
//   pe = ends the path: a byte <= 0x20 (SP, HT, controls), or DEL (pico's ADVANCE_TOKEN set)
// The parser walks the head in pico's order, but each step is a ctz on a cached block mask instead of a
// byte loop: method = up to the first nt (must be SP), path = up to the first pe (must be SP), header name =
// up to the first nt (must be ':'), value = up to the first ct (must be CR LF or LF). Each predicate is
// classified per block only when a search reaches that block, so a 10 KB cookie costs one cheap ct mask per
// 64 bytes. A search that finds nothing before len returns a position >= len (-> -2).
//
// Backends (compile time, same switches as fastparse.h): AVX2 (-mavx2), AVX-512BW/VL on ymm (-DFP_USE_AVX512,
// + -DFP_USE_ZMM for zmm), NEON (aarch64), scalar (-DFP_FORCE_SCALAR or anything else). Test builds:
// -DFP_AVX512_EMU runs the AVX-512 code on plain-C intrinsics (fb_avx512_emu.h). The scalar classify is
// always compiled too; fb_parse_request_scalar() / fb_parse_req_scalar() use it (test reference).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "fbparse.h assumes little-endian loads"
#endif
#if defined(FP_FORCE_SCALAR)
#define FB_BACKEND 0
#elif defined(FP_AVX512_EMU)
#define FB_BACKEND 3
#elif defined(FP_USE_AVX512) && defined(__AVX512BW__) && defined(__AVX512VL__)
#define FB_BACKEND 3
#elif defined(__AVX2__)
#define FB_BACKEND 2
#elif defined(__aarch64__) && defined(__ARM_NEON)
#define FB_BACKEND 1
#else
#define FB_BACKEND 0
#endif
#if defined(FP_USE_AVX512) && FB_BACKEND != 3 && !defined(FP_FORCE_SCALAR)
#error "FP_USE_AVX512 needs -mavx512bw -mavx512vl (e.g. -march=cascadelake)"
#endif

#if FB_BACKEND == 1
#include <arm_neon.h>
#elif defined(FP_AVX512_EMU)
#include "fb_avx512_emu.h"
#elif FB_BACKEND >= 2
#include <immintrin.h>
#endif

#if FB_BACKEND == 0
#define FB_BACKEND_NAME "scalar"
#elif FB_BACKEND == 1
#define FB_BACKEND_NAME "neon"
#elif FB_BACKEND == 2
#define FB_BACKEND_NAME "avx2"
#elif defined(FP_USE_ZMM)
#define FB_BACKEND_NAME "avx512bw-zmm" FB_EMU_TAG
#else
#define FB_BACKEND_NAME "avx512bw-ymm" FB_EMU_TAG
#endif
#ifdef FP_AVX512_EMU
#define FB_EMU_TAG " (C-emulated intrinsics)"
#else
#define FB_EMU_TAG ""
#endif

#define FB_INLINE static inline __attribute__((always_inline))
#ifndef FB_WALK
#define FB_WALK 1   // header loop: 1 = line starts off the LF mask (lines independent), 0 = sequential (A/B switch, same results)
#endif
#define FB_LIKELY(x) __builtin_expect(!!(x), 1)
#define FB_UNLIKELY(x) __builtin_expect(!!(x), 0)

struct fb_header { const char *name; size_t name_len; const char *value; size_t value_len; };   // = struct phr_header

FB_INLINE uint64_t fb_ld64(const char *p) { uint64_t x; memcpy(&x, p, 8); return x; }

// ---------------- classify 64 bytes -> nt / ct / pe masks (bit i = byte i) ----------------
// Token characters (RFC 9110 tchar = pico's token_char_map) as a nibble LUT: one bit per high nibble 2..7
// in FB_HI; FB_LO[lo] has the bits of the high nibbles whose (hi, lo) byte is a tchar. A byte is a tchar
// iff FB_LO[x & 15] & FB_HI[x >> 4] != 0; high nibbles 0, 1, 8..15 have no bits (no tchars there).
#define FB_LO_ROW 0x3a, 0x3f, 0x3e, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3e, 0x3e, 0x3d, 0x15, 0x34, 0x15, 0x3d, 0x1c
#define FB_HI_ROW 0, 0, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0, 0, 0, 0, 0, 0, 0, 0
static const uint8_t FB_LO[64] __attribute__((aligned(64))) = {FB_LO_ROW, FB_LO_ROW, FB_LO_ROW, FB_LO_ROW};
static const uint8_t FB_HI[64] __attribute__((aligned(64))) = {FB_HI_ROW, FB_HI_ROW, FB_HI_ROW, FB_HI_ROW};
FB_INLINE int fb_tchar(unsigned char c) { return (FB_LO[c & 15] & FB_HI[c >> 4]) != 0; }

// Three predicates, each classified per 64-byte block only when a search needs it:
//   nt = not a token character, ct = ends a value (control byte other than HT, or DEL), pe = ends the path
//   (byte <= 0x20, or DEL). Scalar: SWAR for ct/pe, the same nibble LUT byte by byte for nt.
#define FB_ONES 0x0101010101010101ull
#define FB_HIB 0x8080808080808080ull
FB_INLINE uint64_t fb_swar_eq(uint64_t x, uint64_t pat) { uint64_t t = x ^ pat; return ~(((t & ~FB_HIB) + ~FB_HIB) | t) & FB_HIB; }
FB_INLINE uint64_t fb_swar_le(uint64_t w, int c) { return ~(((w & ~FB_HIB) + (0x7f - c) * FB_ONES) | w) & FB_HIB; }   // byte <= c (c < 0x7f)
FB_INLINE uint64_t fb_swar_mm(uint64_t m) { return ((m >> 7) * 0x0102040810204080ull) >> 56; }
FB_INLINE uint64_t fb_nt64_scalar(const char *p) {
  uint64_t n = 0;
  for (int i = 0; i < 64; i++) n |= (uint64_t)!fb_tchar((unsigned char)p[i]) << i;
  return n;
}
FB_INLINE uint64_t fb_ct64_scalar(const char *p) {
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) { uint64_t w = fb_ld64(p + 8 * i);
    c |= fb_swar_mm((fb_swar_le(w, 0x1f) & ~fb_swar_eq(w, '\t' * FB_ONES)) | fb_swar_eq(w, 0x7f * FB_ONES)) << (8 * i); }
  return c;
}
FB_INLINE void fb_ntct64_scalar(const char *p, uint64_t *nt, uint64_t *ct) { *nt = fb_nt64_scalar(p); *ct = fb_ct64_scalar(p); }
FB_INLINE uint64_t fb_pe64_scalar(const char *p) {
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) { uint64_t w = fb_ld64(p + 8 * i); c |= fb_swar_mm(fb_swar_le(w, 0x20) | fb_swar_eq(w, 0x7f * FB_ONES)) << (8 * i); }
  return c;
}
FB_INLINE uint64_t fb_lf64_scalar(const char *p) {
  uint64_t c = 0;
  for (int i = 0; i < 8; i++) c |= fb_swar_mm(fb_swar_eq(fb_ld64(p + 8 * i), '\n' * FB_ONES)) << (8 * i);
  return c;
}
FB_INLINE void fb_lfct64_scalar(const char *p, uint64_t *lf, uint64_t *ct) { *lf = fb_lf64_scalar(p); *ct = fb_ct64_scalar(p); }
FB_INLINE void fb_all64_scalar(const char *p, uint64_t *m) { m[0] = fb_nt64_scalar(p); m[1] = fb_ct64_scalar(p); m[2] = fb_pe64_scalar(p); m[3] = fb_lf64_scalar(p); }

#if FB_BACKEND == 1
// NEON: vld4q de-interleaves (v.val[j] lane i = byte 4i+j); a vsri chain + shrn #4 packs four compare
// vectors into a 64-bit mask in byte order (as fastparse.h's fp_neon_pack4).
FB_INLINE uint64_t fb_neon_pack4(uint8x16_t c0, uint8x16_t c1, uint8x16_t c2, uint8x16_t c3) {
  uint8x16_t t0 = vsriq_n_u8(c1, c0, 1), t1 = vsriq_n_u8(c3, c2, 1);
  uint8x16_t t2 = vsriq_n_u8(t1, t0, 2), t3 = vsriq_n_u8(t2, t2, 4);
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(t3), 4)), 0);
}
#define FB_NEON4(P) fb_neon_pack4(P(v.val[0]), P(v.val[1]), P(v.val[2]), P(v.val[3]))
FB_INLINE uint64_t fb_nt64_simd(const char *p) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
  const uint8x16_t LO = vld1q_u8(FB_LO), HI = vld1q_u8(FB_HI), F = vdupq_n_u8(0x0f);
#define FB_P(x) vceqq_u8(vandq_u8(vqtbl1q_u8(LO, vandq_u8(x, F)), vqtbl1q_u8(HI, vshrq_n_u8(x, 4))), vdupq_n_u8(0))
  return FB_NEON4(FB_P);
#undef FB_P
}
FB_INLINE uint64_t fb_ct64_simd(const char *p) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
#define FB_P(x) vorrq_u8(vbicq_u8(vcleq_u8(x, vdupq_n_u8(0x1f)), vceqq_u8(x, vdupq_n_u8('\t'))), vceqq_u8(x, vdupq_n_u8(0x7f)))
  return FB_NEON4(FB_P);
#undef FB_P
}
FB_INLINE uint64_t fb_pe64_simd(const char *p) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
#define FB_P(x) vorrq_u8(vcleq_u8(x, vdupq_n_u8(0x20)), vceqq_u8(x, vdupq_n_u8(0x7f)))
  return FB_NEON4(FB_P);
#undef FB_P
}
FB_INLINE uint64_t fb_lf64_simd(const char *p) {
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
#define FB_P(x) vceqq_u8(x, vdupq_n_u8('\n'))
  return FB_NEON4(FB_P);
#undef FB_P
}
FB_INLINE void fb_lfct64_simd(const char *p, uint64_t *lf, uint64_t *ct) {   // one vld4 for both
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
#define FB_P(x) vceqq_u8(x, vdupq_n_u8('\n'))
  *lf = FB_NEON4(FB_P);
#undef FB_P
#define FB_P(x) vorrq_u8(vbicq_u8(vcleq_u8(x, vdupq_n_u8(0x1f)), vceqq_u8(x, vdupq_n_u8('\t'))), vceqq_u8(x, vdupq_n_u8(0x7f)))
  *ct = FB_NEON4(FB_P);
#undef FB_P
}
FB_INLINE void fb_all64_simd(const char *p, uint64_t *m) {   // block 0: one vld4 for all four
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
  const uint8x16_t LO = vld1q_u8(FB_LO), HI = vld1q_u8(FB_HI), F = vdupq_n_u8(0x0f);
#define FB_P(x) vceqq_u8(vandq_u8(vqtbl1q_u8(LO, vandq_u8(x, F)), vqtbl1q_u8(HI, vshrq_n_u8(x, 4))), vdupq_n_u8(0))
  m[0] = FB_NEON4(FB_P);
#undef FB_P
#define FB_P(x) vorrq_u8(vbicq_u8(vcleq_u8(x, vdupq_n_u8(0x1f)), vceqq_u8(x, vdupq_n_u8('\t'))), vceqq_u8(x, vdupq_n_u8(0x7f)))
  m[1] = FB_NEON4(FB_P);
#undef FB_P
#define FB_P(x) vorrq_u8(vcleq_u8(x, vdupq_n_u8(0x20)), vceqq_u8(x, vdupq_n_u8(0x7f)))
  m[2] = FB_NEON4(FB_P);
#undef FB_P
#define FB_P(x) vceqq_u8(x, vdupq_n_u8('\n'))
  m[3] = FB_NEON4(FB_P);
#undef FB_P
}
FB_INLINE void fb_ntct64_simd(const char *p, uint64_t *nt, uint64_t *ct) {   // one vld4 for both
  uint8x16x4_t v = vld4q_u8((const uint8_t *)p);
  const uint8x16_t LO = vld1q_u8(FB_LO), HI = vld1q_u8(FB_HI), F = vdupq_n_u8(0x0f);
#define FB_P(x) vceqq_u8(vandq_u8(vqtbl1q_u8(LO, vandq_u8(x, F)), vqtbl1q_u8(HI, vshrq_n_u8(x, 4))), vdupq_n_u8(0))
  *nt = FB_NEON4(FB_P);
#undef FB_P
#define FB_P(x) vorrq_u8(vbicq_u8(vcleq_u8(x, vdupq_n_u8(0x1f)), vceqq_u8(x, vdupq_n_u8('\t'))), vceqq_u8(x, vdupq_n_u8(0x7f)))
  *ct = FB_NEON4(FB_P);
#undef FB_P
}
#undef FB_NEON4
// nt mask of the 16 bytes at p as 4 bits per byte (vshrn nibble mask): the name scan at a line start (fb_core).
FB_INLINE uint64_t fb_nt16_nib(const char *p) {
  const uint8x16_t LO = vld1q_u8(FB_LO), HI = vld1q_u8(FB_HI), F = vdupq_n_u8(0x0f);
  uint8x16_t x = vld1q_u8((const uint8_t *)p);
  uint8x16_t nt = vceqq_u8(vandq_u8(vqtbl1q_u8(LO, vandq_u8(x, F)), vqtbl1q_u8(HI, vshrq_n_u8(x, 4))), vdupq_n_u8(0));
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(nt), 4)), 0);
}
// "maybe a ct byte in the next FB_CTANY_BLOCKS * 64 bytes?" for fb_line()'s long-value loop. It may over-approximate,
// because a positive only sends the block to the exact masks: (int8)(x + 1) < 0x21 holds for every control byte
// (HT too), DEL, and every byte >= 0x80 (not allowed in cookies, rare in other headers). Two ops per 16 bytes (add,
// signed min), like memchr's compare + or; two independent accumulators, one reduction per test; no vld4, no packing.
// -DFB_CTANY_EXACTISH: the unsigned variant instead (min < 0x20, or DEL): no false positives on bytes >= 0x80.
#define FB_HAVE_CTANY 1
#ifndef FB_CTANY_BLOCKS
#define FB_CTANY_BLOCKS 8
#endif
#ifndef FB_CTANY_AFTER
#define FB_CTANY_AFTER 2   // blocks of a line classified exactly before the skip windows start (after the crossing block)
#endif
#ifndef FB_CTANY_PREFETCH
#define FB_CTANY_PREFETCH 3072   // bytes ahead to prefetch in the skip loop (0 = none); long heads are memory-bound
#endif
FB_INLINE int fb_ctany_simd(const char *p, const int nb) {   // nb 64-byte blocks (a constant at each call)
#ifdef FB_CTANY_EXACTISH
  const uint8x16_t DEL = vdupq_n_u8(0x7f);
  uint8x16_t mn0 = vdupq_n_u8(0xff), mn1 = mn0, d0 = vdupq_n_u8(0), d1 = d0;
  for (int i = 0; i < 4 * nb; i += 2) {
    uint8x16_t x0 = vld1q_u8((const uint8_t *)p + 16 * i), x1 = vld1q_u8((const uint8_t *)p + 16 * i + 16);
    mn0 = vminq_u8(mn0, x0); mn1 = vminq_u8(mn1, x1);
    d0 = vorrq_u8(d0, vceqq_u8(x0, DEL)); d1 = vorrq_u8(d1, vceqq_u8(x1, DEL));
  }
  return (vminvq_u8(vminq_u8(mn0, mn1)) < 0x20) || (vmaxvq_u8(vorrq_u8(d0, d1)) != 0);
#else
  const uint8x16_t ONE = vdupq_n_u8(1);   // x + 1 wraps on purpose: done unsigned (no signed overflow), compared signed
  int8x16_t m0 = vdupq_n_s8(127), m1 = m0;
  for (int i = 0; i < 4 * nb; i += 2) {
    m0 = vminq_s8(m0, vreinterpretq_s8_u8(vaddq_u8(vld1q_u8((const uint8_t *)p + 16 * i), ONE)));
    m1 = vminq_s8(m1, vreinterpretq_s8_u8(vaddq_u8(vld1q_u8((const uint8_t *)p + 16 * i + 16), ONE)));
  }
  return vminvq_s8(vminq_s8(m0, m1)) < 0x21;
#endif
}
#elif FB_BACKEND == 2
FB_INLINE uint64_t fb_mm2(__m256i x, __m256i y) {
  return (uint32_t)_mm256_movemask_epi8(x) | ((uint64_t)(uint32_t)_mm256_movemask_epi8(y) << 32);
}
FB_INLINE __m256i fb_nt32(__m256i v) {
  const __m256i LO = _mm256_load_si256((const void *)FB_LO), HI = _mm256_load_si256((const void *)FB_HI), F = _mm256_set1_epi8(0x0f);
  __m256i t = _mm256_and_si256(_mm256_shuffle_epi8(LO, _mm256_and_si256(v, F)), _mm256_shuffle_epi8(HI, _mm256_and_si256(_mm256_srli_epi16(v, 4), F)));
  return _mm256_cmpeq_epi8(t, _mm256_setzero_si256());
}
// x <= c (unsigned) <=> max(x, c) == c
FB_INLINE __m256i fb_le32(__m256i v, char c) { return _mm256_cmpeq_epi8(_mm256_max_epu8(v, _mm256_set1_epi8(c)), _mm256_set1_epi8(c)); }
FB_INLINE __m256i fb_ct32(__m256i v) {
  return _mm256_or_si256(_mm256_andnot_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('\t')), fb_le32(v, 0x1f)), _mm256_cmpeq_epi8(v, _mm256_set1_epi8(0x7f)));
}
FB_INLINE __m256i fb_pe32(__m256i v) { return _mm256_or_si256(fb_le32(v, 0x20), _mm256_cmpeq_epi8(v, _mm256_set1_epi8(0x7f))); }
#define FB_AVX2_64(P) return fb_mm2(P(_mm256_loadu_si256((const void *)p)), P(_mm256_loadu_si256((const void *)(p + 32))))
FB_INLINE uint64_t fb_nt64_simd(const char *p) { FB_AVX2_64(fb_nt32); }
FB_INLINE uint64_t fb_ct64_simd(const char *p) { FB_AVX2_64(fb_ct32); }
FB_INLINE uint64_t fb_pe64_simd(const char *p) { FB_AVX2_64(fb_pe32); }
FB_INLINE __m256i fb_lf32(__m256i v) { return _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n')); }
FB_INLINE uint64_t fb_lf64_simd(const char *p) { FB_AVX2_64(fb_lf32); }
FB_INLINE void fb_ntct64_simd(const char *p, uint64_t *nt, uint64_t *ct) {
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  *nt = fb_mm2(fb_nt32(v0), fb_nt32(v1)); *ct = fb_mm2(fb_ct32(v0), fb_ct32(v1));
}
FB_INLINE void fb_lfct64_simd(const char *p, uint64_t *lf, uint64_t *ct) {
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  *lf = fb_mm2(fb_lf32(v0), fb_lf32(v1)); *ct = fb_mm2(fb_ct32(v0), fb_ct32(v1));
}
FB_INLINE void fb_all64_simd(const char *p, uint64_t *m) {
  __m256i v0 = _mm256_loadu_si256((const void *)p), v1 = _mm256_loadu_si256((const void *)(p + 32));
  m[0] = fb_mm2(fb_nt32(v0), fb_nt32(v1)); m[1] = fb_mm2(fb_ct32(v0), fb_ct32(v1));
  m[2] = fb_mm2(fb_pe32(v0), fb_pe32(v1)); m[3] = fb_mm2(fb_lf32(v0), fb_lf32(v1));
}
#undef FB_AVX2_64
#elif FB_BACKEND == 3
// Same predicates as AVX2, compares straight into mask registers; vptestnmb gives nt without a compare.
// Default ymm (AVX-512VL, no zmm frequency licence on Cascade Lake); -DFP_USE_ZMM: one zmm per block.
// Intrinsic names per width; the test build (-DFP_AVX512_EMU) maps them to fb_avx512_emu.h's C functions.
#if defined(FP_USE_ZMM) && defined(FP_AVX512_EMU)
#define FB_A(op) fbemu_mm512_##op
#define FB_VT fbemu512
#elif defined(FP_AVX512_EMU)
#define FB_A(op) fbemu_mm256_##op
#define FB_VT fbemu256
#elif defined(FP_USE_ZMM)
#define FB_A(op) FB_Z_##op
#define FB_Z_load _mm512_loadu_si512
#define FB_Z_and _mm512_and_si512
#define FB_Z_set1_epi8 _mm512_set1_epi8
#define FB_Z_shuffle_epi8 _mm512_shuffle_epi8
#define FB_Z_srli_epi16 _mm512_srli_epi16
#define FB_Z_cmpeq_epi8_mask _mm512_cmpeq_epi8_mask
#define FB_Z_cmple_epu8_mask _mm512_cmple_epu8_mask
#define FB_Z_testn_epi8_mask _mm512_testn_epi8_mask
#define FB_VT __m512i
#else
#define FB_A(op) FB_Y_##op
#define FB_Y_load _mm256_loadu_si256
#define FB_Y_and _mm256_and_si256
#define FB_Y_set1_epi8 _mm256_set1_epi8
#define FB_Y_shuffle_epi8 _mm256_shuffle_epi8
#define FB_Y_srli_epi16 _mm256_srli_epi16
#define FB_Y_cmpeq_epi8_mask _mm256_cmpeq_epi8_mask
#define FB_Y_cmple_epu8_mask _mm256_cmple_epu8_mask
#define FB_Y_testn_epi8_mask _mm256_testn_epi8_mask
#define FB_VT __m256i
#endif
#if defined(FP_USE_ZMM)
#define FB_X64(E) FB_VT v = FB_A(load)((const void *)p); return (uint64_t)(E)
#else
#define FB_X64(E) uint64_t r = 0; for (int h = 0; h < 2; h++) { FB_VT v = FB_A(load)((const void *)(p + 32 * h)); r |= (uint64_t)(uint32_t)(E) << (32 * h); } return r
#endif
#define FB_S1(c) FB_A(set1_epi8)(c)
#define FB_EQ(v, c) FB_A(cmpeq_epi8_mask)(v, FB_S1(c))
#define FB_LE(v, c) FB_A(cmple_epu8_mask)(v, FB_S1(c))
#define FB_NTM(v) FB_A(testn_epi8_mask)(FB_A(shuffle_epi8)(FB_A(load)((const void *)FB_LO), FB_A(and)(v, FB_S1(0x0f))), \
                  FB_A(shuffle_epi8)(FB_A(load)((const void *)FB_HI), FB_A(and)(FB_A(srli_epi16)(v, 4), FB_S1(0x0f))))
FB_INLINE uint64_t fb_nt64_simd(const char *p) { FB_X64(FB_NTM(v)); }
FB_INLINE uint64_t fb_ct64_simd(const char *p) { FB_X64((FB_LE(v, 0x1f) & ~FB_EQ(v, '\t')) | FB_EQ(v, 0x7f)); }
FB_INLINE uint64_t fb_pe64_simd(const char *p) { FB_X64(FB_LE(v, 0x20) | FB_EQ(v, 0x7f)); }
FB_INLINE uint64_t fb_lf64_simd(const char *p) { FB_X64(FB_EQ(v, '\n')); }
FB_INLINE void fb_ntct64_simd(const char *p, uint64_t *nt, uint64_t *ct) { *nt = fb_nt64_simd(p); *ct = fb_ct64_simd(p); }   // loads are cheap here
FB_INLINE void fb_lfct64_simd(const char *p, uint64_t *lf, uint64_t *ct) { *lf = fb_lf64_simd(p); *ct = fb_ct64_simd(p); }
FB_INLINE void fb_all64_simd(const char *p, uint64_t *m) { m[0] = fb_nt64_simd(p); m[1] = fb_ct64_simd(p); m[2] = fb_pe64_simd(p); m[3] = fb_lf64_simd(p); }
#undef FB_X64
#undef FB_S1
#undef FB_EQ
#undef FB_LE
#undef FB_NTM
#endif

#if (FB_BACKEND == 2 || FB_BACKEND == 3) && !defined(FP_AVX512_EMU) && !defined(FB_NO_CTANY)
// x86 version of the NEON skip test above (AVX2 ops, also on the AVX-512 backend: no zmm): "maybe a ct byte in the
// next FB_CTANY_BLOCKS * 64 bytes?". Over-approximates; a positive only sends the block to the exact masks.
//   default: (int8)(x + 1) < 0x21 (controls, DEL, and every byte >= 0x80, as on NEON): add + signed min per 32 bytes
//   -DFB_CTANY_EXACTISH: min_epu8 < 0x20 (HT counts) or any DEL: no false positives on bytes >= 0x80
// nt mask (not a token character) of the 32 bytes at p, bit i = byte i: the name scan at a line start (fb_core).
FB_INLINE uint32_t fb_nt32_at(const char *p) {
  const __m256i LO = _mm256_load_si256((const void *)FB_LO), HI = _mm256_load_si256((const void *)FB_HI), F = _mm256_set1_epi8(0x0f);
  __m256i v = _mm256_loadu_si256((const void *)p);
  __m256i t = _mm256_and_si256(_mm256_shuffle_epi8(LO, _mm256_and_si256(v, F)), _mm256_shuffle_epi8(HI, _mm256_and_si256(_mm256_srli_epi16(v, 4), F)));
  return (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(t, _mm256_setzero_si256()));
}
#define FB_HAVE_CTANY 1
#ifndef FB_CTANY_BLOCKS
#define FB_CTANY_BLOCKS 8
#endif
#ifndef FB_CTANY_AFTER
#define FB_CTANY_AFTER 2   // blocks of a line classified exactly before the skip windows start (after the crossing block)
#endif
#ifndef FB_CTANY_PREFETCH
#define FB_CTANY_PREFETCH 3072   // bytes ahead to prefetch in the skip loop (0 = none); long heads are memory-bound
#endif
FB_INLINE int fb_ctany_simd(const char *p, const int nb) {   // nb 64-byte blocks (a constant at each call)
  const int nv = 2 * nb;
#ifndef FB_CTANY_EXACTISH
  const __m256i ONE = _mm256_set1_epi8(1);
  __m256i m0 = _mm256_set1_epi8(127), m1 = m0;
  for (int i = 0; i < nv; i += 2) {
    m0 = _mm256_min_epi8(m0, _mm256_add_epi8(_mm256_loadu_si256((const void *)(p + 32 * i)), ONE));
    m1 = _mm256_min_epi8(m1, _mm256_add_epi8(_mm256_loadu_si256((const void *)(p + 32 * i + 32)), ONE));
  }
  __m256i hit = _mm256_cmpgt_epi8(_mm256_set1_epi8(0x21), _mm256_min_epi8(m0, m1));
  return !_mm256_testz_si256(hit, hit);
#else
  const __m256i DEL = _mm256_set1_epi8(0x7f), C20 = _mm256_set1_epi8(0x20);
  __m256i mn = _mm256_set1_epi8(-1), d = _mm256_setzero_si256();
  for (int i = 0; i < nv; i++) {
    __m256i x = _mm256_loadu_si256((const void *)(p + 32 * i));
    mn = _mm256_min_epu8(mn, x); d = _mm256_or_si256(d, _mm256_cmpeq_epi8(x, DEL));
  }
  __m256i lo = _mm256_cmpeq_epi8(_mm256_max_epu8(mn, C20), mn);   // mn >= 0x20 per byte
  return !_mm256_testc_si256(lo, _mm256_set1_epi8(-1)) || !_mm256_testz_si256(d, d);
#endif
}
#endif

// simd is a compile-time constant at every call site
enum { FB_NT, FB_CT, FB_PE, FB_LF };
FB_INLINE uint64_t fb_cls64(int simd, const int which, const char *p) {
#if FB_BACKEND != 0
  if (simd) return which == FB_NT ? fb_nt64_simd(p) : which == FB_CT ? fb_ct64_simd(p) : which == FB_PE ? fb_pe64_simd(p) : fb_lf64_simd(p);
#endif
  (void)simd; return which == FB_NT ? fb_nt64_scalar(p) : which == FB_CT ? fb_ct64_scalar(p) : which == FB_PE ? fb_pe64_scalar(p) : fb_lf64_scalar(p);
}
FB_INLINE void fb_all64(int simd, const char *p, uint64_t *m) {   // m[FB_NT..FB_LF]
#if FB_BACKEND != 0
  if (simd) { fb_all64_simd(p, m); return; }
#endif
  (void)simd; fb_all64_scalar(p, m);
}

FB_INLINE void fb_ntct64(int simd, const char *p, uint64_t *nt, uint64_t *ct) {
#if FB_BACKEND != 0
  if (simd) { fb_ntct64_simd(p, nt, ct); return; }
#endif
  (void)simd; fb_ntct64_scalar(p, nt, ct);
}
FB_INLINE void fb_lfct64(int simd, const char *p, uint64_t *lf, uint64_t *ct) {
#if FB_BACKEND != 0
  if (simd) { fb_lfct64_simd(p, lf, ct); return; }
#endif
  (void)simd; fb_lfct64_scalar(p, lf, ct);
}

// ---------------- the scan: per predicate one cached block mask; searches only move forward ----------------
// First position >= p whose byte is in class `which`, if there is one before len; else some position >= len.
// Blocks are only classified when they start before len, so reads stay within buf[len + 63]. Padding bytes
// are classified like any others: whatever a search finds there is >= len, and callers read that as -2.
// Block 0 is classified for all four predicates at once (request line + first headers). Later blocks only
// for the predicate whose search reaches them; with pair = 1 an nt search classifies ct with it (the
// sequential loop's value search follows in the same block). fb_line() (the walk) classifies lf and ct
// together, and inside a long value only ct (a block without control bytes has no LF either).
typedef struct { int k[4]; uint64_t m[4]; } fb_scan_t;
FB_INLINE int fb_next(const char *b, int len, int p, fb_scan_t *s, const int which, const int pair, const int simd) {
  for (;;) {
    int k = p >> 6;
    if (FB_UNLIKELY(k != s->k[which])) {
      if (p >= len) return p;
      if (which == FB_NT && pair) { fb_ntct64(simd, b + 64 * k, &s->m[FB_NT], &s->m[FB_CT]); s->k[FB_CT] = k; }
      else s->m[which] = fb_cls64(simd, which, b + 64 * k);
      s->k[which] = k;
    }
    uint64_t m = s->m[which] >> (p & 63);
    if (FB_LIKELY(m)) return p + __builtin_ctzll(m);
    p = (k + 1) << 6;
  }
}
// The walk's line scan: returns the first LF >= p and sets *ce to the first ct >= p (both: or some position
// >= len). Classifies lf and ct together; both caches end on the LF's block.
FB_INLINE int fb_line(const char *b, int len, int p, fb_scan_t *s, int *ce, const int simd) {
  int k = p >> 6;
  if (FB_UNLIKELY(k != s->k[FB_LF] || k != s->k[FB_CT])) {
    if (p >= len) { *ce = p; return p; }
    fb_lfct64(simd, b + 64 * k, &s->m[FB_LF], &s->m[FB_CT]); s->k[FB_LF] = s->k[FB_CT] = k;
  }
  uint64_t ml = s->m[FB_LF] >> (p & 63), mc = s->m[FB_CT] >> (p & 63);
  if (FB_LIKELY(ml)) { *ce = p + __builtin_ctzll(mc); return p + __builtin_ctzll(ml); }   // mc != 0: an LF is a ct
  int c = mc ? p + __builtin_ctzll(mc) : -1;
  // The line runs on into the next block. Most lines end there (short headers crossing a block boundary): classify
  // that block exactly (lf and ct in one pass) before trying any skip windows, which only pay on long values.
  p = ++k << 6;
  if (p >= len) { *ce = c >= 0 ? c : p; return p; }
  fb_lfct64(simd, b + p, &s->m[FB_LF], &s->m[FB_CT]); s->k[FB_LF] = s->k[FB_CT] = k;
  if (s->m[FB_CT]) {
    if (c < 0) c = p + __builtin_ctzll(s->m[FB_CT]);
    if (s->m[FB_LF]) { *ce = c; return p + __builtin_ctzll(s->m[FB_LF]); }
  }
  for (int exact = 1;; exact++) {   // a long value: ct first, lf only where ct != 0 (LF is a ct)
    p = ++k << 6;
    if (p >= len) { *ce = c >= 0 ? c : p; return p; }
#ifndef FB_HAVE_CTANY
    (void)exact;
#else
    // Skip control-free stretches with fb_ctany_simd over windows aligned to the address (no cache-line-split loads,
    // as memchr does), growing 256 -> 512 bytes; then find the line's end with exact masks over the same aligned
    // 64-byte lines, in one loop, and return from here. Only for lines already FB_CTANY_AFTER blocks past the
    // crossing: on medium lines (100-300 bytes: user agents, accept lists, small cookies) a window almost always
    // hits and only costs (measured: ~10% on browser requests on x86).
    // Bounds: a window or line starts at most 63 bytes before p (bytes of this same line; masked off below) and never
    // before buf; windows are tested only while their last 64 bytes start before buf + len, lines only while they start
    // before it, so reads stay within buf[len + 63]. Positions found in the padding are >= len, which callers read as
    // "incomplete", exactly as for the block path below. The lf/ct block caches are left as they were (an earlier
    // block of this line): the next fb_line() classifies its own block again.
    if (simd && exact > FB_CTANY_AFTER) {
      const char *a = (const char *)((uintptr_t)(b + p) & ~(uintptr_t)63), *end = b + len;
      for (int nb = 4;;) {
        if (a + 64 * (nb - 1) >= end || (nb == 4 ? fb_ctany_simd(a, 4) : fb_ctany_simd(a, 8))) break;
#if FB_CTANY_PREFETCH > 0
        __builtin_prefetch(a + FB_CTANY_PREFETCH); __builtin_prefetch(a + FB_CTANY_PREFETCH + 64 * nb - 64);
#endif
        a += 64 * nb; if (nb < 8) nb *= 2;
      }
      for (; a < end; a += 64) {
        uint64_t keep = a < b + p ? ~0ull << ((b + p) - a) : ~0ull, ctm = fb_cls64(simd, FB_CT, a) & keep;
        if (FB_LIKELY(!ctm)) continue;
        if (c < 0) c = (int)(a - b) + __builtin_ctzll(ctm);
        uint64_t lfm = fb_cls64(simd, FB_LF, a) & keep;
        if (lfm) { *ce = c; return (int)(a - b) + __builtin_ctzll(lfm); }
      }
      *ce = c >= 0 ? c : (int)(a - b); return (int)(a - b);
    }
#endif
    uint64_t ct = fb_cls64(simd, FB_CT, b + p);
    if (FB_LIKELY(!ct)) continue;
    if (c < 0) c = p + __builtin_ctzll(ct);
    s->m[FB_CT] = ct; s->m[FB_LF] = fb_cls64(simd, FB_LF, b + p); s->k[FB_LF] = s->k[FB_CT] = k;
    if (s->m[FB_LF]) { *ce = c; return p + __builtin_ctzll(s->m[FB_LF]); }
  }
}

// pico's is_complete() (the last_len slowloris check), verbatim semantics
static inline int fb_is_complete(const char *b, int len, size_t last_len) {
  int p = last_len < 3 ? 0 : (int)last_len - 3, cnt = 0;
  for (;;) {
    if (p >= len) return -2;
    if (b[p] == '\r') { if (++p >= len) return -2; if (b[p++] != '\n') return -1; cnt++; }
    else if (b[p] == '\n') { p++; cnt++; }
    else { p++; cnt = 0; }
    if (cnt == 2) return 0;
  }
}

// What happens to each header: mode 0 = store into fb_header[] (the pico API); mode 1 = the req_t adapter.
typedef struct { void *req; int route_auth, have_auth, have_cl, cl, flags, auth; const char *tok; int tok_len; int minor, lite, skip; } fb_ax_t;
// req: the req_t; minor: set before on_reqline; lite: the caller only needs the verdict for minor != 1, so skip = 1 then
// (no routing, no header work: the request falls back whatever its headers say)
enum { FB_TE = 1, FB_BADCL = 2, FB_MINOR = 4 };   // fb_info_t.flags: why fb_parse_req said FP_FALLBACK
FB_INLINE void fb_ax_header(fb_ax_t *a, const char *n, int nl, const char *v, int vl);

// The parser. Returns pico's result; on success the outputs are pico's.
FB_INLINE int fb_core(const char *b, int len, const char **method, size_t *method_len, const char **path, size_t *path_len,
                      int *minor, struct fb_header *h, size_t *num_headers, size_t last_len, const int mode, fb_ax_t *ax,
                      void (*on_reqline)(fb_ax_t *, const char *, int, const char *, int), const int walk, const int simd) {
  size_t max = mode == 0 ? *num_headers : 48;
  if (mode == 0) { *method = NULL; *method_len = 0; *path = NULL; *path_len = 0; *minor = -1; *num_headers = 0; }
  if (last_len != 0) { int c = fb_is_complete(b, len, last_len); if (c) return c; }
  if (len <= 0) return -2;
  fb_scan_t sc = {{0, 0, 0, 0}, {0, 0, 0, 0}};   // cached block masks, block 0 for everything
  fb_all64(simd, b, sc.m);
  // skip one empty line (pico: "some clients add CRLF after POST content")
  int p = 0;
  if (b[0] == '\r') { if (len < 2) return -2; if (b[1] != '\n') return -1; p = 2; }
  else if (b[0] == '\n') p = 1;
  // method: token chars up to SP
  int ms = p, e = fb_next(b, len, p, &sc, FB_NT, !walk, simd);
  if (e >= len) return -2;
  if (b[e] != ' ') return -1;
  int ml = e - ms;
  do { if (++e >= len) return -2; } while (b[e] == ' ');
  // path: up to SP; controls (incl. HT) and DEL are invalid, bytes >= 0x80 are fine
  int ps = e; e = fb_next(b, len, e, &sc, FB_PE, 0, simd);
  if (e >= len) return -2;
  if (b[e] != ' ') return -1;
  int pl = e - ps;
  do { if (++e >= len) return -2; } while (b[e] == ' ');
  if (ml == 0) return -1;
  // "HTTP/1.<digit>" then CRLF or LF; pico wants 9 bytes before looking at any of them
  if (len - e < 9) return -2;
  if (fb_ld64(b + e) << 8 != (fb_ld64("HTTP/1.0") << 8) || (unsigned)(b[e + 7] - '0') > 9) return -1;
  int mv = b[e + 7] - '0';
  p = e + 8;
  if (b[p] == '\r') { if (p + 1 >= len) return -2; if (b[p + 1] != '\n') return -1; p += 2; }
  else if (b[p] == '\n') p++;
  else return -1;
  if (mode == 1) { ax->minor = mv; on_reqline(ax, b + ms, ml, b + ps, pl); }
  // headers
  size_t n = 0;
  for (;; n++) {
    if (p >= len) return -2;
    unsigned char c = (unsigned char)b[p];
    if (c == '\r') { if (p + 1 >= len) return -2; if (b[p + 1] != '\n') return -1; p += 2; break; }
    if (c == '\n') { p++; break; }
    if (n == max) return -1;
    const char *name; int nl, vs;
    int l = len;
    if (walk) {
      // walk: the next line start comes off the LF mask, so lines don't wait on each other's name/value scans.
      // A line [p, l] is valid only if its first control byte is its end (the CR before the LF, or the LF):
      // anything else is an error pico reports before reaching the LF (-1, never -2). A line without an LF
      // before len goes to the sequential code below, which finds pico's exact -1 or -2.
      int ce; l = fb_line(b, len, p, &sc, &ce, simd);
      if (FB_LIKELY(l < len)) {
        // l - 1 >= p: b[p] is not LF, and not CR here. Valid: the first ct is the CR before the LF, or the LF itself.
        if (FB_LIKELY(ce == l - 1 && b[l - 1] == '\r')) e = l - 1;
        else if (ce == l && b[l - 1] != '\r') e = l;
        else return -1;
        if (FB_UNLIKELY(n != 0 && (c == ' ' || c == '\t'))) { name = NULL; nl = 0; vs = p; }
        else {
          int ne;
#if (FB_BACKEND == 2 || FB_BACKEND == 3) && !defined(FP_AVX512_EMU) && !defined(FB_NAME_CACHED)
          // x86: classify the 32 bytes at the line start directly (one load, one LUT pass, tzcnt) instead of a
          // per-block nt mask whose cache misses depend on where lines cross blocks (unpredictable branches).
          // The bytes are readable: the line's LF is < len and the 64-byte padding follows. Names of 32+ bytes
          // fall back to the block masks.
          if (simd) {
            uint32_t ntm = fb_nt32_at(b + p);
            ne = FB_LIKELY(ntm) ? p + __builtin_ctz(ntm) : fb_next(b, len, p, &sc, FB_NT, 0, simd);
          } else
#elif FB_BACKEND == 1 && !defined(FB_NAME_CACHED)
          if (simd) {   // NEON: the same with 16 bytes and a nibble mask
            uint64_t ntm = fb_nt16_nib(b + p);
            ne = FB_LIKELY(ntm) ? p + (__builtin_ctzll(ntm) >> 2) : fb_next(b, len, p, &sc, FB_NT, 0, simd);
          } else
#endif
          ne = fb_next(b, len, p, &sc, FB_NT, 0, simd);   // <= e: CR and LF are not tchars
          unsigned w = (unsigned char)b[ne] | (unsigned)(unsigned char)b[ne + 1] << 8;   // ne + 1 <= e: readable
          if ((w & 0xff) != ':' || ne == p) return -1;
          name = b + p; nl = ne - p; vs = ne + 1 + (w >> 8 == ' ');   // the usual ": " in one step
          while (b[vs] == ' ' || b[vs] == '\t') vs++;          // stops at e at the latest
        }
        p = l + 1;
      }
    }
    if (l >= len) {   // sequential (also the walk's last line when it has no LF before len: ends in -1 or -2)
      if (FB_UNLIKELY(n != 0 && (c == ' ' || c == '\t'))) { name = NULL; nl = 0; vs = p; }   // obs-fold: value keeps its ws
      else {
        e = fb_next(b, len, p, &sc, FB_NT, !walk, simd);
        if (e >= len) return -2;
        if (b[e] != ':' || e == p) return -1;
        name = b + p; nl = e - p;
        vs = e + 1;
        while (vs < len && (b[vs] == ' ' || b[vs] == '\t')) vs++;
        if (vs >= len) return -2;
    }
      e = fb_next(b, len, vs, &sc, FB_CT, 0, simd);
      if (e >= len) return -2;
      if (FB_LIKELY(b[e] == '\r')) { if (e + 1 >= len) return -2; if (b[e + 1] != '\n') return -1; p = e + 2; }
      else if (b[e] == '\n') p = e + 1;
      else return -1;
    }
    int ve = e;
    while (FB_UNLIKELY(ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t'))) ve--;
    if (mode == 0) { h[n].name = name; h[n].name_len = (size_t)nl; h[n].value = b + vs; h[n].value_len = (size_t)(ve - vs); }
    else if (name && !ax->skip) fb_ax_header(ax, name, nl, b + vs, ve - vs);
  }
  if (mode == 0) { *method = b + ms; *method_len = (size_t)ml; *path = b + ps; *path_len = (size_t)pl; *num_headers = n; }
  *minor = mv;
  return p;
}

static inline int fb_parse_request(const char *buf, size_t len, const char **method, size_t *method_len, const char **path,
                                   size_t *path_len, int *minor_version, struct fb_header *headers, size_t *num_headers, size_t last_len) {
  return fb_core(buf, (int)len, method, method_len, path, path_len, minor_version, headers, num_headers, last_len, 0, NULL, NULL, FB_WALK, FB_BACKEND != 0);
}
static inline int fb_parse_request_scalar(const char *buf, size_t len, const char **method, size_t *method_len, const char **path,
                                          size_t *path_len, int *minor_version, struct fb_header *headers, size_t *num_headers, size_t last_len) {
  return fb_core(buf, (int)len, method, method_len, path, path_len, minor_version, headers, num_headers, last_len, 0, NULL, NULL, FB_WALK, 0);
}
// test/A-B entry: the other header loop (walk = 0: sequential, 1: LF-mask walk)
static inline int fb_parse_request_loop(int walk, const char *buf, size_t len, const char **method, size_t *method_len, const char **path,
                                        size_t *path_len, int *minor_version, struct fb_header *headers, size_t *num_headers, size_t last_len) {
  if (walk) return fb_core(buf, (int)len, method, method_len, path, path_len, minor_version, headers, num_headers, last_len, 0, NULL, NULL, 1, FB_BACKEND != 0);
  return fb_core(buf, (int)len, method, method_len, path, path_len, minor_version, headers, num_headers, last_len, 0, NULL, NULL, 0, FB_BACKEND != 0);
}

// ---------------- req_t adapter (needs fastparse.h included first) ----------------
#if defined(FP_MAXHDR)
// lowercase ASCII compare for names already validated as tchars: x | 0x20 only maps 'A'-'Z' onto
// letters and nothing onto '-', so it is exact here (it would not be on arbitrary bytes).
#define FB_LC 0x2020202020202020ull
FB_INLINE int fb_name_is(const char *n, const char *lc, int k) {   // k in 8..16: compare n[0..k) with lc
  return (((fb_ld64(n) | FB_LC) ^ fb_ld64(lc)) | ((fb_ld64(n + k - 8) | FB_LC) ^ fb_ld64(lc + k - 8))) == 0;
}
FB_INLINE void fb_ax_header(fb_ax_t *a, const char *n, int nl, const char *v, int vl) {
  if (nl == 13) {
    if (!a->have_auth && fb_name_is(n, "authorization", 13)) {
      a->have_auth = 1;
      if (vl >= 7 && (fb_ld64(v) << 8) == (fb_ld64("Bearer  ") << 8)) { a->auth = 1; a->tok = v + 7; a->tok_len = vl - 7; }   // 7 bytes, exact
    }
  } else if (nl == 14) {
    if (fb_name_is(n, "content-length", 14)) {
      if (a->have_cl || vl < 1 || vl > 9 || (vl > 1 && v[0] == '0')) { a->flags |= FB_BADCL; return; }
      int x = 0;
      for (int i = 0; i < vl; i++) { unsigned d = (unsigned char)v[i] - '0'; if (d > 9) { a->flags |= FB_BADCL; return; } x = x * 10 + (int)d; }
      a->cl = x; a->have_cl = 1;
    }
  } else if (nl >= 16 && fb_name_is(n, "transfer-encodin", 16)) a->flags |= FB_TE;
}
FB_INLINE void fb_ax_reqline(fb_ax_t *a, const char *m, int ml, const char *p, int pl) {
  if (a->lite && a->minor != 1) { a->skip = 1; return; }   // fb_parse_req: falls back anyway, as ref_parse does
  req_t *r = (req_t *)a->req;
  fp_route(m, ml, p, pl, r);
  a->route_auth = fp_route_auth(r->route);
}
// fb_parse_req_ex: the return value is fb_parse_req's; info is filled whenever the head parsed (ret != -2 /
// NEED_MORE from the head). flags = why FP_FALLBACK. With flags == FB_MINOR only (HTTP/1.0 etc.), r is filled
// as for a success (route, id, auth, body_off/len) and total = head + body bytes, which may exceed len (then
// wait for more): enough to serve an HTTP/1.0 client instead of answering 400.
typedef struct { int minor, flags, total; } fb_info_t;
FB_INLINE int fb_req_core(const char *b, int len, req_t *r, fb_info_t *info, const int walk, const int simd, const int lite) {
  fb_ax_t a; memset(&a, 0, sizeof a); a.req = r; a.lite = lite;
  int minor = -1;
  r->route = R_NOTFOUND; r->id_ok = 0; r->id = 0;
  int ret = fb_core(b, len, NULL, NULL, NULL, NULL, &minor, NULL, NULL, 0, 1, &a, fb_ax_reqline, walk, simd);
  info->minor = minor; info->flags = a.flags | (minor != 1 ? FB_MINOR : 0); info->total = 0;
  if (ret == -2) return FP_NEED_MORE;
  if (ret < 0) return -2;
  r->auth = 0; r->tok = 0; r->tok_len = 0;
  if (a.route_auth && a.auth) { r->auth = 1; r->tok = a.tok; r->tok_len = a.tok_len; }
  r->body_off = ret; r->body_len = a.cl; info->total = ret + a.cl;
  if (info->flags) return FP_FALLBACK;
  if (ret + a.cl > len) return FP_NEED_MORE;
  return ret + a.cl;
}
static inline int fb_parse_req_ex(const char *b, int len, req_t *r, fb_info_t *info) { return fb_req_core(b, len, r, info, FB_WALK, FB_BACKEND != 0, 0); }
static inline int fb_parse_req(const char *b, int len, req_t *r) { fb_info_t i; return fb_req_core(b, len, r, &i, FB_WALK, FB_BACKEND != 0, 1); }
static inline int fb_parse_req_scalar(const char *b, int len, req_t *r) { fb_info_t i; return fb_req_core(b, len, r, &i, FB_WALK, 0, 1); }
static inline int fb_parse_req_loop(int walk, const char *b, int len, req_t *r) {   // test/A-B: the other header loop
  fb_info_t i; return walk ? fb_req_core(b, len, r, &i, 1, FB_BACKEND != 0, 1) : fb_req_core(b, len, r, &i, 0, FB_BACKEND != 0, 1); }
#else
FB_INLINE void fb_ax_header(fb_ax_t *a, const char *n, int nl, const char *v, int vl) { (void)a; (void)n; (void)nl; (void)v; (void)vl; }
#endif
