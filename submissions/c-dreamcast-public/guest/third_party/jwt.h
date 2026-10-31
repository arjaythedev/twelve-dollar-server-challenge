/* jwt.h — batch HS256 JWT verification: one multi-buffer SIMD HMAC-SHA256 pass for many tokens.
 * Part of cursed-jwt.  MIT licence, see LICENSE.
 *
 * Every call recomputes every signature: nothing here caches anything across calls except the
 * key's HMAC ipad/opad midstates, which are a function of the secret alone.  See README.md for
 * the claim semantics and the measured numbers.
 *
 * Build: compile jwt.c (or #include it into your unity TU) for the baseline ISA; no -march is needed.  The engine is
 * chosen at run time from the CPU (see "CPU dispatch" below):
 *   x86-64 with AVX-512 F/VL/BW/DQ -> 8 lanes on ymm with vprord + vpternlogd (no zmm, no frequency-licence drop)
 *   x86-64 with AVX2 + BMI2        -> AVX2 8-lane multi-buffer
 *   AArch64 with SHA2 instructions -> 4 interleaved SHA2 streams
 *   AArch64                        -> NEON 4-lane multi-buffer
 *   anything else                  -> portable scalar
 * Batch 1 and leftover lanes run one message at a time; on x86-64 Linux with -DJWT_OSSL_SHA256 (the Makefile's
 * default there) that is OpenSSL's extracted sha256 block function, src/sha256_x86_64_ossl.S (link it too).
 * Every engine that the target can run is also exported for tests/benchmarks.
 */
#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JWT_OK = 0,
    JWT_INVALID = 1,     /* malformed, wrong alg, bad signature, expired / not yet valid */
    JWT_BAD_PAYLOAD = 2, /* signature and time OK, but sub / username unusable */
};

#ifndef JWT_USERNAME_MAX
#define JWT_USERNAME_MAX 255 /* decoded bytes; longer usernames -> JWT_BAD_PAYLOAD */
#endif
/* -DJWT_INPUT_PADDED: every tok[i] has >= 64 readable bytes after tok[i] + len[i] (padded receive
 * buffers), which lets the HMAC tail block be built with fixed-size loads. */
#ifndef JWT_TOKEN_MAX
#define JWT_TOKEN_MAX 8192 /* longer tokens -> JWT_INVALID (an HTTP header limit is lower anyway) */
#endif

typedef struct {
    uint32_t ih[8]; /* SHA-256 state after compressing (key ^ ipad) */
    uint32_t oh[8]; /* SHA-256 state after compressing (key ^ opad) */
} jwt_key_t;

typedef struct {
    uint8_t status;          /* JWT_OK / JWT_INVALID / JWT_BAD_PAYLOAD */
    uint8_t username_escape; /* 1: username contains '"', '\\' or bytes < 0x20 and must be
                                JSON-escaped before being written into a JSON string.
                                0: the bytes can be copied verbatim between quotes. */
    uint16_t username_len;
    int64_t uid;          /* the integer in "sub", >= 1 */
    const char *username; /* points into ubuf below (decoded, unescaped, valid UTF-8) */
    char ubuf[JWT_USERNAME_MAX + 1];
} jwt_result_t;

/* Secret of any length (> 64 bytes is hashed first, per RFC 2104). */
void jwt_key_init(jwt_key_t *k, const void *secret, size_t len);

/* Verify n tokens (the bytes after "Bearer ").  tok[i]/len[i] need not be NUL-terminated.
 * now_s = current Unix time in whole seconds; a token is live iff nbf <= now_s < exp. */
void jwt_verify_batch(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                      int64_t now_s, jwt_result_t *out);

/* ---- CPU dispatch ----
 * The engine is chosen at run time from the CPU's features (CPUID / HWCAP), whatever -march the library was built
 * with: AVX-512VL, then AVX2, then scalar on x86-64; SHA2 instructions, then NEON on AArch64.  The engine-specific
 * entry points below exist for tests and benchmarks; call one only if jwt_cpu_features() has its bit. */
enum { JWT_CPU_AVX2 = 1, JWT_CPU_AVX512VL = 2, JWT_CPU_BMI2_ADX = 4, JWT_CPU_SHANI = 8, JWT_CPU_IFMA = 16,
       JWT_CPU_SHA2 = 32, JWT_CPU_SHA512 = 64,
       JWT_CPU_FAST_ZMM = 128 /* 512-bit vectors cost no clock speed (AMD Zen 4 / 5): the 16-lane zmm engine */ };
unsigned jwt_cpu_features(void); /* JWT_CPU_* bits; env JWT_CPU_MASK=0x.. masks them (tests, benchmarks) */
const char *jwt_engine(void);    /* the HS256 engine jwt_verify_batch uses: "avx512vl", "avx2", "sha2", ... */

