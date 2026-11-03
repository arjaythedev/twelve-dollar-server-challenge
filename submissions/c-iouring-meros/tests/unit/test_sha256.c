#include "check.h"
#include "hmac_sha256.h"
#include "sha256.h"

static void hex(const uint8_t *bytes, size_t len, char *out)
{
	for (size_t i = 0; i < len; i++)
		sprintf(out + 2 * i, "%02x", bytes[i]);
}

static void check_sha256(const char *input, size_t len, const char *expected_hex)
{
	sha256_ctx ctx;
	uint8_t digest[SHA256_DIGEST_SIZE];
	char text[65];

	sha256_init(&ctx);
	sha256_update(&ctx, input, len);
	sha256_final(&ctx, digest);
	hex(digest, sizeof digest, text);
	CHECK(strcmp(text, expected_hex) == 0);
}

static void check_hmac(const char *key, size_t key_len, const char *message, size_t len, const char *expected_hex)
{
	hmac_sha256_key prepared;
	uint8_t mac[SHA256_DIGEST_SIZE];
	char text[65];

	hmac_sha256_key_init(&prepared, key, key_len);
	hmac_sha256(&prepared, message, len, mac);
	hex(mac, sizeof mac, text);
	CHECK(strcmp(text, expected_hex) == 0);
}

int main(void)
{
	/* FIPS 180-4 examples */
	check_sha256("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	check_sha256("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	check_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
	             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	/* Input split across update calls must not change the digest. */
	{
		sha256_ctx ctx;
		uint8_t digest[SHA256_DIGEST_SIZE];
		char text[65];
		const char *message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

		sha256_init(&ctx);
		for (size_t i = 0; i < 56; i += 7)
			sha256_update(&ctx, message + i, 7);
		sha256_final(&ctx, digest);
		hex(digest, sizeof digest, text);
		CHECK(strcmp(text, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
	}

	/* RFC 4231 test case 2, and case 6 (a key longer than the block size) */
	check_hmac("Jefe", 4, "what do ya want for nothing?", 28,
	           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	{
		char long_key[131];

		memset(long_key, 0xaa, sizeof long_key);
		check_hmac(long_key, sizeof long_key, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
		           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
	}

	CHECK(bytes_equal_constant_time((const uint8_t *)"abc", (const uint8_t *)"abc", 3));
	CHECK(!bytes_equal_constant_time((const uint8_t *)"abc", (const uint8_t *)"abd", 3));
	TEST_MAIN_END();
}
