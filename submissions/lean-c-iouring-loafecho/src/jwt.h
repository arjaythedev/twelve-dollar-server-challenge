/* jwt.h — HS256 JWT verification for "Authorization: Bearer <jwt>".
 * Every token is verified in full on every request (no caching):
 * base64url header -> alg must be "HS256"; HMAC-SHA256 over
 * "<header>.<payload>" compared in constant time with the decoded
 * signature; payload exp must be a number > now; then sub must be a
 * positive-integer string and username a string.
 * Single translation unit helper: everything is static. */
#ifndef TWELVE_JWT_H
#define TWELVE_JWT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "json.h"

enum { JWT_OK = 0, JWT_MISSING, JWT_INVALID, JWT_BAD_PAYLOAD };

#define JWT_MAX_TOKEN 4096
#define JWT_MAX_USERNAME 512

static signed char b64url_tab[256];

static void jwt_init_tables(void) {
    memset(b64url_tab, -1, sizeof b64url_tab);
    const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; i++) b64url_tab[(unsigned char)a[i]] = (signed char)i;
}

/* Unpadded base64url. Returns decoded length or -1. */
static long b64url_decode(const char *s, size_t n, uint8_t *out, size_t cap) {
    if (n % 4 == 1) return -1;
    size_t need = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
    if (need > cap) return -1;
    size_t o = 0, i = 0;
    for (; i + 4 <= n; i += 4) {
        int a = b64url_tab[(unsigned char)s[i]], b = b64url_tab[(unsigned char)s[i+1]];
        int c = b64url_tab[(unsigned char)s[i+2]], d = b64url_tab[(unsigned char)s[i+3]];
        if ((a | b | c | d) < 0) return -1;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        out[o++] = (uint8_t)(v >> 16); out[o++] = (uint8_t)(v >> 8); out[o++] = (uint8_t)v;
    }
    size_t r = n - i;
    if (r) {
        int a = b64url_tab[(unsigned char)s[i]], b = b64url_tab[(unsigned char)s[i+1]];
        int c = r == 3 ? b64url_tab[(unsigned char)s[i+2]] : 0;
        if ((a | b | c) < 0) return -1;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6;
        out[o++] = (uint8_t)(v >> 16);
        if (r == 3) out[o++] = (uint8_t)(v >> 8);
    }
    return (long)o;
}

/* auth/alen: the Authorization header value (may be NULL if absent).
 * now: unix seconds. On JWT_OK fills *uid and the decoded username. */
static int jwt_verify(const hmac_key *key, const char *auth, size_t alen, double now,
                      int64_t *uid, char *uname, size_t ucap, size_t *ulen) {
    if (!auth || alen < 7 || memcmp(auth, "Bearer ", 7) != 0) return JWT_MISSING;
    const char *t = auth + 7;
    size_t tn = alen - 7;
    if (tn == 0 || tn > JWT_MAX_TOKEN) return JWT_INVALID;
    const char *d1 = memchr(t, '.', tn);
    if (!d1) return JWT_INVALID;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(t + tn - d1 - 1));
    if (!d2) return JWT_INVALID;
    if (memchr(d2 + 1, '.', (size_t)(t + tn - d2 - 1))) return JWT_INVALID;

    uint8_t hdr[JWT_MAX_TOKEN], pay[JWT_MAX_TOKEN], sig[64];
    long hn = b64url_decode(t, (size_t)(d1 - t), hdr, sizeof hdr);
    if (hn <= 0) return JWT_INVALID;
    json_field hf[1] = {{"alg", 3, 0, 0, 0}};
    if (json_object_fields((const char *)hdr, (const char *)hdr + hn, hf, 1) != 1) return JWT_INVALID;
    if (hf[0].type != J_STR) return JWT_INVALID;
    {
        char alg[16];
        long al = (hf[0].ve - hf[0].vs) <= (long)sizeof alg
                  ? json_decode_string(hf[0].vs, hf[0].ve, alg, sizeof alg) : -1;
        if (al != 5 || memcmp(alg, "HS256", 5) != 0) return JWT_INVALID;
    }

    long sn = b64url_decode(d2 + 1, (size_t)(t + tn - d2 - 1), sig, sizeof sig);
    if (sn != 32) return JWT_INVALID;
    uint8_t mac[32];
    hmac_sha256(key, t, (size_t)(d2 - t), mac);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(mac[i] ^ sig[i]);
    if (diff) return JWT_INVALID;

    long pn = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), pay, sizeof pay);
    if (pn <= 0) return JWT_INVALID;
    json_field pf[3] = {{"exp", 3, 0, 0, 0}, {"sub", 3, 0, 0, 0}, {"username", 8, 0, 0, 0}};
    if (json_object_fields((const char *)pay, (const char *)pay + pn, pf, 3) != 1) return JWT_INVALID;
    if (pf[0].type != J_NUM) return JWT_INVALID;
    {
        char nb[64];
        size_t l = (size_t)(pf[0].ve - pf[0].vs);
        if (l >= sizeof nb) return JWT_INVALID;
        memcpy(nb, pf[0].vs, l); nb[l] = 0;
        double exp = strtod(nb, NULL);
        if (!(exp > now)) return JWT_INVALID;
    }

    if (pf[1].type != J_STR || pf[2].type != J_STR) return JWT_BAD_PAYLOAD;
    {
        char sb[32];
        long sl = (pf[1].ve - pf[1].vs) <= (long)sizeof sb
                  ? json_decode_string(pf[1].vs, pf[1].ve, sb, sizeof sb) : -1;
        if (sl < 1 || sl > 18 || sb[0] < '1' || sb[0] > '9') return JWT_BAD_PAYLOAD;
        int64_t v = 0;
        for (long i = 0; i < sl; i++) {
            if (sb[i] < '0' || sb[i] > '9') return JWT_BAD_PAYLOAD;
            v = v * 10 + (sb[i] - '0');
        }
        *uid = v;
    }
    long ul = json_decode_string(pf[2].vs, pf[2].ve, uname, ucap);
    if (ul < 0) return JWT_BAD_PAYLOAD;
    *ulen = (size_t)ul;
    return JWT_OK;
}

#endif
