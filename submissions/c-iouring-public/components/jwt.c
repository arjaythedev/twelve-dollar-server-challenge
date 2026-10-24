/* jwt/jwt.c (sha256 3e1c2ebb91de), developed separately for this entry. */
/* jwt.c — see jwt.h.  Sections:
 *   1. SHA-256 constants, HMAC message plans, key setup
 *   2. SHA-256 engines: scalar / NEON 4-lane / AVX2 8-lane / AVX-512VL 8-lane (all from jwt_mb.inc),
 *      ARMv8 SHA2 instructions with 1/2/4 interleaved streams
 *   3. base64url decode: scalar / NEON (vld4 de-interleave + vqtbl4) / AVX2
 *   4. strict JSON for the header and payload
 *   5. the batch front end
 */
#include "jwt.h"

#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define JWT_HAVE_NEON 1
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#define JWT_INLINE static inline __attribute__((always_inline))

/* ======================================================================================
 * 1. constants, plans, key
 * ====================================================================================== */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static const uint32_t IV256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
static const uint8_t zero_block[64] __attribute__((aligned(64))) = {0};

JWT_INLINE uint32_t be32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

/* The inner hash continues from the ipad midstate over msg (L bytes), so its total length is
 * 64 + L and it needs nb = ceil((L + 9) / 64) more blocks: nf of them straight out of msg, the
 * rest (1 or 2) from `tail`, which holds the leftover bytes, 0x80, zeros and the bit length. */
typedef struct {
    const uint8_t *m;
    int nf, nb;
    uint8_t tail[128];
} hplan_t;

static void hplan_init(hplan_t *p, const uint8_t *m, int L) {
    int nf = L >> 6, rem = L & 63, nt = rem + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)(64 + L) * 8;
    p->m = m, p->nf = nf, p->nb = nf + nt;
#ifdef JWT_INPUT_PADDED
    /* Caller guarantees >= 64 readable bytes after each token (padded recv buffers): fixed-size
     * copy + mask instead of variable-length memcpy/memset. */
    static const uint8_t keep[128] = {[0 ... 63] = 0xFF};
    const uint8_t *src = m + (nf << 6);
    for (int i = 0; i < 64; i++) p->tail[i] = src[i] & keep[64 - rem + i];
    memset(p->tail + 64, 0, 64);
    p->tail[rem] = 0x80;
    bits = __builtin_bswap64(bits);
    memcpy(p->tail + nt * 64 - 8, &bits, 8);
#else
    memcpy(p->tail, m + (nf << 6), rem);
    p->tail[rem] = 0x80;
    memset(p->tail + rem + 1, 0, nt * 64 - rem - 1 - 8);
    for (int i = 0; i < 8; i++) p->tail[nt * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
#endif
}

JWT_INLINE const uint8_t *hplan_block(const hplan_t *p, int b) {
    return b < p->nf ? p->m + (b << 6) : p->tail + ((b - p->nf) << 6);
}

static void hmac1_scalar(const jwt_key_t *k, const hplan_t *p, uint32_t mac[8]);

/* ======================================================================================
 * 2. SHA-256 engines
 * ====================================================================================== */

/* vpternlogd truth tables (index = a<<2 | b<<1 | c), used by the AVX-512 engine and test.c */
#define JWT_TL_XOR3 0x96 /* a ^ b ^ c */
#define JWT_TL_CH 0xCA   /* a ? b : c */
#define JWT_TL_MAJ 0xE8  /* majority */

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* ---- scalar (the template with one lane) ---- */
#define MB(name) name##_scalar
#define MB_LANES 1
#define V uint32_t
#define VSET1(x) ((uint32_t)(x))
#define VADD(a, b) ((a) + (b))
#define VXOR(a, b) ((a) ^ (b))
#define VSHR(x, n) ((x) >> (n))
#define VROR(x, n) ROR32(x, n)
#define VXOR3(a, b, c) ((a) ^ (b) ^ (c))
#define VCH(e, f, g) ((g) ^ ((e) & ((f) ^ (g))))
#define VMAJ(a, b, c) ((b) ^ (((a) ^ (b)) & ((b) ^ (c))))
#define VSEL(m, x, y) (((m) & (x)) | (~(m) & (y)))
#define VMASK(arr) ((arr)[0])
#define MB_LOAD16(ptr, w)                                         \
    do {                                                          \
        for (int i_ = 0; i_ < 16; i_++) (w)[i_] = be32((ptr)[0] + 4 * i_); \
    } while (0)
#define MB_STORE8(s, out)                                         \
    do {                                                          \
        for (int i_ = 0; i_ < 8; i_++) (out)[0][i_] = (s)[i_];    \
    } while (0)
#define MB_MIN_LANES 1
#include "jwt_mb.inc"
#undef MB
#undef MB_LANES
#undef V
#undef VSET1
#undef VADD
#undef VXOR
#undef VSHR
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL
#undef VMASK
#undef MB_LOAD16
#undef MB_STORE8
#undef MB_MIN_LANES

static void hmac1_scalar(const jwt_key_t *k, const hplan_t *p, uint32_t mac[8]) {
    const hplan_t *pp = p;
    lanes_scalar(k, 1, &pp, (uint32_t(*)[8])mac);
}

void jwt_key_init(jwt_key_t *k, const void *secret, size_t len) {
    uint8_t key[64] = {0}, blk[64];
    uint32_t w[16], st[8];
    if (len > 64) { /* key = SHA-256(secret) */
        const uint8_t *s = secret;
        size_t off = 0;
        memcpy(st, IV256, 32);
        for (; off + 64 <= len; off += 64) {
            for (int i = 0; i < 16; i++) w[i] = be32(s + off + 4 * i);
            compress_scalar(st, w);
        }
        uint8_t last[128] = {0};
        size_t rem = len - off, nt = rem + 9 > 64 ? 2 : 1;
        memcpy(last, s + off, rem);
        last[rem] = 0x80;
        for (int i = 0; i < 8; i++) last[nt * 64 - 1 - i] = (uint8_t)(((uint64_t)len * 8) >> (8 * i));
        for (size_t b = 0; b < nt; b++) {
            for (int i = 0; i < 16; i++) w[i] = be32(last + 64 * b + 4 * i);
            compress_scalar(st, w);
        }
        for (int i = 0; i < 8; i++) {
            key[4 * i] = st[i] >> 24, key[4 * i + 1] = st[i] >> 16;
            key[4 * i + 2] = st[i] >> 8, key[4 * i + 3] = st[i];
        }
    } else if (len) {
        memcpy(key, secret, len);
    }
    for (int pass = 0; pass < 2; pass++) {
        uint8_t pad = pass ? 0x5c : 0x36;
        for (int i = 0; i < 64; i++) blk[i] = key[i] ^ pad;
        for (int i = 0; i < 16; i++) w[i] = be32(blk + 4 * i);
        memcpy(st, IV256, 32);
        compress_scalar(st, w);
        memcpy(pass ? k->oh : k->ih, st, 32);
    }
}

/* ---- NEON, 4 lanes ---- */
#ifdef JWT_HAVE_NEON
#define MB(name) name##_neon
#define MB_LANES 4
#define V uint32x4_t
#define VSET1(x) vdupq_n_u32(x)
#define VADD(a, b) vaddq_u32(a, b)
#define VXOR(a, b) veorq_u32(a, b)
#define VSHR(x, n) vshrq_n_u32(x, n)
#define VROR(x, n) vsliq_n_u32(vshrq_n_u32(x, n), x, 32 - (n)) /* shift right, then shift-left-insert */
#if defined(__ARM_FEATURE_SHA3)
#define VXOR3(a, b, c) veor3q_u32(a, b, c) /* EOR3 from the SHA3 extension */
#else
#define VXOR3(a, b, c) veorq_u32(veorq_u32(a, b), c)
#endif
#define VCH(e, f, g) vbslq_u32(e, f, g)                /* BSL is literally Ch */
#define VMAJ(a, b, c) vbslq_u32(veorq_u32(a, b), c, b) /* a^b ? c : b(=a) */
#define VSEL(m, x, y) vbslq_u32(m, x, y)
#define VMASK(arr) vld1q_u32(arr)
JWT_INLINE void neon_tr4(uint32x4_t r0, uint32x4_t r1, uint32x4_t r2, uint32x4_t r3, uint32x4_t *o) {
    uint32x4_t t0 = vtrn1q_u32(r0, r1), t1 = vtrn2q_u32(r0, r1);
    uint32x4_t t2 = vtrn1q_u32(r2, r3), t3 = vtrn2q_u32(r2, r3);
    o[0] = vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2)));
    o[1] = vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3)));
    o[2] = vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2)));
    o[3] = vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3)));
}
#define NEON_LDBE(p) vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p)))
#define MB_LOAD16(ptr, w)                                                                  \
    do {                                                                                   \
        for (int j_ = 0; j_ < 4; j_++)                                                     \
            neon_tr4(NEON_LDBE((ptr)[0] + 16 * j_), NEON_LDBE((ptr)[1] + 16 * j_),         \
                     NEON_LDBE((ptr)[2] + 16 * j_), NEON_LDBE((ptr)[3] + 16 * j_), (w) + 4 * j_); \
    } while (0)
#define MB_STORE8(s, out)                                                                  \
    do {                                                                                   \
        uint32x4_t o_[8];                                                                  \
        neon_tr4((s)[0], (s)[1], (s)[2], (s)[3], o_);                                      \
        neon_tr4((s)[4], (s)[5], (s)[6], (s)[7], o_ + 4);                                  \
        for (int l_ = 0; l_ < 4; l_++) vst1q_u32((out)[l_], o_[l_]), vst1q_u32((out)[l_] + 4, o_[4 + l_]); \
    } while (0)
#ifndef JWT_NEON_MIN_LANES
#define JWT_NEON_MIN_LANES 2
#endif
#define MB_MIN_LANES JWT_NEON_MIN_LANES
#include "jwt_mb.inc"
#undef MB
#undef MB_LANES
#undef V
#undef VSET1
#undef VADD
#undef VXOR
#undef VSHR
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL
#undef VMASK
#undef MB_LOAD16
#undef MB_STORE8
#undef MB_MIN_LANES
#endif /* NEON */

/* ---- AVX2 8 lanes, and the AVX-512VL flavour of the very same code ---- */
#if defined(__AVX2__)
JWT_INLINE void avx2_tr8(const __m256i *r, __m256i *o) {
    __m256i t0 = _mm256_unpacklo_epi32(r[0], r[1]), t1 = _mm256_unpackhi_epi32(r[0], r[1]);
    __m256i t2 = _mm256_unpacklo_epi32(r[2], r[3]), t3 = _mm256_unpackhi_epi32(r[2], r[3]);
    __m256i t4 = _mm256_unpacklo_epi32(r[4], r[5]), t5 = _mm256_unpackhi_epi32(r[4], r[5]);
    __m256i t6 = _mm256_unpacklo_epi32(r[6], r[7]), t7 = _mm256_unpackhi_epi32(r[6], r[7]);
    __m256i u0 = _mm256_unpacklo_epi64(t0, t2), u1 = _mm256_unpackhi_epi64(t0, t2);
    __m256i u2 = _mm256_unpacklo_epi64(t1, t3), u3 = _mm256_unpackhi_epi64(t1, t3);
    __m256i u4 = _mm256_unpacklo_epi64(t4, t6), u5 = _mm256_unpackhi_epi64(t4, t6);
    __m256i u6 = _mm256_unpacklo_epi64(t5, t7), u7 = _mm256_unpackhi_epi64(t5, t7);
    o[0] = _mm256_permute2x128_si256(u0, u4, 0x20), o[4] = _mm256_permute2x128_si256(u0, u4, 0x31);
    o[1] = _mm256_permute2x128_si256(u1, u5, 0x20), o[5] = _mm256_permute2x128_si256(u1, u5, 0x31);
    o[2] = _mm256_permute2x128_si256(u2, u6, 0x20), o[6] = _mm256_permute2x128_si256(u2, u6, 0x31);
    o[3] = _mm256_permute2x128_si256(u3, u7, 0x20), o[7] = _mm256_permute2x128_si256(u3, u7, 0x31);
}
#define AVX_BSWAP32 _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, \
                                     3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)
#define MB_LOAD16(ptr, w)                                                                   \
    do {                                                                                    \
        __m256i r_[8], bs_ = AVX_BSWAP32;                                                   \
        for (int h_ = 0; h_ < 2; h_++) {                                                    \
            for (int l_ = 0; l_ < 8; l_++)                                                  \
                r_[l_] = _mm256_shuffle_epi8(                                               \
                    _mm256_loadu_si256((const __m256i *)((ptr)[l_] + 32 * h_)), bs_);       \
            avx2_tr8(r_, (w) + 8 * h_);                                                     \
        }                                                                                   \
    } while (0)
