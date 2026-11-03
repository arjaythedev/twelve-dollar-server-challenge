/* Quick self-test: sha256 vectors, hmac, b64url, json scanner, jwt check.
 * Not part of the server binary; compiled ad hoc during development. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "jwt.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("ok   %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)

static void hex2bin(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

int main(void)
{
    jwt_b64url_init();

    /* --- JWT end to end: build a token with the secret, then verify --- */
    jwt_init("twelve-dollar-challenge", 23);

    /* base64url of {"alg":"HS256","typ":"JWT"} */
    const char *hdr = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    /* base64url of {"sub":"1","username":"golden_ember_1","iat":0,"exp":9999999999} */
    const char *pld = "eyJzdWIiOiIxIiwidXNlcm5hbWUiOiJnb2xkZW5fZW1iZXJfMSIsImlhdCI6MCwiZXhwIjo5OTk5OTk5OTk5fQ";
    char msg[512];
    int mlen = snprintf(msg, sizeof msg, "%s.%s", hdr, pld);
    uint8_t mac[32];
    hmac_sha256_str(msg, (size_t)mlen, mac);

    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char sig[64];
    int sl = 0;
    for (int i = 0; i < 30; i += 3) {
        uint32_t v = (uint32_t)mac[i] << 16 | (uint32_t)mac[i + 1] << 8 | mac[i + 2];
        sig[sl++] = A[v >> 18]; sig[sl++] = A[v >> 12 & 63]; sig[sl++] = A[v >> 6 & 63]; sig[sl++] = A[v & 63];
    }
    uint32_t v = (uint32_t)mac[30] << 16 | (uint32_t)mac[31] << 8;
    sig[sl++] = A[v >> 18]; sig[sl++] = A[v >> 12 & 63]; sig[sl++] = A[v >> 6 & 63];

    char auth[1024];
    int al = snprintf(auth, sizeof auth, "Bearer %s.%s.%.*s", hdr, pld, sl, sig);
    auth_t a;
    int rc = check_auth(auth, (size_t)al, 1000000.0, &a);
    if (rc != 0) printf("  debug: rc=%d\n", rc);
    else printf("  debug: uid=%lld name=%.*s len=%zu\n", (long long)a.uid, (int)a.name_len, a.name, a.name_len);
    CHECK(rc == 0 && a.uid == 1 && a.name_len == 14 && !memcmp(a.name, "golden_ember_1", 14), "valid token verifies");

    /* tamper */
    auth[al - 1] = auth[al - 1] == 'A' ? 'B' : 'A';
    rc = check_auth(auth, (size_t)al, 1000000.0, &a);
    CHECK(rc == 2, "tampered signature rejected");

    /* missing bearer */
    rc = check_auth("Basic abc", 9, 0, &a);
    CHECK(rc == 1, "non-bearer rejected");
    rc = check_auth(NULL, 0, 0, &a);
    CHECK(rc == 1, "missing header rejected");

    /* --- expired token --- */
    {
        /* payload {"sub":"1","username":"u","exp":100} */
        const char *pld2 = "eyJzdWIiOiIxIiwidXNlcm5hbWUiOiJ1IiwiZXhwIjoxMDB9";
        char msg2[512];
        int ml2 = snprintf(msg2, sizeof msg2, "%s.%s", hdr, pld2);
        hmac_sha256_str(msg2, (size_t)ml2, mac);
        sl = 0;
        for (int i = 0; i < 30; i += 3) {
            uint32_t v2 = (uint32_t)mac[i] << 16 | (uint32_t)mac[i + 1] << 8 | mac[i + 2];
            sig[sl++] = A[v2 >> 18]; sig[sl++] = A[v2 >> 12 & 63]; sig[sl++] = A[v2 >> 6 & 63]; sig[sl++] = A[v2 & 63];
        }
        v = (uint32_t)mac[30] << 16 | (uint32_t)mac[31] << 8;
        sig[sl++] = A[v >> 18]; sig[sl++] = A[v >> 12 & 63]; sig[sl++] = A[v >> 6 & 63];
        al = snprintf(auth, sizeof auth, "Bearer %s.%s.%.*s", hdr, pld2, sl, sig);
        rc = check_auth(auth, (size_t)al, 1000000.0, &a);
        CHECK(rc == 2, "expired token rejected");
        rc = check_auth(auth, (size_t)al, 50.0, &a);
        CHECK(rc == 0 && a.uid == 1 && a.name_len == 1 && a.name[0] == 'u', "token valid before exp");
    }

    /* --- bad payload: sub not int --- */
    {
        /* {"sub":"abc","username":"u","exp":9999999999} */
        const char *pld3 = "eyJzdWIiOiJhYmMiLCJ1c2VybmFtZSI6InUiLCJleHAiOjk5OTk5OTk5OTl9";
        char msg3[512];
        int ml3 = snprintf(msg3, sizeof msg3, "%s.%s", hdr, pld3);
        hmac_sha256_str(msg3, (size_t)ml3, mac);
        sl = 0;
        for (int i = 0; i < 30; i += 3) {
            uint32_t v3 = (uint32_t)mac[i] << 16 | (uint32_t)mac[i + 1] << 8 | mac[i + 2];
            sig[sl++] = A[v3 >> 18]; sig[sl++] = A[v3 >> 12 & 63]; sig[sl++] = A[v3 >> 6 & 63]; sig[sl++] = A[v3 & 63];
        }
        v = (uint32_t)mac[30] << 16 | (uint32_t)mac[31] << 8;
        sig[sl++] = A[v >> 18]; sig[sl++] = A[v >> 12 & 63]; sig[sl++] = A[v >> 6 & 63];
        al = snprintf(auth, sizeof auth, "Bearer %s.%s.%.*s", hdr, pld3, sl, sig);
        rc = check_auth(auth, (size_t)al, 1000000.0, &a);
        CHECK(rc == 3, "sub not integer rejected as payload error");
    }

    /* --- JSON scanner --- */
    {
        jfield_t f[2] = { { .name = "body" }, { .name = "x" } };
        int r = json_fields("{\"body\":\"hello\"}", strlen("{\"body\":\"hello\"}"), f, 2);
        CHECK(r == JT_OTHER && f[0].type == JT_STRING && f[0].ve - f[0].v == 5 && !memcmp(f[0].v, "hello", 5), "body field found");
        r = json_fields("{bad json", strlen("{bad json"), f, 2);
        CHECK(r == JT_NONE, "bad json rejected");
        r = json_fields("{\"body\":123}", strlen("{\"body\":123}"), f, 2);
        CHECK(r == JT_OTHER && f[0].type == JT_NUMBER, "numeric body typed");
        r = json_fields("{}", 2, f, 2);
        CHECK(r == JT_OTHER && f[0].type == JT_NONE, "empty object ok");
        const char *nested = "{\"a\":{\"b\":[1,2,{\"c\":\"d\"}]},\"body\":\"e\"}";
        r = json_fields(nested, strlen(nested), f, 2);
        CHECK(r == JT_OTHER && f[0].type == JT_STRING, "nested container skipped");
        r = json_fields("\"hello\"", 7, f, 2);
        CHECK(r == JT_STRING, "string root typed");
        const char *esc = "{\"body\":\"a\\u00e9b\"}";
        r = json_fields(esc, strlen(esc), f, 2);
        CHECK(r == JT_OTHER && f[0].type == JT_STRING, "escaped string ok");
        char out[64];
        size_t n = j_unescape(f[0].v, f[0].ve, out);
        CHECK(n == 4 && (unsigned char)out[1] == 0xc3, "unescape produces utf-8");
    }

    /* --- escape round trip --- */
    {
        char buf[128];
        char *end = j_escape(buf, (const unsigned char *)"a\"b\\c\nd", 7);
        *end = 0;
        CHECK(!strcmp(buf, "a\\\"b\\\\c\\nd"), "escape output");
    }

    printf(fails ? "FAILURES: %d\n" : "all ok\n", fails);
    return fails != 0;
}
