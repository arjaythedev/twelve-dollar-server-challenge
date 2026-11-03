/* HS256 JWT verification. Every call recomputes the signature (rule 5). */
#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

#define JWT_USERNAME_MAX 1024

enum jwt_status {
    JWT_OK = 0,
    JWT_INVALID, /* malformed, bad signature, not HS256, expired, or no exp */
    JWT_PAYLOAD, /* valid signature but sub/username are not usable */
};

struct jwt_claims {
    int64_t sub;                     /* positive integer from the "sub" string */
    char username[JWT_USERNAME_MAX]; /* unescaped UTF-8, not NUL-terminated */
    size_t username_len;
};

/* token: the bytes after "Bearer ". now: current unix time in seconds. */
enum jwt_status jwt_verify(const struct hmac_key *key, const char *token, size_t len, int64_t now,
                           struct jwt_claims *out);

/* Signs {"alg":"HS256","typ":"JWT"} + payload; used by the tools and tests.
 * Returns the token length; `out` needs about (payload_len * 4 / 3) + 100 bytes. */
size_t jwt_sign(const struct hmac_key *key, const char *payload, size_t payload_len, char *out, size_t cap);

#endif
