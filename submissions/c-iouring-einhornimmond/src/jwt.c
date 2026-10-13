#include "jwt.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sodium.h>

#include "arnm/arena.h"
#include "arnm/json_reader.h"

#include "parse_id.h"

#define MAX_SEGMENT 1024
#define JSON_ARENA 8192 /* a multiple of 8, as arnm_init_arena_borrow wants */
#define HMAC_LEN 32

/* The state after the key has been absorbed. It depends on the secret alone, so it is derived
 * once and copied per token: what is left per request is the hash of the token itself. */
static crypto_auth_hmacsha256_state key_state;

void jwt_init(const char *secret)
{
    if (sodium_init() < 0)
        abort();
    crypto_auth_hmacsha256_init(&key_state, (const unsigned char *)secret, strlen(secret));
}

static int b64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size)
{
    size_t written = 0;

    if (sodium_base642bin(out, out_size, in, in_len, NULL, &written, NULL,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
        return -1;
    return (int)written;
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

jwt_result jwt_verify(const char *auth, size_t auth_len, int64_t *uid, char *name, size_t *name_len)
{
    if (auth == NULL || auth_len < 7 || memcmp(auth, "Bearer ", 7) != 0)
        return JWT_MISSING;
    const char *token = auth + 7;
    const size_t token_len = auth_len - 7;

    const char *dot1 = memchr(token, '.', token_len);
    if (dot1 == NULL)
        return JWT_INVALID;
    const char *dot2 = memchr(dot1 + 1, '.', token_len - (size_t)(dot1 + 1 - token));
    if (dot2 == NULL)
        return JWT_INVALID;
    const size_t signed_len = (size_t)(dot2 - token);
    const char *sig = dot2 + 1;
    const size_t sig_len = token_len - signed_len - 1;

    /* Signature first: nothing below is read before it has been vouched for. */
    uint8_t expected[HMAC_LEN], actual[HMAC_LEN];
    crypto_auth_hmacsha256_state state = key_state;
    crypto_auth_hmacsha256_update(&state, (const unsigned char *)token, signed_len);
    crypto_auth_hmacsha256_final(&state, expected);
    if (b64url_decode(sig, sig_len, actual, sizeof actual) != HMAC_LEN ||
        sodium_memcmp(actual, expected, HMAC_LEN) != 0)
        return JWT_INVALID;

    /* The insitu parse writes a few bytes past the document; the buffers carry that slack. */
    char header[MAX_SEGMENT + ARNM_JSON_READER_INSITU_PADDING];
    char payload[MAX_SEGMENT + ARNM_JSON_READER_INSITU_PADDING];
    int header_len = b64url_decode(token, (size_t)(dot1 - token), (uint8_t *)header, MAX_SEGMENT);
    int payload_len =
        b64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1), (uint8_t *)payload, MAX_SEGMENT);
    if (header_len <= 0 || payload_len <= 0)
        return JWT_INVALID;

    _Alignas(8) uint8_t scratch[JSON_ARENA];
    arnm allocator = {0};
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    uint64_t found = 0;

    if (arnm_init_arena_borrow(&allocator, scratch, sizeof scratch) != ARNM_SUCCESS ||
        arnm_json_reader_init(&reader, &allocator) != ARNM_SUCCESS)
        return JWT_INVALID;

    arnm_memory_block alg = {0};
    arnm_json_field header_fields[1] = {ARNM_JSON_FIELD_STRING("alg", &alg)};
    if (arnm_json_reader_parse_insitu(&reader, header, (uint32_t)header_len, sizeof header, false,
                                      &root) != ARNM_SUCCESS)
        return JWT_INVALID;
    arnm_json_read_object(root, header_fields, 1, &found);
    if ((found & 1) != 1 || alg.size != 5 || memcmp(alg.data, "HS256", 5) != 0)
        return JWT_INVALID;

    /* The header's document goes back before the arena is handed out again. */
    arnm_json_reader_release(&reader);
    arnm_reset(&allocator);
    if (arnm_json_reader_parse_insitu(&reader, payload, (uint32_t)payload_len, sizeof payload,
                                      false, &root) != ARNM_SUCCESS)
        return JWT_INVALID;

    /* The table is in the order the tokens carry their members (sub, username, iat, exp), so
     * the walk meets each entry at the first key it compares. Any other order, or members in
     * between, is read as well; it just costs more comparisons. */
    arnm_memory_block sub = {0}, username = {0};
    double exp = 0.0;
    arnm_json_field fields[] = {
      ARNM_JSON_FIELD_STRING("sub", &sub),
      ARNM_JSON_FIELD_STRING("username", &username),
      ARNM_JSON_FIELD_DOUBLE("exp", &exp)
    };
    
    found = 0;
    arnm_json_read_object(root, fields, (uint32_t)(sizeof(fields) / sizeof(fields[0])), &found);
    if (exp <= now_seconds()) return JWT_INVALID;
    if ((found & 3) != 3 || sub.size == 0 || username.size > JWT_NAME_MAX) {
      return JWT_BAD_PAYLOAD;
    }

    const int64_t id = parse_id((const char *)sub.data, sub.size);
    if (id <= 0)
        return JWT_BAD_PAYLOAD;

    *uid = id;
    memcpy(name, username.data, username.size);
    *name_len = username.size;
    return JWT_OK;
}
