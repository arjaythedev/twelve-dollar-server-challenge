#pragma once
// Minimal HS256 JWT verification. No heap use; caller provides scratch.
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t uid; const char *uname; size_t ulen; } auth_t;

enum { JWT_OK = 0, JWT_BAD_TOKEN = 1, JWT_BAD_PAYLOAD = 2 };

// tok: "h.p.s" (no "Bearer " prefix), tlen bytes. secret: HMAC key.
// now_s: current unix time (seconds). scratch: >= 2048 bytes workspace;
// out->uname borrows from scratch (valid until next jwt_verify call).
// Returns JWT_OK, JWT_BAD_TOKEN (401 "invalid or expired token"), or
// JWT_BAD_PAYLOAD (401 "invalid token payload").
int jwt_verify(const char *tok, size_t tlen,
               const uint8_t *secret, size_t slen,
               uint64_t now_s, auth_t *out, char *scratch, size_t scratch_cap);

// base64url (no padding). Encode for tests; decode used by verify.
// Decode returns bytes written, or -1 on invalid input.
int b64url_dec(const char *in, size_t n, uint8_t *out, size_t cap);
size_t b64url_enc(const uint8_t *in, size_t n, char *out);

// Raw SHA-256 + HMAC (exposed for unit tests).
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t buflen; } sha256_t;
void sha256_init(sha256_t *c);
void sha256_update(sha256_t *c, const uint8_t *p, size_t n);
void sha256_final(sha256_t *c, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]);