#define MB_STORE8(s, out)                                                                   \
    do {                                                                                    \
        __m256i o_[8];                                                                      \
        avx2_tr8((s), o_);                                                                  \
        for (int l_ = 0; l_ < 8; l_++) _mm256_storeu_si256((__m256i *)(out)[l_], o_[l_]);   \
    } while (0)
#define MB_LANES 8
#define V __m256i
#define VSET1(x) _mm256_set1_epi32((int)(x))
#define VADD(a, b) _mm256_add_epi32(a, b)
#define VXOR(a, b) _mm256_xor_si256(a, b)
#define VSHR(x, n) _mm256_srli_epi32(x, n)
#define VMASK(arr) _mm256_loadu_si256((const __m256i *)(arr))
#ifndef JWT_AVX_MIN_LANES
#define JWT_AVX_MIN_LANES 2
#endif
#define MB_MIN_LANES JWT_AVX_MIN_LANES

#define MB(name) name##_avx2
#define VROR(x, n) _mm256_or_si256(_mm256_srli_epi32(x, n), _mm256_slli_epi32(x, 32 - (n)))
#define VXOR3(a, b, c) _mm256_xor_si256(_mm256_xor_si256(a, b), c)
#define VCH(e, f, g) _mm256_xor_si256(g, _mm256_and_si256(e, _mm256_xor_si256(f, g)))
/* Maj = b ^ ((a ^ b) & (b ^ c)): b ^ c is the previous round's a ^ b, which the fully unrolled
 * compress lets the compiler reuse, so 2 new ops per round instead of 4 (trick from cpuminer-opt) */
#define VMAJ(a, b, c) _mm256_xor_si256(b, _mm256_and_si256(_mm256_xor_si256(a, b), _mm256_xor_si256(b, c)))
#define VSEL(m, x, y) _mm256_blendv_epi8(y, x, m)
#include "jwt_mb.inc"
#undef MB
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL

#if defined(JWT_AVX512) && defined(__AVX512VL__)
/* UNTESTED ON HARDWARE.  Identical to the AVX2 instantiation except for these five macros.
 * vpternlogd immediates (truth table index = a<<2 | b<<1 | c):
 *   0x96 = a^b^c, 0xCA = a ? b : c (Ch, and the lane select), 0xE8 = majority.
 * test.c runs the template with a scalar ternlog emulation using these same constants. */
#define MB(name) name##_avx512
#define VROR(x, n) _mm256_ror_epi32(x, n) /* vprord */
#define VXOR3(a, b, c) _mm256_ternarylogic_epi32(a, b, c, JWT_TL_XOR3)
#define VCH(e, f, g) _mm256_ternarylogic_epi32(e, f, g, JWT_TL_CH)
#define VMAJ(a, b, c) _mm256_ternarylogic_epi32(a, b, c, JWT_TL_MAJ)
#define VSEL(m, x, y) _mm256_ternarylogic_epi32(m, x, y, JWT_TL_CH)
#include "jwt_mb.inc"
#undef MB
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL
#endif

#undef MB_LANES
#undef V
#undef VSET1
#undef VADD
#undef VXOR
#undef VSHR
#undef VMASK
#undef MB_LOAD16
#undef MB_STORE8
#undef MB_MIN_LANES

/* ---- 16 lanes as two interleaved ymm groups ----
 * One 8-lane compress is latency-bound on Skylake-SP (llvm-mca: ~820 cycles vs ~585 of port
 * pressure), so give the scheduler two independent dependency chains.  With AVX-512VL there are
 * 32 ymm registers to hold both; with plain AVX2 (16 registers) it spills. */
typedef struct {
    __m256i a, b;
} v2_t;
#define V2OP1(name, expr)                                                    \
    JWT_INLINE v2_t name(v2_t x) {                                           \
        v2_t r;                                                              \
        r.a = expr(x.a), r.b = expr(x.b);                                    \
        return r;                                                            \
    }
#define V2OP2(name, expr)                                                    \
    JWT_INLINE v2_t name(v2_t x, v2_t y) {                                   \
        v2_t r;                                                              \
        r.a = expr(x.a, y.a), r.b = expr(x.b, y.b);                          \
        return r;                                                            \
    }
#define V2OP3(name, expr)                                                    \
    JWT_INLINE v2_t name(v2_t x, v2_t y, v2_t z) {                           \
        v2_t r;                                                              \
        r.a = expr(x.a, y.a, z.a), r.b = expr(x.b, y.b, z.b);                \
        return r;                                                            \
    }
JWT_INLINE v2_t v2_set1(uint32_t x) {
    v2_t r;
    r.a = r.b = _mm256_set1_epi32((int)x);
    return r;
}
#define A2_ADD(x, y) _mm256_add_epi32(x, y)
#define A2_XOR(x, y) _mm256_xor_si256(x, y)
V2OP2(v2_add, A2_ADD)
V2OP2(v2_xor, A2_XOR)
#define MB_LANES 16
#define V v2_t
#define VSET1(x) v2_set1(x)
#define VADD(a, b) v2_add(a, b)
#define VXOR(a, b) v2_xor(a, b)
#define VSHR(x, n) ((v2_t){_mm256_srli_epi32((x).a, n), _mm256_srli_epi32((x).b, n)})
#define VMASK(arr) ((v2_t){_mm256_loadu_si256((const __m256i *)(arr)), _mm256_loadu_si256((const __m256i *)(arr) + 1)})
#define MB_LOAD16(ptr, w)                                                                   \
    do {                                                                                    \
        __m256i r_[8], o_[16], bs_ = AVX_BSWAP32;                                           \
        for (int g_ = 0; g_ < 2; g_++) {                                                    \
            for (int h_ = 0; h_ < 2; h_++) {                                                \
                for (int l_ = 0; l_ < 8; l_++)                                              \
                    r_[l_] = _mm256_shuffle_epi8(                                           \
                        _mm256_loadu_si256((const __m256i *)((ptr)[8 * g_ + l_] + 32 * h_)), bs_); \
                avx2_tr8(r_, o_ + 8 * h_);                                                  \
            }                                                                               \
            for (int i_ = 0; i_ < 16; i_++) {                                               \
                if (g_) (w)[i_].b = o_[i_];                                                 \
                else (w)[i_].a = o_[i_];                                                    \
            }                                                                               \
        }                                                                                   \
    } while (0)
#define MB_STORE8(s, out)                                                                   \
    do {                                                                                    \
        __m256i i_[8], o_[8];                                                               \
        for (int g_ = 0; g_ < 2; g_++) {                                                    \
            for (int j_ = 0; j_ < 8; j_++) i_[j_] = g_ ? (s)[j_].b : (s)[j_].a;             \
            avx2_tr8(i_, o_);                                                               \
            for (int l_ = 0; l_ < 8; l_++) _mm256_storeu_si256((__m256i *)(out)[8 * g_ + l_], o_[l_]); \
        }                                                                                   \
    } while (0)
#ifndef JWT_AVX2X_MIN_LANES
#define JWT_AVX2X_MIN_LANES 9 /* <= 8 live lanes: a single 8-lane group is cheaper */
#endif
#define MB_MIN_LANES 1 /* the driver below routes small groups itself */
#define MB_NO_DRIVER

#define A2_ROR(x, n) _mm256_or_si256(_mm256_srli_epi32(x, n), _mm256_slli_epi32(x, 32 - (n)))
#define A2_XOR3(x, y, z) _mm256_xor_si256(_mm256_xor_si256(x, y), z)
#define A2_CH(e, f, g) _mm256_xor_si256(g, _mm256_and_si256(e, _mm256_xor_si256(f, g)))
#define A2_MAJ(a, b, c) _mm256_xor_si256(b, _mm256_and_si256(_mm256_xor_si256(a, b), _mm256_xor_si256(b, c)))
#define A2_SEL(m, x, y) _mm256_blendv_epi8(y, x, m)
V2OP3(v2_xor3_avx2, A2_XOR3)
V2OP3(v2_ch_avx2, A2_CH)
V2OP3(v2_maj_avx2, A2_MAJ)
V2OP3(v2_sel_avx2, A2_SEL)
#define MB(name) name##_avx2x2
#define VROR(x, n) ((v2_t){A2_ROR((x).a, n), A2_ROR((x).b, n)})
#define VXOR3(a, b, c) v2_xor3_avx2(a, b, c)
#define VCH(e, f, g) v2_ch_avx2(e, f, g)
#define VMAJ(a, b, c) v2_maj_avx2(a, b, c)
#define VSEL(m, x, y) v2_sel_avx2(m, x, y)
#include "jwt_mb.inc"
#undef MB
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL

#if defined(JWT_AVX512) && defined(__AVX512VL__)
#define T2_XOR3(x, y, z) _mm256_ternarylogic_epi32(x, y, z, JWT_TL_XOR3)
#define T2_CH(x, y, z) _mm256_ternarylogic_epi32(x, y, z, JWT_TL_CH)
#define T2_MAJ(x, y, z) _mm256_ternarylogic_epi32(x, y, z, JWT_TL_MAJ)
V2OP3(v2_xor3_512, T2_XOR3)
V2OP3(v2_ch_512, T2_CH)
V2OP3(v2_maj_512, T2_MAJ)
#define MB(name) name##_avx512x2
#define VROR(x, n) ((v2_t){_mm256_ror_epi32((x).a, n), _mm256_ror_epi32((x).b, n)})
#define VXOR3(a, b, c) v2_xor3_512(a, b, c)
#define VCH(e, f, g) v2_ch_512(e, f, g)
#define VMAJ(a, b, c) v2_maj_512(a, b, c)
#define VSEL(m, x, y) v2_ch_512(m, x, y)
#include "jwt_mb.inc"
#undef MB
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL
#endif
#undef MB_NO_DRIVER
#undef MB_LANES
#undef V
#undef VSET1
#undef VADD
#undef VXOR
#undef VSHR
#undef VMASK
#undef MB_LOAD16
#undef MB_STORE8
#undef MB_MIN_LANES

/* driver for the 16-lane flavours: full 16-groups go 2x8 interleaved, the rest to the 8-lane engine */
typedef void lanes_fn(const jwt_key_t *, int, const hplan_t *const *, uint32_t (*)[8]);
static void x2_drive(lanes_fn *l16, lanes_fn *l8, const jwt_key_t *k, int n, const uint8_t *const *msg,
                     const int *len, uint32_t (*mac)[8]) {
    enum { CH = 64 };
    hplan_t plan[CH];
    const hplan_t *pp[CH];
    uint32_t tmp[16][8];
    for (int base = 0; base < n; base += CH) {
        int m = n - base < CH ? n - base : CH;
        for (int i = 0; i < m; i++) hplan_init(&plan[i], msg[base + i], len[base + i]);
        for (int i = 0; i < m; i++) {
            const hplan_t *p = &plan[i];
            int j = i;
            while (j > 0 && pp[j - 1]->nb > p->nb) pp[j] = pp[j - 1], j--;
            pp[j] = p;
        }
        for (int g = 0; g < m;) {
            int nl = m - g;
            if (nl >= JWT_AVX2X_MIN_LANES) {
                if (nl > 16) nl = 16;
                l16(k, nl, &pp[g], tmp);
            } else {
                if (nl > 8) nl = 8;
                if (nl < JWT_AVX_MIN_LANES) {
                    for (int l = 0; l < nl; l++) hmac1_scalar(k, pp[g + l], tmp[l]);
                } else {
                    l8(k, nl, &pp[g], tmp);
                }
            }
            for (int l = 0; l < nl; l++) memcpy(mac[base + (int)(pp[g + l] - plan)], tmp[l], 32);
            g += nl;
        }
    }
}
void jwt_hmac_avx2x2(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
    x2_drive(lanes_avx2x2, lanes_avx2, k, n, msg, len, mac);
}
#if defined(JWT_AVX512) && defined(__AVX512VL__)
void jwt_hmac_avx512x2(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
    x2_drive(lanes_avx512x2, lanes_avx512, k, n, msg, len, mac);
}

/* ---- zmm, 16 lanes: NOT the default.  Fewest cycles per lane, but heavy 512-bit integer work can
 * drop the core's frequency licence for everything that runs after it; bench/licence.c measures
 * that on real hardware.  Loads/stores reuse the ymm 8x8 transposes and glue the halves. */
