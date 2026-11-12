/* Standalone tests for sha256.c / jwt.c (no external libraries). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../src/sha256.h"
#include "../src/jwt.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name)
{
    checks++;
    if (cond) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexeq(const uint8_t *b, size_t n, const char *hex)
{
    if (strlen(hex) != 2 * n) return 0;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        if (b[i] != (uint8_t)((hi << 4) | lo)) return 0;
    }
    return 1;
}

static size_t b64url_enc(const uint8_t *in, size_t n, char *out)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0, i = 0;
    while (n - i >= 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
    }
    out[o] = '\0';
    return o;
}

/* Builds "<b64(header)>.<b64(payload)>.<b64(hmac)>", returns length. */
static size_t make_token(const char *header, const char *payload,
                         const uint8_t *key, size_t keylen, char *out)
{
    char hb[1024], pb[8192], sb[64];
    size_t hl = b64url_enc((const uint8_t *)header, strlen(header), hb);
    size_t pl = b64url_enc((const uint8_t *)payload, strlen(payload), pb);

    memcpy(out, hb, hl);
    out[hl] = '.';
    memcpy(out + hl + 1, pb, pl);
    size_t signed_len = hl + 1 + pl;

    uint8_t mac[32];
    hmac_sha256(key, keylen, out, signed_len, mac);
    size_t sl = b64url_enc(mac, sizeof mac, sb);

    out[signed_len] = '.';
    memcpy(out + signed_len + 1, sb, sl);
    out[signed_len + 1 + sl] = '\0';
    return signed_len + 1 + sl;
}

static const char *SECRET = "twelve-dollar-challenge";
static const char *HDR = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
static const int64_t NOW = 1700000000;

static enum jwt_result V(const char *auth, int64_t now, struct jwt_claims *c)
{
    return jwt_verify(auth, strlen(auth), (const uint8_t *)SECRET, strlen(SECRET), now, c);
}

