#ifndef JWT_H
#define JWT_H
#include <stddef.h>
#include <stdint.h>

enum jwt_result {
    JWT_OK = 0,
    JWT_MISSING,   /* no Authorization header, or it does not start with "Bearer " */
    JWT_INVALID,   /* bad signature, expired, alg != HS256, malformed */
    JWT_PAYLOAD    /* valid signature, but sub is not a positive integer string or username is not a string */
};

struct jwt_claims {
    int64_t user_id;
    char username[256];  /* JSON-decoded UTF-8, no NUL terminator required */
    int username_len;
};

/* auth/auth_len is the raw Authorization header value, e.g. "Bearer eyJ...". */
enum jwt_result jwt_verify(const char *auth, size_t auth_len,
                           const uint8_t *secret, size_t secret_len,
                           int64_t now_seconds, struct jwt_claims *out);

#endif