#define MB(name) name##_zmm
#define MB_LANES 16
#define V __m512i
#define VSET1(x) _mm512_set1_epi32((int)(x))
#define VADD(a, b) _mm512_add_epi32(a, b)
#define VXOR(a, b) _mm512_xor_si512(a, b)
#define VSHR(x, n) _mm512_srli_epi32(x, n)
#define VROR(x, n) _mm512_ror_epi32(x, n)
#define VXOR3(a, b, c) _mm512_ternarylogic_epi32(a, b, c, JWT_TL_XOR3)
#define VCH(e, f, g) _mm512_ternarylogic_epi32(e, f, g, JWT_TL_CH)
#define VMAJ(a, b, c) _mm512_ternarylogic_epi32(a, b, c, JWT_TL_MAJ)
#define VSEL(m, x, y) _mm512_ternarylogic_epi32(m, x, y, JWT_TL_CH)
#define VMASK(arr) _mm512_loadu_si512((const void *)(arr))
#define MB_LOAD16(ptr, w)                                                                   \
    do {                                                                                    \
        __m256i r_[8], o_[2][16], bs_ = AVX_BSWAP32;                                        \
        for (int g_ = 0; g_ < 2; g_++)                                                      \
            for (int h_ = 0; h_ < 2; h_++) {                                                \
                for (int l_ = 0; l_ < 8; l_++)                                              \
                    r_[l_] = _mm256_shuffle_epi8(                                           \
                        _mm256_loadu_si256((const __m256i *)((ptr)[8 * g_ + l_] + 32 * h_)), bs_); \
                avx2_tr8(r_, o_[g_] + 8 * h_);                                              \
            }                                                                               \
        for (int i_ = 0; i_ < 16; i_++)                                                     \
            (w)[i_] = _mm512_inserti64x4(_mm512_castsi256_si512(o_[0][i_]), o_[1][i_], 1);  \
    } while (0)
#define MB_STORE8(s, out)                                                                   \
    do { /* extract immediates must be literal constants at every -O level */               \
        __m256i i_[8], o_[8];                                                               \
        for (int j_ = 0; j_ < 8; j_++) i_[j_] = _mm512_castsi512_si256((s)[j_]);            \
        avx2_tr8(i_, o_);                                                                   \
        for (int l_ = 0; l_ < 8; l_++) _mm256_storeu_si256((__m256i *)(out)[l_], o_[l_]);   \
        for (int j_ = 0; j_ < 8; j_++) i_[j_] = _mm512_extracti64x4_epi64((s)[j_], 1);      \
        avx2_tr8(i_, o_);                                                                   \
        for (int l_ = 0; l_ < 8; l_++) _mm256_storeu_si256((__m256i *)(out)[8 + l_], o_[l_]); \
    } while (0)
#define MB_MIN_LANES 1
#define MB_NO_DRIVER
#include "jwt_mb.inc"
#undef MB_NO_DRIVER
#undef MB
#undef MB_LANES
#undef V
#undef VSET1
#undef VADD
#undef VXOR
#undef VSHR
#undef VROR
#undef VXOR3
#undef VCH
#undef VMAJ
#undef VSEL
#undef VMASK
#undef MB_LOAD16
#undef MB_STORE8
#undef MB_MIN_LANES
void jwt_hmac_zmm(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
    x2_drive(lanes_zmm, lanes_avx512, k, n, msg, len, mac);
}
#endif
#endif /* AVX2 */

/* ---- ARMv8 SHA2 instructions: S independent streams interleaved for latency hiding ---- */
#if defined(__ARM_FEATURE_SHA2)
/* 64 rounds on S streams; m[s][0..3] are the 16 message words (host order) of stream s. */
JWT_INLINE void ce_compress(int S, uint32x4_t *st0, uint32x4_t *st1, uint32x4_t (*m)[4]) {
    uint32x4_t a0[4], a1[4];
    for (int s = 0; s < S; s++) a0[s] = st0[s], a1[s] = st1[s];
#if defined(__clang__)
#pragma clang loop unroll(full)
#else
#pragma GCC unroll 16
#endif
    for (int r = 0; r < 16; r++) {
        uint32x4_t kr = vld1q_u32(K256 + 4 * r);
        for (int s = 0; s < S; s++) {
            uint32x4_t wk = vaddq_u32(m[s][r & 3], kr), t = st0[s];
            st0[s] = vsha256hq_u32(st0[s], st1[s], wk);
            st1[s] = vsha256h2q_u32(st1[s], t, wk);
            if (r < 12)
                m[s][r & 3] = vsha256su1q_u32(vsha256su0q_u32(m[s][r & 3], m[s][(r + 1) & 3]),
                                              m[s][(r + 2) & 3], m[s][(r + 3) & 3]);
        }
    }
    for (int s = 0; s < S; s++) st0[s] = vaddq_u32(st0[s], a0[s]), st1[s] = vaddq_u32(st1[s], a1[s]);
}

/* S plans with identical nb */
JWT_INLINE void ce_hmac(int S, const jwt_key_t *k, const hplan_t *const *p, uint32_t *const *mac) {
    uint32x4_t st0[4], st1[4], m[4][4];
    uint32x4_t ih0 = vld1q_u32(k->ih), ih1 = vld1q_u32(k->ih + 4);
    for (int s = 0; s < S; s++) st0[s] = ih0, st1[s] = ih1;
    for (int b = 0; b < p[0]->nb; b++) {
        for (int s = 0; s < S; s++) {
            const uint8_t *blk = hplan_block(p[s], b);
            for (int j = 0; j < 4; j++) m[s][j] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blk + 16 * j)));
        }
        ce_compress(S, st0, st1, m);
    }
    const uint32_t pad2[4] = {0x80000000u, 0, 0, 0}, pad3[4] = {0, 0, 0, (64 + 32) * 8};
    for (int s = 0; s < S; s++) {
        m[s][0] = st0[s], m[s][1] = st1[s], m[s][2] = vld1q_u32(pad2), m[s][3] = vld1q_u32(pad3);
        st0[s] = vld1q_u32(k->oh), st1[s] = vld1q_u32(k->oh + 4);
    }
    ce_compress(S, st0, st1, m);
    for (int s = 0; s < S; s++) vst1q_u32(mac[s], st0[s]), vst1q_u32(mac[s] + 4, st1[s]);
}

static void ce_drive(int S, const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len,
                     uint32_t (*mac)[8]) {
    enum { CH = 64 };
    hplan_t plan[CH];
    const hplan_t *pp[CH];
    for (int base = 0; base < n; base += CH) {
        int m = n - base < CH ? n - base : CH;
        for (int i = 0; i < m; i++) hplan_init(&plan[i], msg[base + i], len[base + i]);
        for (int i = 0; i < m; i++) { /* sort by nb so groups of S share a block count */
            const hplan_t *p = &plan[i];
            int j = i;
            while (j > 0 && pp[j - 1]->nb > p->nb) pp[j] = pp[j - 1], j--;
            pp[j] = p;
        }
        int g = 0;
        while (g < m) {
            int s = S;
            if (m - g < s) s = m - g;
            for (int i = 1; i < s; i++)
                if (pp[g + i]->nb != pp[g]->nb) { s = i; break; }
            uint32_t *out[4];
            for (int i = 0; i < s; i++) out[i] = mac[base + (int)(pp[g + i] - plan)];
            switch (s) { /* constant S per call so ce_hmac fully specialises */
            case 4: ce_hmac(4, k, pp + g, out); break;
            case 3: ce_hmac(2, k, pp + g, out); ce_hmac(1, k, pp + g + 2, out + 2); break;
            case 2: ce_hmac(2, k, pp + g, out); break;
            default: ce_hmac(1, k, pp + g, out); break;
            }
            g += s;
        }
    }
}

void jwt_hmac_sha2_1(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
    ce_drive(1, k, n, msg, len, mac);
}
void jwt_hmac_sha2(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
#ifndef JWT_SHA2_STREAMS
#define JWT_SHA2_STREAMS 4
#endif
    ce_drive(JWT_SHA2_STREAMS, k, n, msg, len, mac);
}
void jwt_hmac_sha2_2(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
    ce_drive(2, k, n, msg, len, mac);
}
void jwt_hmac_sha2_4(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len, uint32_t (*mac)[8]) {
    ce_drive(4, k, n, msg, len, mac);
}
#endif /* SHA2 */

/* ======================================================================================
 * 3. base64url
 * ====================================================================================== */

/* sextet value for the 64 base64url characters, 0xFF for everything else */
#define B64U_ROW(c) (((c) >= 'A' && (c) <= 'Z')   ? (c) - 'A' \
                     : ((c) >= 'a' && (c) <= 'z') ? (c) - 'a' + 26 \
                     : ((c) >= '0' && (c) <= '9') ? (c) - '0' + 52 \
                     : (c) == '-'                 ? 62 \
                     : (c) == '_'                 ? 63 \
                                                  : 0xFF)
#define B4(c) (uint8_t)B64U_ROW(c), (uint8_t)B64U_ROW(c + 1), (uint8_t)B64U_ROW(c + 2), (uint8_t)B64U_ROW(c + 3)
#define B16(c) B4(c), B4(c + 4), B4(c + 8), B4(c + 12)
static const uint8_t b64u_tab[256] __attribute__((aligned(64))) = {
    B16(0),   B16(16),  B16(32),  B16(48),  B16(64),  B16(80),  B16(96),  B16(112),
    B16(128), B16(144), B16(160), B16(176), B16(192), B16(208), B16(224), B16(240)};

/* checks the length class and the canonical-zero trailing bits; decodes the final < 4 chars */
JWT_INLINE int b64u_tail(uint8_t *dst, const char *src, int r) {
    const uint8_t *s = (const uint8_t *)src;
    uint32_t a, b, c;
    switch (r) {
    case 0: return 0;
    case 2:
        a = b64u_tab[s[0]], b = b64u_tab[s[1]];
        if ((a | b) & 0xC0 || b & 15) return -1;
        dst[0] = (uint8_t)(a << 2 | b >> 4);
        return 1;
    case 3:
        a = b64u_tab[s[0]], b = b64u_tab[s[1]], c = b64u_tab[s[2]];
        if ((a | b | c) & 0xC0 || c & 3) return -1;
        dst[0] = (uint8_t)(a << 2 | b >> 4), dst[1] = (uint8_t)(b << 4 | c >> 2);
        return 2;
    default: return -1;
    }
}

int jwt_b64url_decode_scalar(uint8_t *dst, const char *src, int n) {
    const uint8_t *s = (const uint8_t *)src;
    int q = n >> 2, o = 0;
    uint32_t bad = 0;
    for (int i = 0; i < q; i++, s += 4) {
        uint32_t a = b64u_tab[s[0]], b = b64u_tab[s[1]], c = b64u_tab[s[2]], d = b64u_tab[s[3]];
        bad |= a | b | c | d;
        uint32_t v = a << 18 | b << 12 | c << 6 | d;
        dst[o] = (uint8_t)(v >> 16), dst[o + 1] = (uint8_t)(v >> 8), dst[o + 2] = (uint8_t)v;
        o += 3;
    }
    if (bad & 0xC0) return -1;
    int t = b64u_tail(dst + o, (const char *)s, n & 3);
    return t < 0 ? -1 : o + t;
}

#if defined(JWT_HAVE_NEON)
/* 64 chars -> 48 bytes: vld4 de-interleaves the 4 sextets of every quantum into 4 registers,
 * vqtbl4/vqtbx4 translate through a 128-byte table, vst3 re-interleaves the 3 output bytes.
 * Table misses give 0xFF; bytes >= 0x80 index nothing and are caught by OR-ing in the input. */
