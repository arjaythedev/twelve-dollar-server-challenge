// SHA-256, HMAC-SHA256 and HS256 JWT verification. Every token's signature is
// checked on every request; only the HMAC key schedule (ipad/opad states) is
// computed once at startup.
#include "td.h"

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void td_sha256_init(TDSha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->n = 0;
    c->blen = 0;
}

void td_sha256_update(TDSha256 *c, const void *d, size_t n) {
    const uint8_t *p = d;
    c->n += n;
    if (c->blen) {
        size_t take = 64 - c->blen < n ? 64 - c->blen : n;
        memcpy(c->buf + c->blen, p, take);
        c->blen += (uint32_t)take; p += take; n -= take;
        if (c->blen < 64) return;
        sha_block(c->h, c->buf);
        c->blen = 0;
    }
    while (n >= 64) { sha_block(c->h, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->blen = (uint32_t)n; }
}

void td_sha256_final(TDSha256 *c, uint8_t out[32]) {
    uint64_t bits = c->n * 8;
    uint8_t pad = 0x80;
    td_sha256_update(c, &pad, 1);
    static const uint8_t zero[64];
    size_t z = (c->blen <= 56) ? 56 - c->blen : 120 - c->blen;
    td_sha256_update(c, zero, z);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    td_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16);
        out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i];
    }
}

// base64url without padding -> bytes; returns length or -1
static int b64url_dec(const char *s, uint32_t n, uint8_t *out, uint32_t cap) {
    static int8_t tab[256];
    static bool init;
    if (!init) {
        memset(tab, -1, sizeof tab);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; i++) tab[(uint8_t)a[i]] = (int8_t)i;
        init = true;
    }
    if (n % 4 == 1) return -1;
    uint32_t o = 0, acc = 0, bits = 0;
    for (uint32_t i = 0; i < n; i++) {
        int v = tab[(uint8_t)s[i]];
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

static void b64url_enc32(const uint8_t in[32], char out[43]) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t o = 0, i = 0;
    for (; i + 3 <= 30; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8 | in[i+2];
        out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63]; out[o++] = a[v & 63];
    }
    uint32_t v = (uint32_t)in[30] << 16 | (uint32_t)in[31] << 8;
    out[o++] = a[v >> 18]; out[o++] = a[(v >> 12) & 63]; out[o++] = a[(v >> 6) & 63];
}

@implementation TDAuth {
    TDSha256 inner, outer;   // HMAC key schedule
}

- (instancetype)initWithSecret:(const char *)secret {
    uint8_t key[64] = {0};
    size_t kl = strlen(secret);
    if (kl > 64) {
        TDSha256 c; td_sha256_init(&c); td_sha256_update(&c, secret, kl); td_sha256_final(&c, key);
    } else memcpy(key, secret, kl);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = key[i] ^ 0x36; op[i] = key[i] ^ 0x5c; }
    td_sha256_init(&inner); td_sha256_update(&inner, ip, 64);
    td_sha256_init(&outer); td_sha256_update(&outer, op, 64);
    return self;
}

- (int)verify:(const char *)hdr length:(uint32_t)n now:(int64_t)now
          uid:(int64_t *)uid name:(char *)name nameLen:(uint32_t *)nameLen {
    if (!hdr || n < 7 || memcmp(hdr, "Bearer ", 7)) return TD_AUTH_MISSING;
    const char *t = hdr + 7;
    uint32_t tn = n - 7;
    const char *d1 = memchr(t, '.', tn);
    if (!d1) return TD_AUTH_INVALID;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(t + tn - d1 - 1));
    if (!d2 || memchr(d2 + 1, '.', (size_t)(t + tn - d2 - 1))) return TD_AUTH_INVALID;
    uint32_t signedLen = (uint32_t)(d2 - t);
    const char *sig = d2 + 1;
    uint32_t sigLen = (uint32_t)(t + tn - sig);
    if (sigLen != 43 || signedLen > 4096) return TD_AUTH_INVALID;

    // signature
    TDSha256 c = inner;
    td_sha256_update(&c, t, signedLen);
    uint8_t mac[32];
    td_sha256_final(&c, mac);
    c = outer;
    td_sha256_update(&c, mac, 32);
    td_sha256_final(&c, mac);
    char enc[43];
    b64url_enc32(mac, enc);
    uint8_t diff = 0;
    for (int i = 0; i < 43; i++) diff |= (uint8_t)(enc[i] ^ sig[i]);
    if (diff) return TD_AUTH_INVALID;

    // header: must be a JSON object with "alg":"HS256"
    uint8_t buf[3072];
    int hl = b64url_dec(t, (uint32_t)(d1 - t), buf, sizeof buf);
    if (hl < 0) return TD_AUTH_INVALID;
    TDJField hf[1] = {{ .key = "alg", .keylen = 3 }};
    if (td_json_object((const char *)buf, (uint32_t)hl, hf, 1) != 1) return TD_AUTH_INVALID;
    if (hf[0].type != TDJ_STRING || hf[0].vlen != 5 || memcmp(hf[0].v, "HS256", 5)) return TD_AUTH_INVALID;

    // payload
    int pl = b64url_dec(d1 + 1, (uint32_t)(d2 - d1 - 1), buf, sizeof buf - 1);
    if (pl < 0) return TD_AUTH_INVALID;
    TDJField pf[3] = {{ .key = "exp", .keylen = 3 }, { .key = "sub", .keylen = 3 }, { .key = "username", .keylen = 8 }};
    if (td_json_object((const char *)buf, (uint32_t)pl, pf, 3) != 1) return TD_AUTH_INVALID;
    if (pf[0].type != TDJ_NUMBER) return TD_AUTH_INVALID;
    {
        char num[64];
        uint32_t l = pf[0].vlen < 63 ? pf[0].vlen : 63;
        memcpy(num, pf[0].v, l); num[l] = 0;
        double exp = strtod(num, NULL);
        if (!(exp > (double)now)) return TD_AUTH_INVALID;
    }
    // sub: positive integer string
    if (pf[1].type != TDJ_STRING || pf[1].escaped || pf[1].vlen == 0 || pf[1].vlen > 15) return TD_AUTH_PAYLOAD;
    int64_t id = 0;
    for (uint32_t i = 0; i < pf[1].vlen; i++) {
        char ch = pf[1].v[i];
        if (ch < '0' || ch > '9') return TD_AUTH_PAYLOAD;
        id = id * 10 + (ch - '0');
    }
    if (id <= 0) return TD_AUTH_PAYLOAD;
    if (pf[2].type != TDJ_STRING) return TD_AUTH_PAYLOAD;
    int ul;
    if (pf[2].escaped) ul = td_json_unescape(pf[2].v, pf[2].vlen, name, TD_UNAME_MAX);
    else if (pf[2].vlen <= TD_UNAME_MAX) { memcpy(name, pf[2].v, pf[2].vlen); ul = (int)pf[2].vlen; }
    else ul = -1;
    if (ul < 0) return TD_AUTH_PAYLOAD;
    *uid = id;
    *nameLen = (uint32_t)ul;
    return TD_AUTH_OK;
}
@end
