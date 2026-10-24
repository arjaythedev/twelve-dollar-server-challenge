/* jwt/jwt.h (sha256 c559480f2247), developed separately for this entry. */
/* jwt.h — batch HS256 JWT verification for the $12 server challenge.
 *
 * Every call recomputes every signature (challenge rule 5): nothing here caches anything
 * across calls except the key's HMAC ipad/opad midstates, which are a function of the secret
 * alone.  See RESULTS.md for the semantics we chose and the measured numbers.
 *
 * Build: compile jwt.c (or #include it into your unity TU).  The SHA-256 engine is chosen at
 * compile time from the target:
 *   aarch64 with __ARM_FEATURE_SHA2   -> ARMv8 SHA2 instructions, 4 streams interleaved (default)
 *   aarch64                            -> NEON 4-lane multi-buffer
 *   x86-64 with -mavx2                 -> AVX2 8-lane multi-buffer
 *   x86-64 with -mavx512vl -DJWT_AVX512 -> same 8-lane code, vprord + vpternlogd (UNTESTED: no
 *                                         AVX-512 hardware available to the author)
 *   anything else                       -> portable scalar
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
    JWT_INVALID = 1,     /* 401 {"error":"invalid or expired token"} */
    JWT_BAD_PAYLOAD = 2, /* 401 {"error":"invalid token payload"} */
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

/* ---- engine-specific entry points (same contract), for tests and benchmarks ---- */
void jwt_verify_batch_scalar(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                             int64_t now_s, jwt_result_t *out);
#if defined(__aarch64__) || defined(_M_ARM64)
void jwt_verify_batch_neon(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                           int64_t now_s, jwt_result_t *out);
#endif
#if defined(__ARM_FEATURE_SHA2)
void jwt_verify_batch_sha2(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                           int64_t now_s, jwt_result_t *out);
#endif
#if defined(__AVX2__)
void jwt_verify_batch_avx2(const jwt_key_t *k, int n, const char *const *tok, const int *len,
                           int64_t now_s, jwt_result_t *out);
#endif
#if defined(JWT_AVX512) && defined(__AVX512VL__)
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
#if defined(__ARM_FEATURE_SHA2)
jwt_hmac_fn jwt_hmac_sha2, jwt_hmac_sha2_1, jwt_hmac_sha2_2, jwt_hmac_sha2_4; /* JWT_SHA2_STREAMS (default 4), 1, 4 */
#endif
#if defined(__AVX2__)
jwt_hmac_fn jwt_hmac_avx2, jwt_hmac_avx2x2; /* x2: 16 lanes = two interleaved 8-lane groups */
#endif
#if defined(JWT_AVX512) && defined(__AVX512VL__)
jwt_hmac_fn jwt_hmac_avx512, jwt_hmac_avx512x2, jwt_hmac_zmm; /* zmm: 16 lanes in 512-bit regs, opt-in */
#endif

/* base64url decode (no padding, canonical trailing bits).  Returns decoded length or -1.
 * dst needs room for (n/4)*3 + 3 bytes + 32 bytes of slack (SIMD stores overrun). */
int jwt_b64url_decode(uint8_t *dst, const char *src, int n);
int jwt_b64url_decode_scalar(uint8_t *dst, const char *src, int n);

#ifdef __cplusplus
}
#endif
#endif
