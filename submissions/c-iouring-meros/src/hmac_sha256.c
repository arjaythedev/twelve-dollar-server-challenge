#include "hmac_sha256.h"

#include <string.h>

static void absorb_padded_key(sha256_ctx *ctx, const uint8_t key_block[SHA256_BLOCK_SIZE], uint8_t pad_byte)
{
	uint8_t padded[SHA256_BLOCK_SIZE];

	for (size_t i = 0; i < sizeof padded; i++)
		padded[i] = key_block[i] ^ pad_byte;
	sha256_init(ctx);
	sha256_update(ctx, padded, sizeof padded);
}

void hmac_sha256_key_init(hmac_sha256_key *key, const void *secret, size_t secret_len)
{
	uint8_t key_block[SHA256_BLOCK_SIZE] = { 0 };

	if (secret_len > SHA256_BLOCK_SIZE) {
		sha256_ctx hash;

		sha256_init(&hash);
		sha256_update(&hash, secret, secret_len);
		sha256_final(&hash, key_block);
	} else {
		memcpy(key_block, secret, secret_len);
	}
	absorb_padded_key(&key->inner, key_block, 0x36);
	absorb_padded_key(&key->outer, key_block, 0x5c);
}

void hmac_sha256(const hmac_sha256_key *key, const void *message, size_t len, uint8_t mac[SHA256_DIGEST_SIZE])
{
	sha256_ctx ctx = key->inner;
	uint8_t inner_digest[SHA256_DIGEST_SIZE];

	sha256_update(&ctx, message, len);
	sha256_final(&ctx, inner_digest);

	ctx = key->outer;
	sha256_update(&ctx, inner_digest, sizeof inner_digest);
	sha256_final(&ctx, mac);
}

bool bytes_equal_constant_time(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t difference = 0;

	for (size_t i = 0; i < len; i++)
		difference |= a[i] ^ b[i];
	return difference == 0;
}
