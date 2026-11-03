// Portable unit tests: crypto/JWT/JSON. Builds on macOS and Linux:
//   cc -O1 -std=gnu11 -Wall -Wextra -Werror -Isrc tests/unit.c src/jwt.c src/json.c -o /tmp/unit && /tmp/unit
// Crypto vectors verified against Python hashlib/hmac (oracle).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "jwt.h"
#include "json.h"
#include "http.h"

static int fails = 0;
#define CHECK(cond, ...) do { \
  if (!(cond)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)
#define SL(s) (s), (sizeof(s) - 1)  // string literal + length (no hand counting)

static void hex(const uint8_t *p, size_t n, char *o) {
  static const char *h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { o[2*i] = h[p[i]>>4]; o[2*i+1] = h[p[i]&15]; }
  o[2*n] = 0;
}

static void t_sha256(void) {
  sha256_t c; uint8_t d[32]; char h[65];
  sha256_init(&c); sha256_final(&c, d); hex(d, 32, h);
  CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty %s", h);
  sha256_init(&c); sha256_update(&c, (uint8_t*)"abc", 3); sha256_final(&c, d); hex(d, 32, h);
  CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc %s", h);
  sha256_init(&c);
  for (int i = 0; i < 10; i++) sha256_update(&c, (uint8_t*)"0123456789abcdef", 16);
  sha256_final(&c, d); hex(d, 32, h);
  CHECK(!strcmp(h, "c84e5648e5bd3dd9e37b57ecb80fc505e3324f53487e07532c9020e5b9e4e76d"), "sha256 160B %s", h);
}

static void t_hmac(void) {
  uint8_t k1[20]; memset(k1, 0x0b, 20);
  uint8_t d[32]; char h[65];
  hmac_sha256(k1, 20, (uint8_t*)"Hi There", 8, d); hex(d, 32, h);
  CHECK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "hmac rfc4231#1 %s", h);
  hmac_sha256((uint8_t*)"Jefe", 4, (uint8_t*)"what do ya want for nothing?", 28, d); hex(d, 32, h);
  CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac rfc4231#2 %s", h);
  uint8_t k3[131]; memset(k3, 0xaa, 131);
  hmac_sha256(k3, 131, (uint8_t*)"Test Using Larger Than Block-Size Key - Hash Key First", 54, d); hex(d, 32, h);
  CHECK(!strcmp(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "hmac rfc4231#3 %s", h);
}

static void t_b64(void) {
  uint8_t out[64];
  CHECK(b64url_dec("Zg", 2, out, 64) == 1 && out[0] == 'f', "b64 Zg");
  CHECK(b64url_dec("Zm9v", 4, out, 64) == 3 && !memcmp(out, "foo", 3), "b64 Zm9v");
  CHECK(b64url_dec("Zm9", 3, out, 64) == 2 && !memcmp(out, "fo", 2), "b64 Zm9");
  CHECK(b64url_dec("Z", 1, out, 64) == -1, "b64 len4==1 rejected");
  CHECK(b64url_dec("Zm9*", 4, out, 64) == -1, "b64 bad char rejected");
  char e[64]; size_t n = b64url_enc((uint8_t*)"foo", 3, e); e[n] = 0;
  CHECK(n == 4 && !memcmp(e, "Zm9v", 4), "b64 enc %s", e);
}

// JSON-escape a raw string into dst (for building test tokens).
static size_t esc(const char *s, char *dst, size_t cap) {
  size_t o = 0;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { if (o + 2 > cap) break; dst[o++] = '\\'; dst[o++] = *s; }
    else { if (o + 1 > cap) break; dst[o++] = *s; }
  }
  return o;
}