JWT_INLINE uint8x16_t neon_b64_64(const uint8_t *src, uint8_t *dst, uint8x16x4_t lo, uint8x16x4_t hi) {
    const uint8x16_t x40 = vdupq_n_u8(0x40);
#define NEON_XLAT(v) vqtbx4q_u8(vqtbl4q_u8(lo, v), hi, veorq_u8(v, x40))
    uint8x16x4_t c = vld4q_u8(src);
    uint8x16_t a = NEON_XLAT(c.val[0]), b = NEON_XLAT(c.val[1]);
    uint8x16_t cc = NEON_XLAT(c.val[2]), d = NEON_XLAT(c.val[3]);
#undef NEON_XLAT
    uint8x16_t err = vorrq_u8(vorrq_u8(vorrq_u8(a, b), vorrq_u8(cc, d)),
                              vorrq_u8(vorrq_u8(c.val[0], c.val[1]), vorrq_u8(c.val[2], c.val[3])));
    uint8x16x3_t out;
    out.val[0] = vorrq_u8(vshlq_n_u8(a, 2), vshrq_n_u8(b, 4));
    out.val[1] = vorrq_u8(vshlq_n_u8(b, 4), vshrq_n_u8(cc, 2));
    out.val[2] = vorrq_u8(vshlq_n_u8(cc, 6), d);
    vst3q_u8(dst, out);
    return err;
}
int jwt_b64url_decode(uint8_t *dst, const char *src, int n) {
    const uint8x16x4_t lo = vld1q_u8_x4(b64u_tab), hi = vld1q_u8_x4(b64u_tab + 64);
    uint8x16_t err = vdupq_n_u8(0);
    int i = 0, o = 0;
    for (; i + 64 <= n; i += 64, o += 48) err = vorrq_u8(err, neon_b64_64((const uint8_t *)src + i, dst + o, lo, hi));
    int q = (n - i) & ~3;
    if (q) { /* whole quanta of the tail: one more SIMD block over an 'A'-padded copy */
        uint8_t buf[64], tmp[48];
        memset(buf, 'A', 64);
        memcpy(buf, src + i, q);
        err = vorrq_u8(err, neon_b64_64(buf, tmp, lo, hi));
        memcpy(dst + o, tmp, q / 4 * 3);
        i += q, o += q / 4 * 3;
    }
    if (vmaxvq_u8(err) & 0x80) return -1;
    int r = b64u_tail(dst + o, src + i, n - i);
    return r < 0 ? -1 : o + r;
}
#elif defined(__AVX2__)
/* 32 chars -> 24 bytes (Muła/Lemire packing; base64url translation via range compares).
 * Stores 32 bytes per block: dst needs the 32 bytes of slack jwt.h asks for. */
JWT_INLINE __m256i avx2_b64_32(const char *src, uint8_t *dst) {
#define RNG(v, lo, hi) _mm256_and_si256(_mm256_cmpgt_epi8(v, _mm256_set1_epi8((lo) - 1)), \
                                        _mm256_cmpgt_epi8(_mm256_set1_epi8((hi) + 1), v))
    __m256i v = _mm256_loadu_si256((const __m256i *)src);
    __m256i up = RNG(v, 'A', 'Z'), lw = RNG(v, 'a', 'z'), dg = RNG(v, '0', '9');
#undef RNG
    __m256i da = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('-')), us = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('_'));
    __m256i off = _mm256_or_si256(
        _mm256_or_si256(_mm256_and_si256(up, _mm256_set1_epi8(-65)), _mm256_and_si256(lw, _mm256_set1_epi8(-71))),
        _mm256_or_si256(_mm256_and_si256(dg, _mm256_set1_epi8(4)),
                        _mm256_or_si256(_mm256_and_si256(da, _mm256_set1_epi8(17)),
                                        _mm256_and_si256(us, _mm256_set1_epi8(-32)))));
    __m256i ok = _mm256_or_si256(_mm256_or_si256(up, lw), _mm256_or_si256(dg, _mm256_or_si256(da, us)));
    __m256i x = _mm256_add_epi8(v, off);
    x = _mm256_maddubs_epi16(x, _mm256_set1_epi32(0x01400140));
    x = _mm256_madd_epi16(x, _mm256_set1_epi32(0x00011000));
    x = _mm256_shuffle_epi8(x, _mm256_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1,
                                                2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1));
    x = _mm256_permutevar8x32_epi32(x, _mm256_setr_epi32(0, 1, 2, 4, 5, 6, 7, 7));
    _mm256_storeu_si256((__m256i *)dst, x);
    return _mm256_xor_si256(ok, _mm256_set1_epi8(-1)); /* 0xFF where invalid */
}
int jwt_b64url_decode(uint8_t *dst, const char *src, int n) {
    int i = 0, o = 0;
    __m256i bad = _mm256_setzero_si256();
    for (; i + 32 <= n; i += 32, o += 24) bad = _mm256_or_si256(bad, avx2_b64_32(src + i, dst + o));
    int q = (n - i) & ~3;
    if (q) { /* whole quanta of the tail: one more SIMD block over an 'A'-padded copy */
        char buf[32];
        memset(buf, 'A', 32);
        memcpy(buf, src + i, q);
        bad = _mm256_or_si256(bad, avx2_b64_32(buf, dst + o));
        i += q, o += q / 4 * 3;
    }
    if (!_mm256_testz_si256(bad, bad)) return -1;
    int r = b64u_tail(dst + o, src + i, n - i);
    return r < 0 ? -1 : o + r;
}
#else
int jwt_b64url_decode(uint8_t *dst, const char *src, int n) { return jwt_b64url_decode_scalar(dst, src, n); }
#endif

/* ======================================================================================
 * 4. strict JSON
 *
 * Grammar is RFC 8259 exactly (no comments, no trailing commas, no NaN, UTF-8 validated
 * inside strings, nesting depth <= 32).  Choices for claims (see RESULTS.md):
 *   - a repeated sub / username / exp / nbf (or alg in the header) -> INVALID (JSON.parse would
 *     silently take the last one; we refuse to guess)
 *   - keys are compared after unescaping, so "sub" is "sub"
 *   - lone surrogate escapes in username decode to U+FFFD
 * ====================================================================================== */

typedef struct {
    const uint8_t *p, *e;
} jcur_t;

JWT_INLINE void jws(jcur_t *c) {
    while (c->p < c->e && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static int hex4(const uint8_t *p, uint32_t *v) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t ch = p[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f') d = (ch | 0x20) - 'a' + 10;
        else return 0;
        r = r << 4 | d;
    }
    *v = r;
    return 1;
}

/* Validates a string at c->p (which must be at '"'); returns its raw contents and whether it has
 * escapes.  0 on any grammar or UTF-8 error. */
/* 1 for bytes that end the fast path inside a string: '"', '\\', < 0x20, >= 0x80 */
#define JC4(c) (c) < 0x20 || (c) >= 0x80 || (c) == '"' || (c) == '\\', (c) + 1 < 0x20 || (c) + 1 >= 0x80 || (c) + 1 == '"' || (c) + 1 == '\\', \
               (c) + 2 < 0x20 || (c) + 2 >= 0x80 || (c) + 2 == '"' || (c) + 2 == '\\', (c) + 3 < 0x20 || (c) + 3 >= 0x80 || (c) + 3 == '"' || (c) + 3 == '\\'
#define JC16(c) JC4(c), JC4(c + 4), JC4(c + 8), JC4(c + 12)
static const uint8_t jstop[256] = {JC16(0),   JC16(16),  JC16(32),  JC16(48),  JC16(64),  JC16(80),
                                   JC16(96),  JC16(112), JC16(128), JC16(144), JC16(160), JC16(176),
                                   JC16(192), JC16(208), JC16(224), JC16(240)};

JWT_INLINE int jstr(jcur_t *c, const uint8_t **s, int *n, int *esc) {
    const uint8_t *p = c->p, *e = c->e;
    if (p >= e || *p != '"') return 0;
    *s = ++p;
    *esc = 0;
    for (;;) {
        while (p < e && !jstop[*p]) p++;
        if (p >= e) return 0;
        uint8_t ch = *p;
        if (ch == '"') break;
        if (ch < 0x20) return 0;
        if (ch == '\\') {
            *esc = 1;
            if (p + 1 >= e) return 0;
            uint8_t x = p[1];
            if (x == 'u') {
                uint32_t v;
                if (p + 6 > e || !hex4(p + 2, &v)) return 0;
                p += 6;
            } else if (x == '"' || x == '\\' || x == '/' || x == 'b' || x == 'f' || x == 'n' || x == 'r' || x == 't') {
                p += 2;
            } else {
                return 0;
            }
            continue;
        }
        if (ch < 0x80) { p++; continue; }
        /* UTF-8: reject overlongs, surrogates, > U+10FFFF */
        int len;
        uint32_t cp;
        if (ch >= 0xC2 && ch <= 0xDF) len = 2, cp = ch & 0x1F;
        else if (ch >= 0xE0 && ch <= 0xEF) len = 3, cp = ch & 0x0F;
        else if (ch >= 0xF0 && ch <= 0xF4) len = 4, cp = ch & 0x07;
        else return 0;
        if (p + len > e) return 0;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (p[i] & 0x3F);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
            return 0;
        p += len;
    }
    *n = (int)(p - *s);
    c->p = p + 1;
    return 1;
}

/* Unescape a validated raw string into dst (cap bytes).  Returns length, or -1 if it won't fit. */
static int junescape(uint8_t *dst, int cap, const uint8_t *s, int n) {
    int o = 0;
    for (int i = 0; i < n;) {
        uint32_t cp = 0;
        if (s[i] != '\\') {
            if (o >= cap) return -1;
            dst[o++] = s[i++];
            continue;
        }
        uint8_t x = s[i + 1];
        i += 2;
        switch (x) {
        case 'b': cp = 8; break;
        case 'f': cp = 12; break;
        case 'n': cp = 10; break;
        case 'r': cp = 13; break;
        case 't': cp = 9; break;
        case 'u':
            hex4(s + i, &cp);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u' && hex4(s + i + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            break;
        default: cp = x; break; /* " \ / */
        }
        int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (o + need > cap) return -1;
        if (need == 1) dst[o++] = (uint8_t)cp;
        else if (need == 2) dst[o++] = (uint8_t)(0xC0 | cp >> 6), dst[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        else if (need == 3)
            dst[o++] = (uint8_t)(0xE0 | cp >> 12), dst[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)),
            dst[o++] = (uint8_t)(0x80 | (cp & 0x3F));
        else
            dst[o++] = (uint8_t)(0xF0 | cp >> 18), dst[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)),
            dst[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)), dst[o++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* A JSON number at c->p: validates the grammar and returns its span.  *isint is set when it is a
 * plain integer of <= 18 digits (value via jint); nothing is computed for numbers we skip. */
JWT_INLINE int jnum(jcur_t *c, const uint8_t **s, int *n, int *isint) {
    const uint8_t *p = c->p, *e = c->e, *d;
    int simple = 1;
    *s = p;
    if (p < e && *p == '-') p++;
    if (p >= e) return 0;
    d = p;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9')
        while (p < e && *p >= '0' && *p <= '9') p++;
    else return 0;
    int digits = (int)(p - d);
    if (p < e && *p == '.') {
        simple = 0, p++;
        if (p >= e || *p < '0' || *p > '9') return 0;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        simple = 0, p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return 0;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    *n = (int)(p - *s);
    *isint = simple && digits <= 18;
    c->p = p;
    return 1;
}

/* 8 ASCII digits -> value, SWAR (three multiplies instead of an 8-long multiply-add chain) */
JWT_INLINE uint64_t swar8(uint64_t x) { /* x = 8 digits, first digit in the low byte */
    x -= 0x3030303030303030ull;
    x = x * 10 + (x >> 8);
    x = ((x & 0x000000FF000000FFull) * (100 + (1000000ull << 32)) +
         ((x >> 16) & 0x000000FF000000FFull) * (1 + (10000ull << 32))) >> 32;
    return x;
}
/* value of a validated integer span of <= 18 digits (optional '-') */
static int64_t jint(const uint8_t *s, int n) {
    int neg = *s == '-';
    s += neg, n -= neg;
    uint64_t v = 0;
    while (n >= 8) {
        uint64_t x;
        memcpy(&x, s, 8);
        v = v * 100000000u + swar8(x), s += 8, n -= 8;
    }
    if (n) {
        uint64_t x = 0x3030303030303030ull, y = 0;
        memcpy(&y, s, n); /* little-endian: digits go to the high bytes, '0's pad the low ones */
        x = (x & ~(~0ull << (8 * (8 - n)))) | (y << (8 * (8 - n)));
        uint64_t p10 = 1;
        for (int i = 0; i < n; i++) p10 *= 10;
        v = v * p10 + swar8(x);
    }
    return neg ? -(int64_t)v : (int64_t)v;
}

/* Compare a JSON number with now: returns -1 / 0 / +1 for num < / == / > now. */
/* "now", formatted once per batch: an all-digit JSON integer (no leading zeros by grammar) compares
 * with it by digit count, then bytewise, with no number conversion at all. */
typedef struct {
    int64_t t;
    int len;
    uint8_t s[24];
} jnow_t;
static void jnow_init(jnow_t *jn, int64_t now) {
    uint8_t tmp[24];
    int l = 0;
    uint64_t u = now > 0 ? (uint64_t)now : 0;
    do tmp[l++] = (uint8_t)('0' + u % 10), u /= 10; while (u);
    for (int i = 0; i < l; i++) jn->s[i] = tmp[l - 1 - i];
    jn->len = now > 0 ? l : 0, jn->t = now;
}

/* Compare a JSON number with now: returns -1 / 0 / +1 for num < / == / > now. */
static int jnum_cmp(const uint8_t *s, int n, int isint, const jnow_t *now) {
    if (isint && now->len && s[0] != '-') {
        if (n != now->len) return n < now->len ? -1 : 1;
        for (int i = 0; i < n; i++)
            if (s[i] != now->s[i]) return s[i] < now->s[i] ? -1 : 1;
        return 0;
    }
    if (isint) {
        int64_t iv = jint(s, n);
        return iv < now->t ? -1 : iv > now->t;
    }
    char buf[64];
    if (n >= (int)sizeof buf) { /* absurdly long number: strtod on a heap copy */
        char *big = malloc(n + 1);
        if (!big) return -1;
        memcpy(big, s, n), big[n] = 0;
        double d = strtod(big, NULL);
        free(big);
        return d < (double)now->t ? -1 : d > (double)now->t;
    }
    memcpy(buf, s, n), buf[n] = 0;
    double d = strtod(buf, NULL);
    return d < (double)now->t ? -1 : d > (double)now->t;
}

static int jskip(jcur_t *c, int depth) {
    jws(c);
    if (c->p >= c->e) return 0;
    const uint8_t *s;
    int n, esc, isint;
    switch (*c->p) {
    case '"': return jstr(c, &s, &n, &esc);
    case '{':
    case '[': {
        uint8_t close = *c->p == '{' ? '}' : ']';
        int obj = close == '}';
        if (depth >= 32) return 0;
        c->p++;
        jws(c);
        if (c->p < c->e && *c->p == close) return c->p++, 1;
        for (;;) {
            if (obj) {
                jws(c);
                if (!jstr(c, &s, &n, &esc)) return 0;
                jws(c);
                if (c->p >= c->e || *c->p++ != ':') return 0;
            }
            if (!jskip(c, depth + 1)) return 0;
            jws(c);
            if (c->p >= c->e) return 0;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == close) return c->p++, 1;
            return 0;
        }
    }
    case 't': if (c->e - c->p >= 4 && !memcmp(c->p, "true", 4)) return c->p += 4, 1; return 0;
    case 'f': if (c->e - c->p >= 5 && !memcmp(c->p, "false", 5)) return c->p += 5, 1; return 0;
    case 'n': if (c->e - c->p >= 4 && !memcmp(c->p, "null", 4)) return c->p += 4, 1; return 0;
    default: return jnum(c, &s, &n, &isint);
    }
}

/* key -> small id; keys are unescaped first when needed */
enum { K_OTHER, K_SUB, K_USERNAME, K_EXP, K_NBF, K_ALG };
JWT_INLINE int jkey(const uint8_t *s, int n, int esc) {
    uint8_t buf[16];
    if (esc) {
        n = junescape(buf, sizeof buf, s, n);
        if (n < 0) return K_OTHER;
        s = buf;
    }
    switch (n) {
    case 3:
        if (!memcmp(s, "sub", 3)) return K_SUB;
        if (!memcmp(s, "exp", 3)) return K_EXP;
        if (!memcmp(s, "nbf", 3)) return K_NBF;
        if (!memcmp(s, "alg", 3)) return K_ALG;
        return K_OTHER;
    case 8: return memcmp(s, "username", 8) ? K_OTHER : K_USERNAME;
    default: return K_OTHER;
    }
}

/* Header: an object whose "alg" is exactly "HS256". */
static int header_ok(const uint8_t *h, int n) {
    jcur_t c = {h, h + n};
    const uint8_t *s;
    int len, esc, alg = 0;
    jws(&c);
    if (c.p >= c.e || *c.p++ != '{') return 0;
    jws(&c);
    if (c.p < c.e && *c.p == '}') return 0; /* {} has no alg */
    for (;;) {
        jws(&c);
        if (!jstr(&c, &s, &len, &esc)) return 0;
        int key = jkey(s, len, esc);
        jws(&c);
        if (c.p >= c.e || *c.p++ != ':') return 0;
        jws(&c);
        if (key == K_ALG) {
            uint8_t v[8];
            if (alg++ || !jstr(&c, &s, &len, &esc)) return 0;
            if (esc) {
                len = junescape(v, sizeof v, s, len);
                s = v;
            }
            if (len != 5 || memcmp(s, "HS256", 5)) return 0;
        } else if (!jskip(&c, 1)) {
            return 0;
        }
        jws(&c);
        if (c.p >= c.e) return 0;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p++ != '}') return 0;
        break;
    }
    jws(&c);
    return c.p == c.e && alg == 1;
}

/* "sub": digits only (leading zeros allowed), 1 <= value <= INT64_MAX */
JWT_INLINE int sub_value(const uint8_t *s, int len, int64_t *uid) {
    int64_t v = 0;
    if (len <= 0) return 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9' || v > (INT64_MAX - (s[i] - '0')) / 10) return 0;
        v = v * 10 + (s[i] - '0');
    }
    if (v <= 0) return 0;
    *uid = v;
    return 1;
}

