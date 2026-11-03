/* SHA-256 (FIPS 180-4), portable C. */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

enum { SHA256_BLOCK_SIZE = 64, SHA256_DIGEST_SIZE = 32 };

typedef struct sha256_ctx {
	uint32_t state[8];
	uint64_t total_bytes;
	uint8_t pending[SHA256_BLOCK_SIZE];
	size_t pending_len;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif
