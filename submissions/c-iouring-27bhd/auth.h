#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#ifdef __SHA__
#include <immintrin.h>
#endif

#define AUTH_OK 0
#define AUTH_ERR_MISSING_BEARER 1
#define AUTH_ERR_INVALID_OR_EXPIRED 2
#define AUTH_ERR_INVALID_PAYLOAD 3

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} sha256_ctx_t;

typedef struct {
    uint32_t inner_state[8];
    uint32_t outer_state[8];
} hmac_midstate_t;

static inline uint32_t ror32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t bswap32(uint32_t x) {
    return __builtin_bswap32(x);
}

static inline uint64_t bswap64(uint64_t x) {
    return __builtin_bswap64(x);
}

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
#ifdef __SHA__
    __m128i s0, s1, abef, cdgh, abef_save, cdgh_save, msg, tmp;
    __m128i m0, m1, m2, m3;
    const __m128i shuf_mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

    s0 = _mm_loadu_si128((const __m128i *)&state[0]);
    s1 = _mm_loadu_si128((const __m128i *)&state[4]);
    s0 = _mm_shuffle_epi32(s0, 0xB1);
    s1 = _mm_shuffle_epi32(s1, 0x1B);
    abef = _mm_alignr_epi8(s0, s1, 8);
    cdgh = _mm_blend_epi16(s1, s0, 0xF0);
    abef_save = abef;
    cdgh_save = cdgh;

    m0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 0)), shuf_mask);
    msg = _mm_add_epi32(m0, _mm_loadu_si128((const __m128i *)&K256[0]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);

    m1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 16)), shuf_mask);
    msg = _mm_add_epi32(m1, _mm_loadu_si128((const __m128i *)&K256[4]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);
    m0 = _mm_sha256msg1_epu32(m0, m1);

    m2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 32)), shuf_mask);
    msg = _mm_add_epi32(m2, _mm_loadu_si128((const __m128i *)&K256[8]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);
    m1 = _mm_sha256msg1_epu32(m1, m2);

    m3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(data + 48)), shuf_mask);
    msg = _mm_add_epi32(m3, _mm_loadu_si128((const __m128i *)&K256[12]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    tmp = _mm_alignr_epi8(m3, m2, 4);
    m0 = _mm_add_epi32(m0, tmp);
    m0 = _mm_sha256msg2_epu32(m0, m3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);
    m2 = _mm_sha256msg1_epu32(m2, m3);

    #define SHA256_RNDS4(i, ma, mb, mc, md) \
        msg = _mm_add_epi32(ma, _mm_loadu_si128((const __m128i *)&K256[i])); \
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg); \
        tmp = _mm_alignr_epi8(ma, md, 4); \
        mb = _mm_add_epi32(mb, tmp); \
        mb = _mm_sha256msg2_epu32(mb, ma); \
        msg = _mm_shuffle_epi32(msg, 0x0E); \
        abef = _mm_sha256rnds2_epu32(abef, cdgh, msg); \
        mc = _mm_sha256msg1_epu32(mc, ma);

    SHA256_RNDS4(16, m0, m1, m3, m3)
    SHA256_RNDS4(20, m1, m2, m0, m0)
    SHA256_RNDS4(24, m2, m3, m1, m1)
    SHA256_RNDS4(28, m3, m0, m2, m2)
    SHA256_RNDS4(32, m0, m1, m3, m3)
    SHA256_RNDS4(36, m1, m2, m0, m0)
    SHA256_RNDS4(40, m2, m3, m1, m1)
    SHA256_RNDS4(44, m3, m0, m2, m2)
    SHA256_RNDS4(48, m0, m1, m3, m3)

    #undef SHA256_RNDS4

    msg = _mm_add_epi32(m1, _mm_loadu_si128((const __m128i *)&K256[52]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    tmp = _mm_alignr_epi8(m1, m0, 4);
    m2 = _mm_add_epi32(m2, tmp);
    m2 = _mm_sha256msg2_epu32(m2, m1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);

    msg = _mm_add_epi32(m2, _mm_loadu_si128((const __m128i *)&K256[56]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    tmp = _mm_alignr_epi8(m2, m1, 4);
    m3 = _mm_add_epi32(m3, tmp);
    m3 = _mm_sha256msg2_epu32(m3, m2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);

    msg = _mm_add_epi32(m3, _mm_loadu_si128((const __m128i *)&K256[60]));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);

    abef = _mm_add_epi32(abef, abef_save);
    cdgh = _mm_add_epi32(cdgh, cdgh_save);

    s0 = _mm_shuffle_epi32(abef, 0x1B);
    s1 = _mm_shuffle_epi32(cdgh, 0xB1);
    _mm_storeu_si128((__m128i *)&state[0], _mm_blend_epi16(s0, s1, 0xF0));
    _mm_storeu_si128((__m128i *)&state[4], _mm_alignr_epi8(s1, s0, 8));
#else
    uint32_t w[64];
    const uint32_t *dp = (const uint32_t *)data;
    for (int i = 0; i < 16; i++) {
        w[i] = bswap32(dp[i]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + K256[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
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
#endif
}

static inline void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static inline void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t buffer_len = (uint32_t)((ctx->count >> 3) & 63);
    ctx->count += ((uint64_t)len) << 3;

    if (buffer_len > 0) {
        uint32_t need = 64 - buffer_len;
        if (len < need) {
            memcpy(ctx->buffer + buffer_len, p, len);
            return;
        }
        memcpy(ctx->buffer + buffer_len, p, need);
        sha256_transform(ctx->state, ctx->buffer);
        p += need;
        len -= need;
    }

    while (len >= 64) {
        sha256_transform(ctx->state, p);
        p += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(ctx->buffer, p, len);
    }
}

static inline void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]) {
    uint8_t pad[64] = {0x80};
    uint64_t total_bits = bswap64(ctx->count);
    uint32_t buffer_len = (uint32_t)((ctx->count >> 3) & 63);
    uint32_t pad_len = (buffer_len < 56) ? (56 - buffer_len) : (120 - buffer_len);

    sha256_update(ctx, pad, pad_len);
    sha256_update(ctx, &total_bits, 8);

    uint32_t *out = (uint32_t *)digest;
    for (int i = 0; i < 8; i++) {
        out[i] = bswap32(ctx->state[i]);
    }
}

/* Precomputed HMAC-SHA256 Midstates */
static inline void hmac_precompute_midstate(const char *secret, size_t secret_len, hmac_midstate_t *mid) {
    uint8_t key[64] = {0};
    if (secret_len > 64) {
        sha256_ctx_t c;
        sha256_init(&c);
        sha256_update(&c, secret, secret_len);
        sha256_final(&c, key);
    } else {
        memcpy(key, secret, secret_len);
    }

    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = key[i] ^ 0x36;
        k_opad[i] = key[i] ^ 0x5c;
    }

    sha256_ctx_t c_in;
    sha256_init(&c_in);
    sha256_transform(c_in.state, k_ipad);
    memcpy(mid->inner_state, c_in.state, sizeof(mid->inner_state));

    sha256_ctx_t c_out;
    sha256_init(&c_out);
    sha256_transform(c_out.state, k_opad);
    memcpy(mid->outer_state, c_out.state, sizeof(mid->outer_state));
}

static inline void hmac_sha256_fast(const hmac_midstate_t *mid, const void *data, size_t len, uint8_t out[32]) {
    sha256_ctx_t inner;
    memcpy(inner.state, mid->inner_state, sizeof(mid->inner_state));
    inner.count = 512;

    sha256_update(&inner, data, len);
    uint8_t inner_hash[32];
    sha256_final(&inner, inner_hash);

    sha256_ctx_t outer;
    memcpy(outer.state, mid->outer_state, sizeof(mid->outer_state));
    outer.count = 512;

    sha256_update(&outer, inner_hash, 32);
    sha256_final(&outer, out);
}

static const int8_t B64URL_DEC[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static const char B64URL_ENC[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static inline int base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t max_out, size_t *out_len) {
    size_t ol = 0;
    size_t i = 0;
    while (i < in_len) {
        size_t rem = in_len - i;
        if (rem == 1) return -1;

        int8_t c0 = B64URL_DEC[(uint8_t)in[i]];
        int8_t c1 = B64URL_DEC[(uint8_t)in[i + 1]];
        if (c0 < 0 || c1 < 0) return -1;

        if (ol >= max_out) return -1;
        out[ol++] = (uint8_t)((c0 << 2) | (c1 >> 4));

        if (rem == 2) {
            i += 2;
            break;
        }

        int8_t c2 = B64URL_DEC[(uint8_t)in[i + 2]];
        if (c2 < 0) return -1;

        if (ol >= max_out) return -1;
        out[ol++] = (uint8_t)(((c1 & 0x0f) << 4) | (c2 >> 2));

        if (rem == 3) {
            i += 3;
            break;
        }

        int8_t c3 = B64URL_DEC[(uint8_t)in[i + 3]];
        if (c3 < 0) return -1;

        if (ol >= max_out) return -1;
        out[ol++] = (uint8_t)(((c2 & 0x03) << 6) | c3);
        i += 4;
    }
    *out_len = ol;
    return 0;
}

static inline void base64url_encode32(const uint8_t in[32], char out[44]) {
    size_t i = 0, j = 0;
    while (i + 3 <= 32) {
        uint32_t val = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[j++] = B64URL_ENC[(val >> 18) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 12) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 6) & 0x3f];
        out[j++] = B64URL_ENC[val & 0x3f];
        i += 3;
    }
    uint32_t val = (in[30] << 8) | in[31];
    out[j++] = B64URL_ENC[(val >> 10) & 0x3f];
    out[j++] = B64URL_ENC[(val >> 4) & 0x3f];
    out[j++] = B64URL_ENC[(val << 2) & 0x3f];
    out[43] = '\0';
}

static inline size_t base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t max_out) {
    size_t i = 0, j = 0;
    while (i + 3 <= in_len) {
        if (j + 4 >= max_out) break;
        uint32_t val = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[j++] = B64URL_ENC[(val >> 18) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 12) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 6) & 0x3f];
        out[j++] = B64URL_ENC[val & 0x3f];
        i += 3;
    }
    size_t rem = in_len - i;
    if (rem == 1 && j + 2 < max_out) {
        uint32_t val = (uint32_t)in[i] << 16;
        out[j++] = B64URL_ENC[(val >> 18) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 12) & 0x3f];
    } else if (rem == 2 && j + 3 < max_out) {
        uint32_t val = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[j++] = B64URL_ENC[(val >> 18) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 12) & 0x3f];
        out[j++] = B64URL_ENC[(val >> 6) & 0x3f];
    }
    if (j < max_out) out[j] = '\0';
    return j;
}

