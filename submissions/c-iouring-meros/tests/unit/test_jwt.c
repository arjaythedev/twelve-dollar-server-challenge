#include "check.h"
#include "jwt.h"

static const char *secret = "twelve-dollar-challenge";
static const time_t now = 1800000000;

static void base64url_encode(const uint8_t *in, size_t len, char *out)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;

	for (size_t i = 0; i < len; i += 3) {
		uint32_t chunk = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);

		out[o++] = alphabet[chunk >> 18];
		out[o++] = alphabet[(chunk >> 12) & 63];
		if (i + 1 < len)
			out[o++] = alphabet[(chunk >> 6) & 63];
		if (i + 2 < len)
			out[o++] = alphabet[chunk & 63];
	}
	out[o] = '\0';
}

/* Builds "header.payload.signature" signed with `key`. */
static void make_token(const char *header_json, const char *payload_json, const char *key, char *token)
{
	char header[256], payload[512], signature[64];
	hmac_sha256_key hmac_key;
	uint8_t mac[SHA256_DIGEST_SIZE];

	base64url_encode((const uint8_t *)header_json, strlen(header_json), header);
	base64url_encode((const uint8_t *)payload_json, strlen(payload_json), payload);
	sprintf(token, "%s.%s", header, payload);
	hmac_sha256_key_init(&hmac_key, key, strlen(key));
	hmac_sha256(&hmac_key, token, strlen(token), mac);
	base64url_encode(mac, sizeof mac, signature);
	sprintf(token + strlen(token), ".%s", signature);
}

static jwt_status verify(const jwt_verifier *v, const char *token, jwt_claims *claims)
{
	return jwt_verify(v, strview_make(token, strlen(token)), now, claims);
}

static jwt_status verify_payload(const jwt_verifier *v, const char *payload)
{
	char token[1024];
	jwt_claims claims;

	make_token("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token);
	return verify(v, token, &claims);
}

int main(void)
{
	jwt_verifier v;
	jwt_claims claims;
	char token[1024];
	const char *hs256 = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

	jwt_verifier_init(&v, strview_make(secret, strlen(secret)));

	make_token(hs256, "{\"sub\":\"42\",\"username\":\"golden_ember_1\",\"iat\":1799999000,\"exp\":1800003600}", secret, token);
	CHECK(verify(&v, token, &claims) == JWT_OK);
	CHECK(claims.user_id == 42);
	CHECK(claims.username_len == 14 && memcmp(claims.username, "golden_ember_1", 14) == 0);

	/* the signature */
	make_token(hs256, "{\"sub\":\"42\",\"username\":\"u\",\"exp\":1800003600}", "another-secret", token);
	CHECK(verify(&v, token, &claims) == JWT_INVALID_TOKEN);
	make_token(hs256, "{\"sub\":\"42\",\"username\":\"u\",\"exp\":1800003600}", secret, token);
	token[strlen(token) - 3] ^= 1; /* flip a bit of the signature text */
	CHECK(verify(&v, token, &claims) == JWT_INVALID_TOKEN);

	/* the algorithm: alg=none and other algorithms are refused even with a matching HMAC */
	make_token("{\"alg\":\"none\",\"typ\":\"JWT\"}", "{\"sub\":\"1\",\"username\":\"u\"}", secret, token);
	CHECK(verify(&v, token, &claims) == JWT_INVALID_TOKEN);
	make_token("{\"alg\":\"HS384\"}", "{\"sub\":\"1\",\"username\":\"u\"}", secret, token);
	CHECK(verify(&v, token, &claims) == JWT_INVALID_TOKEN);

	/* shape */
	CHECK(verify(&v, "abc", &claims) == JWT_INVALID_TOKEN);
	CHECK(verify(&v, "a.b", &claims) == JWT_INVALID_TOKEN);
	CHECK(verify(&v, "a.b.c.d", &claims) == JWT_INVALID_TOKEN);
	CHECK(verify(&v, "..", &claims) == JWT_INVALID_TOKEN);
	CHECK(verify(&v, "", &claims) == JWT_INVALID_TOKEN);

	/* time claims */
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"exp\":1800000000}") == JWT_INVALID_TOKEN);  /* exp == now: expired */
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"exp\":1800000001}") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"exp\":null}") == JWT_INVALID_TOKEN);
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"nbf\":1800000001,\"exp\":1800000001}") == JWT_INVALID_TOKEN);  /* not valid yet */
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"nbf\":1800000000,\"exp\":1800000001}") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"exp\":\"soon\"}") == JWT_INVALID_TOKEN);
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\"}") == JWT_INVALID_TOKEN);                           /* exp is required */
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":\"u\",\"nbf\":1800000000}") == JWT_INVALID_TOKEN);  /* nbf alone is not enough */

	/* payload claims */
	CHECK(verify_payload(&v, "{\"sub\":\"abc\",\"username\":\"u\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"0\",\"username\":\"u\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"-5\",\"username\":\"u\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"01\",\"username\":\"u\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":7,\"username\":\"u\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);                      /* number, not string */
	CHECK(verify_payload(&v, "{\"username\":\"u\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"1\",\"username\":5,\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "[1,2]") == JWT_INVALID_TOKEN);

	/* the layout the challenge's tokens have is read by a fast path; every deviation must still get the general answer */
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"a_b_7\",\"iat\":1799999000,\"exp\":1800000001}") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"a_b_7\",\"iat\":1799999000,\"exp\":1800000000}") == JWT_INVALID_TOKEN);
	CHECK(verify_payload(&v, "{\"sub\":\"07\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":01,\"exp\":1800000001}") == JWT_INVALID_TOKEN);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001 }") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001,\"nbf\":1800000001}") == JWT_INVALID_TOKEN);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001.5}") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001,\"exp\":1}") == JWT_INVALID_TOKEN); /* last one counts */
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"\\u0041x\",\"iat\":1,\"exp\":1800000001}") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"\\\"\",\"iat\":1,\"exp\":1800000001}") == JWT_OK);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001}x") == JWT_INVALID_TOKEN);
	CHECK(verify_payload(&v, "{\"sub\":\"1234567890123456789\",\"username\":\"u\",\"iat\":1,\"exp\":1800000001}") == JWT_INVALID_PAYLOAD);
	CHECK(verify_payload(&v, "{\"sub\":\"7\",\"username\":\"u\",\"iat\":1,\"exp\":9999999999999999}") == JWT_OK); /* 16 digits: general reader */
	TEST_MAIN_END();
}
