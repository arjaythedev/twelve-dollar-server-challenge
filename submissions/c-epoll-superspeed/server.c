// c-epoll-superspeed: a single-threaded epoll + SQLite feed server for the $12 Server Challenge.
//
// Design:
//   * one process, one thread, one epoll loop (the box has one vCPU)
//   * hand-written HTTP/1.1, JSON, JWT (HS256) and base64url code
//   * SQLite amalgamation compiled in, prepared statements, WAL + synchronous=NORMAL
//   * group commit: one transaction per epoll batch, response sent only after COMMIT
//   * a forked helper process runs PASSIVE WAL checkpoints off the request path
//   * OPTIONAL: WORKERS=N forks N processes sharing the port with SO_REUSEPORT
//
// No caching across requests: every request runs its own SQL and verifies its own token.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"

// ---------------------------------------------------------------------------
// tunables
// ---------------------------------------------------------------------------
#define MAX_EVENTS      512
#define MAX_RESPS       4096
#define ARENA_SIZE      (16u * 1024u * 1024u)
#define ARENA_RESERVE   (128u * 1024u)
#define HDR_RESERVE     256
#define MAX_HTTP_BODY   65536
#define MAX_CONN_IN     65536

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
#define PUTLIT(p, lit) do { memcpy((p), (lit), sizeof(lit) - 1); (p) += sizeof(lit) - 1; } while (0)

static char *write_u64(char *p, uint64_t v) {
    char tmp[20];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *write_i64(char *p, int64_t v) {
    if (v < 0) { *p++ = '-'; return write_u64(p, (uint64_t)(-(v + 1)) + 1u); }
    return write_u64(p, (uint64_t)v);
}

static int is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static size_t count_cp(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static const char HEXD[] = "0123456789abcdef";

static int needs_json_escape(const char *s, size_t n) {
#if defined(__SSE2__)
    size_t i = 0;
    const __m128i q = _mm_set1_epi8('"');
    const __m128i b = _mm_set1_epi8('\\');
    const __m128i ctl = _mm_set1_epi8(0x20);
    const __m128i sign = _mm_set1_epi8((char)0x80);
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, b));
        __m128i vs = _mm_xor_si128(v, sign);
        __m128i cs = _mm_cmplt_epi8(vs, _mm_xor_si128(ctl, sign));
        m = _mm_or_si128(m, cs);
        if (_mm_movemask_epi8(m)) return 1;
    }
    for (; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '"' || c == '\\') return 1;
    }
    return 0;
#else
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '"' || c == '\\') return 1;
    }
    return 0;
#endif
}

static size_t json_escape(char *d, const char *s, size_t n) {
    if (!needs_json_escape(s, n)) { memcpy(d, s, n); return n; }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  d[o++] = '\\'; d[o++] = '"';  break;
            case '\\': d[o++] = '\\'; d[o++] = '\\'; break;
            case '\b': d[o++] = '\\'; d[o++] = 'b';  break;
            case '\f': d[o++] = '\\'; d[o++] = 'f';  break;
            case '\n': d[o++] = '\\'; d[o++] = 'n';  break;
            case '\r': d[o++] = '\\'; d[o++] = 'r';  break;
            case '\t': d[o++] = '\\'; d[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    d[o++] = '\\'; d[o++] = 'u'; d[o++] = '0'; d[o++] = '0';
                    d[o++] = HEXD[c >> 4]; d[o++] = HEXD[c & 15];
                } else {
                    d[o++] = (char)c;
                }
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// SHA-256 + HMAC-SHA256
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
} sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c; c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    size_t idx = (size_t)(c->len & 63);
    c->len += len;
    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) { memcpy(c->buf + idx, p, len); return; }
        memcpy(c->buf + idx, p, fill);
        p += fill; len -= fill;
        sha256_block(c, c->buf);
    }
    while (len >= 64) { sha256_block(c, p); p += 64; len -= 64; }
    if (len) memcpy(c->buf, p, len);
}

