#include "sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void expand_schedule(uint32_t w[64], const uint8_t block[SHA256_BLOCK_SIZE])
{
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
		       (uint32_t)block[4 * i + 2] << 8 | (uint32_t)block[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
}

/* One round. The caller rotates the roles of a..h, so no value is moved between registers. */
#define ROUND(a, b, c, d, e, f, g, h, i)                                                                   \
	do {                                                                                               \
		uint32_t t1 = (h) + (rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25)) +     \
		              ((g) ^ ((e) & ((f) ^ (g)))) + round_constants[i] + w[i];                     \
		uint32_t t2 = (rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22)) +           \
		              (((a) & (b)) | ((c) & ((a) | (b))));                                         \
		(d) += t1;                                                                                 \
		(h) = t1 + t2;                                                                             \
	} while (0)

static void compress_block(uint32_t state[8], const uint8_t block[SHA256_BLOCK_SIZE])
{
	uint32_t w[64];
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

	expand_schedule(w, block);
	for (int i = 0; i < 64; i += 8) {
		ROUND(a, b, c, d, e, f, g, h, i);
		ROUND(h, a, b, c, d, e, f, g, i + 1);
		ROUND(g, h, a, b, c, d, e, f, i + 2);
		ROUND(f, g, h, a, b, c, d, e, i + 3);
		ROUND(e, f, g, h, a, b, c, d, i + 4);
		ROUND(d, e, f, g, h, a, b, c, i + 5);
		ROUND(c, d, e, f, g, h, a, b, i + 6);
		ROUND(b, c, d, e, f, g, h, a, i + 7);
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
	static const uint32_t initial_state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(ctx->state, initial_state, sizeof initial_state);
	ctx->total_bytes = 0;
	ctx->pending_len = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *bytes = data;

	ctx->total_bytes += len;
	if (ctx->pending_len != 0) {
		size_t take = SHA256_BLOCK_SIZE - ctx->pending_len;

		if (take > len)
			take = len;
		memcpy(ctx->pending + ctx->pending_len, bytes, take);
		ctx->pending_len += take;
		bytes += take;
		len -= take;
		if (ctx->pending_len < SHA256_BLOCK_SIZE)
			return;
		compress_block(ctx->state, ctx->pending);
		ctx->pending_len = 0;
	}
	for (; len >= SHA256_BLOCK_SIZE; bytes += SHA256_BLOCK_SIZE, len -= SHA256_BLOCK_SIZE)
		compress_block(ctx->state, bytes);
	if (len != 0) {
		memcpy(ctx->pending, bytes, len);
		ctx->pending_len = len;
	}
}

void sha256_final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
	/* Padding: 0x80, zeros up to 56 mod 64, then the message length in bits as a 64-bit big-endian number. */
	uint64_t bit_count = ctx->total_bytes * 8;
	size_t zeros = (ctx->pending_len < 56) ? 56 - ctx->pending_len : 120 - ctx->pending_len;
	uint8_t padding[SHA256_BLOCK_SIZE + 8] = { 0x80 };

	for (int i = 0; i < 8; i++)
		padding[zeros + i] = (uint8_t)(bit_count >> (56 - 8 * i));
	sha256_update(ctx, padding, zeros + 8);

	for (int i = 0; i < 8; i++) {
		digest[4 * i] = (uint8_t)(ctx->state[i] >> 24);
		digest[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[4 * i + 3] = (uint8_t)ctx->state[i];
	}
}