/* Zero-allocation helper: searches for "key": in a JSON string */
static inline const char *json_find_key(const char *json, size_t json_len, const char *key) {
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen + 3 <= json_len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, klen) == 0 && json[i + 1 + klen] == '"') {
            size_t p = i + 1 + klen + 1;
            while (p < json_len && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
            if (p < json_len && json[p] == ':') {
                p++;
                while (p < json_len && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
                return json + p;
            }
        }
    }
    return NULL;
}

static inline bool parse_i64_claim(const char *val, int64_t *out) {
    if (val[0] < '0' || val[0] > '9') return false;
    int64_t v = 0;
    while (*val >= '0' && *val <= '9') {
        v = v * 10 + (*val - '0');
        val++;
    }
    *out = v;
    return true;
}

static inline int auth_verify_jwt(
    const hmac_midstate_t *mid,
    const char *token,
    size_t token_len,
    time_t now,
    int64_t *out_user_id,
    char *out_username,
    size_t max_username_len,
    size_t *out_username_len
) {
    if (!token || token_len == 0) return AUTH_ERR_MISSING_BEARER;

    /* JWT format: header.payload.signature */
    const char *dot1 = memchr(token, '.', token_len);
    if (!dot1) return AUTH_ERR_INVALID_OR_EXPIRED;
    size_t h_len = (size_t)(dot1 - token);

    const char *payload_start = dot1 + 1;
    size_t rem = token_len - h_len - 1;
    const char *dot2 = memchr(payload_start, '.', rem);
    if (!dot2) return AUTH_ERR_INVALID_OR_EXPIRED;
    size_t p_len = (size_t)(dot2 - payload_start);

    const char *sig = dot2 + 1;
    size_t sig_len = rem - p_len - 1;
    if (sig_len != 43) return AUTH_ERR_INVALID_OR_EXPIRED;

    /* Verify HMAC-SHA256 signature */
    size_t signed_len = (size_t)(dot2 - token);
    uint8_t digest[32];
    hmac_sha256_fast(mid, token, signed_len, digest);

    char expected_sig[44];
    base64url_encode32(digest, expected_sig);

    int diff = 0;
    for (int i = 0; i < 43; i++) {
        diff |= ((uint8_t)sig[i] ^ (uint8_t)expected_sig[i]);
    }
    if (diff != 0) return AUTH_ERR_INVALID_OR_EXPIRED;

    /* Fast-path check for standard HS256 JWT headers, with full decode fallback */
    bool hdr_ok = (h_len == 36 && (
        memcmp(token, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9", 36) == 0 ||
        memcmp(token, "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9", 36) == 0
    ));
    if (!hdr_ok) {
        char hdr_buf[256];
        size_t hdr_len = 0;
        if (base64url_decode(token, h_len, (uint8_t *)hdr_buf, sizeof(hdr_buf) - 1, &hdr_len) != 0) {
            return AUTH_ERR_INVALID_OR_EXPIRED;
        }
        hdr_buf[hdr_len] = '\0';

        const char *alg_val = json_find_key(hdr_buf, hdr_len, "alg");
        if (!alg_val || alg_val[0] != '"') return AUTH_ERR_INVALID_OR_EXPIRED;
        if (strncmp(alg_val + 1, "HS256\"", 6) != 0) return AUTH_ERR_INVALID_OR_EXPIRED;
    }

    /* Decode & verify Payload */
    char pay_buf[1024];
    size_t pay_len = 0;
    if (base64url_decode(payload_start, p_len, (uint8_t *)pay_buf, sizeof(pay_buf) - 1, &pay_len) != 0) {
        return AUTH_ERR_INVALID_OR_EXPIRED;
    }
    pay_buf[pay_len] = '\0';

    /* Verify 'exp' if present */
    const char *exp_val = json_find_key(pay_buf, pay_len, "exp");
    if (exp_val) {
        int64_t exp_sec = 0;
        if (!parse_i64_claim(exp_val, &exp_sec)) return AUTH_ERR_INVALID_OR_EXPIRED;
        if ((int64_t)now >= exp_sec) return AUTH_ERR_INVALID_OR_EXPIRED;
    }

    /* Verify 'nbf' if present */
    const char *nbf_val = json_find_key(pay_buf, pay_len, "nbf");
    if (nbf_val) {
        int64_t nbf_sec = 0;
        if (!parse_i64_claim(nbf_val, &nbf_sec)) return AUTH_ERR_INVALID_OR_EXPIRED;
        if ((int64_t)now < nbf_sec) return AUTH_ERR_INVALID_OR_EXPIRED;
    }

    /* Parse 'sub': MUST be a string containing a positive integer */
    const char *sub_val = json_find_key(pay_buf, pay_len, "sub");
    if (!sub_val || sub_val[0] != '"') return AUTH_ERR_INVALID_PAYLOAD;
    sub_val++;
    if (*sub_val < '1' || *sub_val > '9') return AUTH_ERR_INVALID_PAYLOAD;

    int64_t uid = 0;
    while (*sub_val >= '0' && *sub_val <= '9') {
        uid = uid * 10 + (*sub_val - '0');
        sub_val++;
    }
    if (*sub_val != '"' || uid <= 0) return AUTH_ERR_INVALID_PAYLOAD;

    /* Parse 'username': MUST be a non-empty string */
    const char *user_val = json_find_key(pay_buf, pay_len, "username");
    if (!user_val || user_val[0] != '"') return AUTH_ERR_INVALID_PAYLOAD;
    user_val++;
    const char *user_end = strchr(user_val, '"');
    if (!user_end) return AUTH_ERR_INVALID_PAYLOAD;
    size_t ulen = (size_t)(user_end - user_val);
    if (ulen == 0 || ulen >= max_username_len) return AUTH_ERR_INVALID_PAYLOAD;

    if (out_username) {
        memcpy(out_username, user_val, ulen);
        out_username[ulen] = '\0';
    }
    if (out_username_len) *out_username_len = ulen;
    *out_user_id = uid;

    return AUTH_OK;
}

#endif /* AUTH_H */