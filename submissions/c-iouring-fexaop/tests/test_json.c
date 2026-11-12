#include "../src/json.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static size_t mk_repeat(char *buf, size_t n, char c)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = c;
    return n;
}

static enum json_body_result extract(const char *json, char *out, size_t *out_len)
{
    return json_extract_body(json, strlen(json), out, 4096, out_len);
}

int main(void)
{
    char out[4096];
    size_t out_len = 0;
    enum json_body_result r;

    /* Special characters, trimming, UTF-8 passthrough. */
    const char *special =
        "{\"body\":\"  test: <b>&amp;</b> \\\"quoted\\\" / caf\xC3\xA9 "
        "\xE2\x9C\x93  \"}";
    const char *special_expect =
        "test: <b>&amp;</b> \"quoted\" / caf\xC3\xA9 \xE2\x9C\x93";
    r = extract(special, out, &out_len);
    check(r == JSON_BODY_OK && out_len == strlen(special_expect) &&
              memcmp(out, special_expect, out_len) == 0,
          "special characters trimmed round-trip");

    /* 500 a's accepted. */
    {
        char json[600];
        size_t p = 0;
        memcpy(json + p, "{\"body\":\"", 9);
        p += 9;
        p += mk_repeat(json + p, 500, 'a');
        memcpy(json + p, "\"}", 2);
        p += 2;
        json[p] = '\0';
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_OK && out_len == 500 && out[0] == 'a' &&
                  out[499] == 'a',
              "500 chars OK");
    }

    /* 501 a's rejected. */
    {
        char json[600];
        size_t p = 0;
        memcpy(json + p, "{\"body\":\"", 9);
        p += 9;
        p += mk_repeat(json + p, 501, 'a');
        memcpy(json + p, "\"}", 2);
        p += 2;
        json[p] = '\0';
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_TOO_LONG, "501 chars TOO_LONG");
    }

    /* Length after trimming: leading/trailing spaces do not count. */
    {
        char json[600];
        size_t p = 0;
        memcpy(json + p, "{\"body\":\"  ", 11);
        p += 11;
        p += mk_repeat(json + p, 500, 'b');
        memcpy(json + p, "  \"}", 4);
        p += 4;
        json[p] = '\0';
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_OK && out_len == 500, "500 after trim OK");
    }

    /* Missing / wrong-type / empty bodies. */
    r = extract("{\"body\":\"\"}", out, &out_len);
    check(r == JSON_BODY_MISSING, "empty body MISSING");
    r = extract("{\"body\":\"   \"}", out, &out_len);
    check(r == JSON_BODY_MISSING, "whitespace body MISSING");
    r = extract("{\"body\":123}", out, &out_len);
    check(r == JSON_BODY_MISSING, "numeric body MISSING");
    r = extract("{}", out, &out_len);
    check(r == JSON_BODY_MISSING, "missing body MISSING");
    r = extract("{\"body\":null}", out, &out_len);
    check(r == JSON_BODY_MISSING, "null body MISSING");

    /* Malformed documents. */
    r = extract("{bad json", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "malformed JSON");
    r = extract("{\"body\":\"x\"} garbage", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "trailing garbage");
    r = extract("{\"body\":\"x\"}{}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "second document");
    r = extract("{\"n\":01,\"body\":\"x\"}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "leading zero number");
    r = extract("{\"n\":1.,\"body\":\"x\"}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "number trailing dot");
    r = extract("{\"n\":1e,\"body\":\"x\"}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "number empty exponent");
    r = extract("{\"body\":\"x\",}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "trailing comma");

    /* Valid JSON that is not an object, or has no body. */
    r = extract("[1,2,3]", out, &out_len);
    check(r == JSON_BODY_MISSING, "top-level array MISSING");
    r = extract("\"hello\"", out, &out_len);
    check(r == JSON_BODY_MISSING, "top-level string MISSING");
    r = extract("123", out, &out_len);
    check(r == JSON_BODY_MISSING, "top-level number MISSING");
    r = extract("true", out, &out_len);
    check(r == JSON_BODY_MISSING, "top-level literal MISSING");

    /* Escapes: \u00e9 and a surrogate pair. */
    {
        const char *json = "{\"body\":\"caf\\u00e9 \\ud83d\\ude00\"}";
        const char *expect = "caf\xC3\xA9 \xF0\x9F\x98\x80";
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_OK && out_len == strlen(expect) &&
                  memcmp(out, expect, out_len) == 0,
              "unicode escapes and surrogate pair");
    }
    {
        const char *json = "{\"body\":\"\\u00e9\"}";
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_OK && out_len == 2 && out[0] == (char)0xC3 &&
                  out[1] == (char)0xA9,
              "\\u00e9 decodes to UTF-8");
    }
    r = extract("{\"body\":\"\\ud800\"}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "lone high surrogate malformed");
    r = extract("{\"body\":\"\\udc00\"}", out, &out_len);
    check(r == JSON_BODY_MALFORMED, "lone low surrogate malformed");

    /* Duplicate body: last one wins. */
    r = extract("{\"body\":\"first\",\"body\":\"second\"}", out, &out_len);
    check(r == JSON_BODY_OK && out_len == 6 &&
              memcmp(out, "second", 6) == 0,
          "duplicate body last wins");
    {
        const char *json =
            "{\"body\":\"first\",\"x\":1,\"body\":\"  last  \"}";
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_OK && out_len == 4 &&
                  memcmp(out, "last", 4) == 0,
              "duplicate body last wins with trim");
    }

    /* Nested arrays/objects are validated and parsed. */
    {
        const char *json =
            "{\"x\":{\"a\":[1,-2,3.5,1e10,true,false,null,\"s\"]},"
            "\"body\":\"ok\"}";
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_OK && out_len == 2 && memcmp(out, "ok", 2) == 0,
              "nested structures valid");
    }

    /* Whitespace after the document is allowed. */
    r = extract("{\"body\":\"x\"}   \n\t\r ", out, &out_len);
    check(r == JSON_BODY_OK && out_len == 1 && out[0] == 'x',
          "trailing whitespace allowed");

    /* Nesting depth limit: 64 levels valid, 65 malformed. */
    {
        char json[200];
        size_t p = 0;
        p += mk_repeat(json + p, 64, '[');
        json[p++] = '0';
        p += mk_repeat(json + p, 64, ']');
        json[p] = '\0';
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_MISSING, "64-level nesting valid");

        p = 0;
        p += mk_repeat(json + p, 65, '[');
        json[p++] = '0';
        p += mk_repeat(json + p, 65, ']');
        json[p] = '\0';
        r = extract(json, out, &out_len);
        check(r == JSON_BODY_MALFORMED, "65-level nesting malformed");
    }

    /* Scope: body key must be top-level. */
    r = extract("{\"x\":{\"body\":\"nested\"},\"body\":\"top\"}", out,
                &out_len);
    check(r == JSON_BODY_OK && out_len == 3 && memcmp(out, "top", 3) == 0,
          "only top-level body considered");

    /* json_escape. */
    {
        char dst[64];
        static const unsigned char src[] = {'"', '\\', '\n', 0xC3, 0xA9};
        static const unsigned char expect[] = {'\\', '"', '\\', '\\', '\\',
                                               'n',  0xC3, 0xA9};
        size_t n = json_escape(dst, (const char *)src, sizeof src);
        check(n == sizeof expect && memcmp(dst, expect, n) == 0,
              "json_escape quote/backslash/newline/utf8");
    }
    {
        char dst[64];
        static const unsigned char src[] = {'<', '>', '&', '/', ' ', 0x01};
        static const unsigned char expect[] = {'<',  '>',  '&',  '/',
                                               ' ',  '\\', 'u',  '0',
                                               '0',  '0',  '1'};
        size_t n = json_escape(dst, (const char *)src, sizeof src);
        check(n == sizeof expect && memcmp(dst, expect, n) == 0,
              "json_escape passthrough and \\u00xx control");
    }

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
