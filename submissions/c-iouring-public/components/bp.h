/* batch-parse/bp.h (sha256 45b4ce352957), developed separately for this entry. */
// bp.h - lane-parallel HTTP request parsing: N requests from N different connections at once.
// Builds on explore/parser/http.h (included, never modified): every lane returns exactly what
// fast_parse() returns for that buffer (same ret, same req_t), including NEED_MORE / FALLBACK.
//
//   const char *buf[n]; int len[n];  req_t out[n]; int ret[n];
//   bp_parse(n, buf, len, out, ret);    // ret[i] = fast_parse(buf[i], len[i], &out[i]), any n >= 0
//
// Same padding contract as http.h: >= 64 readable bytes after buf[i][len[i]]. Lanes may alias.
//
// How (details in RESULTS.md):
//   A. request line, W lanes per vector: the first 24 bytes of W requests are gathered into u64
//      lanes (vpgatherqq with a NULL base: the "index" is the pointer itself), compared against every
//      canonical route prefix at once, the id digits of all lanes are counted (psadbw / vplzcntq) and
//      converted (pmaddubsw -> pmaddwd -> pmuludq), and a second gather at the per-lane offset
//      p + ndigits checks the " HTTP/1.1\r\n" / "/like HTTP/1.1\r\n" tails. Out: route, l0, id.
//   B. lanes are bucketed by route class (GET / POST / non-canonical) with branch-free appends, so
//      every per-lane loop below runs one route shape only (predictable branches over a random mix).
//   C. GET lanes: http.h's fp_quick_get claims the trivial case (end within 128 B, nothing for the cand
//      core to look at); POST lanes and every declined GET lane run http.h's own core with l0 known
//      (FP_CORE_L); non-canonical lanes run fast_parse. Needs http.h >= 049a66bdf1d9 (fp_quick_get).
//      No header rule lives here, so every lane is identical by construction.
//
// Phase A backends (BP_RL_*), chosen per call site so one binary can benchmark them all:
//   BP_RL_C       portable scalar-per-lane, branch-free (the reference)
//   BP_RL_CANON   http.h's own fp_reqline_canon per lane (branchy; THE DEFAULT: fastest on the Xeon)
//   BP_RL_AVX2    4 lanes / ymm, vpgatherqq                       (needs -mavx2)
//   BP_RL_AVX2I   4 lanes / ymm, loads + vpinsrq instead of gathers
//   BP_RL_Z       8 lanes / zmm, AVX-512F/BW/CD: k-mask compares, masked gathers, vplzcntq.
//                 -DBP_AVX512 (+ -march=cascadelake); -DBP_AVX512_EMU runs
//                 the same code on plain-C stand-ins (bp_emu512.h) for logic testing anywhere.
#pragma once
#ifndef BP_HTTP_H
#define BP_HTTP_H "../parser/http.h"
#endif
#include BP_HTTP_H

#define BP_CHUNK 64      // lanes per internal round (bp_parse splits bigger n)

enum { BP_RL_C = 0, BP_RL_AVX2 = 1, BP_RL_AVX2I = 2, BP_RL_Z = 3, BP_RL_CANON = 4 };

#if defined(__AVX2__)
#define BP_HAVE_AVX2 1
#endif
#if defined(BP_AVX512_EMU)
#include "bp_emu512.h"
#define BP_HAVE_Z 1
#define BP_ZTAG " (C-emulated)"
#elif defined(BP_AVX512)
#if !(defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512CD__))
#error "BP_AVX512 needs -mavx512f -mavx512bw -mavx512cd (e.g. -march=cascadelake)"
#endif
#include <immintrin.h>
#define BP_HAVE_Z 1
#define BP_ZTAG ""
#endif
#if defined(BP_HAVE_AVX2) && !defined(FP_AVX512_EMU)
#include <immintrin.h>
#endif

// default phase-A backend for bp_parse(): canon on every platform. On the real Xeon (exp5) it beat the
// AVX2 gather/insert and zmm variants at every batch size; those stay selectable (bp_parse_rl / -DBP_RL_DEFAULT).
#ifndef BP_RL_DEFAULT
#define BP_RL_DEFAULT BP_RL_CANON
#endif

