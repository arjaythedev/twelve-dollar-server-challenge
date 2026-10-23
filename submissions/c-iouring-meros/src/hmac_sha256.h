/* HMAC-SHA256 (RFC 2104) with the key schedule computed once. */
#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/* The hash states after absorbing the key XOR ipad and the key XOR opad. */
typedef struct hmac_sha256_key {
	sha256_ctx inner;
	sha256_ctx outer;
} hmac_sha256_key;

void hmac_sha256_key_init(hmac_sha256_key *key, const void *secret, size_t secret_len);
void hmac_sha256(const hmac_sha256_key *key, const void *message, size_t len, uint8_t mac[SHA256_DIGEST_SIZE]);

/* Compares in time that does not depend on where the first difference is. */
bool bytes_equal_constant_time(const uint8_t *a, const uint8_t *b, size_t len);

#endif
