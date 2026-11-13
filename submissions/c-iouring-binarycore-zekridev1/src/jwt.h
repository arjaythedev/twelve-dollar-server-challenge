#ifndef BINARYCORE_JWT_H
#define BINARYCORE_JWT_H

#include "json.h"
#include "sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* HS256 bearer-token verification. Every token is fully verified on every
 * request; only the secret's HMAC key schedule is precomputed. Modeled on
 * c-iouring-pavlyhalim/src/jwt.h. */

enum { bc_jwt_ok = 0, bc_jwt_invalid = 1,   /* "invalid or expired token" */
       bc_jwt_payload = 2 };                 /* "invalid token payload" */

#define bc_jwt_max_name 255

struct bc_jwt_claims {
    uint64_t sub;
    unsigned username_len;
    uint8_t username[bc_jwt_max_name + 1];
};

/* base64url decode table ('-' -> 62, '_' -> 63, standard alphabet shifted). */
static const int8_t bc_b64url_tab[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,
    -1,-1,-1,-1,-1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
    25,-1,-1,-1,-1,-1,63,-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/* base64url decode (padding optional). Returns decoded length or -1. */
static long bc_b64url_decode(const char *s, size_t n, uint8_t *out, size_t cap) {
    while (n && s[n - 1] == '=') n--;
    if (n % 4 == 1) return -1;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = bc_b64url_tab[(unsigned char)s[i]];
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)o;
}

/* Constant-time equality comparison. */
static inline int bc_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* Verify a HS256 JWT. `now` is current epoch seconds. Returns bc_jwt_ok on
 * success, bc_jwt_invalid on signature/algorithm/expiry failure,
 * bc_jwt_payload on bad claims. Signature is checked before payload parsing
 * so a forged token never touches the JSON parser. */
static int bc_jwt_verify(const struct bc_hmac_key *key, const char *tok,
                         size_t n, int64_t now, struct bc_jwt_claims *c) {
    if (n == 0 || n > 8192) return bc_jwt_invalid;
    const char *d1 = memchr(tok, '.', n);
    if (!d1) return bc_jwt_invalid;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(tok + n - d1 - 1));
    if (!d2) return bc_jwt_invalid;
    if (memchr(d2 + 1, '.', (size_t)(tok + n - d2 - 1))) return bc_jwt_invalid;

    /* Signature first: 32 bytes, constant-time compare. */
    uint8_t sig[32];
    if (bc_b64url_decode(d2 + 1, (size_t)(tok + n - d2 - 1), sig, sizeof sig) != 32)
        return bc_jwt_invalid;
    uint8_t mac[32];
    bc_hmac_sha256(key, (const uint8_t *)tok, (size_t)(d2 - tok), mac);
    if (!bc_ct_equal(sig, mac, 32)) return bc_jwt_invalid;

    /* Header: alg must be HS256. */
    uint8_t hdr[512];
    long hl = bc_b64url_decode(tok, (size_t)(d1 - tok), hdr, sizeof hdr);
    if (hl < 0) return bc_jwt_invalid;
    struct bc_jwant hw[1] = {{"alg", 3, {0}}};
    if (bc_json_doc((const char *)hdr, (size_t)hl, hw, 1) != bc_j_object)
        return bc_jwt_invalid;
    if (hw[0].v.type != bc_j_string) return bc_jwt_invalid;
    uint8_t alg[16];
    long al = bc_json_unescape(hw[0].v.p, hw[0].v.n, alg, sizeof alg);
    if (al != 5 || memcmp(alg, "HS256", 5) != 0) return bc_jwt_invalid;

    /* Payload: exp required and in the future. */
    uint8_t pl[4096];
    long pn = bc_b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), pl, sizeof pl);
    if (pn < 0) return bc_jwt_invalid;
    struct bc_jwant pw[3] = {{"sub", 3, {0}}, {"username", 8, {0}}, {"exp", 3, {0}}};
    if (bc_json_doc((const char *)pl, (size_t)pn, pw, 3) != bc_j_object)
        return bc_jwt_invalid;
    if (pw[2].v.type != bc_j_number) return bc_jwt_invalid;
    char num[64];
    if (pw[2].v.n >= sizeof num) return bc_jwt_invalid;
    memcpy(num, pw[2].v.p, pw[2].v.n);
    num[pw[2].v.n] = 0;
    double exp = strtod(num, NULL);
    if (!(exp > (double)now)) return bc_jwt_invalid;

    /* Claims: sub must be a positive integer string, username a string. */
    if (pw[0].v.type != bc_j_string || pw[1].v.type != bc_j_string)
        return bc_jwt_payload;
    if (pw[0].v.n == 0 || pw[0].v.n > 18) return bc_jwt_payload;
    uint64_t sub = 0;
    for (size_t i = 0; i < pw[0].v.n; i++) {
        char ch = pw[0].v.p[i];
        if (ch < '0' || ch > '9') return bc_jwt_payload;
        sub = sub * 10 + (uint64_t)(ch - '0');
    }
    if (sub == 0) return bc_jwt_payload;
    long ul = bc_json_unescape(pw[1].v.p, pw[1].v.n, c->username, bc_jwt_max_name);
    if (ul < 0) return bc_jwt_payload;
    c->sub = sub;
    c->username_len = (unsigned)ul;
    return bc_jwt_ok;
}

#endif /* BINARYCORE_JWT_H */