// constants of the canonical request lines (as fp_reqline_canon)
#define BP_C8(s) ((uint64_t)fp_c8(s, sizeof(s) - 1))
#define BP_GETFEE  BP_C8("GET /fee")
#define BP_DHTTP1  BP_C8("d HTTP/1")
#define BP_GETPOS  BP_C8("GET /pos")
#define BP_POSTPO  BP_C8("POST /po")
#define BP_STSHTTP BP_C8("sts HTTP")
#define BP_TS      BP_C8("ts/")
#define BP_STS     BP_C8("sts/")
#define BP_DOT1    BP_C8(".1\r\n")
#define BP_V11     BP_C8("/1.1\r\n")
#define BP_SPHTTP  BP_C8(" HTTP/1.")
#define BP_ONECRLF BP_C8("1\r\n")
#define BP_LIKEHT  BP_C8("/like HT")
#define BP_TP11    BP_C8("TP/1.1\r\n")

// ---------------- phase A: request line of many lanes ----------------
// Output per lane: rt = route if canonical else 0 (R_NOTFOUND is never canonical), l0 = index of the
// request line's LF (0 = not canonical), id = the id (0 unless GETPOST/LIKE). Exactly fp_reqline_canon.

// Portable reference: one lane, no branches on the data.
FP_INLINE void bp_rl_c1(const char *b, int64_t *rt, int64_t *l0, int64_t *id) {
  uint64_t w0 = fp_ld64(b), w1 = fp_ld64(b + 8), w2 = fp_ld64(b + 16);
  int F = (w0 == BP_GETFEE) & (w1 == BP_DHTTP1) & ((uint32_t)w2 == (uint32_t)BP_DOT1);
  int C = (w0 == BP_POSTPO) & (w1 == BP_STSHTTP) & ((w2 & 0xFFFFFFFFFFFFull) == BP_V11);
  int G = (w0 == BP_GETPOS) & ((w1 & 0xFFFFFF) == BP_TS);
  int L = (w0 == BP_POSTPO) & ((uint32_t)w1 == (uint32_t)BP_STS);
  int p = 11 + L;
  uint64_t d = fp_ld64(b + p), nd = fp_nondigit(d);
  int n = nd ? fp_ctz(nd) >> 3 : 8;
  const char *q = b + p + n;
  uint64_t t1 = fp_ld64(q), t2 = fp_ld64(q + 8);
  int okn = (n >= 1) & (n <= 7);
  int Gk = G & okn & (t1 == BP_SPHTTP) & ((t2 & 0xFFFFFF) == BP_ONECRLF);
  int Lk = L & okn & (t1 == BP_LIKEHT) & (t2 == BP_TP11);
  int64_t v = (int64_t)fp_swar8(d, n);
  *rt = F * R_FEED + Gk * R_GETPOST + C * R_CREATE + Lk * R_LIKE;
  *l0 = F * 19 + C * 21 + Gk * (21 + n) + Lk * (27 + n);
  *id = (Gk | Lk) ? v : 0;
}