/* ---- engine-specific entry points (same contract), for tests and benchmarks ---- */
void jwt_verify_batch_scalar(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                             int64_t now_s, jwt_result_t *out);
#if defined(__aarch64__) || defined(_M_ARM64)
void jwt_verify_batch_neon(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                           int64_t now_s, jwt_result_t *out);
#endif
#if defined(__aarch64__)
void jwt_verify_batch_sha2(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                           int64_t now_s, jwt_result_t *out);
#endif
#if defined(__x86_64__)
void jwt_verify_batch_avx2(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                           int64_t now_s, jwt_result_t *out);
#endif
#if defined(__x86_64__)
void jwt_verify_batch_avx512(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                             int64_t now_s, jwt_result_t *out);
#endif

/* Raw HMAC-SHA256 tags of n messages, engine-specific (exposed for tests/benchmarks).
 * mac[i] receives the 8 big-endian-interpreted state words (i.e. word j = bytes 4j..4j+3). */
typedef void jwt_hmac_fn(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len,
                         uint32_t (*mac)[8]);
jwt_hmac_fn jwt_hmac_scalar;
#if defined(__aarch64__) || defined(_M_ARM64)
jwt_hmac_fn jwt_hmac_neon;
#endif
#if defined(__aarch64__)
jwt_hmac_fn jwt_hmac_sha2, jwt_hmac_sha2_1, jwt_hmac_sha2_2, jwt_hmac_sha2_4; /* JWT_SHA2_STREAMS (default 4), 1, 4 */
#endif
#if defined(JWT_OSSL_SHA256) && defined(__x86_64__) && defined(__ELF__)
jwt_hmac_fn jwt_hmac_ossl; /* one message at a time, OpenSSL's extracted x86-64 block function (batch-1 path) */
#endif
#if defined(__x86_64__)
jwt_hmac_fn jwt_hmac_avx2, jwt_hmac_avx2x2; /* x2: 16 lanes = two interleaved 8-lane groups */
#endif
#if defined(__x86_64__)
jwt_hmac_fn jwt_hmac_avx512, jwt_hmac_avx512x2, jwt_hmac_zmm; /* zmm: 16 lanes in 512-bit regs, opt-in */
#endif

/* ---- RS256 / ES256 (prototype; build with -DJWT_PK, needs scripts/fetch-fiat.sh, see PK.md) ----
 * Optional asm backends: -DJWT_S2N (s2n-bignum, scripts/fetch-s2n-bignum.sh, link build/libs2n.a) and
 * -DJWT_OPENSSL (libcrypto BN for RSA, link -lcrypto); with both, the fastest kernel per platform and key size.
 * The same front end and claim rules as jwt_verify_batch, with a public key instead of a secret.  The key
 * fixes the algorithm: a token's header "alg" must equal it (no alg confusion).  Key setup does the
 * expensive per-key work once (ES256: comb tables of 528 KB for u1*G + u2*Q; RS256: Montgomery constants). */
typedef struct jwt_pkey jwt_pkey_t;
/* ES256: the public point's affine coordinates, big-endian.  NULL if not a point on P-256. */
jwt_pkey_t *jwt_pkey_es256(const uint8_t x[32], const uint8_t y[32]);
/* RS256: modulus and public exponent, big-endian; 2048..4096-bit odd n, odd e in [3, 2^64).  NULL otherwise. */
jwt_pkey_t *jwt_pkey_rs256(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen);
/* EdDSA (Ed25519, RFC 8037 "alg":"EdDSA"): the 32-byte public key.  NULL if it does not decode to a point. */
jwt_pkey_t *jwt_pkey_ed25519(const uint8_t pub[32]);
void jwt_pkey_free(jwt_pkey_t *k);
void jwt_verify_batch_pk(const jwt_pkey_t *k, int n, const char *const *tok, const int *len, int64_t now_s,
                         jwt_result_t *out);
/* Raw signature check of n messages (the JWS signing input); sig[i] is JOSE-format (ES256: r || s, 64 bytes;
 * RS256: k bytes for a k-byte modulus).  ok[i] = 1 iff valid.  For tests and benchmarks. */
void jwt_pk_verify_raw(const jwt_pkey_t *k, int n, const uint8_t *const *msg, const int *len,
                       const uint8_t *const *sig, const int *slen, uint8_t *ok);

/* Plain SHA-256 of n messages through the same multi-buffer engines (the RS256 / ES256 signing inputs).
 * dig[i] receives the 8 big-endian-interpreted state words.  jwt_sha256_batch picks the default engine. */
typedef void jwt_sha256_fn(int n, const uint8_t *const *msg, const int *len, uint32_t (*dig)[8]);
jwt_sha256_fn jwt_sha256_batch, jwt_sha256_scalar;
#if defined(__aarch64__) || defined(_M_ARM64)
jwt_sha256_fn jwt_sha256_neon;
#endif
#if defined(__aarch64__)
jwt_sha256_fn jwt_sha256_sha2;
#endif
#if defined(__x86_64__)
jwt_sha256_fn jwt_sha256_avx2, jwt_sha256_avx512;
#endif

/* base64url decode (no padding, canonical trailing bits).  Returns decoded length or -1.
 * dst needs room for (n/4)*3 + 3 bytes + 32 bytes of slack (SIMD stores overrun). */
int jwt_b64url_decode(uint8_t *dst, const char *src, int n);
int jwt_b64url_decode_scalar(uint8_t *dst, const char *src, int n);

#ifdef __cplusplus
}
#endif
#endif
