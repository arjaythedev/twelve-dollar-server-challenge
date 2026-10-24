/* SHA-256, HMAC-SHA256 with precomputed pads, and base64url. Portable C only. */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK  64
#define SHA256_DIGEST 32

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[SHA256_BLOCK];
    uint32_t buf_len;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]);

/* An HMAC key with the inner and outer pads already absorbed: one verify costs
 * only the compressions over the message plus one outer block. */
struct hmac_key {
    struct sha256 inner; /* state after absorbing key ^ ipad */
    struct sha256 outer; /* state after absorbing key ^ opad */
};

void hmac_key_init(struct hmac_key *k, const void *key, size_t key_len);
void hmac_sha256(const struct hmac_key *k, const void *msg, size_t msg_len, uint8_t out[SHA256_DIGEST]);

/* Constant-time comparison; returns 1 when equal. */
int crypto_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* base64url without padding. Decoding rejects '=', '+', '/' and any other
 * non-alphabet byte and returns -1; otherwise the decoded length. The output
 * buffer needs at least (len * 3) / 4 bytes. */
int base64url_decode(const char *in, size_t len, uint8_t *out, size_t out_cap);
/* Encodes; returns the encoded length. The output needs (len * 4 + 2) / 3 bytes. */
size_t base64url_encode(const uint8_t *in, size_t len, char *out);

#endif