static void test_sha256(void)
{
    uint8_t out[32];

    sha256("abc", 3, out);
    check(hexeq(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256(\"abc\")");

    sha256("", 0, out);
    check(hexeq(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "SHA-256(\"\")");

    /* RFC 4231 test case 1 */
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof key1);
    hmac_sha256(key1, sizeof key1, "Hi There", 8, out);
    check(hexeq(out, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
          "RFC 4231 HMAC-SHA256 case 1");

    /* RFC 4231 test case 2 */
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    check(hexeq(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
          "RFC 4231 HMAC-SHA256 case 2");

    /* multi-block message and padding boundary (NIST vector, 56 bytes) */
    const char *m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m56, strlen(m56), out);
    check(hexeq(out, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "SHA-256 56-byte NIST vector");

    /* RFC 4231 case 6: key longer than the 64-byte block, 54-byte message */
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof key6);
    const char *msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key6, sizeof key6, msg6, strlen(msg6), out);
    check(hexeq(out, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
          "RFC 4231 HMAC-SHA256 case 6 (131-byte key)");

    /* RFC 4231 case 7: 152-byte message, multiple blocks */
    const char *msg7 =
        "This is a test using a larger than block-size key and a larger than block-size data. "
        "The key needs to be hashed before being used by the HMAC algorithm.";
    hmac_sha256(key6, sizeof key6, msg7, strlen(msg7), out);
    check(hexeq(out, 32, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
          "RFC 4231 HMAC-SHA256 case 7 (multi-block)");
}

static void test_b64url(void)
{
    uint8_t buf[512];
    char enc[1024];

    int n = b64url_decode("YWJj", 4, buf, sizeof buf);
    check(n == 3 && memcmp(buf, "abc", 3) == 0, "b64url decode \"YWJj\" == \"abc\"");

    n = b64url_decode("", 0, buf, sizeof buf);
    check(n == 0, "b64url decode empty");

    n = b64url_decode("-w", 2, buf, sizeof buf);
    check(n == 1 && buf[0] == 0xfb, "b64url decode \"-w\" == 0xfb");

    n = b64url_decode("AA", 2, buf, sizeof buf);
    check(n == 1 && buf[0] == 0x00, "b64url decode \"AA\" == 0x00");

    check(b64url_decode("A", 1, buf, sizeof buf) == -1, "b64url rejects len 1 mod 4");
    check(b64url_decode("AAAAA", 5, buf, sizeof buf) == -1, "b64url rejects len 5");
    check(b64url_decode("AB", 2, buf, sizeof buf) == -1, "b64url rejects non-zero trailing bits (2)");
    check(b64url_decode("AAB", 3, buf, sizeof buf) == -1, "b64url rejects non-zero trailing bits (3)");
    check(b64url_decode("AA==", 4, buf, sizeof buf) == -1, "b64url rejects padding");
    check(b64url_decode("a+b/", 4, buf, sizeof buf) == -1, "b64url rejects standard alphabet");
    check(b64url_decode("YWJj", 4, buf, 2) == -1, "b64url rejects output overflow");

    /* round trip all byte values */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    size_t el = b64url_enc(all, sizeof all, enc);
    n = b64url_decode(enc, el, buf, sizeof buf);
    check(n == 256 && memcmp(buf, all, 256) == 0, "b64url round-trip 0..255");

    static const size_t lens[] = {0, 1, 2, 3, 4, 5, 6, 31, 32, 33, 63, 64, 65};
    int ok = 1;
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        size_t el2 = b64url_enc(all, lens[i], enc);
        int d = b64url_decode(enc, el2, buf, sizeof buf);
        if (d != (int)lens[i] || memcmp(buf, all, lens[i]) != 0) ok = 0;
    }
    check(ok, "b64url round-trip lengths 0..65");
}

static void test_jwt_ok(void)
{
    char token[8192];
    char auth[8256];
    struct jwt_claims c;

    make_token(HDR, "{\"sub\":\"1\",\"username\":\"golden_ember_1\",\"iat\":1700000000,"
                    "\"exp\":1700003600}", (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);

    memset(&c, 0xAA, sizeof c);
    check(V(auth, NOW, &c) == JWT_OK, "valid token verifies");
    check(c.user_id == 1, "valid token user_id == 1");
    check(c.username_len == 14 && memcmp(c.username, "golden_ember_1", 14) == 0,
          "valid token username == golden_ember_1");

    /* whitespace between tokens is allowed */
    make_token(HDR, "{ \"sub\" : \"42\" , \"username\" : \"u\" , \"exp\" : 1700003600 }",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_OK && c.user_id == 42 && c.username_len == 1,
          "payload JSON with whitespace");

    /* last duplicate key wins */
    make_token(HDR, "{\"sub\":\"2\",\"sub\":\"1\",\"username\":\"old\",\"username\":\"new\","
                    "\"exp\":1700003600}", (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_OK && c.user_id == 1 && c.username_len == 3 &&
          memcmp(c.username, "new", 3) == 0, "duplicate keys: last wins");

    /* JSON string escapes, including surrogate pair */
    make_token(HDR, "{\"sub\":\"7\",\"username\":\"caf\\u00e9\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_OK && c.username_len == 5 &&
          memcmp(c.username, "caf\xc3\xa9", 5) == 0, "username \\u00e9 decodes to UTF-8");

    make_token(HDR, "{\"sub\":\"7\",\"username\":\"\\ud83d\\ude00x\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_OK && c.username_len == 5 &&
          memcmp(c.username, "\xf0\x9f\x98\x80x", 5) == 0, "surrogate pair decodes to UTF-8");

    /* unknown members (including iat and nested values) are skipped */
    make_token(HDR, "{\"iat\":1700000000,\"meta\":{\"a\":[1,2,{\"b\":null}],\"s\":\"x\"},"
                    "\"sub\":\"9\",\"username\":\"z\",\"exp\":1700003601}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_OK && c.user_id == 9, "unknown/nested members skipped");
}

static void test_jwt_invalid(void)
{
    char token[8192];
    char auth[8256];
    struct jwt_claims c;

    static const char *expired =
        "{\"sub\":\"1\",\"username\":\"golden_ember_1\",\"iat\":1699992800,\"exp\":1699996400}";
    make_token(HDR, expired, (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "expired token -> JWT_INVALID");

    /* alg none signed with the real secret */
    make_token("{\"alg\":\"none\",\"typ\":\"JWT\"}",
               "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "alg none -> JWT_INVALID");

    make_token("{\"alg\":\"HS512\"}",
               "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "alg HS512 -> JWT_INVALID");

    make_token("{\"typ\":\"JWT\"}",
               "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "missing alg -> JWT_INVALID");

    /* tampered signature */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    size_t tl = strlen(token);
    token[tl - 5] = (token[tl - 5] == 'A') ? 'B' : 'A';
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "tampered signature -> JWT_INVALID");

    /* missing exp */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\"}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "missing exp -> JWT_INVALID");

    /* non-numeric exp */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\",\"exp\":\"1700003600\"}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "string exp -> JWT_INVALID");

    /* exp exactly now is not > now */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700000000}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "exp == now -> JWT_INVALID");

    /* malformed payload JSON, valid signature */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "malformed payload JSON -> JWT_INVALID");

    /* expired but also bad sub: exp is checked first */
    make_token(HDR, "{\"sub\":\"abc\",\"username\":\"x\",\"exp\":1699996400}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "expired + bad sub -> JWT_INVALID");

    /* wrong key */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600}",
               (const uint8_t *)"another-secret", 14, token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "wrong secret -> JWT_INVALID");
}

static void test_jwt_payload(void)
{
    char token[8192];
    char auth[8256];
    struct jwt_claims c;

    struct {
        const char *payload;
        const char *name;
    } cases[] = {
        {"{\"sub\":\"abc\",\"username\":\"x\",\"exp\":1700003600}", "sub abc -> JWT_PAYLOAD"},
        {"{\"sub\":\"0\",\"username\":\"x\",\"exp\":1700003600}", "sub \"0\" -> JWT_PAYLOAD"},
        {"{\"sub\":\"01\",\"username\":\"x\",\"exp\":1700003600}", "sub \"01\" -> JWT_PAYLOAD"},
        {"{\"sub\":\"-1\",\"username\":\"x\",\"exp\":1700003600}", "sub \"-1\" -> JWT_PAYLOAD"},
        {"{\"sub\":\"1.5\",\"username\":\"x\",\"exp\":1700003600}", "sub \"1.5\" -> JWT_PAYLOAD"},
        {"{\"sub\":\"\",\"username\":\"x\",\"exp\":1700003600}", "empty sub -> JWT_PAYLOAD"},
        {"{\"username\":\"x\",\"exp\":1700003600}", "missing sub -> JWT_PAYLOAD"},
        {"{\"sub\":1,\"username\":\"x\",\"exp\":1700003600}", "numeric sub -> JWT_PAYLOAD"},
        {"{\"sub\":\"1\",\"username\":123,\"exp\":1700003600}", "username number -> JWT_PAYLOAD"},
        {"{\"sub\":\"1\",\"username\":null,\"exp\":1700003600}", "username null -> JWT_PAYLOAD"},
        {"{\"sub\":\"1\",\"exp\":1700003600}", "missing username -> JWT_PAYLOAD"},
        {"{\"sub\":\"1\",\"username\":\"x\"}", "missing exp is INVALID not PAYLOAD"},
        {"{\"sub\":\"99999999999999999999\",\"username\":\"x\",\"exp\":1700003600}",
         "sub overflow -> JWT_PAYLOAD"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        make_token(HDR, cases[i].payload, (const uint8_t *)SECRET, strlen(SECRET), token);
        snprintf(auth, sizeof auth, "Bearer %s", token);
        enum jwt_result want = (i == 11) ? JWT_INVALID : JWT_PAYLOAD;
        check(V(auth, NOW, &c) == want, cases[i].name);
    }

    /* username longer than 255 bytes decoded */
    char longname[300];
    char payload[600];
    memset(longname, 'a', 256);
    longname[256] = '\0';
    snprintf(payload, sizeof payload,
             "{\"sub\":\"1\",\"username\":\"%s\",\"exp\":1700003600}", longname);
    make_token(HDR, payload, (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_PAYLOAD, "256-byte username -> JWT_PAYLOAD");

    /* 255 bytes is accepted */
    memset(longname, 'a', 255);
    longname[255] = '\0';
    snprintf(payload, sizeof payload,
             "{\"sub\":\"1\",\"username\":\"%s\",\"exp\":1700003600}", longname);
    make_token(HDR, payload, (const uint8_t *)SECRET, strlen(SECRET), token);
    snprintf(auth, sizeof auth, "Bearer %s", token);
    check(V(auth, NOW, &c) == JWT_OK && c.username_len == 255, "255-byte username accepted");
}

static void test_jwt_missing(void)
{
    char token[8192];
    char auth[8256];
    struct jwt_claims c;

    check(jwt_verify(NULL, 0, (const uint8_t *)SECRET, strlen(SECRET), NOW, &c) == JWT_MISSING,
          "no Authorization header -> JWT_MISSING");
    check(jwt_verify("", 0, (const uint8_t *)SECRET, strlen(SECRET), NOW, &c) == JWT_MISSING,
          "empty Authorization header -> JWT_MISSING");
    check(V("Basic abc", NOW, &c) == JWT_MISSING, "Basic auth -> JWT_MISSING");
    check(V("Bearer ", NOW, &c) == JWT_MISSING, "empty bearer token -> JWT_MISSING");
    check(V("bearer abc", NOW, &c) == JWT_MISSING, "lowercase bearer -> JWT_MISSING");
    check(V("Bearer  abc", NOW, &c) == JWT_INVALID, "double space -> JWT_INVALID");
    check(V("Bearer abc", NOW, &c) == JWT_INVALID, "garbage token -> JWT_INVALID");
    check(V("Bearer a.b.c", NOW, &c) == JWT_INVALID, "garbage 3-part token -> JWT_INVALID");

    /* correct token without the "Bearer " prefix */
    make_token(HDR, "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1700003600}",
               (const uint8_t *)SECRET, strlen(SECRET), token);
    check(V(token, NOW, &c) == JWT_MISSING, "token without Bearer prefix -> JWT_MISSING");

    /* correct token with an extra trailing segment */
    snprintf(auth, sizeof auth, "Bearer %s.x", token);
    check(V(auth, NOW, &c) == JWT_INVALID, "four JWT segments -> JWT_INVALID");

    /* alg none token in the exact shape test/test.sh builds: <hdr>.<payload>. */
    {
        const char *pstart = strchr(token, '.') + 1;
        size_t plen = (size_t)(strrchr(token, '.') - pstart);
        char nh[64];
        size_t nhl = b64url_enc((const uint8_t *)"{\"alg\":\"none\",\"typ\":\"JWT\"}",
                                strlen("{\"alg\":\"none\",\"typ\":\"JWT\"}"), nh);
        snprintf(auth, sizeof auth, "Bearer %s.%.*s.", nh, (int)plen, pstart);
        check(V(auth, NOW, &c) == JWT_INVALID, "test.sh-style alg=none token -> JWT_INVALID");
        (void)nhl;
    }
}

int main(void)
{
    test_sha256();
    test_b64url();
    test_jwt_ok();
    test_jwt_invalid();
    test_jwt_payload();
    test_jwt_missing();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
