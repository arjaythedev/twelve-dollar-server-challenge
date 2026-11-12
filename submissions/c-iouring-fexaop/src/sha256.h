#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

/* SHA-256 and HMAC-SHA256 (RFC 6234 / RFC 2104). */
void sha256(const void *data, size_t len, uint8_t out[32]);
void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[32]);

/* Strict base64url decoder (RFC 4648 §5, no padding). Returns the decoded length, or -1 if the
 * input has an invalid character, a length ≡ 1 (mod 4), or non-zero trailing bits (so only the
 * canonical spelling of a byte string decodes). */
int b64url_decode(const char *s, size_t n, uint8_t *out, size_t cap);

#endif
