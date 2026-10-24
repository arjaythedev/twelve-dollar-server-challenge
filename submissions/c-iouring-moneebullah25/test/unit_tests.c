/* Unit tests for the pure modules: crypto, json, jwt, http. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/crypto.h"
#include "../src/http.h"
#include "../src/json.h"
#include "../src/jwt.h"
#include "../src/util.h"

static int fails;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fails++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

static void hex(const uint8_t *d, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[2 * n] = 0;
}

static void test_sha256(void) {
    uint8_t d[32];
    char h[65];
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    sha256_init(&s);
    sha256_final(&s, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    /* two-block message, fed in odd pieces */
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&s);
    sha256_update(&s, m, 10);
    sha256_update(&s, m + 10, 30);
    sha256_update(&s, m + 40, strlen(m) - 40);
    sha256_final(&s, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    /* one million 'a' */
    char a[1000];
    memset(a, 'a', sizeof a);
    sha256_init(&s);
    for (int i = 0; i < 1000; i++) sha256_update(&s, a, sizeof a);
    sha256_final(&s, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_hmac(void) {
    /* RFC 4231 test case 2 */
    struct hmac_key k;
    uint8_t d[32];
    char h[65];
    hmac_key_init(&k, "Jefe", 4);
    hmac_sha256(&k, "what do ya want for nothing?", 28, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
    /* test case 1 */
    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    hmac_key_init(&k, key, sizeof key);
    hmac_sha256(&k, "Hi There", 8, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0);
    /* test case 6: key longer than a block */
    uint8_t lk[131];
    memset(lk, 0xaa, sizeof lk);
    hmac_key_init(&k, lk, sizeof lk);
    hmac_sha256(&k, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    hex(d, 32, h);
    CHECK(strcmp(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0);
    CHECK(crypto_equal(d, d, 32));
    uint8_t e[32];
    memcpy(e, d, 32);
    e[31] ^= 1;
    CHECK(!crypto_equal(d, e, 32));
}

static void test_base64url(void) {
    uint8_t out[64];
    char enc[64];
    CHECK(base64url_decode("", 0, out, sizeof out) == 0);
    CHECK(base64url_decode("YQ", 2, out, sizeof out) == 1 && out[0] == 'a');
    CHECK(base64url_decode("YWI", 3, out, sizeof out) == 2 && out[1] == 'b');
    CHECK(base64url_decode("YWJj", 4, out, sizeof out) == 3 && out[2] == 'c');
    CHECK(base64url_decode("YWJj=", 5, out, sizeof out) == -1);
    CHECK(base64url_decode("YR", 2, out, sizeof out) == -1); /* non-zero leftover bits */
    CHECK(base64url_decode("YW+j", 4, out, sizeof out) == -1);
    CHECK(base64url_decode("Y", 1, out, sizeof out) == -1);
    CHECK(base64url_decode("YWJj", 4, out, 2) == -1);
    uint8_t bin[] = {0xfb, 0xff, 0xbf, 0x00};
    size_t n = base64url_encode(bin, 3, enc);
    CHECK(n == 4 && memcmp(enc, "-_-_", 4) == 0);
    n = base64url_encode(bin, 4, enc);
    CHECK(n == 6 && memcmp(enc, "-_-_AA", 6) == 0);
    CHECK(base64url_decode(enc, 6, out, sizeof out) == 4 && memcmp(out, bin, 4) == 0);
    for (int i = 0; i < 200; i++) {
        uint8_t r[40];
        size_t len = (size_t)(i % 40);
        for (size_t j = 0; j < len; j++) r[j] = (uint8_t)(i * 31 + (int)j * 7);
        n = base64url_encode(r, len, enc);
        int m = base64url_decode(enc, n, out, sizeof out);
        CHECK(m == (int)len && memcmp(out, r, len) == 0);
    }
}

static void test_json(void) {
    char body[2048];
    struct json_field f[] = {{.name = "body", .buf = body, .cap = sizeof body}};
    const char *doc = "{\"body\":\"hi \\\"there\\\" \\u00e9\\ud83d\\ude00\\n\"}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_OK);
    CHECK(f[0].kind == JSON_STRING);
    CHECK(f[0].len == strlen("hi \"there\" \xc3\xa9\xf0\x9f\x98\x80\n"));
    CHECK(memcmp(body, "hi \"there\" \xc3\xa9\xf0\x9f\x98\x80\n", f[0].len) == 0);
    doc = "{bad json";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_MALFORMED);
    doc = "{}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_OK && f[0].kind == JSON_MISSING);
    doc = "{\"body\":123}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_OK && f[0].kind == JSON_NUMBER && f[0].len == 3);
    doc = "{\"body\":null,\"x\":[1,2,{\"a\":true}],\"body\":\"last\"}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_OK && f[0].kind == JSON_STRING && f[0].len == 4);
    doc = "[1]";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_NOT_OBJECT);
    doc = "\"str\"";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_NOT_OBJECT);
    doc = "{\"body\":\"x\"} trailing";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_MALFORMED);
    doc = "{\"body\":\"x\",}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_MALFORMED);
    doc = "{\"body\":\"\\ud800\"}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_MALFORMED);
    doc = "{\"body\":\"tab\there\"}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_MALFORMED);
    doc = "{\"body\":-0.5e+3}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_OK && f[0].kind == JSON_NUMBER);
    doc = "{\"body\":01}";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_MALFORMED);
    doc = "  {\n \"body\" : \"x\" }\n";
    CHECK(json_parse_object(doc, strlen(doc), f, 1) == JSON_OK && f[0].len == 1);
    /* deep nesting is rejected, not stack-exhausting */
    char deep[200];
    memset(deep, '[', 100);
    memset(deep + 100, ']', 100);
    CHECK(json_parse_object(deep, 200, f, 1) == JSON_MALFORMED);
    /* truncation */
    char small[4];
    struct json_field g[] = {{.name = "body", .buf = small, .cap = sizeof small}};
    doc = "{\"body\":\"toolongvalue\"}";
    CHECK(json_parse_object(doc, strlen(doc), g, 1) == JSON_OK && g[0].truncated && g[0].len == 4);
    /* writer */
    char out[64];
    size_t n = json_write_string(out, sizeof out, "a\"b\\c\n\x01", 7);
    CHECK(n == strlen("\"a\\\"b\\\\c\\n\\u0001\"") && memcmp(out, "\"a\\\"b\\\\c\\n\\u0001\"", n) == 0);
    CHECK(json_write_string(out, 5, "abcdef", 6) == 0);
}