/* Payload claims -> result.  Precedence: malformed/expired/not-yet-valid -> INVALID, then
 * sub/username problems -> BAD_PAYLOAD. */
static int payload_check(const uint8_t *pl, int n, const jnow_t *now, jwt_result_t *r) {
    jcur_t c = {pl, pl + n};
    const uint8_t *s;
    int len, esc, seen = 0, bad = 0, isint;
    jws(&c);
    if (c.p >= c.e || *c.p++ != '{') return JWT_INVALID;
    jws(&c);
    if (c.p < c.e && *c.p == '}') return JWT_INVALID; /* no exp */
    for (;;) {
        jws(&c);
        if (!jstr(&c, &s, &len, &esc)) return JWT_INVALID;
        int key = jkey(s, len, esc);
        jws(&c);
        if (c.p >= c.e || *c.p++ != ':') return JWT_INVALID;
        jws(&c);
        if (key != K_OTHER && key != K_ALG) {
            if (seen & (1 << key)) return JWT_INVALID;
            seen |= 1 << key;
        }
        if (c.p >= c.e) return JWT_INVALID;
        if (key == K_EXP || key == K_NBF) {
            if (*c.p != '-' && (*c.p < '0' || *c.p > '9')) return JWT_INVALID; /* must be a number */
            if (!jnum(&c, &s, &len, &isint)) return JWT_INVALID;
            int cmp = jnum_cmp(s, len, isint, now);
            if (key == K_EXP ? cmp <= 0 : cmp > 0) return JWT_INVALID;
        } else if (key == K_SUB) {
            if (*c.p != '"') {
                bad = 1;
                if (!jskip(&c, 1)) return JWT_INVALID;
            } else {
                if (!jstr(&c, &s, &len, &esc)) return JWT_INVALID;
                uint8_t buf[32];
                if (esc) {
                    len = junescape(buf, sizeof buf, s, len);
                    s = buf;
                }
                if (!sub_value(s, len, &r->uid)) bad = 1;
            }
        } else if (key == K_USERNAME) {
            if (*c.p != '"') {
                bad = 1;
                if (!jskip(&c, 1)) return JWT_INVALID;
            } else {
                if (!jstr(&c, &s, &len, &esc)) return JWT_INVALID;
                if (!esc) {
                    /* raw JSON string content without escapes cannot contain '"', '\' or < 0x20 */
                    if (len > JWT_USERNAME_MAX) bad = 1;
                    else memcpy(r->ubuf, s, len), r->username_len = (uint16_t)len, r->username_escape = 0;
                } else {
                    int m = junescape((uint8_t *)r->ubuf, JWT_USERNAME_MAX, s, len);
                    if (m < 0) {
                        bad = 1;
                    } else {
                        int x = 0;
                        for (int i = 0; i < m; i++) {
                            uint8_t ch = (uint8_t)r->ubuf[i];
                            x |= ch < 0x20 || ch == '"' || ch == '\\';
                        }
                        r->username_len = (uint16_t)m, r->username_escape = (uint8_t)x;
                    }
                }
            }
        } else if (!jskip(&c, 1)) {
            return JWT_INVALID;
        }
        jws(&c);
        if (c.p >= c.e) return JWT_INVALID;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p++ != '}') return JWT_INVALID;
        break;
    }
    jws(&c);
    if (c.p != c.e) return JWT_INVALID;
    if (!(seen & (1 << K_EXP))) return JWT_INVALID;
    if (bad || !(seen & (1 << K_SUB)) || !(seen & (1 << K_USERNAME))) return JWT_BAD_PAYLOAD;
    r->ubuf[r->username_len] = 0;
    return JWT_OK;
}

/* ======================================================================================
 * 4b. simdjson-style fast path for the payload
 *
 * Stage 1 (after simdjson, Apache-2.0/MIT, github.com/simdjson/simdjson): classify 64-byte
 * blocks into bitmasks (whitespace and operators via simdjson's 16-entry shuffle tables, quotes,
 * backslashes, control and non-ASCII bytes), turn quotes into an in-string mask with prefix-XOR
 * (carry-less multiply on x86, shift cascade on ARM, as simdjson does), and derive the token starts:
 * operators, opening quotes and the first byte of each scalar.  Stage 2 walks those bits once,
 * checking the object grammar and the claims as it goes.
 *
 * Opt-in (-DJWT_FAST_JSON): on the real Xeon it measured 3-4% slower end to end than the strict
 * parser (exp21), and a wash on the M5, so the strict parser is the default.
 *
 * It covers only the common subset: <= 256 bytes, ASCII, no backslashes, no nested containers,
 * no control characters.  Anything else, including anything malformed, returns -1 and the strict
 * parser above decides, so the strict parser stays the single source of truth for odd inputs;
 * test.c cross-checks the two on fuzzed payloads.
 * ====================================================================================== */

/* Per 64-byte block: space, operator and quote bitmasks, plus "odd": any backslash, control byte
 * (incl. \t \n \r) or non-ASCII byte, which sends the whole payload to the strict parser. */
typedef struct {
    uint64_t sp, op, quote, odd;
} jcls_t;

