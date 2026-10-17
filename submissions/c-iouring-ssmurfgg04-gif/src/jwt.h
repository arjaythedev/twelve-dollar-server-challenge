/* jwt.h - HS256 verification with a key fixed at startup */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t uid;
    char name[1024]; /* decoded username */
    size_t name_len;
} auth_t;

void jwt_init(const char *secret, size_t secret_len);

/* Verifies Authorization: Bearer <token>.
 * Returns 0 on success, 1 missing/malformed header, 2 invalid or expired
 * token, 3 valid signature but bad payload. */
int check_auth(const char *h, size_t hn, double now, auth_t *a);

/* exposed for the PGO trainer */
void hmac_sha256_str(const char *msg, size_t mlen, uint8_t out[32]);
void jwt_b64url_init(void);