static void sha256_final(sha256_ctx *c, unsigned char out[32]) {
    uint64_t bits = c->len * 8;
    size_t idx = (size_t)(c->len & 63);
    c->buf[idx++] = 0x80;
    if (idx > 56) {
        memset(c->buf + idx, 0, 64 - idx);
        sha256_block(c, c->buf);
        idx = 0;
    }
    memset(c->buf + idx, 0, 56 - idx);
    for (int i = 0; i < 8; i++) c->buf[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

typedef struct {
    sha256_ctx inner;
    sha256_ctx outer;
} hmac_state;

static void hmac_prepare(hmac_state *hs, const unsigned char *key, size_t keylen) {
    unsigned char k[64];
    memset(k, 0, sizeof(k));
    if (keylen > 64) {
        sha256_ctx t;
        sha256_init(&t);
        sha256_update(&t, key, keylen);
        sha256_final(&t, k);
    } else {
        memcpy(k, key, keylen);
    }
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha256_init(&hs->inner);
    sha256_update(&hs->inner, ipad, 64);
    sha256_init(&hs->outer);
    sha256_update(&hs->outer, opad, 64);
}

static void hmac_compute(const hmac_state *hs, const void *msg, size_t len, unsigned char out[32]) {
    sha256_ctx c = hs->inner;
    sha256_update(&c, msg, len);
    unsigned char inner[32];
    sha256_final(&c, inner);
    c = hs->outer;
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

static int ct_eq(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

// ---------------------------------------------------------------------------
// base64url
// ---------------------------------------------------------------------------
static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static int b64url_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap, size_t *outlen) {
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        int c = (unsigned char)in[i];
        if (c == '=') break;
        int v = b64v(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outcap) return -1;
            out[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *outlen = n;
    return 0;
}

// ---------------------------------------------------------------------------
// minimal JSON parser (enough for JWT payloads and {"body": "..."})
// ---------------------------------------------------------------------------
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} jp;

typedef struct {
    int is_obj;
    int has_alg; char alg[16]; size_t alg_len; int alg_is_string;
    int has_sub; char sub[32]; size_t sub_len; int sub_is_string;
    int has_username; char username[1024]; size_t username_len; int username_is_string;
    int has_exp; int64_t exp; int exp_is_int;
    int has_body; char body[20480]; size_t body_len; int body_is_string; int body_overflow;
} jfields;

static jfields JF;
static jfields HF;

static void jf_reset(jfields *f) {
    f->is_obj = 0;
    f->has_alg = 0; f->alg_len = 0; f->alg_is_string = 0;
    f->has_sub = 0; f->sub_len = 0; f->sub_is_string = 0;
    f->has_username = 0; f->username_len = 0; f->username_is_string = 0;
    f->has_exp = 0; f->exp = 0; f->exp_is_int = 0;
    f->has_body = 0; f->body_len = 0; f->body_is_string = 0; f->body_overflow = 0;
}

static void jp_ws(jp *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// returns 0 on success; *outlen is the full decoded length (out beyond cap is dropped)
static int jp_string(jp *p, char *out, size_t cap, size_t *outlen) {
    if (p->pos >= p->len || p->s[p->pos] != '"') return -1;
    p->pos++;
    size_t n = 0;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos++];
        if (c == '"') { *outlen = n; return 0; }
        if (c == '\\') {
            if (p->pos >= p->len) return -1;
            unsigned char e = (unsigned char)p->s[p->pos++];
            unsigned char uc;
            switch (e) {
                case '"':  uc = '"';  break;
                case '\\': uc = '\\'; break;
                case '/':  uc = '/';  break;
                case 'b':  uc = '\b'; break;
                case 'f':  uc = '\f'; break;
                case 'n':  uc = '\n'; break;
                case 'r':  uc = '\r'; break;
                case 't':  uc = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) return -1;
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hexval((unsigned char)p->s[p->pos++]);
                        if (h < 0) return -1;
                        cp = (cp << 4) | (uint32_t)h;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                            p->pos += 2;
                            uint32_t lo = 0;
                            for (int i = 0; i < 4; i++) {
                                int h = hexval((unsigned char)p->s[p->pos++]);
                                if (h < 0) return -1;
                                lo = (lo << 4) | (uint32_t)h;
                            }
                            if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            return -1;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return -1;
                    }
                    unsigned char u8[4];
                    size_t k = 0;
                    if (cp < 0x80) {
                        u8[k++] = (unsigned char)cp;
                    } else if (cp < 0x800) {
                        u8[k++] = (unsigned char)(0xC0 | (cp >> 6));
                        u8[k++] = (unsigned char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        u8[k++] = (unsigned char)(0xE0 | (cp >> 12));
                        u8[k++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                        u8[k++] = (unsigned char)(0x80 | (cp & 0x3F));
                    } else {
                        u8[k++] = (unsigned char)(0xF0 | (cp >> 18));
                        u8[k++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                        u8[k++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                        u8[k++] = (unsigned char)(0x80 | (cp & 0x3F));
                    }
                    for (size_t i = 0; i < k; i++) { if (n < cap) out[n] = (char)u8[i]; n++; }
                    continue;
                }
                default: return -1;
            }
            if (n < cap) out[n] = (char)uc;
            n++;
            continue;
        }
        if (c < 0x20) return -1;
        if (n < cap) out[n] = (char)c;
        n++;
    }
    return -1;
}

static int jp_number(jp *p, int64_t *out, int *is_int) {
    size_t start = p->pos;
    if (p->pos >= p->len) return -1;
    if (p->s[p->pos] == '-') p->pos++;
    if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') return -1;
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    int ii = 1;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        ii = 0; p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') return -1;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        ii = 0; p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') return -1;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    *is_int = ii;
    if (ii && out) {
        int neg = 0;
        size_t i = start;
        if (p->s[i] == '-') { neg = 1; i++; }
        int64_t v = 0;
        for (; i < p->pos; i++) {
            if (v > (INT64_MAX - 9) / 10) { *is_int = 0; return 0; }
            v = v * 10 + (p->s[i] - '0');
        }
        *out = neg ? -v : v;
    }
    return 0;
}

static int jp_skip_value(jp *p, int depth) {
    if (depth > 32) return -1;
    jp_ws(p);
    if (p->pos >= p->len) return -1;
    unsigned char c = (unsigned char)p->s[p->pos];
    if (c == '"') { size_t n; return jp_string(p, NULL, 0, &n); }
    if (c == '{' || c == '[') {
        int isobj = (c == '{');
        p->pos++;
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == (isobj ? '}' : ']')) { p->pos++; return 0; }
        for (;;) {
            if (isobj) {
                size_t k;
                if (jp_string(p, NULL, 0, &k) != 0) return -1;
                jp_ws(p);
                if (p->pos >= p->len || p->s[p->pos] != ':') return -1;
                p->pos++;
            }
            if (jp_skip_value(p, depth + 1) != 0) return -1;
            jp_ws(p);
            if (p->pos >= p->len) return -1;
            if (p->s[p->pos] == ',') { p->pos++; jp_ws(p); continue; }
            if (p->s[p->pos] == (isobj ? '}' : ']')) { p->pos++; return 0; }
            return -1;
        }
    }
    if (c == 't') { if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) { p->pos += 4; return 0; } return -1; }
    if (c == 'f') { if (p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return 0; } return -1; }
    if (c == 'n') { if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0) { p->pos += 4; return 0; } return -1; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        int64_t v; int ii;
        return jp_number(p, &v, &ii);
    }
    return -1;
}