#if defined(__AVX2__)
JWT_INLINE uint64_t avx2_mask64(__m256i lo, __m256i hi) {
    return (uint32_t)_mm256_movemask_epi8(lo) | (uint64_t)(uint32_t)_mm256_movemask_epi8(hi) << 32;
}
JWT_INLINE void jclassify(const uint8_t *p, jcls_t *c) {
    /* simdjson's operator table: | 0x20 folds [ ] into { }, low nibble picks the candidate */
    const __m256i op_t = _mm256_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0);
    __m256i v0 = _mm256_loadu_si256((const __m256i *)p), v1 = _mm256_loadu_si256((const __m256i *)(p + 32));
    __m256i x20 = _mm256_set1_epi8(0x20);
    __m256i op0 = _mm256_cmpeq_epi8(_mm256_shuffle_epi8(op_t, v0), _mm256_or_si256(v0, x20));
    __m256i op1 = _mm256_cmpeq_epi8(_mm256_shuffle_epi8(op_t, v1), _mm256_or_si256(v1, x20));
    /* odd: < 0x20 (signed compare also catches >= 0x80, which is negative) or '\\' */
    __m256i bs = _mm256_set1_epi8('\\');
    __m256i odd0 = _mm256_or_si256(_mm256_cmpgt_epi8(x20, v0), _mm256_cmpeq_epi8(v0, bs));
    __m256i odd1 = _mm256_or_si256(_mm256_cmpgt_epi8(x20, v1), _mm256_cmpeq_epi8(v1, bs));
    c->odd = avx2_mask64(odd0, odd1);
    c->op = avx2_mask64(op0, op1);
    c->sp = avx2_mask64(_mm256_cmpeq_epi8(v0, x20), _mm256_cmpeq_epi8(v1, x20));
    __m256i q = _mm256_set1_epi8('"');
    c->quote = avx2_mask64(_mm256_cmpeq_epi8(v0, q), _mm256_cmpeq_epi8(v1, q));
}
JWT_INLINE uint64_t prefix_xor(uint64_t m) { /* simdjson haswell: one carry-less multiply */
#if defined(__PCLMUL__)
    return (uint64_t)_mm_cvtsi128_si64(_mm_clmulepi64_si128(_mm_set_epi64x(0, (long long)m), _mm_set1_epi8((char)0xFF), 0));
#else
    m ^= m << 1, m ^= m << 2, m ^= m << 4, m ^= m << 8, m ^= m << 16, m ^= m << 32;
    return m;
#endif
}
#elif defined(JWT_HAVE_NEON)
JWT_INLINE uint64_t neon_mask64(uint8x16_t m0, uint8x16_t m1, uint8x16_t m2, uint8x16_t m3) {
    const uint8x16_t bit = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t s0 = vpaddq_u8(vandq_u8(m0, bit), vandq_u8(m1, bit));
    uint8x16_t s1 = vpaddq_u8(vandq_u8(m2, bit), vandq_u8(m3, bit));
    s0 = vpaddq_u8(s0, s1);
    s0 = vpaddq_u8(s0, s0);
    return vgetq_lane_u64(vreinterpretq_u64_u8(s0), 0);
}
JWT_INLINE void jclassify(const uint8_t *p, jcls_t *c) {
    const uint8x16_t op_t = {0xff, 0, ',', ':', 0, '[', ']', '{', '}', 0, 0, 0, 0, 0, 0, 0}; /* simdjson arm64 */
    uint8x16_t v[4], op[4], sp[4], q[4], odd[4];
    for (int i = 0; i < 4; i++) {
        v[i] = vld1q_u8(p + 16 * i);
        op[i] = vceqq_u8(vqtbl1q_u8(op_t, vshrq_n_u8(vaddq_u8(v[i], vdupq_n_u8(3)), 4)), v[i]);
        sp[i] = vceqq_u8(v[i], vdupq_n_u8(' '));
        q[i] = vceqq_u8(v[i], vdupq_n_u8('"'));
        /* odd: (v - 0x20) >= 0x60 catches < 0x20 and >= 0x80 in one unsigned compare */
        odd[i] = vorrq_u8(vcgeq_u8(vsubq_u8(v[i], vdupq_n_u8(0x20)), vdupq_n_u8(0x60)),
                          vceqq_u8(v[i], vdupq_n_u8('\\')));
    }
    c->odd = neon_mask64(odd[0], odd[1], odd[2], odd[3]);
    c->op = neon_mask64(op[0], op[1], op[2], op[3]);
    c->sp = neon_mask64(sp[0], sp[1], sp[2], sp[3]);
    c->quote = neon_mask64(q[0], q[1], q[2], q[3]);
}
JWT_INLINE uint64_t prefix_xor(uint64_t m) { /* simdjson arm64: PMULL round trips cost more than this */
    m ^= m << 1, m ^= m << 2, m ^= m << 4, m ^= m << 8, m ^= m << 16, m ^= m << 32;
    return m;
}
#else
JWT_INLINE void jclassify(const uint8_t *p, jcls_t *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 64; i++) {
        uint64_t b = 1ull << i;
        uint8_t ch = p[i];
        if (ch == ' ') c->sp |= b;
        if (ch == '{' || ch == '}' || ch == '[' || ch == ']' || ch == ':' || ch == ',') c->op |= b;
        if (ch == '"') c->quote |= b;
        if (ch < 0x20 || ch >= 0x80 || ch == '\\') c->odd |= b;
    }
}
JWT_INLINE uint64_t prefix_xor(uint64_t m) {
    m ^= m << 1, m ^= m << 2, m ^= m << 4, m ^= m << 8, m ^= m << 16, m ^= m << 32;
    return m;
}
#endif

/* bitmask -> positions (simdjson's "flatten", 4 per step so the loop branch rarely mispredicts;
 * the extra slots it may write are past the count and never read) */
JWT_INLINE int jflatten(uint16_t *out, int cnt, uint64_t m, int base) {
    int c = __builtin_popcountll(m);
    uint16_t *o = out + cnt;
    for (int i = 0; i < c; i += 4) {
        o[i] = (uint16_t)(base + __builtin_ctzll(m | 1ull << 63)), m &= m - 1;
        o[i + 1] = (uint16_t)(base + __builtin_ctzll(m | 1ull << 63)), m &= m - 1;
        o[i + 2] = (uint16_t)(base + __builtin_ctzll(m | 1ull << 63)), m &= m - 1;
        o[i + 3] = (uint16_t)(base + __builtin_ctzll(m | 1ull << 63)), m &= m - 1;
    }
    return cnt + c;
}

/* first set bit of a 4-word mask at or after position i (or 256) */
JWT_INLINE int jbit_from(const uint64_t *m, int i) {
    uint64_t w = m[i >> 6] & (~0ull << (i & 63));
    if (w) return (i & ~63) + __builtin_ctzll(w);
    for (int b = (i >> 6) + 1; b < 4; b++)
        if (m[b]) return (b << 6) + __builtin_ctzll(m[b]);
    return 256;
}

/* pl must be readable up to n rounded up to 64.  The bytes past n are not written (a padding
 * memset right before the vector loads stalls store forwarding); they're masked out of the
 * bitmasks instead, as if they were spaces. */
static int payload_fast(const uint8_t *pl, int n, const jnow_t *now, jwt_result_t *r) {
    if (n < 2 || n > 256) return -1;
    int nb = (n + 63) >> 6;
    uint16_t tk[256 + 4]; /* token starts */
    int nt = 0;
    uint64_t prev_in = 0, sc[4] = {0}, nsc[4] = {~0ull, ~0ull, ~0ull, ~0ull}, cq[4] = {0};
    jcls_t c[4];
    for (int b = 0; b < nb; b++) {
        jclassify(pl + (b << 6), &c[b]);
        if (b == nb - 1 && (n & 63)) { /* bytes past n behave as spaces */
            uint64_t valid = (1ull << (n & 63)) - 1;
            c[b].odd &= valid, c[b].op &= valid, c[b].quote &= valid, c[b].sp |= ~valid;
        }
        if (c[b].odd) return -1;
        uint64_t in = prefix_xor(c[b].quote) ^ prev_in; /* opening quote .. byte before the closing one */
        prev_in = (uint64_t)((int64_t)in >> 63);
        uint64_t op = c[b].op & ~in;
        sc[b] = ~(in | c[b].sp | op | c[b].quote); /* scalar bytes outside strings */
        nsc[b] = ~sc[b];
        cq[b] = c[b].quote & ~in; /* closing quotes */
        uint64_t prev_sc = b ? sc[b - 1] >> 63 : 0;
        nt = jflatten(tk, nt, op | (c[b].quote & in) | (sc[b] & ~(sc[b] << 1 | prev_sc)), b << 6);
    }
    if (prev_in) return -1;

    /* stage 2: '{' (key ':' value (',' key ':' value)*) '}', all values flat */
    int t = 0, seen = 0, badp = 0;
    if (nt < 5 || pl[tk[t++]] != '{') return -1;
    for (;;) {
        if (t + 3 > nt) return -1;
        int k = tk[t++], colon = tk[t++], v = tk[t++];
        if (pl[k] != '"' || pl[colon] != ':') return -1;
        int ke = jbit_from(cq, k + 1), key = jkey(pl + k + 1, ke - k - 1, 0), str = pl[v] == '"', vend;
        if (str) {
            vend = jbit_from(cq, v + 1);
        } else {
            uint8_t ch = pl[v];
            if (ch == '{' || ch == '[' || ch == '}' || ch == ']' || ch == ',' || ch == ':') return -1;
            vend = jbit_from(nsc, v);
        }
        if (key != K_OTHER && key != K_ALG) {
            if (seen & (1 << key)) return JWT_INVALID;
            seen |= 1 << key;
        }
        const uint8_t *vs = pl + v + str;
        int vl = vend - v - str;
        if (!str) { /* the scalar must be exactly one number or literal */
            jcur_t cc = {pl + v, pl + vend};
            const uint8_t *nsp;
            int nl, isint = 0;
            int isnum = (*vs == '-' || (*vs >= '0' && *vs <= '9')) && jnum(&cc, &nsp, &nl, &isint) && cc.p == cc.e;
            if (!isnum && !(vl == 4 && (!memcmp(vs, "true", 4) || !memcmp(vs, "null", 4))) &&
                !(vl == 5 && !memcmp(vs, "false", 5)))
                return -1;
            if (key == K_EXP || key == K_NBF) {
                if (!isnum) return JWT_INVALID;
                int cmp = jnum_cmp(vs, vl, isint, now);
                if (key == K_EXP ? cmp <= 0 : cmp > 0) return JWT_INVALID;
            } else if (key == K_SUB || key == K_USERNAME) {
                badp = 1;
            }
        } else if (key == K_EXP || key == K_NBF) {
            return JWT_INVALID;
        } else if (key == K_SUB) {
            if (!sub_value(vs, vl, &r->uid)) badp = 1;
        } else if (key == K_USERNAME) { /* ASCII, no escapes, no control bytes: verbatim */
            if (vl > JWT_USERNAME_MAX) badp = 1;
            else memcpy(r->ubuf, vs, vl), r->username_len = (uint16_t)vl, r->username_escape = 0;
        }
        if (t >= nt) return -1;
        uint8_t sep = pl[tk[t++]];
        if (sep == ',') continue;
        if (sep != '}' || t != nt) return -1;
        break;
    }
    if (!(seen & (1 << K_EXP))) return JWT_INVALID;
    if (badp || !(seen & (1 << K_SUB)) || !(seen & (1 << K_USERNAME))) return JWT_BAD_PAYLOAD;
    r->ubuf[r->username_len] = 0;
    return JWT_OK;
}

/* ======================================================================================
 * 4c. shape-matched fast path (default; -DJWT_NO_SHAPE_JSON turns it off)
 *
 * The spec defines the payload as {"sub":"<id>","username":"<name>","iat":<n>,"exp":<n>}, and every
 * token the challenge mints (seed/generate.mjs, test.sh) is JSON.stringify output in exactly that
 * order with no whitespace.  This matcher accepts only that exact byte shape: literal separators,
 * ASCII digits, a username with no '"', '\\', control or non-ASCII bytes, integers without leading
 * zeros.  Anything else, a reordered or extra claim included, returns -1 and the strict parser
 * decides, so results are identical for every input (test.c fuzzes the two against each other).
 * Needs 16 readable bytes past n (padded decode buffers).
 * ====================================================================================== */

/* leading ASCII digits in p[0..7] (SWAR: borrows/carries only travel towards later bytes) */
JWT_INLINE int jdigits8(const uint8_t *p) {
    uint64_t x;
    memcpy(&x, p, 8);
    uint64_t nd = ((x - 0x3030303030303030ull) | (x + 0x4646464646464646ull)) & 0x8080808080808080ull;
    return nd ? __builtin_ctzll(nd) >> 3 : 8;
}
JWT_INLINE int jdigits(const uint8_t *p, int max) {
    int n = 0, d;
    do n += d = jdigits8(p + n);
    while (d == 8 && n < max);
    return n;
}
/* offset of the first '"', '\\', control or non-ASCII byte */
JWT_INLINE int jstrspan(const uint8_t *p, int max) {
    int i = 0;
#if defined(__SSE2__)
    for (; i < max; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i st = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))),
                                  _mm_cmplt_epi8(v, _mm_set1_epi8(0x20))); /* signed: also >= 0x80 */
        int m = _mm_movemask_epi8(st);
        if (m) return i + __builtin_ctz(m);
    }
#elif defined(JWT_HAVE_NEON)
    for (; i < max; i += 16) {
        uint8x16_t v = vld1q_u8(p + i);
        uint8x16_t st = vorrq_u8(vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))),
                                 vcgeq_u8(vsubq_u8(v, vdupq_n_u8(0x20)), vdupq_n_u8(0x60)));
        uint64_t m = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(st), 4)), 0);
        if (m) return i + (__builtin_ctzll(m) >> 2);
    }
#else
    for (; i < max; i++)
        if (jstop[p[i]]) return i;
#endif
    return max;
}
#define JLIT(p, s) (!memcmp((p), (s), sizeof(s) - 1))

static int payload_shape(const uint8_t *pl, int n, const jnow_t *now, jwt_result_t *r) {
    const uint8_t *p = pl, *e = pl + n;
    if (n < 44 || !JLIT(p, "{\"sub\":\"")) return -1;
    p += 8;
    const uint8_t *sub = p;
    int subl = jdigits(p, 24);
    p += subl;
    if (subl == 0 || subl > 20 || e - p < 14 || !JLIT(p, "\",\"username\":\"")) return -1;
    p += 14;
    const uint8_t *user = p;
    int ul = jstrspan(p, (int)(e - p));
    p += ul;
    if (p >= e || *p != '"' || ul > JWT_USERNAME_MAX || e - p < 9 || !JLIT(p, "\",\"iat\":")) return -1;
    p += 8;
    int il = jdigits(p, 24);
    if (il == 0 || il > 18 || (*p == '0' && il > 1)) return -1;
    p += il;
    if (e - p < 8 || !JLIT(p, ",\"exp\":")) return -1;
    p += 7;
    const uint8_t *exp = p;
    int el = jdigits(p, 24);
    if (el == 0 || el > 18 || (*p == '0' && el > 1) || p + el + 1 != e || p[el] != '}') return -1;
    if (jnum_cmp(exp, el, 1, now) <= 0) return JWT_INVALID;
    if (!sub_value(sub, subl, &r->uid)) return JWT_BAD_PAYLOAD;
    memcpy(r->ubuf, user, ul);
    r->ubuf[ul] = 0, r->username_len = (uint16_t)ul, r->username_escape = 0;
    return JWT_OK;
}