#ifdef BP_HAVE_AVX2
// 4 lanes. gather = vpgatherqq of the u64 at pp[i] + off[i], NULL base (the index is the address);
// otherwise 4 plain loads + vpinsrq (AVX2I).
FP_INLINE __m256i bp_g4(__m256i A, const char *const *pp, const int64_t *o, int off, int gather) {
  if (gather) return _mm256_i64gather_epi64((const long long *)0, _mm256_add_epi64(A, _mm256_set1_epi64x(off)), 1);
  return _mm256_set_epi64x((long long)fp_ld64(pp[3] + o[3] + off), (long long)fp_ld64(pp[2] + o[2] + off),
                           (long long)fp_ld64(pp[1] + o[1] + off), (long long)fp_ld64(pp[0] + o[0] + off));
}
#define BP_S1(x) _mm256_set1_epi64x((long long)(x))
FP_INLINE void bp_rl_avx2(const char *const *pp, int64_t *rt, int64_t *l0, int64_t *id, const int gather) {
  const __m256i P = _mm256_loadu_si256((const void *)pp);
  static const int64_t Z4[4] = {0, 0, 0, 0};
  __m256i w0 = bp_g4(P, pp, Z4, 0, gather), w1 = bp_g4(P, pp, Z4, 8, gather), w2 = bp_g4(P, pp, Z4, 16, gather);
#define EQ(a, b) _mm256_cmpeq_epi64(a, b)
#define AND _mm256_and_si256
  __m256i F = AND(AND(EQ(w0, BP_S1(BP_GETFEE)), EQ(w1, BP_S1(BP_DHTTP1))), EQ(AND(w2, BP_S1(0xFFFFFFFFull)), BP_S1(BP_DOT1)));
  __m256i isPO = EQ(w0, BP_S1(BP_POSTPO));
  __m256i C = AND(AND(isPO, EQ(w1, BP_S1(BP_STSHTTP))), EQ(AND(w2, BP_S1(0xFFFFFFFFFFFFull)), BP_S1(BP_V11)));
  __m256i G = AND(EQ(w0, BP_S1(BP_GETPOS)), EQ(AND(w1, BP_S1(0xFFFFFF)), BP_S1(BP_TS)));
  __m256i L = AND(isPO, EQ(AND(w1, BP_S1(0xFFFFFFFFull)), BP_S1(BP_STS)));
  // digits at p = 11 (GET) / 12 (LIKE): funnel-shifted out of w1:w2, no load needed
  __m256i dG = _mm256_or_si256(_mm256_srli_epi64(w1, 24), _mm256_slli_epi64(w2, 40));
  __m256i dL = _mm256_or_si256(_mm256_srli_epi64(w1, 32), _mm256_slli_epi64(w2, 32));
  __m256i d = _mm256_blendv_epi8(dG, dL, L);
  __m256i t = _mm256_sub_epi8(d, _mm256_set1_epi8('0'));
  __m256i dig = _mm256_cmpeq_epi8(_mm256_min_epu8(t, _mm256_set1_epi8(9)), t);   // 0xFF per digit byte
  __m256i u = _mm256_andnot_si256(dig, _mm256_set1_epi8(-1));                    // non-digit bytes
  u = _mm256_or_si256(u, _mm256_slli_epi64(u, 8)); u = _mm256_or_si256(u, _mm256_slli_epi64(u, 16));
  u = _mm256_or_si256(u, _mm256_slli_epi64(u, 32));                              // at/after the first non-digit
  __m256i n = _mm256_sad_epu8(_mm256_andnot_si256(u, _mm256_set1_epi8(1)), _mm256_setzero_si256());   // leading digits 0..8
  __m256i okn = AND(_mm256_cmpgt_epi64(n, _mm256_setzero_si256()), _mm256_cmpgt_epi64(BP_S1(8), n));
  // value: the n leading digits moved to the top bytes, then 8-digit SIMD atoi
  __m256i x = _mm256_sllv_epi64(t, _mm256_sub_epi64(BP_S1(64), _mm256_slli_epi64(n, 3)));
  __m256i v = _mm256_maddubs_epi16(x, _mm256_set1_epi16(0x010A));
  v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x00010064));
  v = _mm256_add_epi64(_mm256_mul_epu32(v, BP_S1(10000)), _mm256_srli_epi64(v, 32));
  // tails at q = b + p + n
  __m256i qo = _mm256_add_epi64(n, _mm256_sub_epi64(BP_S1(11), L));   // L = -1 -> 12
  int64_t qa[4]; if (!gather) _mm256_storeu_si256((void *)qa, qo);
  __m256i Q = _mm256_add_epi64(P, qo);
  __m256i t1 = bp_g4(Q, pp, qa, 0, gather), t2 = bp_g4(Q, pp, qa, 8, gather);
  __m256i Gk = AND(AND(G, okn), AND(EQ(t1, BP_S1(BP_SPHTTP)), EQ(AND(t2, BP_S1(0xFFFFFF)), BP_S1(BP_ONECRLF))));
  __m256i Lk = AND(AND(L, okn), AND(EQ(t1, BP_S1(BP_LIKEHT)), EQ(t2, BP_S1(BP_TP11))));
  __m256i r = _mm256_or_si256(_mm256_or_si256(AND(F, BP_S1(R_FEED)), AND(Gk, BP_S1(R_GETPOST))),
                              _mm256_or_si256(AND(C, BP_S1(R_CREATE)), AND(Lk, BP_S1(R_LIKE))));
  __m256i l = _mm256_or_si256(_mm256_or_si256(AND(F, BP_S1(19)), AND(C, BP_S1(21))),
                              _mm256_or_si256(AND(Gk, _mm256_add_epi64(n, BP_S1(21))), AND(Lk, _mm256_add_epi64(n, BP_S1(27)))));
  _mm256_storeu_si256((void *)rt, r); _mm256_storeu_si256((void *)l0, l);
  _mm256_storeu_si256((void *)id, AND(_mm256_or_si256(Gk, Lk), v));