// returns 1 for a valid object, 0 for a valid non-object, -1 for malformed
static int json_fields(const char *s, size_t len, jfields *f) {
    jf_reset(f);
    jp p = { s, len, 0 };
    jp_ws(&p);
    if (p.pos >= p.len) return -1;
    if (p.s[p.pos] != '{') {
        if (jp_skip_value(&p, 0) != 0) return -1;
        jp_ws(&p);
        return p.pos == p.len ? 0 : -1;
    }
    f->is_obj = 1;
    p.pos++;
    jp_ws(&p);
    if (p.pos < p.len && p.s[p.pos] == '}') {
        p.pos++; jp_ws(&p);
        return p.pos == p.len ? 1 : -1;
    }
    for (;;) {
        char key[64];
        size_t klen;
        if (jp_string(&p, key, sizeof(key), &klen) != 0) return -1;
        jp_ws(&p);
        if (p.pos >= p.len || p.s[p.pos] != ':') return -1;
        p.pos++;
        jp_ws(&p);
        if (klen == 3 && memcmp(key, "alg", 3) == 0) {
            f->has_alg = 1;
            if (p.pos < p.len && p.s[p.pos] == '"') {
                f->alg_is_string = 1;
                if (jp_string(&p, f->alg, sizeof(f->alg), &f->alg_len) != 0) return -1;
            } else {
                if (jp_skip_value(&p, 0) != 0) return -1;
            }
        } else if (klen == 3 && memcmp(key, "sub", 3) == 0) {
            f->has_sub = 1;
            if (p.pos < p.len && p.s[p.pos] == '"') {
                f->sub_is_string = 1;
                if (jp_string(&p, f->sub, sizeof(f->sub), &f->sub_len) != 0) return -1;
            } else {
                if (jp_skip_value(&p, 0) != 0) return -1;
            }
        } else if (klen == 8 && memcmp(key, "username", 8) == 0) {
            f->has_username = 1;
            if (p.pos < p.len && p.s[p.pos] == '"') {
                f->username_is_string = 1;
                if (jp_string(&p, f->username, sizeof(f->username), &f->username_len) != 0) return -1;
            } else {
                if (jp_skip_value(&p, 0) != 0) return -1;
            }
        } else if (klen == 3 && memcmp(key, "exp", 3) == 0) {
            f->has_exp = 1;
            int64_t v; int ii;
            if (jp_number(&p, &v, &ii) != 0) return -1;
            if (ii) { f->exp_is_int = 1; f->exp = v; }
        } else if (klen == 4 && memcmp(key, "body", 4) == 0) {
            f->has_body = 1;
            if (p.pos < p.len && p.s[p.pos] == '"') {
                f->body_is_string = 1;
                size_t n;
                if (jp_string(&p, f->body, sizeof(f->body), &n) != 0) return -1;
                f->body_len = n;
                f->body_overflow = (n > sizeof(f->body));
            } else {
                if (jp_skip_value(&p, 0) != 0) return -1;
            }
        } else {
            if (jp_skip_value(&p, 0) != 0) return -1;
        }
        jp_ws(&p);
        if (p.pos >= p.len) return -1;
        if (p.s[p.pos] == ',') { p.pos++; jp_ws(&p); continue; }
        if (p.s[p.pos] == '}') { p.pos++; jp_ws(&p); return p.pos == p.len ? 1 : -1; }
        return -1;
    }
}

// ---------------------------------------------------------------------------
// JWT
// ---------------------------------------------------------------------------
typedef struct {
    int64_t user_id;
    char username[1024];
    size_t username_len;
} auth_t;

static hmac_state g_hmac;

