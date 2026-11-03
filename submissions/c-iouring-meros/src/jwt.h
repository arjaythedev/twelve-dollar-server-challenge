/* JWT verification: HS256 only, signature checked on every call (no token cache). */
#ifndef JWT_H
#define JWT_H

#include <stdint.h>
#include <time.h>

#include "hmac_sha256.h"
#include "strview.h"

enum { JWT_USERNAME_CAP = 256 };

typedef enum jwt_status {
	JWT_OK,
	JWT_INVALID_TOKEN,   /* malformed, wrong algorithm, bad signature, expired or not yet valid */
	JWT_INVALID_PAYLOAD, /* signed correctly, but "sub" or "username" is unusable */
} jwt_status;

typedef struct jwt_claims {
	int64_t user_id;
	char username[JWT_USERNAME_CAP];
	size_t username_len;
} jwt_claims;

typedef struct jwt_verifier {
	hmac_sha256_key key;
} jwt_verifier;

void jwt_verifier_init(jwt_verifier *verifier, strview secret);
jwt_status jwt_verify(const jwt_verifier *verifier, strview token, time_t now, jwt_claims *claims);

#endif