#undef EQ
#undef AND
}
#undef BP_S1
#endif

#ifdef BP_HAVE_Z
// 8 lanes / zmm. Compares chain through k-masks (vpcmpeqq k{k}), the tail gathers are masked to the
// GETPOST/LIKE lanes, the digit count is vplzcntq of the isolated lowest non-digit byte.
#define ZS(x) _mm512_set1_epi64((long long)(x))
FP_INLINE void bp_rl_z(const char *const *pp, int64_t *rt, int64_t *l0, int64_t *id) {
  const __m512i P = _mm512_loadu_si512((const void *)pp), Z0 = _mm512_setzero_si512();
  __m512i w0 = _mm512_i64gather_epi64(P, (const void *)0, 1);
  __m512i w1 = _mm512_i64gather_epi64(_mm512_add_epi64(P, ZS(8)), (const void *)0, 1);
  __m512i w2 = _mm512_i64gather_epi64(_mm512_add_epi64(P, ZS(16)), (const void *)0, 1);
  __mmask8 kF = _mm512_cmpeq_epi64_mask(w0, ZS(BP_GETFEE));
  kF = _mm512_mask_cmpeq_epi64_mask(kF, w1, ZS(BP_DHTTP1));
  kF = _mm512_mask_cmpeq_epi64_mask(kF, _mm512_and_si512(w2, ZS(0xFFFFFFFFull)), ZS(BP_DOT1));
  __mmask8 kPO = _mm512_cmpeq_epi64_mask(w0, ZS(BP_POSTPO));
  __mmask8 kC = _mm512_mask_cmpeq_epi64_mask(kPO, w1, ZS(BP_STSHTTP));
  kC = _mm512_mask_cmpeq_epi64_mask(kC, _mm512_and_si512(w2, ZS(0xFFFFFFFFFFFFull)), ZS(BP_V11));
  __mmask8 kG = _mm512_cmpeq_epi64_mask(w0, ZS(BP_GETPOS));
  kG = _mm512_mask_cmpeq_epi64_mask(kG, _mm512_and_si512(w1, ZS(0xFFFFFF)), ZS(BP_TS));
  __mmask8 kL = _mm512_mask_cmpeq_epi64_mask(kPO, _mm512_and_si512(w1, ZS(0xFFFFFFFFull)), ZS(BP_STS));
  __m512i dG = _mm512_or_si512(_mm512_srli_epi64(w1, 24), _mm512_slli_epi64(w2, 40));
  __m512i dL = _mm512_or_si512(_mm512_srli_epi64(w1, 32), _mm512_slli_epi64(w2, 32));
  __m512i d = _mm512_mask_blend_epi64(kL, dG, dL);
  __m512i t = _mm512_sub_epi8(d, _mm512_set1_epi8('0'));
  __mmask64 kdig = _mm512_cmplt_epu8_mask(t, _mm512_set1_epi8(10));              // 64 bits = 8 lanes x 8 bytes
  __m512i nd = _mm512_movm_epi8(~kdig);                                           // 0xFF per non-digit byte
  __m512i lz = _mm512_lzcnt_epi64(_mm512_and_si512(nd, _mm512_sub_epi64(Z0, nd)));
  __m512i n = _mm512_srai_epi64(_mm512_sub_epi64(ZS(63), lz), 3);                 // leading digits; -1 if all 8
  __mmask8 kok = _mm512_mask_cmpgt_epi64_mask(kG | kL, n, Z0);
  __m512i x = _mm512_sllv_epi64(t, _mm512_sub_epi64(ZS(64), _mm512_slli_epi64(n, 3)));
  __m512i v = _mm512_maddubs_epi16(x, _mm512_set1_epi16(0x010A));
  v = _mm512_madd_epi16(v, _mm512_set1_epi32(0x00010064));
  v = _mm512_add_epi64(_mm512_mul_epu32(v, ZS(10000)), _mm512_srli_epi64(v, 32));
  __m512i Q = _mm512_add_epi64(P, _mm512_add_epi64(n, _mm512_mask_blend_epi64(kL, ZS(11), ZS(12))));
  __m512i t1 = _mm512_mask_i64gather_epi64(Z0, kok, Q, (const void *)0, 1);
  __m512i t2 = _mm512_mask_i64gather_epi64(Z0, kok, _mm512_add_epi64(Q, ZS(8)), (const void *)0, 1);
  __mmask8 kGk = _mm512_mask_cmpeq_epi64_mask(kok & kG, t1, ZS(BP_SPHTTP));
  kGk = _mm512_mask_cmpeq_epi64_mask(kGk, _mm512_and_si512(t2, ZS(0xFFFFFF)), ZS(BP_ONECRLF));
  __mmask8 kLk = _mm512_mask_cmpeq_epi64_mask(kok & kL, t1, ZS(BP_LIKEHT));
  kLk = _mm512_mask_cmpeq_epi64_mask(kLk, t2, ZS(BP_TP11));
  __m512i r = _mm512_maskz_mov_epi64(kF, ZS(R_FEED));
  r = _mm512_mask_mov_epi64(r, kGk, ZS(R_GETPOST)); r = _mm512_mask_mov_epi64(r, kC, ZS(R_CREATE));
  r = _mm512_mask_mov_epi64(r, kLk, ZS(R_LIKE));
  __m512i l = _mm512_maskz_mov_epi64(kF, ZS(19));
  l = _mm512_mask_mov_epi64(l, kC, ZS(21));
  l = _mm512_mask_add_epi64(l, kGk, n, ZS(21)); l = _mm512_mask_add_epi64(l, kLk, n, ZS(27));
  _mm512_storeu_si512((void *)rt, r); _mm512_storeu_si512((void *)l0, l);
  _mm512_storeu_si512((void *)id, _mm512_maskz_mov_epi64(kGk | kLk, v));
}
#undef ZS
#endif