static void test_jwt(void) {
    struct hmac_key k;
    hmac_key_init(&k, "twelve-dollar-challenge", 23);
    char tok[1024];
    const char *pl = "{\"sub\":\"42\",\"username\":\"golden_ember_1\",\"iat\":1700000000,\"exp\":2000000000}";
    size_t n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(n > 0);
    struct jwt_claims c;
    CHECK(jwt_verify(&k, tok, n, 1800000000, &c) == JWT_OK);
    CHECK(c.sub == 42 && c.username_len == 14 && memcmp(c.username, "golden_ember_1", 14) == 0);
    /* expired */
    CHECK(jwt_verify(&k, tok, n, 2000000000, &c) == JWT_INVALID);
    /* tampered signature: a non-canonical last character, then a real change */
    char saved = tok[n - 1];
    tok[n - 1] = (saved == 'A') ? 'B' : 'A';
    CHECK(jwt_verify(&k, tok, n, 1800000000, &c) == JWT_INVALID);
    tok[n - 1] = saved;
    saved = tok[n - 43];
    tok[n - 43] = (saved == 'A') ? 'B' : 'A';
    CHECK(jwt_verify(&k, tok, n, 1800000000, &c) == JWT_INVALID);
    tok[n - 43] = saved;
    CHECK(jwt_verify(&k, tok, n, 1800000000, &c) == JWT_OK);
    /* tampered payload */
    char t2[1024];
    memcpy(t2, tok, n);
    char *dot = memchr(t2, '.', n);
    dot[1] = (dot[1] == 'e') ? 'f' : 'e';
    CHECK(jwt_verify(&k, t2, n, 1800000000, &c) == JWT_INVALID);
    /* wrong key */
    struct hmac_key k2;
    hmac_key_init(&k2, "other", 5);
    CHECK(jwt_verify(&k2, tok, n, 1800000000, &c) == JWT_INVALID);
    /* garbage, no dots, two dots, empty signature */
    CHECK(jwt_verify(&k, "abc", 3, 0, &c) == JWT_INVALID);
    CHECK(jwt_verify(&k, "a.b", 3, 0, &c) == JWT_INVALID);
    CHECK(jwt_verify(&k, "a.b.", 4, 0, &c) == JWT_INVALID);
    CHECK(jwt_verify(&k, "", 0, 0, &c) == JWT_INVALID);
    /* alg=none with empty signature, as the suite sends it */
    const char *none = "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJzdWIiOiIxIn0.";
    CHECK(jwt_verify(&k, none, strlen(none), 0, &c) == JWT_INVALID);
    /* missing exp -> invalid; bad sub -> payload; missing username -> payload */
    pl = "{\"sub\":\"42\",\"username\":\"x\"}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_INVALID);
    pl = "{\"sub\":\"abc\",\"username\":\"x\",\"exp\":2000000000}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_PAYLOAD);
    pl = "{\"sub\":\"0\",\"username\":\"x\",\"exp\":2000000000}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_PAYLOAD);
    pl = "{\"sub\":42,\"username\":\"x\",\"exp\":2000000000}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_PAYLOAD);
    pl = "{\"sub\":\"42\",\"exp\":2000000000}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_PAYLOAD);
    pl = "{\"sub\":\"42\",\"username\":5,\"exp\":2000000000}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_PAYLOAD);
    pl = "{\"sub\":\"42\",\"username\":\"x\",\"exp\":\"2000000000\"}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_INVALID);
    pl = "{\"sub\":\"42\",\"username\":\"x\",\"exp\":2000000000.7}";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_OK);
    /* payload that is not JSON */
    pl = "not json";
    n = jwt_sign(&k, pl, strlen(pl), tok, sizeof tok);
    CHECK(jwt_verify(&k, tok, n, 1, &c) == JWT_INVALID);
    /* HS256 header but a different, validly signed alg (e.g. HS512 claimed) */
    {
        const char *hdr = "{\"alg\":\"HS512\",\"typ\":\"JWT\"}";
        char t[512];
        size_t o = base64url_encode((const uint8_t *)hdr, strlen(hdr), t);
        t[o++] = '.';
        const char *p2 = "{\"sub\":\"1\",\"username\":\"x\",\"exp\":2000000000}";
        o += base64url_encode((const uint8_t *)p2, strlen(p2), t + o);
        uint8_t mac[32];
        hmac_sha256(&k, t, o, mac);
        t[o++] = '.';
        o += base64url_encode(mac, 32, t + o);
        CHECK(jwt_verify(&k, t, o, 1, &c) == JWT_INVALID);
    }
}

