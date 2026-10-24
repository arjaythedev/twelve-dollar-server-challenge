/* HS256 verification over libsodium, with arnm reading the JSON -- the arrangement gradido-unicorn's
 * service-core uses, cut down to the claims this API reads. */
#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdint.h>

typedef enum jwt_result {
    JWT_OK = 0,
    JWT_MISSING = 1,     /* no Authorization header, or not "Bearer " */
    JWT_INVALID = 2,     /* bad signature, expired, not HS256, malformed */
    JWT_BAD_PAYLOAD = 3  /* signed and current, but sub or username is not usable */
} jwt_result;

#define JWT_NAME_MAX 1024

/** Derives the HMAC state from the secret once. */
void jwt_init(const char *secret);

/**
 * Verifies the Authorization header value @p auth. The signature of every token is computed
 * and compared on every call; nothing is remembered between calls.
 *
 * @p name receives the `username` claim (not NUL-terminated), at most JWT_NAME_MAX bytes.
 */
jwt_result jwt_verify(const char *auth, size_t auth_len, int64_t *uid, char *name, size_t *name_len);

#endif
