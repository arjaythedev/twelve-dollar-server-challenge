#include "jwt.h"

#include "base64url.h"
#include "json_read.h"

enum {
	MAX_TOKEN_LEN = 2048,
	SIGNATURE_BASE64_LEN = 43, /* 32 bytes, unpadded */
	MAX_USER_ID_DIGITS = 18,   /* always fits in int64_t */
	HEADER_CAP = 256,
	PAYLOAD_CAP = 1536,
};

typedef struct token_parts {
	strview signed_part; /* "header.payload", the bytes the signature covers */
	strview header;
	strview payload;
	strview signature;
} token_parts;

/* Splits "a.b.c" into exactly three non-empty parts. */
static bool split_token(strview token, token_parts *parts)
{
	const char *first_dot = memchr(token.ptr, '.', token.len);
	const char *end = token.ptr + token.len;

	if (first_dot == NULL)
		return false;
	const char *second_dot = memchr(first_dot + 1, '.', (size_t)(end - first_dot - 1));

	if (second_dot == NULL || memchr(second_dot + 1, '.', (size_t)(end - second_dot - 1)) != NULL)
		return false;

	parts->signed_part = strview_make(token.ptr, (size_t)(second_dot - token.ptr));
	parts->header = strview_make(token.ptr, (size_t)(first_dot - token.ptr));
	parts->payload = strview_make(first_dot + 1, (size_t)(second_dot - first_dot - 1));
	parts->signature = strview_make(second_dot + 1, (size_t)(end - second_dot - 1));
	return parts->header.len != 0 && parts->payload.len != 0 && parts->signature.len == SIGNATURE_BASE64_LEN;
}

static bool signature_matches(const jwt_verifier *verifier, const token_parts *parts)
{
	uint8_t claimed[SHA256_DIGEST_SIZE + 2], expected[SHA256_DIGEST_SIZE];
	size_t claimed_len;

	if (!base64url_decode(parts->signature, claimed, sizeof claimed, &claimed_len) || claimed_len != SHA256_DIGEST_SIZE)
		return false;
	hmac_sha256(&verifier->key, parts->signed_part.ptr, parts->signed_part.len, expected);
	return bytes_equal_constant_time(claimed, expected, SHA256_DIGEST_SIZE);
}

/* Decodes a base64url segment and reads the top-level members of the JSON object inside. */
static bool read_segment(strview segment, size_t decoded_cap, json_field *fields, size_t field_count)
{
	uint8_t decoded[PAYLOAD_CAP];
	size_t decoded_len;

	if (decoded_cap > sizeof decoded || !base64url_decode(segment, decoded, decoded_cap, &decoded_len))
		return false;
	return json_read_object(strview_make((const char *)decoded, decoded_len), fields, field_count) == JSON_DOC_OBJECT;
}

static bool header_says_hs256(strview header_segment)
{
	char algorithm[16];
	json_field fields[] = { JSON_FIELD("alg", algorithm, sizeof algorithm) };

	return read_segment(header_segment, HEADER_CAP, fields, 1) && fields[0].kind == JSON_STRING &&
	       !fields[0].truncated && strview_equals(strview_make(algorithm, fields[0].text_len), STRVIEW("HS256"));
}

/* A claim that is a time must be a JSON number when present. */
static bool time_claim_ok(const json_field *field, time_t now, bool must_be_future)
{
	if (field->kind == JSON_ABSENT)
		return true;
	if (field->kind != JSON_NUMBER)
		return false;
	return must_be_future ? field->number > (double)now : field->number <= (double)now;
}

/* "sub" is a decimal string for a positive integer without leading zeros. */
static bool parse_user_id(strview digits, int64_t *user_id)
{
	int64_t value = 0;

	if (digits.len == 0 || digits.len > MAX_USER_ID_DIGITS || digits.ptr[0] == '0')
		return false;
	for (size_t i = 0; i < digits.len; i++) {
		if (digits.ptr[i] < '0' || digits.ptr[i] > '9')
			return false;
		value = value * 10 + (digits.ptr[i] - '0');
	}
	*user_id = value;
	return true;
}

void jwt_verifier_init(jwt_verifier *verifier, strview secret)
{
	hmac_sha256_key_init(&verifier->key, secret.ptr, secret.len);
}

jwt_status jwt_verify(const jwt_verifier *verifier, strview token, time_t now, jwt_claims *claims)
{
	token_parts parts;
	char subject[MAX_USER_ID_DIGITS + 2];

	if (token.len > MAX_TOKEN_LEN || !split_token(token, &parts) || !signature_matches(verifier, &parts) ||
	    !header_says_hs256(parts.header))
		return JWT_INVALID_TOKEN;

	json_field payload[] = {
		JSON_FIELD("sub", subject, sizeof subject),
		JSON_FIELD("username", claims->username, sizeof claims->username),
		JSON_FIELD("exp", NULL, 0),
		JSON_FIELD("nbf", NULL, 0),
	};
	enum { SUB, USERNAME, EXP, NBF };

	if (!read_segment(parts.payload, PAYLOAD_CAP, payload, 4) ||
	    !time_claim_ok(&payload[EXP], now, true) || !time_claim_ok(&payload[NBF], now, false))
		return JWT_INVALID_TOKEN;

	if (payload[SUB].kind != JSON_STRING || payload[SUB].truncated ||
	    !parse_user_id(strview_make(subject, payload[SUB].text_len), &claims->user_id))
		return JWT_INVALID_PAYLOAD;
	if (payload[USERNAME].kind != JSON_STRING || payload[USERNAME].truncated)
		return JWT_INVALID_PAYLOAD;
	claims->username_len = payload[USERNAME].text_len;
	return JWT_OK;
}