static void test_http(void) {
    struct http_req r;
    const char *q = "GET /feed HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE);
    CHECK(r.method == HTTP_GET && r.path_len == 5 && memcmp(r.path, "/feed", 5) == 0);
    CHECK(r.keep_alive && r.body_len == 0 && r.total_len == strlen(q) && r.auth == NULL);
    /* partial: header incomplete, then body incomplete */
    q = "POST /posts HTTP/1.1\r\nContent-Length: 10\r\nAuthorization:  Bearer abc \r\n\r\n{\"body\"";
    CHECK(http_parse(q, 20, 8192, 16384, &r) == HTTP_PARTIAL);
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_PARTIAL);
    q = "POST /posts HTTP/1.1\r\nContent-Length: 10\r\nAuthorization:  Bearer abc \r\n\r\n{\"body\":1}extra";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE);
    CHECK(r.method == HTTP_POST && r.body_len == 10 && memcmp(r.body, "{\"body\":1}", 10) == 0);
    CHECK(r.auth_len == 10 && memcmp(r.auth, "Bearer abc", 10) == 0);
    CHECK(r.total_len == strlen(q) - 5);
    /* connection close, HTTP/1.0 */
    q = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE && !r.keep_alive);
    q = "GET / HTTP/1.0\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE && !r.keep_alive);
    q = "GET / HTTP/1.0\r\nconnection: Keep-Alive\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE && r.keep_alive);
    q = "GET / HTTP/1.1\r\nExpect: 100-continue\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE && r.expect_100);
    /* bad requests */
    const char *bad[] = {
        "GET\r\n\r\n",
        "GET /feed\r\n\r\n",
        "GET /feed HTTP/2.0\r\n\r\n",
        "GET /feed HTTP/1.1\r\nNoColon\r\n\r\n",
        "GET /feed HTTP/1.1\r\n: x\r\n\r\n",
        "GET /feed HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "GET /feed HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "GET /feed HTTP/1.1\r\nContent-Length: 12345678901\r\n\r\n",
        "GET /feed HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /feed HTTP/1.1\r\n Folded: x\r\n\r\n",
        "G\x01T /feed HTTP/1.1\r\n\r\n",
        "GET /fe\x7f HTTP/1.1\r\n\r\n",
        "GET /feed HTTP/1.1\r\nX: a\x01"
        "b\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(http_parse(bad[i], (uint32_t)strlen(bad[i]), 8192, 16384, &r) == HTTP_BAD);
    }
    /* too large: headers without an end past the limit, and a huge body */
    char big[9000];
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /feed HTTP/1.1\r\nX: ", 23);
    CHECK(http_parse(big, sizeof big, 8192, 16384, &r) == HTTP_TOO_LARGE);
    CHECK(http_parse(big, 100, 8192, 16384, &r) == HTTP_PARTIAL);
    q = "POST /posts HTTP/1.1\r\nContent-Length: 20000\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_TOO_LARGE);
    /* other methods are tokens */
    q = "DELETE /posts/1 HTTP/1.1\r\n\r\n";
    CHECK(http_parse(q, (uint32_t)strlen(q), 8192, 16384, &r) == HTTP_COMPLETE && r.method == HTTP_OTHER);
    /* response head */
    char h[256];
    size_t n = http_write_head(h, sizeof h, 201, 1234, 0);
    CHECK(n == strlen("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 1234\r\n\r\n"));
    CHECK(memcmp(h, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 1234\r\n\r\n", n) == 0);
    n = http_write_head(h, sizeof h, 400, 7, 1);
    CHECK(memcmp(h + n - 21, "Connection: close\r\n\r\n", 21) == 0);
}

static void test_util(void) {
    char b[32];
    CHECK(fmt_u64(b, 0) == 1 && b[0] == '0');
    CHECK(fmt_u64(b, 1234567890123ULL) == 13 && memcmp(b, "1234567890123", 13) == 0);
    CHECK(fmt_i64(b, -42) == 3 && memcmp(b, "-42", 3) == 0);
    uint64_t v;
    CHECK(parse_u64("007", 3, &v) == 0 && v == 7);
    CHECK(parse_u64("1.5", 3, &v) == -1);
    CHECK(parse_u64("", 0, &v) == -1);
    CHECK(parse_u64("1234567890123456789", 19, &v) == -1);
}

int main(void) {
    test_sha256();
    test_hmac();
    test_base64url();
    test_json();
    test_jwt();
    test_http();
    test_util();
    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("unit tests: all passed\n");
    return 0;
}