static int build_token(const char *sub, const char *user, int64_t exp,
                       const char *alg, char *tok, const char *secret) {
  char sube[128], usere[256];
  size_t sl = esc(sub, sube, sizeof sube), ul = esc(user, usere, sizeof usere);
  char h[128], p[768];
  int hl = snprintf(h, sizeof h, "{\"alg\":\"%s\",\"typ\":\"JWT\"}", alg);
  int pl = snprintf(p, sizeof p, "{\"sub\":\"%.*s\",\"username\":\"%.*s\",\"iat\":1000,\"exp\":%lld}",
                    (int)sl, sube, (int)ul, usere, (long long)exp);
  char hb[256], pb[1024];
  size_t hbl = b64url_enc((uint8_t*)h, (size_t)hl, hb);
  size_t pbl = b64url_enc((uint8_t*)p, (size_t)pl, pb);
  char msg[1400];
  memcpy(msg, hb, hbl); msg[hbl] = '.'; memcpy(msg + hbl + 1, pb, pbl);
  uint8_t mac[32];
  hmac_sha256((uint8_t*)secret, strlen(secret), (uint8_t*)msg, hbl + 1 + pbl, mac);
  char sb[128];
  size_t sbl = b64url_enc(mac, 32, sb);
  memcpy(tok, msg, hbl + 1 + pbl); tok[hbl+1+pbl] = '.';
  memcpy(tok + hbl + 1 + pbl + 1, sb, sbl);
  return (int)(hbl + 1 + pbl + 1 + sbl);
}