// ---------------- phase C: one GET lane's headers (canonical request line already known) ----------------
// http.h's fp_quick_get claims the common shape (headers end within 128 B, CRLF-clean, nothing for the
// cand core to look at) and returns hend, exactly what fast_parse returns; anything else runs http.h's
// own core with l0 known (FP_CORE_L). The header rules live in http.h only (since 049a66bdf1d9).
FP_INLINE int bp_get2(const char *b, int len, req_t *r, int l0) {
  int h = fp_quick_get(b, len, FP_BACKEND != 0);
  if (FP_LIKELY(h > 0)) { r->body_off = h; r->body_len = 0; return h; }
  return FP_CORE_L(b, len, r, l0);
}

// ---------------- the batch ----------------
static const char bp_dummy[128] = {0};   // fills vector tail lanes (never canonical)
typedef struct { uint64_t lanes, canon_get, canon_post, slow; } bp_stats_t;
static bp_stats_t bp_stats;              // optional counters (BP_STATS)

FP_INLINE void bp_chunk(int n, const char *const *buf, const int *len, req_t *out, int *ret, const int rl) {
  const char *P[BP_CHUNK + 8]; int64_t rt[BP_CHUNK + 8], l0[BP_CHUNK + 8], id[BP_CHUNK + 8];
  int W = rl == BP_RL_Z ? 8 : (rl == BP_RL_C || rl == BP_RL_CANON) ? 1 : 4, nv = (n + W - 1) / W * W;
  for (int i = 0; i < n; i++) { P[i] = buf[i]; __builtin_prefetch(buf[i] + 64); }
  for (int i = n; i < nv; i++) P[i] = bp_dummy;
  // A. request lines
  for (int i = 0; i < nv; i += W) {
#ifdef BP_HAVE_Z
    if (rl == BP_RL_Z) { bp_rl_z(P + i, rt + i, l0 + i, id + i); continue; }
#endif
#ifdef BP_HAVE_AVX2
    if (rl == BP_RL_AVX2 || rl == BP_RL_AVX2I) { bp_rl_avx2(P + i, rt + i, l0 + i, id + i, rl == BP_RL_AVX2); continue; }
#endif
    if (rl == BP_RL_CANON) { req_t t; l0[i] = fp_reqline_canon(P[i], &t); rt[i] = t.route; id[i] = t.id; continue; }
    bp_rl_c1(P[i], rt + i, l0 + i, id + i);
  }
  // B. buckets: GET-shaped canonical, POST-shaped canonical, the rest (branch-free appends)
  int gl[BP_CHUNK], pl[BP_CHUNK], xl[BP_CHUNK], ng = 0, np = 0, nx = 0;
  for (int i = 0; i < n; i++) {
    int canon = l0[i] != 0, post = fp_route_auth((int)rt[i]);
    gl[ng] = i; ng += canon & !post;
    pl[np] = i; np += canon & post;
    xl[nx] = i; nx += !canon;
  }
#ifdef BP_STATS
  bp_stats.lanes += n; bp_stats.canon_get += ng; bp_stats.canon_post += np;
#endif
  // C. headers, one route shape per loop
#ifdef BP_PHASE_A_ONLY
  for (int i = 0; i < n; i++) { ret[i] = (int)(l0[i] + rt[i] + id[i]); out[i].route = (int)rt[i]; } return;
#endif
  for (int j = 0; j < ng; j++) {
    int i = gl[j]; req_t *r = &out[i];
    r->route = (int)rt[i]; r->id = id[i]; r->id_ok = id[i] > 0; r->auth = 0; r->tok = 0; r->tok_len = 0;
    int c = bp_get2(buf[i], len[i], r, (int)l0[i]);
    ret[i] = c;
  }
  for (int j = 0; j < np; j++) {
    int i = pl[j]; req_t *r = &out[i];
    r->route = (int)rt[i]; r->id = id[i]; r->id_ok = id[i] > 0; r->auth = 0; r->tok = 0; r->tok_len = 0;
    int c = FP_CORE_L(buf[i], len[i], r, (int)l0[i]);   // http.h's own core: POST lanes walk every line
    ret[i] = c;
  }
#ifdef BP_STATS
  bp_stats.slow += nx;   // non-canonical lanes (GET lanes declined by bp_get2 are not counted)
#endif
  for (int j = 0; j < nx; j++) { int i = xl[j]; ret[i] = fast_parse(buf[i], len[i], &out[i]); }
}

FP_INLINE void bp_parse_rl(int n, const char *const *buf, const int *len, req_t *out, int *ret, const int rl) {
  for (int o = 0; o < n; o += BP_CHUNK) bp_chunk(n - o < BP_CHUNK ? n - o : BP_CHUNK, buf + o, len + o, out + o, ret + o, rl);
}
static inline void bp_parse(int n, const char *const *buf, const int *len, req_t *out, int *ret) {
  bp_parse_rl(n, buf, len, out, ret, BP_RL_DEFAULT);
}
