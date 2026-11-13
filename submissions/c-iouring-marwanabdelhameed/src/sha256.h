#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t fill;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

/* HMAC-SHA256 with the key schedule computed once (the keyed pad states hold no request data). */
typedef struct {
    sha256_ctx inner;
    sha256_ctx outer;
} hmac_key;

void hmac_key_init(hmac_key *k, const void *key, size_t klen);
void hmac_sha256(const hmac_key *k, const void *msg, size_t n, uint8_t out[32]);

#endif