/* ======================================================================================
 * 4d. lane-parallel JSON: one payload per SIMD byte lane (32 on AVX2, 16 on NEON)
 *
 * The payloads are transposed (16x16 byte zips) so that one vector holds byte p of every payload.
 * One branch-free step per byte position then runs, in every lane at once:
 *   - a 10-state DFA for a flat object of string / non-negative-integer values (next state via one
 *     16-entry shuffle table per character class, merged with blends);
 *   - key matching against sub / username / exp / nbf: per-lane offset into the key, expected byte via
 *     a shuffle of each key's spelling, candidate masks AND-ed down;
 *   - per-key value spans (start, end, type) captured with blends, and duplicate-key detection.
 * A scalar post-pass per lane applies the claim rules to the captured spans.  Any lane that leaves
 * the subset (escapes, non-ASCII, control bytes, negative/float/literal values, nesting, leading
 * zeros, malformed) is flagged and goes to the strict parser instead.  Cost is per batch, not per
 * token: it only pays when batches are large.
 *
 * Opt-in (-DJWT_LANE_JSON), NOT recommended: on the real Xeon (exp24) a 32-lane batch costs ~3 us of
 * JSON (93 ns/token at batch 32) vs 35 ns/token for the shape matcher at any batch size.
 * ====================================================================================== */
#define JL_MAXN 240 /* byte positions must fit in a byte lane */
#ifndef JWT_LANES_MIN
#define JWT_LANES_MIN 8 /* fewer MAC-valid tokens in a batch: per-token parsing */
#endif
#if defined(__AVX2__) || defined(JWT_HAVE_NEON)
#if defined(__AVX2__)
#define JL 32
typedef __m256i jv8;
#define JV_SET1(x) _mm256_set1_epi8((char)(x))
#define JV_LOAD(p) _mm256_load_si256((const __m256i *)(p))
#define JV_STORE(p, v) _mm256_store_si256((__m256i *)(p), v)
#define JV_EQ(a, b) _mm256_cmpeq_epi8(a, b)
#define JV_OR(a, b) _mm256_or_si256(a, b)
#define JV_AND(a, b) _mm256_and_si256(a, b)
#define JV_ANDN(a, b) _mm256_andnot_si256(a, b) /* ~a & b */
#define JV_ADD(a, b) _mm256_add_epi8(a, b)
#define JV_SUB(a, b) _mm256_sub_epi8(a, b)
#define JV_MINU(a, b) _mm256_min_epu8(a, b)
#define JV_MAXU(a, b) _mm256_max_epu8(a, b)
#define JV_SEL(m, a, b) _mm256_blendv_epi8(b, a, m) /* m ? a : b */
#define JV_TBL(t) _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(t)))
#define JV_LOOK(t, i) _mm256_shuffle_epi8(t, i)
typedef __m128i jv16;
#define JZ_LO(a, b) _mm_unpacklo_epi8(a, b)
#define JZ_HI(a, b) _mm_unpackhi_epi8(a, b)
#define JZ_LOAD(p) _mm_loadu_si128((const __m128i *)(p))
#define JZ_STORE(p, v) _mm_store_si128((__m128i *)(p), v)
#else
#define JL 16
typedef uint8x16_t jv8;
#define JV_SET1(x) vdupq_n_u8((uint8_t)(x))
#define JV_LOAD(p) vld1q_u8(p)
#define JV_STORE(p, v) vst1q_u8(p, v)
#define JV_EQ(a, b) vceqq_u8(a, b)
#define JV_OR(a, b) vorrq_u8(a, b)
#define JV_AND(a, b) vandq_u8(a, b)
#define JV_ANDN(a, b) vbicq_u8(b, a)
#define JV_ADD(a, b) vaddq_u8(a, b)
#define JV_SUB(a, b) vsubq_u8(a, b)
#define JV_MINU(a, b) vminq_u8(a, b)
#define JV_MAXU(a, b) vmaxq_u8(a, b)
#define JV_SEL(m, a, b) vbslq_u8(m, a, b)
#define JV_TBL(t) vld1q_u8(t)
#define JV_LOOK(t, i) vqtbl1q_u8(t, i)
typedef uint8x16_t jv16;
#define JZ_LO(a, b) vzip1q_u8(a, b)
#define JZ_HI(a, b) vzip2q_u8(a, b)
#define JZ_LOAD(p) vld1q_u8(p)
#define JZ_STORE(p, v) vst1q_u8(p, v)
#endif

typedef struct {
    uint8_t ok, seen;
    uint8_t start[5], end[5], type[5]; /* by key id: 1 sub, 2 username, 3 exp, 4 nbf; type 1 str, 2 int */
} jlane_t;

/* 16 rows of 16 bytes -> 16 columns: four rounds of byte zips (perfect shuffle) */
JWT_INLINE void jl_tr16(jv16 *x) {
    for (int r = 0; r < 4; r++) {
        jv16 y[16];
        for (int i = 0; i < 8; i++) y[2 * i] = JZ_LO(x[i], x[i + 8]), y[2 * i + 1] = JZ_HI(x[i], x[i + 8]);
        for (int i = 0; i < 16; i++) x[i] = y[i];
    }
}

enum { JS_OBJ, JS_KEY0, JS_KEY, JS_COLON, JS_VAL, JS_STR, JS_INT, JS_SEP, JS_DONE, JS_FAIL };
#define F JS_FAIL
static const uint8_t jl_t_other[16] = {F, F, JS_KEY, F, F, JS_STR, F, F, F, F, F, F, F, F, F, F};
static const uint8_t jl_t_ws[16] = {JS_OBJ, JS_KEY0, JS_KEY, JS_COLON, JS_VAL, JS_STR, JS_SEP, JS_SEP, JS_DONE, F, F, F, F, F, F, F};
static const uint8_t jl_t_q[16] = {F, JS_KEY, JS_COLON, F, JS_STR, JS_SEP, F, F, F, F, F, F, F, F, F, F};
static const uint8_t jl_t_colon[16] = {F, F, JS_KEY, JS_VAL, F, JS_STR, F, F, F, F, F, F, F, F, F, F};
static const uint8_t jl_t_comma[16] = {F, F, JS_KEY, F, F, JS_STR, JS_KEY0, JS_KEY0, F, F, F, F, F, F, F, F};
static const uint8_t jl_t_lb[16] = {JS_KEY0, F, JS_KEY, F, F, JS_STR, F, F, F, F, F, F, F, F, F, F};
static const uint8_t jl_t_rb[16] = {F, F, JS_KEY, F, F, JS_STR, JS_DONE, JS_DONE, F, F, F, F, F, F, F, F};
static const uint8_t jl_t_dg[16] = {F, F, JS_KEY, F, JS_INT, JS_STR, JS_INT, F, F, F, F, F, F, F, F, F};
#undef F
static const uint8_t jl_k_sub[16] = "sub", jl_k_user[16] = "username", jl_k_exp[16] = "exp", jl_k_nbf[16] = "nbf";
static const uint8_t jl_seenbit[16] = {0, 1, 2, 4, 8};
static const uint8_t jl_zero_payload[256 + 64] __attribute__((aligned(64))) = {0};

/* m <= JL payloads, each readable for its length rounded up to 16 (+16).  Lanes with n > JL_MAXN
 * come back !ok. */
static void json_lanes(int m, const uint8_t *const *pl, const int *n, jlane_t *out) {
    uint8_t T[JL_MAXN + 16][JL] __attribute__((aligned(32)));
    uint8_t nl[JL] __attribute__((aligned(32)));
    int maxn = 0;
    for (int l = 0; l < JL; l++) {
        int ln = l < m ? n[l] : 0;
        if (ln > JL_MAXN) ln = 0; /* -> never reaches DONE */
        nl[l] = (uint8_t)ln;
        if (ln > maxn) maxn = ln;
    }
    int rows = (maxn + 15) & ~15;
    for (int g = 0; g < JL / 16; g++)
        for (int c = 0; c < rows; c += 16) {
            jv16 x[16];
            for (int i = 0; i < 16; i++) {
                int l = g * 16 + i;
                x[i] = JZ_LOAD((l < m && nl[l] ? pl[l] : jl_zero_payload) + c);
            }
            jl_tr16(x);
            for (int k = 0; k < 16; k++) JZ_STORE(&T[c + k][g * 16], x[k]);
        }
    const jv8 t_other = JV_TBL(jl_t_other), t_ws = JV_TBL(jl_t_ws), t_q = JV_TBL(jl_t_q), t_co = JV_TBL(jl_t_colon);
    const jv8 t_cm = JV_TBL(jl_t_comma), t_lb = JV_TBL(jl_t_lb), t_rb = JV_TBL(jl_t_rb), t_dg = JV_TBL(jl_t_dg);
    const jv8 k_sub = JV_TBL(jl_k_sub), k_user = JV_TBL(jl_k_user), k_exp = JV_TBL(jl_k_exp), k_nbf = JV_TBL(jl_k_nbf);
    const jv8 seenbit = JV_TBL(jl_seenbit), ones = JV_SET1(0xFF), one = JV_SET1(1), zero = JV_SET1(0);
    const jv8 nvec = JV_LOAD(nl);
    jv8 st = JV_SET1(JS_OBJ), fail = zero, seen = zero, koff = zero, c_sub = zero, c_user = zero, c_exp = zero, c_nbf = zero;
    jv8 curkey = zero, vstart = zero, vtype = zero, z = zero;
    jv8 s1 = zero, s2 = zero, s3 = zero, s4 = zero, e1 = zero, e2 = zero, e3 = zero, e4 = zero;
    jv8 y1 = zero, y2 = zero, y3 = zero, y4 = zero;
    for (int p = 0; p < maxn; p++) {
        jv8 P = JV_SET1(p), P1 = JV_SET1(p + 1);
        jv8 v = JV_SEL(JV_EQ(JV_MAXU(nvec, P1), nvec), JV_LOAD(T[p]), JV_SET1(' ')); /* past n: space */
        jv8 ws = JV_EQ(v, JV_SET1(' ')), q = JV_EQ(v, JV_SET1('"')), co = JV_EQ(v, JV_SET1(':'));
        jv8 cm = JV_EQ(v, JV_SET1(',')), lb = JV_EQ(v, JV_SET1('{')), rb = JV_EQ(v, JV_SET1('}'));
        jv8 d = JV_SUB(v, JV_SET1('0')), dg = JV_EQ(JV_MINU(d, JV_SET1(9)), d);
        jv8 x = JV_SUB(v, JV_SET1(0x20));
        fail = JV_OR(fail, JV_OR(JV_EQ(JV_MAXU(x, JV_SET1(0x60)), x), JV_EQ(v, JV_SET1('\\')))); /* <0x20, >=0x80, '\\' */
        /* events from the old state */
        jv8 inkey = JV_EQ(st, JV_SET1(JS_KEY)), inval = JV_EQ(st, JV_SET1(JS_VAL)), inint = JV_EQ(st, JV_SET1(JS_INT));
        jv8 kbeg = JV_AND(JV_EQ(st, JV_SET1(JS_KEY0)), q), kbyte = JV_ANDN(q, inkey), kend = JV_AND(inkey, q);
        jv8 sbeg = JV_AND(inval, q), ibeg = JV_AND(inval, dg);
        jv8 vend = JV_OR(JV_AND(JV_EQ(st, JV_SET1(JS_STR)), q), JV_AND(inint, JV_OR(ws, JV_OR(cm, rb))));
        /* next state */
        jv8 nx = JV_LOOK(t_other, st);
        nx = JV_SEL(ws, JV_LOOK(t_ws, st), nx);
        nx = JV_SEL(q, JV_LOOK(t_q, st), nx);
        nx = JV_SEL(co, JV_LOOK(t_co, st), nx);
        nx = JV_SEL(cm, JV_LOOK(t_cm, st), nx);
        nx = JV_SEL(lb, JV_LOOK(t_lb, st), nx);
        nx = JV_SEL(rb, JV_LOOK(t_rb, st), nx);
        nx = JV_SEL(dg, JV_LOOK(t_dg, st), nx);
        st = nx;
        /* integers: no leading zeros */
        fail = JV_OR(fail, JV_AND(JV_AND(inint, dg), z));
        z = JV_SEL(ibeg, JV_EQ(v, JV_SET1('0')), z);
        /* key matching */
        jv8 ko = JV_MINU(koff, JV_SET1(15));
        c_sub = JV_SEL(kbyte, JV_AND(c_sub, JV_EQ(v, JV_LOOK(k_sub, ko))), c_sub);
        c_user = JV_SEL(kbyte, JV_AND(c_user, JV_EQ(v, JV_LOOK(k_user, ko))), c_user);
        c_exp = JV_SEL(kbyte, JV_AND(c_exp, JV_EQ(v, JV_LOOK(k_exp, ko))), c_exp);
        c_nbf = JV_SEL(kbyte, JV_AND(c_nbf, JV_EQ(v, JV_LOOK(k_nbf, ko))), c_nbf);
        koff = JV_SEL(kbeg, zero, JV_ADD(koff, JV_AND(kbyte, one)));
        c_sub = JV_OR(c_sub, kbeg), c_user = JV_OR(c_user, kbeg), c_exp = JV_OR(c_exp, kbeg), c_nbf = JV_OR(c_nbf, kbeg);
        jv8 l3 = JV_EQ(koff, JV_SET1(3));
        jv8 id = JV_SEL(JV_AND(c_nbf, l3), JV_SET1(4), zero);
        id = JV_SEL(JV_AND(c_exp, l3), JV_SET1(3), id);
        id = JV_SEL(JV_AND(c_user, JV_EQ(koff, JV_SET1(8))), JV_SET1(2), id);
        id = JV_SEL(JV_AND(c_sub, l3), one, id);
        curkey = JV_SEL(kend, id, curkey);
        jv8 sb = JV_AND(kend, JV_LOOK(seenbit, id));
        fail = JV_OR(fail, JV_ANDN(JV_EQ(JV_AND(seen, sb), zero), ones)); /* duplicate claim */
        seen = JV_OR(seen, sb);
        /* values */
        vstart = JV_SEL(sbeg, P1, JV_SEL(ibeg, P, vstart));
        vtype = JV_SEL(sbeg, one, JV_SEL(ibeg, JV_SET1(2), vtype));
#define JL_CAPTURE(i, s_, e_, y_)                                   \
        do {                                                        \
            jv8 mk = JV_AND(vend, JV_EQ(curkey, JV_SET1(i)));       \
            s_ = JV_SEL(mk, vstart, s_), e_ = JV_SEL(mk, P, e_);    \
            y_ = JV_SEL(mk, vtype, y_);                             \
        } while (0)
        JL_CAPTURE(1, s1, e1, y1);
        JL_CAPTURE(2, s2, e2, y2);
        JL_CAPTURE(3, s3, e3, y3);
        JL_CAPTURE(4, s4, e4, y4);
#undef JL_CAPTURE
    }
    uint8_t A[16][JL] __attribute__((aligned(32)));
    JV_STORE(A[0], JV_ANDN(fail, JV_EQ(st, JV_SET1(JS_DONE))));
    JV_STORE(A[1], seen);
    JV_STORE(A[2], s1), JV_STORE(A[3], s2), JV_STORE(A[4], s3), JV_STORE(A[5], s4);
    JV_STORE(A[6], e1), JV_STORE(A[7], e2), JV_STORE(A[8], e3), JV_STORE(A[9], e4);
    JV_STORE(A[10], y1), JV_STORE(A[11], y2), JV_STORE(A[12], y3), JV_STORE(A[13], y4);
    for (int l = 0; l < m; l++) {
        jlane_t *o = &out[l];
        o->ok = A[0][l] && nl[l], o->seen = A[1][l];
        for (int i = 1; i <= 4; i++) o->start[i] = A[1 + i][l], o->end[i] = A[5 + i][l], o->type[i] = A[9 + i][l];
    }
}