static void t_jwt(void) {
  char tok[2048], scratch[4096];
  auth_t a;
  const char *sec = "twelve-dollar-challenge";
  int n = build_token("1", "golden_ember_1", 2000000000, "HS256", tok, sec);
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_OK
        && a.uid == 1 && a.ulen == 14 && !memcmp(a.uname, "golden_ember_1", 14), "jwt valid");
  tok[n-1] = (char)(tok[n-1] == 'A' ? 'B' : 'A');
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_TOKEN, "jwt bad sig");
  n = build_token("1", "u", 500, "HS256", tok, sec);
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_TOKEN, "jwt expired");
  n = build_token("1", "u", 2000000000, "none", tok, sec);
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_TOKEN, "jwt alg none");
  n = build_token("abc", "u", 2000000000, "HS256", tok, sec);
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_PAYLOAD, "jwt bad sub");
  n = build_token("0", "u", 2000000000, "HS256", tok, sec);
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_PAYLOAD, "jwt sub 0");
  n = build_token("7", "a\"b\\c", 2000000000, "HS256", tok, sec);
  CHECK(jwt_verify(tok, (size_t)n, (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_OK
        && a.uid == 7 && a.ulen == 5 && !memcmp(a.uname, "a\"b\\c", 5), "jwt escaped user");
  CHECK(jwt_verify(SL("abc"), (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_TOKEN, "jwt garbage");
  CHECK(jwt_verify(SL("a.b.c"), (uint8_t*)sec, strlen(sec), 1000, &a, scratch, sizeof scratch) == JWT_BAD_TOKEN, "jwt short parts");
}

static void t_json(void) {
  char out[1024]; size_t bl;
  CHECK(json_extract_body(SL("{\"body\":\"hi\"}"), out, sizeof out, &bl) == JSON_OK && bl == 2 && !memcmp(out, "hi", 2), "json basic");
  CHECK(json_extract_body(SL(" { \"body\" : \" x \" } "), out, sizeof out, &bl) == JSON_OK && bl == 3, "json ws");
  CHECK(json_extract_body(SL("{\"body\":123}"), out, sizeof out, &bl) == JSON_NO_BODY, "json numeric body");
  CHECK(json_extract_body(SL("{}"), out, sizeof out, &bl) == JSON_NO_BODY, "json missing");
  CHECK(json_extract_body(SL("{bad json"), out, sizeof out, &bl) == JSON_MALFORMED, "json malformed");
  CHECK(json_extract_body(SL("{\"body\":\"hi\"}trailing"), out, sizeof out, &bl) == JSON_MALFORMED, "json trailing");
  CHECK(json_extract_body(SL("{\"a\":{\"body\":\"nested\"}}"), out, sizeof out, &bl) == JSON_NO_BODY, "json nested ignored");
  CHECK(json_extract_body(SL("{\"body\":\"caf\\u00e9 \\u2713\"}"), out, sizeof out, &bl) == JSON_OK
        && bl == 9 && !memcmp(out, "caf\xc3\xa9 \xe2\x9c\x93", 9), "json u escapes");
  CHECK(json_extract_body(SL("{\"body\":\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"}"), out, sizeof out, &bl) == JSON_OK, "json escapes");
  CHECK(json_extract_body(SL("{\"body\":\"\\ud800\"}"), out, sizeof out, &bl) == JSON_MALFORMED, "json lone surrogate");
  CHECK(json_extract_body(SL("{\"body\":\"\\ud83d\\ude00\"}"), out, sizeof out, &bl) == JSON_OK && bl == 4, "json surrogate pair");
  CHECK(json_extract_body(SL("[1,2,{\"x\":true},null]"), out, sizeof out, &bl) == JSON_NO_BODY, "json array");
  char esc[64];
  int el = json_escape(SL("a\"b\\c\x01" "d"), esc, sizeof esc);
  CHECK(el == 14 && !memcmp(esc, "a\\\"b\\\\c\\u0001d", 14), "json_escape %d", el);
  size_t chars = 0;
  CHECK(utf8_count_chars(SL("caf\xc3\xa9 \xe2\x9c\x93"), &chars) == 0 && chars == 6, "utf8 count %zu", chars);
  CHECK(utf8_count_chars(SL("\xff"), &chars) == -1, "utf8 invalid");
  CHECK(utf8_count_chars(SL("\xc0\xaf"), &chars) == -1, "utf8 overlong");
  CHECK(utf8_count_chars(SL(""), &chars) == 0 && chars == 0, "utf8 empty");
}


static void t_http(void) {
  char b[8192]; req_t r; int rc;
  // 1. simple GET
  strcpy(b, "GET /feed HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.mlen == 3 && !memcmp(r.method, "GET", 3)
        && r.plen == 5 && !memcmp(r.path, "/feed", 5)
        && r.body_len == 0 && r.consumed == strlen(b) && !r.conn_close, "http get");
  // 2. POST with auth + body
  strcpy(b, "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer abc.def.ghi\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}");
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.auth_len == 18 && !memcmp(r.auth, "Bearer abc.def.ghi", 18)
        && r.body_len == 12 && !memcmp(r.body, "{\"body\":\"x\"}", 12), "http post auth");
  // 3. split delivery
  strcpy(b, "GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n");
  rc = http_parse(b, 20, &r);
  CHECK(rc == 0, "http partial %d", rc);
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.plen == 8 && !memcmp(r.path, "/posts/1", 8), "http split");
  // 4. pipelined
  strcpy(b, "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n");
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.consumed == 28 && r.plen == 2, "http pipe1 %d %zu", rc, r.consumed);
  memmove(b, b + r.consumed, strlen(b) - r.consumed + 1);
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.plen == 2 && !memcmp(r.path, "/b", 2), "http pipe2");
  // 5. connection close + query strip
  strcpy(b, "GET /feed?x=1 HTTP/1.0\r\nConnection: close\r\n\r\n");
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.conn_close && r.plen == 5 && !memcmp(r.path, "/feed", 5), "http close+q");
  // 6. expect 100-continue, body pending
  strcpy(b, "POST /posts HTTP/1.1\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\nabc");
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 0 && r.headers_done == 1 && r.expect_continue == 1, "http 100-continue pending %d", rc);
  // 7. chunked
  strcpy(b, "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  rc = http_parse(b, strlen(b), &r);
  CHECK(rc == 1 && r.body_len == 11 && !memcmp(r.body, "hello world", 11), "http chunked %d %zu", rc, r.body_len);
  // 8. errors
  strcpy(b, "GET /x HTTP/2.0\r\n\r\n");
  CHECK(http_parse(b, strlen(b), &r) == -1, "http bad version");
  strcpy(b, "POST /p HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\na");
  CHECK(http_parse(b, strlen(b), &r) == -1, "http dup clen");
  strcpy(b, "POST /p HTTP/1.1\r\nContent-Length: 999999\r\n\r\n");
  CHECK(http_parse(b, strlen(b), &r) == -2, "http huge clen");
  strcpy(b, "GET /x HTTP/1.1\r\n");
  CHECK(http_parse(b, strlen(b), &r) == 0, "http incomplete");
}

int main(void) {
  t_sha256(); t_hmac(); t_b64(); t_jwt(); t_json(); t_http();
  if (fails == 0) printf("ALL UNIT TESTS PASSED\n");
  else printf("%d FAILURES\n", fails);
  return fails != 0;
}
