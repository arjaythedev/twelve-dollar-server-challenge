#include "jwt.h"

#include <string.h>

#include "json.h"

#define JWT_PART_MAX 8192

/* Parses a decimal integer string that is "a positive integer": digits only,
 * value >= 1, at most 18 digits (ids beyond 2^53 are out of scope). */
static int parse_positive(const char *s, size_t len, int64_t *out) {
    if (len == 0 || len > 18) return -1;
    int64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v < 1) return -1;
    *out = v;
    return 0;
}

/* Parses a JSON number (as text) into whole seconds; fractions are truncated
 * and exponents rejected. Returns -1 when it is not a plain number. */
static int parse_seconds(const char *s, size_t len, int64_t *out) {
    size_t i = 0;
    int neg = 0;
    if (len && s[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i >= len) return -1;
    int64_t v = 0;
    size_t digits = 0;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, digits++) {
        if (digits >= 18) return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (digits == 0) return -1;
    if (i < len && s[i] == '.') {
        for (i++; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        }
    }
    if (i != len) return -1; /* exponent or junk */
    *out = neg ? -v : v;
    return 0;
}

enum jwt_status jwt_verify(const struct hmac_key *key, const char *token, size_t len, int64_t now,
                           struct jwt_claims *out) {
    const char *dot1 = memchr(token, '.', len);
    if (!dot1) return JWT_INVALID;
    const char *dot2 = memchr(dot1 + 1, '.', (size_t)(token + len - dot1 - 1));
    if (!dot2) return JWT_INVALID;
    size_t hlen = (size_t)(dot1 - token);
    size_t plen = (size_t)(dot2 - dot1 - 1);
    size_t slen = (size_t)(token + len - dot2 - 1);
    if (hlen == 0 || plen == 0 || slen == 0 || hlen > JWT_PART_MAX || plen > JWT_PART_MAX) return JWT_INVALID;
    if (memchr(dot2 + 1, '.', slen)) return JWT_INVALID; /* a fourth part */

    /* Signature first: a token that does not verify tells us nothing else. */
    uint8_t sig[SHA256_DIGEST + 3];
    int n = base64url_decode(dot2 + 1, slen, sig, sizeof sig);
    if (n != SHA256_DIGEST) return JWT_INVALID;
    uint8_t mac[SHA256_DIGEST];
    hmac_sha256(key, token, (size_t)(dot2 - token), mac);
    if (!crypto_equal(mac, sig, SHA256_DIGEST)) return JWT_INVALID;

    /* Header: must declare HS256. (The signature already matched, so an
     * "alg":"none" header with a valid HMAC is the only way to get here with
     * something other than HS256; reject it anyway, as the spec says.) */
    uint8_t hdr[JWT_PART_MAX];
    n = base64url_decode(token, hlen, hdr, sizeof hdr);
    if (n < 0) return JWT_INVALID;
    char alg[16];
    struct json_field hf[] = {{.name = "alg", .buf = alg, .cap = sizeof alg}};
    if (json_parse_object((const char *)hdr, (size_t)n, hf, 1) != JSON_OK) return JWT_INVALID;
    if (hf[0].kind != JSON_STRING || hf[0].len != 5 || memcmp(alg, "HS256", 5) != 0) return JWT_INVALID;

    /* Payload */
    uint8_t pl[JWT_PART_MAX];
    n = base64url_decode(dot1 + 1, plen, pl, sizeof pl);
    if (n < 0) return JWT_INVALID;
    char sub[32], exp[32];
    struct json_field pf[] = {
        {.name = "sub", .buf = sub, .cap = sizeof sub},
        {.name = "exp", .buf = exp, .cap = sizeof exp},
        {.name = "username", .buf = out->username, .cap = sizeof out->username},
    };
    if (json_parse_object((const char *)pl, (size_t)n, pf, 3) != JSON_OK) return JWT_INVALID;
    int64_t exp_s;
    if (pf[1].kind != JSON_NUMBER || pf[1].truncated || parse_seconds(exp, pf[1].len, &exp_s)) return JWT_INVALID;
    if (exp_s <= now) return JWT_INVALID;
    if (pf[0].kind != JSON_STRING || pf[0].truncated || parse_positive(sub, pf[0].len, &out->sub)) return JWT_PAYLOAD;
    if (pf[2].kind != JSON_STRING || pf[2].truncated) return JWT_PAYLOAD;
    out->username_len = pf[2].len;
    return JWT_OK;
}

size_t jwt_sign(const struct hmac_key *key, const char *payload, size_t payload_len, char *out, size_t cap) {
    static const char header[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"; /* {"alg":"HS256","typ":"JWT"} */
    size_t need = sizeof header - 1 + 1 + (payload_len * 4 + 2) / 3 + 1 + 43;
    if (cap < need) return 0;
    size_t o = sizeof header - 1;
    memcpy(out, header, o);
    out[o++] = '.';
    o += base64url_encode((const uint8_t *)payload, payload_len, out + o);
    uint8_t mac[SHA256_DIGEST];
    hmac_sha256(key, out, o, mac);
    out[o++] = '.';
    o += base64url_encode(mac, SHA256_DIGEST, out + o);
    return o;
}