/* claim rules on a lane's captured spans; -1 sends the payload to the strict parser */
static int json_lane_claims(const uint8_t *pl, const jlane_t *o, const jnow_t *now, jwt_result_t *r) {
    if (!o->ok) return -1;
    if (!(o->seen & 4)) return JWT_INVALID; /* no exp */
    int el = o->end[3] - o->start[3];
    if (o->type[3] != 2) return JWT_INVALID;
    if (el > 18) return -1; /* huge integer: let strtod in the strict parser decide */
    if (jnum_cmp(pl + o->start[3], el, 1, now) <= 0) return JWT_INVALID;
    if (o->seen & 8) {
        int nl = o->end[4] - o->start[4];
        if (o->type[4] != 2) return JWT_INVALID;
        if (nl > 18) return -1;
        if (jnum_cmp(pl + o->start[4], nl, 1, now) > 0) return JWT_INVALID;
    }
    int bad = !(o->seen & 1) || !(o->seen & 2) || o->type[1] != 1 || o->type[2] != 1;
    if (!bad && !sub_value(pl + o->start[1], o->end[1] - o->start[1], &r->uid)) bad = 1;
    int ul = o->end[2] - o->start[2];
    if (!bad && ul > JWT_USERNAME_MAX) bad = 1;
    if (bad) return JWT_BAD_PAYLOAD;
    memcpy(r->ubuf, pl + o->start[2], ul);
    r->ubuf[ul] = 0, r->username_len = (uint16_t)ul, r->username_escape = 0;
    return JWT_OK;
}
#define JWT_HAVE_LANES 1
#endif

/* ======================================================================================
 * 5. batch front end
 * ====================================================================================== */

static const char canon_hdr[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"; /* {"alg":"HS256","typ":"JWT"} */

static void verify_impl(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                        jwt_result_t *out, jwt_hmac_fn *hmac) {
    enum { CH = 64 };
    const uint8_t *msg[CH];
    int mlen[CH], idx[CH], poff[CH];
    uint32_t want[CH][8], mac[CH][8];
    jnow_t jn;
    jnow_init(&jn, now);
    for (int base = 0; base < n; base += CH) {
        int m = n - base < CH ? n - base : CH, c = 0;
        for (int i = 0; i < m; i++) {
            jwt_result_t *r = &out[base + i];
            const char *t = tok[base + i];
            int L = len[base + i];
            r->status = JWT_INVALID, r->username_escape = 0, r->username_len = 0, r->uid = 0;
            r->username = r->ubuf, r->ubuf[0] = 0;
            if (L <= 0 || L > JWT_TOKEN_MAX) continue;
            const char *d1 = memchr(t, '.', L);
            if (!d1) continue;
            const char *d2 = memchr(d1 + 1, '.', L - (d1 + 1 - t));
            if (!d2 || t + L - (d2 + 1) != 43) continue; /* 32-byte MAC = 43 base64url chars */
            int hl = (int)(d1 - t), pl = (int)(d2 - d1 - 1);
            if (hl == 0 || pl == 0) continue;
            if (!(hl == 36 && !memcmp(t, canon_hdr, 36))) {
                uint8_t hb[JWT_TOKEN_MAX * 3 / 4 + 64];
                int hn = jwt_b64url_decode(hb, t, hl);
                if (hn < 0 || !header_ok(hb, hn)) continue;
            }
            uint8_t sb[64];
            if (jwt_b64url_decode(sb, d2 + 1, 43) != 32) continue;
            for (int j = 0; j < 8; j++) want[c][j] = be32(sb + 4 * j);
            msg[c] = (const uint8_t *)t, mlen[c] = (int)(d2 - t), idx[c] = i, poff[c] = hl + 1;
            c++;
        }
        hmac(k, c, msg, mlen, mac);
        int good[CH], ng = 0;
        for (int j = 0; j < c; j++) {
            uint32_t diff = 0; /* constant time over the 32 bytes */
            for (int w = 0; w < 8; w++) diff |= mac[j][w] ^ want[j][w];
            if (!diff) good[ng++] = j;
        }
#if defined(JWT_LANE_JSON) && defined(JWT_HAVE_LANES)
        if (ng >= JWT_LANES_MIN) { /* lane-parallel JSON over the whole batch */
            _Static_assert(JL <= CH, "lane group larger than a chunk");
            uint8_t lb[CH][JL_MAXN + 80] __attribute__((aligned(16)));
            const uint8_t *lp[CH];
            int ln[CH], li[CH], nl = 0;
            for (int g = 0; g < ng; g++) {
                int j = good[g], el = mlen[j] - poff[j];
                jwt_result_t *r = &out[base + idx[j]];
                if (el / 4 * 3 + 2 > JL_MAXN) { /* too long for byte lanes: strict, per token */
                    uint8_t pb[JWT_TOKEN_MAX * 3 / 4 + 64];
                    int pn = jwt_b64url_decode(pb, (const char *)msg[j] + poff[j], el);
                    if (pn < 0) continue;
                    r->status = (uint8_t)payload_check(pb, pn, &jn, r);
                    if (r->status != JWT_OK) r->uid = 0, r->username_len = 0, r->ubuf[0] = 0;
                    continue;
                }
                int pn = jwt_b64url_decode(lb[nl], (const char *)msg[j] + poff[j], el);
                if (pn < 0) continue;
                lp[nl] = lb[nl], ln[nl] = pn, li[nl] = j, nl++;
            }
            for (int g0 = 0; g0 < nl; g0 += JL) {
                int m = nl - g0 < JL ? nl - g0 : JL;
                jlane_t o[JL];
                json_lanes(m, lp + g0, ln + g0, o);
                for (int q = 0; q < m; q++) {
                    jwt_result_t *r = &out[base + idx[li[g0 + q]]];
                    int st = json_lane_claims(lp[g0 + q], &o[q], &jn, r);
                    if (st < 0) st = payload_check(lp[g0 + q], ln[g0 + q], &jn, r);
                    r->status = (uint8_t)st;
                    if (r->status != JWT_OK) r->uid = 0, r->username_len = 0, r->ubuf[0] = 0;
                }
            }
            continue;
        }
#endif
        for (int g = 0; g < ng; g++) {
            int j = good[g];
            jwt_result_t *r = &out[base + idx[j]];
            uint8_t pb[JWT_TOKEN_MAX * 3 / 4 + 64];
            int pn = jwt_b64url_decode(pb, (const char *)msg[j] + poff[j], mlen[j] - poff[j]);
            if (pn < 0) continue;
#if defined(JWT_FAST_JSON) /* opt-in: measured 3-4% slower end to end on the Xeon (exp21) */
            int st = payload_fast(pb, pn, &jn, r);
#elif !defined(JWT_NO_SHAPE_JSON) /* default: JSON 131 -> 37 ns/token on the Xeon (exp24) */
            int st = payload_shape(pb, pn, &jn, r);
#else
            int st = -1;
#endif
            if (st < 0) st = payload_check(pb, pn, &jn, r);
            r->status = (uint8_t)st;
            if (r->status != JWT_OK) r->uid = 0, r->username_len = 0, r->ubuf[0] = 0;
        }
    }
}

void jwt_verify_batch_scalar(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                             jwt_result_t *out) {
    verify_impl(k, n, tok, len, now, out, jwt_hmac_scalar);
}
#ifdef JWT_HAVE_NEON
void jwt_verify_batch_neon(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                           jwt_result_t *out) {
    verify_impl(k, n, tok, len, now, out, jwt_hmac_neon);
}
#endif
#if defined(__ARM_FEATURE_SHA2)
void jwt_verify_batch_sha2(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                           jwt_result_t *out) {
    verify_impl(k, n, tok, len, now, out, jwt_hmac_sha2);
}
#endif
#if defined(__AVX2__)
void jwt_verify_batch_avx2(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                           jwt_result_t *out) {
    verify_impl(k, n, tok, len, now, out, jwt_hmac_avx2);
}
#endif
#if defined(JWT_AVX512) && defined(__AVX512VL__)
void jwt_verify_batch_avx512(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                             jwt_result_t *out) {
    verify_impl(k, n, tok, len, now, out, jwt_hmac_avx512);
}
#endif

void jwt_verify_batch(const jwt_key_t *k, int n, const char *const *tok, const int *len, int64_t now,
                      jwt_result_t *out) {
#if defined(__ARM_FEATURE_SHA2)
    verify_impl(k, n, tok, len, now, out, jwt_hmac_sha2);
#elif defined(JWT_AVX512) && defined(__AVX512VL__)
    verify_impl(k, n, tok, len, now, out, jwt_hmac_avx512);
#elif defined(__AVX2__)
    verify_impl(k, n, tok, len, now, out, jwt_hmac_avx2);
#elif defined(JWT_HAVE_NEON)
    verify_impl(k, n, tok, len, now, out, jwt_hmac_neon);
#else
    verify_impl(k, n, tok, len, now, out, jwt_hmac_scalar);
#endif
}
