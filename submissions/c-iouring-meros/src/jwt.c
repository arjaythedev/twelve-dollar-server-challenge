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

/*
 * The challenge's tokens (seed/generate.mjs) all carry this header and this payload layout. jwt_verify tries
 * a byte-exact match for both first and falls back to the general reader on any difference, so a token in
 * another shape (whitespace, escapes, extra or reordered claims) gets the same answer as before, only slower.
 */
#define KNOWN_HEADER STRVIEW("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9") /* {"alg":"HS256","typ":"JWT"} */

static bool decode_segment(strview segment, size_t decoded_cap, uint8_t *decoded, size_t *decoded_len)
{
	return base64url_decode(segment, decoded, decoded_cap, decoded_len);
}

/* Decodes a base64url segment and reads the top-level members of the JSON object inside. */
static bool read_segment(strview segment, size_t decoded_cap, json_field *fields, size_t field_count)
{
	uint8_t decoded[PAYLOAD_CAP];
	size_t decoded_len;

	if (decoded_cap > sizeof decoded || !decode_segment(segment, decoded_cap, decoded, &decoded_len))
		return false;
	return json_read_object(strview_make((const char *)decoded, decoded_len), fields, field_count) == JSON_DOC_OBJECT;
}

static bool header_says_hs256(strview header_segment)
{
	if (strview_equals(header_segment, KNOWN_HEADER))
		return true;

	char algorithm[16];
	json_field fields[] = { JSON_FIELD("alg", algorithm, sizeof algorithm) };

	return read_segment(header_segment, HEADER_CAP, fields, 1) && fields[0].kind == JSON_STRING &&
	       !fields[0].truncated && strview_equals(strview_make(algorithm, fields[0].text_len), STRVIEW("HS256"));
}

enum { MAX_TIME_DIGITS = 15 }; /* below 2^53, so the integer is exact and equals the double the general reader makes */

/* Matches `text` at *cursor and advances past it. */
static bool take_literal(const uint8_t **cursor, const uint8_t *end, const char *text, size_t len)
{
	if ((size_t)(end - *cursor) < len || memcmp(*cursor, text, len) != 0)
		return false;
	*cursor += len;
	return true;
}

/* Reads 1 to `max_digits` digits without a leading zero and advances past them. */
static bool take_integer(const uint8_t **cursor, const uint8_t *end, size_t max_digits, int64_t *value)
{
	const uint8_t *p = *cursor;
	int64_t number = 0;

	if (p == end || *p < '1' || *p > '9')
		return false;
	while (p != end && *p >= '0' && *p <= '9') {
		if ((size_t)(p - *cursor) == max_digits)
			return false;
		number = number * 10 + (*p - '0');
		p++;
	}
	*value = number;
	*cursor = p;
	return true;
}

/*
 * Reads {"sub":"<id>","username":"<name>","iat":<integer>,"exp":<integer>} byte for byte. Returns false when the
 * payload has any other shape (the caller then reads it in general), and true when it has this one, with
 * *status set to the answer the general reader would give.
 */
static bool read_known_payload(const uint8_t *payload, size_t len, time_t now, jwt_claims *claims, jwt_status *status)
{
	const uint8_t *cursor = payload, *end = payload + len;
	int64_t user_id, issued_at, expires_at;

	if (!take_literal(&cursor, end, "{\"sub\":\"", 8) || !take_integer(&cursor, end, MAX_USER_ID_DIGITS, &user_id) ||
	    !take_literal(&cursor, end, "\",\"username\":\"", 15))
		return false;

	const uint8_t *name = cursor;

	while (cursor != end && *cursor >= 0x20 && *cursor <= 0x7e && *cursor != '"' && *cursor != '\\')
		cursor++;
	size_t name_len = (size_t)(cursor - name);

	if (name_len >= JWT_USERNAME_CAP || !take_literal(&cursor, end, "\",\"iat\":", 8) ||
	    !take_integer(&cursor, end, MAX_TIME_DIGITS, &issued_at) || !take_literal(&cursor, end, ",\"exp\":", 7) ||
	    !take_integer(&cursor, end, MAX_TIME_DIGITS, &expires_at) || !take_literal(&cursor, end, "}", 1) || cursor != end)
		return false;

	if (expires_at <= (int64_t)now) {
		*status = JWT_INVALID_TOKEN;
		return true;
	}
	claims->user_id = user_id;
	memcpy(claims->username, name, name_len);
	claims->username_len = name_len;
	*status = JWT_OK;
	return true;
}

/* A claim that is a time must be a JSON number. An absent claim is accepted only when it is optional. */
static bool time_claim_ok(const json_field *field, time_t now, bool must_be_future, bool required)
{
	if (field->kind == JSON_ABSENT)
		return !required;
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
	uint8_t decoded[PAYLOAD_CAP];
	size_t decoded_len;
	jwt_status known_status;

	if (token.len > MAX_TOKEN_LEN || !split_token(token, &parts) || !signature_matches(verifier, &parts) ||
	    !header_says_hs256(parts.header))
		return JWT_INVALID_TOKEN;

	if (!decode_segment(parts.payload, sizeof decoded, decoded, &decoded_len))
		return JWT_INVALID_TOKEN;
	if (read_known_payload(decoded, decoded_len, now, claims, &known_status))
		return known_status;

	json_field payload[] = {
		JSON_FIELD("sub", subject, sizeof subject),
		JSON_FIELD("username", claims->username, sizeof claims->username),
		JSON_FIELD("exp", NULL, 0),
		JSON_FIELD("nbf", NULL, 0),
	};
	enum { SUB, USERNAME, EXP, NBF };

	if (json_read_object(strview_make((const char *)decoded, decoded_len), payload, 4) != JSON_DOC_OBJECT ||
	    !time_claim_ok(&payload[EXP], now, true, true) || !time_claim_ok(&payload[NBF], now, false, false))
		return JWT_INVALID_TOKEN;

	if (payload[SUB].kind != JSON_STRING || payload[SUB].truncated ||
	    !parse_user_id(strview_make(subject, payload[SUB].text_len), &claims->user_id))
		return JWT_INVALID_PAYLOAD;
	if (payload[USERNAME].kind != JSON_STRING || payload[USERNAME].truncated)
		return JWT_INVALID_PAYLOAD;
	claims->username_len = payload[USERNAME].text_len;
	return JWT_OK;
}