// 0 = ok, 1 = invalid/expired token, 2 = invalid payload
static int verify_jwt(const char *tok, size_t len, auth_t *a) {
    const char *d1 = (const char *)memchr(tok, '.', len);
    if (!d1) return 1;
    const char *d2 = (const char *)memchr(d1 + 1, '.', (size_t)(tok + len - (d1 + 1)));
    if (!d2) return 1;
    if (memchr(d2 + 1, '.', (size_t)(tok + len - (d2 + 1)))) return 1;

    size_t hlen = (size_t)(d1 - tok);
    size_t plen = (size_t)(d2 - d1 - 1);
    size_t slen = (size_t)(tok + len - (d2 + 1));

    unsigned char hdr[512], sig[64], pay[2048];
    size_t hdrlen, siglen, paylen;
    if (b64url_decode(tok, hlen, hdr, sizeof(hdr), &hdrlen) != 0) return 1;
    if (b64url_decode(d2 + 1, slen, sig, sizeof(sig), &siglen) != 0) return 1;
    if (siglen != 32) return 1;

    if (json_fields((const char *)hdr, hdrlen, &HF) != 1) return 1;
    if (!HF.has_alg || !HF.alg_is_string || HF.alg_len != 5 || memcmp(HF.alg, "HS256", 5) != 0) return 1;

    unsigned char mac[32];
    hmac_compute(&g_hmac, tok, (size_t)(d2 - tok), mac);
    if (!ct_eq(mac, sig, 32)) return 1;

    if (b64url_decode(d1 + 1, plen, pay, sizeof(pay), &paylen) != 0) return 1;
    if (json_fields((const char *)pay, paylen, &JF) != 1) return 1;
    if (!JF.has_exp || !JF.exp_is_int) return 1;
    if (JF.exp <= (int64_t)time(NULL)) return 1;

    if (!JF.has_sub || !JF.sub_is_string || JF.sub_len == 0 || JF.sub_len > 18) return 2;
    int64_t uid = 0;
    for (size_t i = 0; i < JF.sub_len; i++) {
        if (JF.sub[i] < '0' || JF.sub[i] > '9') return 2;
        uid = uid * 10 + (JF.sub[i] - '0');
    }
    if (uid <= 0) return 2;
    if (!JF.has_username || !JF.username_is_string || JF.username_len == 0 ||
        JF.username_len >= sizeof(a->username)) return 2;

    memcpy(a->username, JF.username, JF.username_len);
    a->username_len = JF.username_len;
    a->user_id = uid;
    return 0;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
typedef struct {
    const char *method; size_t method_len;
    const char *path;   size_t path_len;
    const char *body;   size_t body_len;
    const char *auth;   size_t auth_len;
    int has_auth;
    int minor;
    int close;
} http_req;

static int method_is(const http_req *r, const char *m) {
    size_t n = strlen(m);
    return r->method_len == n && memcmp(r->method, m, n) == 0;
}
static int path_eq(const http_req *r, const char *p) {
    size_t n = strlen(p);
    return r->path_len == n && memcmp(r->path, p, n) == 0;
}
static int path_prefix(const http_req *r, const char *p) {
    size_t n = strlen(p);
    return r->path_len >= n && memcmp(r->path, p, n) == 0;
}
static int hdr_eq(const char *p, size_t n, const char *name) {
    size_t m = strlen(name);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) {
        int a = (unsigned char)p[i], b = (unsigned char)name[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static const char *find_header_end(const char *d, size_t len) {
    if (len < 4) return NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (d[i] == '\r' && d[i + 1] == '\n' && d[i + 2] == '\r' && d[i + 3] == '\n')
            return d + i;
    return NULL;
}

// 1 = complete, 0 = need more, -1 = bad request, -2 = unsupported/too large
static int parse_req(const char *d, size_t len, http_req *r, size_t *consumed, int *need_more) {
    *need_more = 0;
    const char *he = find_header_end(d, len);
    if (!he) { *need_more = 1; return 0; }
    size_t hlen = (size_t)(he - d) + 4;

    memset(r, 0, sizeof(*r));
    const char *p = d, *end = he + 2;
    const char *sp1 = (const char *)memchr(p, ' ', (size_t)(end - p));
    if (!sp1) return -1;
    const char *sp2 = (const char *)memchr(sp1 + 1, ' ', (size_t)(end - (sp1 + 1)));
    if (!sp2) return -1;
    r->method = p;
    r->method_len = (size_t)(sp1 - p);
    r->path = sp1 + 1;
    r->path_len = (size_t)(sp2 - (sp1 + 1));
    if (sp2 + 9 > end || memcmp(sp2 + 1, "HTTP/1.", 7) != 0) return -1;
    r->minor = sp2[8] - '0';

    const char *q = (const char *)memchr(r->path, '?', r->path_len);
    if (q) r->path_len = (size_t)(q - r->path);

    const char *eol = (const char *)memmem(sp2, (size_t)(end - sp2), "\r\n", 2);
    if (!eol) return -1;
    p = eol + 2;

    long clen = 0;
    int has_clen = 0;
    while (p < end) {
        if (p + 1 < end && p[0] == '\r' && p[1] == '\n') break;
        const char *e = (const char *)memmem(p, (size_t)(end - p), "\r\n", 2);
        if (!e) return -1;
        const char *colon = (const char *)memchr(p, ':', (size_t)(e - p));
        if (!colon) return -1;
        size_t nlen = (size_t)(colon - p);
        const char *v = colon + 1, *ve = e;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vlen = (size_t)(ve - v);

        if (hdr_eq(p, nlen, "authorization")) {
            r->has_auth = 1; r->auth = v; r->auth_len = vlen;
        } else if (hdr_eq(p, nlen, "content-length")) {
            long x = 0;
            if (vlen == 0) return -1;
            for (const char *z = v; z < ve; z++) {
                if (*z < '0' || *z > '9') return -1;
                x = x * 10 + (*z - '0');
                if (x > MAX_HTTP_BODY) return -2;
            }
            clen = x; has_clen = 1;
        } else if (hdr_eq(p, nlen, "transfer-encoding")) {
            if (vlen >= 7 && strncasecmp(v, "chunked", 7) == 0) return -2;
        } else if (hdr_eq(p, nlen, "connection")) {
            if (vlen == 5 && strncasecmp(v, "close", 5) == 0) r->close = 1;
        }
        p = e + 2;
    }
    (void)has_clen;
    if (len < hlen + (size_t)clen) { *need_more = 1; return 0; }
    r->body = d + hlen;
    r->body_len = (size_t)clen;
    *consumed = hlen + (size_t)clen;
    return 1;
}

// ---------------------------------------------------------------------------
// server state
// ---------------------------------------------------------------------------
typedef struct Conn Conn;
struct Conn {
    int fd;
    uint32_t events;
    char *in;
    uint32_t in_len, in_cap;
    char *out;
    uint32_t out_len, out_off, out_cap;
    Conn *dead_next;
    uint8_t dead;
};

typedef struct {
    Conn *c;
    uint32_t off;
    uint32_t len;
} resp_t;

static sqlite3 *db;
static sqlite3_stmt *feed_stmt, *get_stmt, *ins_post_stmt, *like_stmt, *post_exists_stmt, *health_stmt;

static int epfd = -1, lfd = -1;
static Conn **conns;
static int max_fd;
static Conn *dead_head;
static char rbuf[65536];
static char *arena;
static size_t arena_used;
static resp_t resps[MAX_RESPS];
static int resp_n;
static int in_txn;
static time_t start_time;
static const char *g_db_path, *g_host;
static int g_port, g_workers;

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void close_conn(Conn *c);

// ---------------------------------------------------------------------------
// response arena
// ---------------------------------------------------------------------------
static void send_resp(resp_t *r);

static void flush_batch(void) {
    if (in_txn) {
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "commit failed: %s\n", sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            in_txn = 0;
            for (int i = 0; i < resp_n; i++) {
                Conn *c = resps[i].c;
                if (c && !c->dead) close_conn(c);
            }
            resp_n = 0;
            arena_used = 0;
            return;
        }
        in_txn = 0;
    }
    for (int i = 0; i < resp_n; i++) send_resp(&resps[i]);
    resp_n = 0;
    arena_used = 0;
}

static int resp_reserve(Conn *c) {
    if (resp_n >= MAX_RESPS || arena_used + ARENA_RESERVE > ARENA_SIZE) {
        flush_batch();
        if (c->dead) return -1;
    }
    return 0;
}

static char *resp_body_ptr(void) { return arena + arena_used + HDR_RESERVE; }

static const char *status_text(int s) {
    switch (s) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

static void append_response(Conn *c, int status, size_t body_len) {
    char *base = arena + arena_used;
    char *body = base + HDR_RESERVE;
    char hdr[192];
    char *h = hdr;
    PUTLIT(h, "HTTP/1.1 ");
    h = write_u64(h, (uint64_t)status);
    *h++ = ' ';
    const char *st = status_text(status);
    size_t sl = strlen(st);
    memcpy(h, st, sl);
    h += sl;
    PUTLIT(h, "\r\nContent-Type: application/json\r\nContent-Length: ");
    h = write_u64(h, (uint64_t)body_len);
    PUTLIT(h, "\r\nConnection: keep-alive\r\n\r\n");
    size_t hlen = (size_t)(h - hdr);
    if (hlen > HDR_RESERVE) { close_conn(c); return; }
    memmove(base + hlen, body, body_len);
    memcpy(base, hdr, hlen);
    resps[resp_n].c = c;
    resps[resp_n].off = (uint32_t)arena_used;
    resps[resp_n].len = (uint32_t)(hlen + body_len);
    resp_n++;
    arena_used += hlen + body_len;
}

static void send_error(Conn *c, int status, const char *msg) {
    if (resp_reserve(c) < 0) return;
    char *base = resp_body_ptr();
    char *w = base;
    PUTLIT(w, "{\"error\":\"");
    size_t n = strlen(msg);
    memcpy(w, msg, n);
    w += n;
    PUTLIT(w, "\"}");
    append_response(c, status, (size_t)(w - base));
}

// ---------------------------------------------------------------------------
// post serialization
// ---------------------------------------------------------------------------
static size_t write_post(char *w, int64_t id, const char *body, int blen,
                         const char *created, int clen, const char *author,
                         int alen, int64_t likes) {
    char *s = w;
    PUTLIT(s, "{\"id\":");
    s = write_i64(s, id);
    PUTLIT(s, ",\"body\":\"");
    s += json_escape(s, body ? body : "", blen > 0 ? (size_t)blen : 0);
    PUTLIT(s, "\",\"created_at\":\"");
    if (created && clen > 0) { memcpy(s, created, (size_t)clen); s += clen; }
    PUTLIT(s, "\",\"author\":\"");
    s += json_escape(s, author ? author : "", alen > 0 ? (size_t)alen : 0);
    PUTLIT(s, "\",\"like_count\":");
    s = write_i64(s, likes);
    *s++ = '}';
    return (size_t)(s - w);
}

// ---------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------
static void set_events(Conn *c, uint32_t ev);

static void handle_feed(Conn *c) {
    if (resp_reserve(c) < 0) return;
    char *base = resp_body_ptr();
    char *w = base;
    PUTLIT(w, "{\"posts\":[");
    sqlite3_reset(feed_stmt);
    int rc, n = 0;
    while ((rc = sqlite3_step(feed_stmt)) == SQLITE_ROW) {
        if (n++) *w++ = ',';
        const char *body = (const char *)sqlite3_column_text(feed_stmt, 1);
        int blen = sqlite3_column_bytes(feed_stmt, 1);
        const char *created = (const char *)sqlite3_column_text(feed_stmt, 2);
        int clen = sqlite3_column_bytes(feed_stmt, 2);
        const char *author = (const char *)sqlite3_column_text(feed_stmt, 3);
        int alen = sqlite3_column_bytes(feed_stmt, 3);
        w += write_post(w, sqlite3_column_int64(feed_stmt, 0), body, blen,
                        created, clen, author, alen, sqlite3_column_int64(feed_stmt, 4));
    }
    sqlite3_reset(feed_stmt);
    if (rc != SQLITE_DONE) { send_error(c, 500, "internal server error"); return; }
    PUTLIT(w, "]}");
    append_response(c, 200, (size_t)(w - base));
}

static void handle_get_post(Conn *c, int64_t id) {
    if (resp_reserve(c) < 0) return;
    sqlite3_reset(get_stmt);
    sqlite3_bind_int64(get_stmt, 1, id);
    int rc = sqlite3_step(get_stmt);
    if (rc == SQLITE_ROW) {
        char *base = resp_body_ptr();
        char *w = base;
        PUTLIT(w, "{\"post\":");
        const char *body = (const char *)sqlite3_column_text(get_stmt, 1);
        int blen = sqlite3_column_bytes(get_stmt, 1);
        const char *created = (const char *)sqlite3_column_text(get_stmt, 2);
        int clen = sqlite3_column_bytes(get_stmt, 2);
        const char *author = (const char *)sqlite3_column_text(get_stmt, 3);
        int alen = sqlite3_column_bytes(get_stmt, 3);
        w += write_post(w, sqlite3_column_int64(get_stmt, 0), body, blen,
                        created, clen, author, alen, sqlite3_column_int64(get_stmt, 4));
        *w++ = '}';
        sqlite3_reset(get_stmt);
        append_response(c, 200, (size_t)(w - base));
    } else {
        sqlite3_reset(get_stmt);
        if (rc == SQLITE_DONE) send_error(c, 404, "post not found");
        else send_error(c, 500, "internal server error");
    }
}

static int ensure_txn(void) {
    if (in_txn) return 0;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    in_txn = 1;
    return 0;
}

// Short write transaction: commit immediately after the write, before the response
// is queued. Multiple worker processes then never hold the write lock across reads.
static int commit_txn(void) {
    if (!in_txn) return 0;
    int rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    in_txn = 0;
    if (rc != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static void handle_create(Conn *c, http_req *r, auth_t *a) {
    int jr = json_fields(r->body, r->body_len, &JF);
    if (jr < 0) { send_error(c, 400, "malformed JSON body"); return; }
    if (jr != 1 || !JF.has_body || !JF.body_is_string) { send_error(c, 400, "body is required"); return; }
    if (JF.body_overflow) { send_error(c, 400, "body must be at most 500 characters"); return; }

    size_t b0 = 0, b1 = JF.body_len;
    while (b0 < b1 && is_ws((unsigned char)JF.body[b0])) b0++;
    while (b1 > b0 && is_ws((unsigned char)JF.body[b1 - 1])) b1--;
    size_t blen = b1 - b0;
    if (blen == 0) { send_error(c, 400, "body is required"); return; }
    if (count_cp(JF.body + b0, blen) > 500) {
        send_error(c, 400, "body must be at most 500 characters");
        return;
    }

    if (resp_reserve(c) < 0) return;
    if (ensure_txn() < 0) { send_error(c, 500, "internal server error"); return; }
    sqlite3_reset(ins_post_stmt);
    sqlite3_bind_int64(ins_post_stmt, 1, a->user_id);
    sqlite3_bind_text(ins_post_stmt, 2, JF.body + b0, (int)blen, SQLITE_STATIC);
    int rc = sqlite3_step(ins_post_stmt);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(ins_post_stmt, 0);
        const char *created = (const char *)sqlite3_column_text(ins_post_stmt, 1);
        int clen = sqlite3_column_bytes(ins_post_stmt, 1);
        char *base = resp_body_ptr();
        char *w = base;
        PUTLIT(w, "{\"post\":");
        w += write_post(w, id, JF.body + b0, (int)blen, created, clen,
                        a->username, (int)a->username_len, 0);
        *w++ = '}';
        sqlite3_reset(ins_post_stmt);
        if (commit_txn() != 0) { send_error(c, 500, "internal server error"); return; }
        append_response(c, 201, (size_t)(w - base));
    } else {
        sqlite3_reset(ins_post_stmt);
        send_error(c, 500, "internal server error");
    }
}

static void handle_like(Conn *c, auth_t *a, int64_t id) {
    if (resp_reserve(c) < 0) return;
    if (ensure_txn() < 0) { send_error(c, 500, "internal server error"); return; }
    sqlite3_reset(like_stmt);
    sqlite3_bind_int64(like_stmt, 1, a->user_id);
    sqlite3_bind_int64(like_stmt, 2, id);
    int rc = sqlite3_step(like_stmt);
    int changes = sqlite3_changes(db);
    sqlite3_reset(like_stmt);
    if (rc != SQLITE_DONE) { send_error(c, 500, "internal server error"); return; }

    int status, already;
    if (changes > 0) {
        status = 201;
        already = 0;
    } else {
        sqlite3_reset(post_exists_stmt);
        sqlite3_bind_int64(post_exists_stmt, 1, id);
        int erc = sqlite3_step(post_exists_stmt);
        sqlite3_reset(post_exists_stmt);
        if (erc == SQLITE_ROW) { status = 200; already = 1; }
        else if (erc == SQLITE_DONE) { send_error(c, 404, "post not found"); return; }
        else { send_error(c, 500, "internal server error"); return; }
    }

    if (commit_txn() != 0) { send_error(c, 500, "internal server error"); return; }

    char *base = resp_body_ptr();
    char *w = base;
    PUTLIT(w, "{\"liked\":true,\"already_liked\":");
    if (already) PUTLIT(w, "true"); else PUTLIT(w, "false");
    PUTLIT(w, ",\"post_id\":");
    w = write_i64(w, id);
    *w++ = '}';
    append_response(c, status, (size_t)(w - base));
}

static void handle_health(Conn *c) {
    if (resp_reserve(c) < 0) return;
    sqlite3_reset(health_stmt);
    int rc = sqlite3_step(health_stmt);
    sqlite3_reset(health_stmt);
    if (rc == SQLITE_ROW) {
        char *base = resp_body_ptr();
        char *w = base;
        PUTLIT(w, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        w = write_i64(w, (int64_t)(time(NULL) - start_time));
        *w++ = '}';
        append_response(c, 200, (size_t)(w - base));
    } else {
        const char *msg = sqlite3_errmsg(db);
        char *base = resp_body_ptr();
        char *w = base;
        PUTLIT(w, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
        w += json_escape(w, msg, strlen(msg));
        PUTLIT(w, "\"}");
        append_response(c, 503, (size_t)(w - base));
    }
}

static int get_auth(http_req *r, auth_t *a) {
    if (!r->has_auth || r->auth_len < 7 || memcmp(r->auth, "Bearer ", 7) != 0) return 1;
    int v = verify_jwt(r->auth + 7, r->auth_len - 7, a);
    if (v == 0) return 0;
    if (v == 1) return 2;
    return 3;
}

static void send_auth_error(Conn *c, int ar) {
    if (ar == 1) send_error(c, 401, "missing bearer token");
    else if (ar == 2) send_error(c, 401, "invalid or expired token");
    else send_error(c, 401, "invalid token payload");
}

static int parse_id(const char *s, size_t n, int64_t *out) {
    if (n == 0 || n > 18) return -1;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v <= 0) return -1;
    *out = v;
    return 0;
}

static void handle_posts(Conn *c, http_req *r) {
    const char *p = r->path + 7;
    size_t n = r->path_len - 7;
    int is_like = 0;
    if (n >= 5 && memcmp(p + n - 5, "/like", 5) == 0) { is_like = 1; n -= 5; }

    for (size_t i = 0; i < n; i++)
        if (p[i] == '/') { send_error(c, 404, "not found"); return; }

    if (is_like) {
        if (!method_is(r, "POST")) { send_error(c, 404, "not found"); return; }
        auth_t a;
        int ar = get_auth(r, &a);
        if (ar) { send_auth_error(c, ar); return; }
        int64_t id;
        if (parse_id(p, n, &id) < 0) { send_error(c, 400, "invalid post id"); return; }
        handle_like(c, &a, id);
    } else {
        if (!method_is(r, "GET")) { send_error(c, 404, "not found"); return; }
        int64_t id;
        if (parse_id(p, n, &id) < 0) { send_error(c, 400, "invalid post id"); return; }
        handle_get_post(c, id);
    }
}

static void handle_request(Conn *c, http_req *r) {
    if (method_is(r, "GET") && path_eq(r, "/health")) { handle_health(c); return; }
    if (method_is(r, "GET") && path_eq(r, "/feed")) { handle_feed(c); return; }
    if (method_is(r, "POST") && path_eq(r, "/posts")) {
        auth_t a;
        int ar = get_auth(r, &a);
        if (ar) { send_auth_error(c, ar); return; }
        handle_create(c, r, &a);
        return;
    }
    if (path_prefix(r, "/posts/")) { handle_posts(c, r); return; }
    send_error(c, 404, "not found");
}

// ---------------------------------------------------------------------------
// connections
// ---------------------------------------------------------------------------
static void set_events(Conn *c, uint32_t ev) {
    if (c->dead || c->events == ev) return;
    struct epoll_event e;
    e.events = ev;
    e.data.u64 = (uint64_t)c->fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &e) == 0) c->events = ev;
}

static void close_conn(Conn *c) {
    if (!c || c->dead) return;
    c->dead = 1;
    if (c->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
        if (c->fd < max_fd && conns[c->fd] == c) conns[c->fd] = NULL;
        close(c->fd);
        c->fd = -1;
    }
    free(c->in);  c->in = NULL;
    free(c->out); c->out = NULL;
    c->in_len = c->in_cap = c->out_len = c->out_off = c->out_cap = 0;
    c->dead_next = dead_head;
    dead_head = c;
}

static void free_graveyard(void) {
    while (dead_head) {
        Conn *n = dead_head->dead_next;
        free(dead_head);
        dead_head = n;
    }
}

static int append_out(Conn *c, const char *data, size_t n) {
    if (c->out_off > 0) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if ((size_t)c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 8192;
        while (cap < (size_t)c->out_len + n) cap <<= 1;
        if (cap > (2u * 1024u * 1024u)) return -1;
        char *p = (char *)realloc(c->out, cap);
        if (!p) return -1;
        c->out = p;
        c->out_cap = (uint32_t)cap;
    }
    memcpy(c->out + c->out_len, data, n);
    c->out_len += (uint32_t)n;
    set_events(c, EPOLLIN | EPOLLOUT);
    return 0;
}

static void send_resp(resp_t *r) {
    Conn *c = r->c;
    if (!c || c->dead) return;
    if (c->out_len > c->out_off) {
        if (append_out(c, arena + r->off, r->len) < 0) close_conn(c);
        return;
    }
    size_t sent = 0;
    while (sent < r->len) {
        ssize_t n = send(c->fd, arena + r->off + sent, r->len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (append_out(c, arena + r->off + sent, r->len - sent) < 0) close_conn(c);
            return;
        }
        if (n < 0 && errno == EINTR) continue;
        close_conn(c);
        return;
    }
}

static void consume_input(Conn *c, char *data, size_t len);

static void flush_out(Conn *c) {
    if (c->dead) return;
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) { c->out_off += (uint32_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        close_conn(c);
        return;
    }
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_off = c->out_cap = 0;
    set_events(c, EPOLLIN);
    if (c->in && c->in_len) consume_input(c, c->in, c->in_len);
}

static int process_one(Conn *c, const char *data, size_t len, size_t off,
                       size_t *consumed, int *need_more) {
    http_req r;
    int rc = parse_req(data + off, len - off, &r, consumed, need_more);
    if (rc != 1) return rc;
    handle_request(c, &r);
    return 0;
}

static void consume_input(Conn *c, char *data, size_t len) {
    size_t off = 0;
    while (off < len && !c->dead) {
        size_t consumed = 0;
        int need_more = 0;
        int rc = process_one(c, data, len, off, &consumed, &need_more);
        if (rc < 0) { close_conn(c); return; }
        if (need_more) break;
        if (consumed == 0) break;
        off += consumed;
    }
    if (c->dead) return;

    size_t rem = len - off;
    if (rem) {
        if (data == c->in) {
            memmove(c->in, c->in + off, rem);
            c->in_len = (uint32_t)rem;
        } else {
            size_t cap = 8192;
            while (cap < rem) cap <<= 1;
            char *p = (char *)malloc(cap);
            if (!p) { close_conn(c); return; }
            memcpy(p, data + off, rem);
            c->in = p;
            c->in_len = (uint32_t)rem;
            c->in_cap = (uint32_t)cap;
        }
    } else {
        if (data == c->in) {
            free(c->in);
            c->in = NULL;
            c->in_len = c->in_cap = 0;
        }
    }
}

static void read_conn(Conn *c) {
    if (c->dead || (c->out && c->out_len > c->out_off)) return;

    if (c->in) {
        if (c->in_len == c->in_cap) {
            if (c->in_cap >= MAX_CONN_IN) { close_conn(c); return; }
            size_t ncap = (size_t)c->in_cap * 2;
            if (ncap < 8192) ncap = 8192;
            char *p = (char *)realloc(c->in, ncap);
            if (!p) { close_conn(c); return; }
            c->in = p;
            c->in_cap = (uint32_t)ncap;
        }
        ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n == 0) { close_conn(c); return; }
        if (n < 0) {
            if (errno == EINTR) return;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close_conn(c);
            return;
        }
        c->in_len += (uint32_t)n;
        consume_input(c, c->in, c->in_len);
    } else {
        ssize_t n = recv(c->fd, rbuf, sizeof(rbuf), 0);
        if (n == 0) { close_conn(c); return; }
        if (n < 0) {
            if (errno == EINTR) return;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close_conn(c);
            return;
        }
        consume_input(c, rbuf, (size_t)n);
    }
}

static void accept_all(void) {
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) { usleep(1000); break; }
            break;
        }
        if (fd >= max_fd) { close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        Conn *c = (Conn *)calloc(1, sizeof(Conn));
        if (!c) { close(fd); continue; }
        c->fd = fd;
        c->events = EPOLLIN;
        conns[fd] = c;

        struct epoll_event e;
        e.events = EPOLLIN;
        e.data.u64 = (uint64_t)fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e) < 0) close_conn(c);
    }
}

