/* Randomized mutation testing of the parsers (run under ASAN/UBSAN). Seeds
 * are valid inputs; each iteration mutates one and checks that the parser
 * stays within its contract. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/crypto.h"
#include "../src/http.h"
#include "../src/json.h"
#include "../src/jwt.h"

static uint64_t rng = 0x243f6a8885a308d3ULL;
static uint64_t next(void) {
    rng ^= rng << 7;
    rng ^= rng >> 9;
    return rng;
}

static const char *http_seeds[] = {
    "GET /feed HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    "POST /posts HTTP/1.1\r\nContent-Length: 12\r\nAuthorization: Bearer a.b.c\r\n\r\n{\"body\":\"x\"}",
    "POST /posts/12/like HTTP/1.0\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n",
    "GET /posts/1 HTTP/1.1\r\nX-A: 1\r\nX-B: 2\r\nContent-Length: 0\r\n\r\nGET /feed HTTP/1.1\r\n\r\n",
};
static const char *json_seeds[] = {
    "{\"body\":\"hello \\\"world\\\" \\u00e9\\ud83d\\ude00\"}",
    "{\"a\":[1,2.5e-3,true,false,null,{\"b\":\"c\"}],\"body\":\"x\",\"n\":-0}",
    "[1,2,3]",
    "{\"sub\":\"42\",\"username\":\"name\",\"iat\":1700000000,\"exp\":2000000000}",
};

static void mutate(char *buf, size_t *len, size_t cap) {
    int kind = (int)(next() % 6);
    if (*len == 0) kind = 3;
    switch (kind) {
    case 0: buf[next() % *len] = (char)(next() & 0xff); break;          /* flip a byte */
    case 1: buf[next() % *len] = "\r\n :\"\\{}[]0"[next() % 12]; break; /* structural byte */
    case 2: {                                                           /* delete a range */
        size_t a = next() % *len, b = a + next() % 8;
        if (b > *len) b = *len;
        memmove(buf + a, buf + b, *len - b);
        *len -= b - a;
        break;
    }
    case 3: { /* insert bytes */
        size_t n = 1 + next() % 8;
        if (*len + n > cap) break;
        size_t a = next() % (*len + 1);
        memmove(buf + a + n, buf + a, *len - a);
        for (size_t i = 0; i < n; i++) buf[a + i] = (char)(next() & 0xff);
        *len += n;
        break;
    }
    case 4: { /* duplicate a range */
        size_t a = next() % *len, n = 1 + next() % 16;
        if (a + n > *len || *len + n > cap) break;
        memmove(buf + a + n, buf + a, *len - a);
        *len += n;
        break;
    }
    default: *len = next() % (*len + 1); break; /* truncate */
    }
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    if (iters <= 0) iters = 200000;
    struct hmac_key key;
    hmac_key_init(&key, "twelve-dollar-challenge", 23);
    char tok_seed[512];
    const char *pl = "{\"sub\":\"42\",\"username\":\"golden_ember_1\",\"iat\":1700000000,\"exp\":2000000000}";
    size_t tok_seed_len = jwt_sign(&key, pl, strlen(pl), tok_seed, sizeof tok_seed);
    char buf[4096];
    char out[4096];
    long ok_http = 0, ok_json = 0, ok_jwt = 0;
    for (long it = 0; it < iters; it++) {
        int which = (int)(it % 3);
        size_t len;
        if (which == 0) {
            const char *s = http_seeds[next() % 4];
            len = strlen(s);
            memcpy(buf, s, len);
            for (int m = (int)(next() % 4); m >= 0; m--) mutate(buf, &len, sizeof buf);
            struct http_req req;
            enum http_parse_status st = http_parse(buf, (uint32_t)len, 2048, 2048, &req);
            if (st == HTTP_COMPLETE) {
                if (req.total_len > len || req.head_len > req.total_len || req.path_len > len ||
                    (req.auth && req.auth_len > len) || req.body_len > 2048) {
                    fprintf(stderr, "http contract violated at iteration %ld\n", it);
                    return 1;
                }
                ok_http++;
            }
        } else if (which == 1) {
            const char *s = json_seeds[next() % 4];
            len = strlen(s);
            memcpy(buf, s, len);
            for (int m = (int)(next() % 4); m >= 0; m--) mutate(buf, &len, sizeof buf);
            char body[64];
            struct json_field f[] = {{.name = "body", .buf = body, .cap = sizeof body},
                                     {.name = "sub", .buf = out, .cap = 8}};
            enum json_status st = json_parse_object(buf, len, f, 2);
            if (st == JSON_OK) {
                if (f[0].len > sizeof body || f[1].len > 8) {
                    fprintf(stderr, "json contract violated at iteration %ld\n", it);
                    return 1;
                }
                ok_json++;
                size_t n = json_write_string(out, sizeof out, body, f[0].len);
                if (n > sizeof out) return 1;
            }
        } else {
            len = tok_seed_len;
            memcpy(buf, tok_seed, len);
            for (int m = (int)(next() % 3); m >= 0; m--) mutate(buf, &len, sizeof buf);
            struct jwt_claims c;
            enum jwt_status st = jwt_verify(&key, buf, len, 1800000000, &c);
            if (st == JWT_OK) {
                if (c.username_len > sizeof c.username || c.sub < 1) {
                    fprintf(stderr, "jwt contract violated at iteration %ld\n", it);
                    return 1;
                }
                ok_jwt++;
            }
        }
    }
    printf("fuzz: %ld iterations, accepted http=%ld json=%ld jwt=%ld\n", iters, ok_http, ok_json, ok_jwt);
    return 0;
}