// ---------------------------------------------------------------------------
// SQLite
// ---------------------------------------------------------------------------
static int db_exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s: %s\n", sql, err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static sqlite3_stmt *db_prep(const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s\n  %s\n", sql, sqlite3_errmsg(db));
        return NULL;
    }
    return s;
}

static int open_db(void) {
    if (sqlite3_open_v2(g_db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        fprintf(stderr, "open %s: %s\n", g_db_path, sqlite3_errmsg(db));
        return -1;
    }
    // busy_timeout first: several workers may open the same database at once
    if (db_exec("PRAGMA busy_timeout=5000;") ||
        db_exec("PRAGMA journal_mode=WAL;") ||
        db_exec("PRAGMA synchronous=NORMAL;") ||
        db_exec("PRAGMA mmap_size=1073741824;") ||
        db_exec("PRAGMA cache_size=-16384;") ||
        db_exec("PRAGMA temp_store=MEMORY;") ||
        db_exec("PRAGMA wal_autocheckpoint=50000;") ||
        db_exec("PRAGMA busy_timeout=50;")) return -1;

    feed_stmt = db_prep(
        "SELECT p.id, p.body, p.created_at, u.username, "
        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
        "FROM posts p JOIN users u ON u.id = p.user_id "
        "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    get_stmt = db_prep(
        "SELECT p.id, p.body, p.created_at, u.username, "
        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
        "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1");
    ins_post_stmt = db_prep(
        "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    like_stmt = db_prep(
        "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 "
        "WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
        "ON CONFLICT (user_id, post_id) DO NOTHING");
    post_exists_stmt = db_prep("SELECT 1 FROM posts WHERE id = ?1");
    health_stmt = db_prep("SELECT 1");

    if (!feed_stmt || !get_stmt || !ins_post_stmt || !like_stmt ||
        !post_exists_stmt || !health_stmt) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// checkpoint helper process
// ---------------------------------------------------------------------------
static void ckpt_loop(const char *path) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGTERM, SIG_DFL);
    usleep(200000);

    sqlite3_initialize();
    sqlite3 *cdb = NULL;
    if (sqlite3_open_v2(path, &cdb, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sqlite3_close(cdb);
        _exit(0);
    }
    sqlite3_busy_timeout(cdb, 500);
    // Open the WAL eagerly; checkpointing needs pBt->pWal to exist.
    sqlite3_exec(cdb, "SELECT count(*) FROM sqlite_master;", NULL, NULL, NULL);

    int dbg = getenv("DEBUG_CKPT") != NULL;
    time_t last = 0;
    pid_t parent = getppid();
    for (;;) {
        if (getppid() != parent || getppid() == 1) break;
        int nlog = 0, nckpt = 0;
        int rc = sqlite3_wal_checkpoint_v2(cdb, NULL, SQLITE_CHECKPOINT_PASSIVE, &nlog, &nckpt);
        if (dbg) {
            time_t now = time(NULL);
            if (now != last) { last = now; fprintf(stderr, "ckpt rc=%d nlog=%d nckpt=%d\n", rc, nlog, nckpt); }
        }
        if (nlog > 4000) usleep(1000);
        else if (nlog > 500) usleep(5000);
        else usleep(50000);
    }
    sqlite3_close(cdb);
    _exit(0);
}

// ---------------------------------------------------------------------------
// listener + event loop
// ---------------------------------------------------------------------------
static int create_listener(void) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", g_port);

    int rc = getaddrinfo(g_host, portstr, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", g_host, portstr, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        if (ai->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 4096) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void event_loop(void) {
    struct epoll_event ev, evs[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.u64 = (uint64_t)lfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        perror("epoll_ctl listener");
        return;
    }

    while (!stop_flag) {
        int n = epoll_wait(epfd, evs, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = (int)evs[i].data.u64;
            if (fd == lfd) { accept_all(); continue; }
            Conn *c = (fd >= 0 && fd < max_fd) ? conns[fd] : NULL;
            if (!c) continue;
            uint32_t e = evs[i].events;
            if (e & (EPOLLERR | EPOLLHUP)) { close_conn(c); continue; }
            if (e & EPOLLOUT) { flush_out(c); if (c->dead) continue; }
            if (e & EPOLLIN) read_conn(c);
        }
        flush_batch();
        free_graveyard();
    }

    flush_batch();
    free_graveyard();
}

static void worker_main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        if (rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
        getrlimit(RLIMIT_NOFILE, &rl);
        max_fd = (int)rl.rlim_cur;
    } else {
        max_fd = 65536;
    }
    if (max_fd < 1024) max_fd = 1024;
    if (max_fd > 4000000) max_fd = 4000000;

    conns = (Conn **)calloc((size_t)max_fd, sizeof(Conn *));
    if (!conns) { fprintf(stderr, "calloc conns failed\n"); exit(1); }
    arena = (char *)mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) { perror("mmap arena"); exit(1); }

    sqlite3_initialize();
    if (open_db() < 0) exit(1);

    lfd = create_listener();
    if (lfd < 0) {
        fprintf(stderr, "cannot listen on %s:%d\n", g_host, g_port);
        exit(1);
    }
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }

    fprintf(stderr, "c-epoll-superspeed: listening %s:%d (worker pid %d, nofile %d)\n",
            g_host, g_port, (int)getpid(), max_fd);

    event_loop();

    sqlite3_close(db);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_db_path = getenv("SQLITE_PATH");
    if (!g_db_path || !*g_db_path) g_db_path = "feed.db";
    const char *secret = getenv("JWT_SECRET");
    if (!secret) secret = "twelve-dollar-challenge";
    g_host = getenv("HOST");
    if (!g_host || !*g_host) g_host = "127.0.0.1";
    const char *ps = getenv("PORT");
    g_port = ps ? atoi(ps) : 3000;
    const char *ws = getenv("WORKERS");
    if (ws && *ws) {
        g_workers = atoi(ws);
    } else {
        // Direct serving (0.0.0.0 / a public address) needs more than one process
        // to get past the 65535-descriptor per-process limit. Behind Nginx (which
        // keeps only 64 connections) one process is best.
        int direct = strcmp(g_host, "127.0.0.1") != 0 && strcmp(g_host, "localhost") != 0;
        g_workers = direct ? 4 : 1;
    }
    if (g_workers < 1) g_workers = 1;
    if (g_workers > 32) g_workers = 32;

    start_time = time(NULL);
    hmac_prepare(&g_hmac, (const unsigned char *)secret, strlen(secret));

    signal(SIGPIPE, SIG_IGN);

    // checkpoint helper (forked before any SQLite handle exists)
    pid_t ck = fork();
    if (ck == 0) ckpt_loop(g_db_path);

    // extra workers share the port with SO_REUSEPORT
    for (int i = 1; i < g_workers; i++) {
        pid_t p = fork();
        if (p == 0) { worker_main(); _exit(0); }
    }
    worker_main();
    return 0;
}
