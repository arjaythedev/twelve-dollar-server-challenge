// Feed API: one epoll loop per process, SQLite in-process, group commit per loop iteration.
// See README.md for the design. Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.h"

#define MAXFD 65536
#define RBUF (64 * 1024)
#define MAXREQ (64 * 1024)    // header + body cap per request
#define BATCH_OUT (4 << 20)   // response bytes held per loop iteration until its commit

typedef struct {
  char *in; uint32_t inlen, incap;     // partial request carried between reads (rare)
  char *out; uint32_t outlen, outoff;  // unsent response bytes after a short write (rare)
  uint8_t open, closing;
} conn_t;

static conn_t conns[MAXFD];
static int epfd, nconns;
static sqlite3 *db;
static sqlite3_stmt *st_feed, *st_post, *st_ins, *st_like, *st_exists, *st_one, *st_begin, *st_beginimm, *st_commit, *st_rollback;
static time_t started, now_s;

// ---------- output: one arena per loop iteration, sent after COMMIT ----------
static char *arena; static size_t alen;
typedef struct { int fd; uint32_t off, len; } seg_t;
static seg_t segs[MAXFD]; static int nsegs;
static int deferred[MAXFD], ndef, arena_full;
static int in_txn, txn_writes, batch_failed;

static inline void put(const void *p, size_t n) { memcpy(arena + alen, p, n); alen += n; }
#define PUTS(s) put(s, sizeof(s) - 1)
static inline void putu(uint64_t v) {
  char b[24]; int i = 24;
  do { b[--i] = '0' + v % 10; v /= 10; } while (v);
  put(b + i, 24 - i);
}
static const char hexd[] = "0123456789abcdef";
static void putjs(const unsigned char *s, size_t n) {  // JSON string contents, escaped
  size_t i = 0, run = 0;
  for (; i < n; i++) {
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    put(s + run, i - run); run = i + 1;
    char e[6] = {'\\', 0};
    switch (c) {
      case '"': e[1] = '"'; put(e, 2); break;
      case '\\': e[1] = '\\'; put(e, 2); break;
      case '\n': e[1] = 'n'; put(e, 2); break;
      case '\r': e[1] = 'r'; put(e, 2); break;
      case '\t': e[1] = 't'; put(e, 2); break;
      default: e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hexd[c >> 4]; e[5] = hexd[c & 15]; put(e, 6);
    }
  }
  put(s + run, n - run);
}

// response head with a blank Content-Length that finish() fills in
static size_t body_start, cl_pos;
static void begin_resp(int status, int close) {
  switch (status) {
    case 200: PUTS("HTTP/1.1 200 OK\r\n"); break;
    case 201: PUTS("HTTP/1.1 201 Created\r\n"); break;
    case 400: PUTS("HTTP/1.1 400 Bad Request\r\n"); break;
    case 401: PUTS("HTTP/1.1 401 Unauthorized\r\n"); break;
    case 404: PUTS("HTTP/1.1 404 Not Found\r\n"); break;
    case 503: PUTS("HTTP/1.1 503 Service Unavailable\r\n"); break;
    default: PUTS("HTTP/1.1 500 Internal Server Error\r\n"); break;
  }
  if (close) PUTS("Connection: close\r\n");
  PUTS("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
  cl_pos = alen - 10; body_start = alen;
}
static void finish(void) {
  size_t n = alen - body_start; char *p = arena + cl_pos + 6;
  do { *--p = '0' + n % 10; n /= 10; } while (n);
}
static void err(int status, const char *msg, int close) {
  begin_resp(status, close); PUTS("{\"error\":\""); put(msg, strlen(msg)); PUTS("\"}"); finish();
}

// ---------- SHA-256 / HMAC ----------
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; uint32_t bl; } sha_t;
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t *h, const unsigned char *p) {
  uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
  for (int i = 0; i < 64; i++) {
    t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
static void sha_init(sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, 32); s->n = 0; s->bl = 0;
}
static void sha_upd(sha_t *s, const unsigned char *p, size_t n) {
  s->n += n;
  if (s->bl) { size_t t = 64 - s->bl; if (t > n) t = n; memcpy(s->b + s->bl, p, t); s->bl += t; p += t; n -= t; if (s->bl == 64) { sha_block(s->h, s->b); s->bl = 0; } }
  for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
  if (n) { memcpy(s->b, p, n); s->bl = n; }
}
static void sha_fin(sha_t *s, unsigned char *out) {
  uint64_t bits = s->n * 8; unsigned char pad = 0x80, z[64] = {0}, len[8];
  sha_upd(s, &pad, 1);
  sha_upd(s, z, (s->bl <= 56 ? 56 : 120) - s->bl);
  for (int i = 0; i < 8; i++) len[i] = bits >> (56 - 8 * i);
  sha_upd(s, len, 8);
  for (int i = 0; i < 8; i++) { out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16; out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i]; }
}
static sha_t hm_in, hm_out;  // HMAC key schedule: hash state after absorbing key^ipad / key^opad
static void hmac_key(const char *key) {
  unsigned char k[64] = {0}, pad[64]; size_t kl = strlen(key);
  if (kl > 64) { sha_t s; sha_init(&s); sha_upd(&s, (const unsigned char *)key, kl); sha_fin(&s, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha_init(&hm_in); sha_upd(&hm_in, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha_init(&hm_out); sha_upd(&hm_out, pad, 64);
}
static void hmac(const unsigned char *m, size_t n, unsigned char *out) {
  sha_t s = hm_in; unsigned char ih[32];
  sha_upd(&s, m, n); sha_fin(&s, ih);
  s = hm_out; sha_upd(&s, ih, 32); sha_fin(&s, out);
}

// ---------- base64url ----------
static signed char b64t[256];
static int b64dec(const char *s, size_t n, unsigned char *out) {  // -1 on bad input
  if (n % 4 == 1) return -1;
  uint32_t acc = 0; int bits = 0, o = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64t[(unsigned char)s[i]]; if (v < 0) return -1;
    acc = acc << 6 | v; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = acc >> bits; }
  }
  return o;
}

// ---------- JSON (strict, RFC 8259) ----------
typedef struct { const char *p, *e; } js_t;
static inline void ws(js_t *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int utf8_len(const unsigned char *p, const unsigned char *e) {  // length of one valid UTF-8 sequence, 0 if invalid
  unsigned c = p[0];
  if (c < 0x80) return 1;
  if (c >= 0xc2 && c <= 0xdf) return e - p >= 2 && (p[1] & 0xc0) == 0x80 ? 2 : 0;
  if (c >= 0xe0 && c <= 0xef) {
    if (e - p < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f)) return 0;
    return 3;
  }
  if (c >= 0xf0 && c <= 0xf4) {
    if (e - p < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80) return 0;
    if ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f)) return 0;
    return 4;
  }
  return 0;
}
// Parses the string at j->p (on its opening quote), decoding UTF-8 into out when out is non-NULL.
// Returns the decoded length, -1 on a syntax error, -2 if it doesn't fit in cap (the string is still validated).
static long jstr(js_t *j, char *out, size_t cap) {
  const unsigned char *p = (const unsigned char *)j->p + 1, *e = (const unsigned char *)j->e; size_t o = 0; int over = 0;
  while (p < e) {
    unsigned c = *p;
    if (c == '"') { j->p = (const char *)p + 1; return over ? -2 : (long)o; }
    if (c < 0x20) return -1;
    if (c == '\\') {
      if (++p >= e) return -1;
      char ch;
      switch (*p) {
        case '"': ch = '"'; break; case '\\': ch = '\\'; break; case '/': ch = '/'; break;
        case 'b': ch = '\b'; break; case 'f': ch = '\f'; break; case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break; case 't': ch = '\t'; break;
        case 'u': {
          if (e - p < 5) return -1;
          int a = hexv(p[1]), b = hexv(p[2]), c2 = hexv(p[3]), d = hexv(p[4]);
          if ((a | b | c2 | d) < 0) return -1;
          uint32_t cp = a << 12 | b << 8 | c2 << 4 | d; p += 4;
          if (cp >= 0xd800 && cp <= 0xdbff && e - p >= 7 && p[1] == '\\' && p[2] == 'u') {
            int a2 = hexv(p[3]), b2 = hexv(p[4]), c3 = hexv(p[5]), d2 = hexv(p[6]);
            if ((a2 | b2 | c3 | d2) < 0) return -1;
            uint32_t lo = a2 << 12 | b2 << 8 | c3 << 4 | d2;
            if (lo >= 0xdc00 && lo <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
          }
          if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;  // lone surrogate: U+FFFD, as JS TextEncoder does
          char u[4]; int ul;
          if (cp < 0x80) { u[0] = cp; ul = 1; }
          else if (cp < 0x800) { u[0] = 0xc0 | cp >> 6; u[1] = 0x80 | (cp & 63); ul = 2; }
          else if (cp < 0x10000) { u[0] = 0xe0 | cp >> 12; u[1] = 0x80 | (cp >> 6 & 63); u[2] = 0x80 | (cp & 63); ul = 3; }
          else { u[0] = 0xf0 | cp >> 18; u[1] = 0x80 | (cp >> 12 & 63); u[2] = 0x80 | (cp >> 6 & 63); u[3] = 0x80 | (cp & 63); ul = 4; }
          if (out && o + ul > cap) { out = NULL; over = 1; }
          if (out) memcpy(out + o, u, ul);
          o += ul; p++; continue;
        }
        default: return -1;
      }
      if (out && o >= cap) { out = NULL; over = 1; }
      if (out) out[o] = ch;
      o++; p++; continue;
    }
    int l = utf8_len(p, e); if (!l) return -1;
    if (out && o + l > cap) { out = NULL; over = 1; }
    if (out) memcpy(out + o, p, l);
    o += l; p += l;
  }
  return -1;
}
static int jnumber(js_t *j) {
  const char *p = j->p, *e = j->e;
  if (p < e && *p == '-') p++;
  if (p >= e) return 0;
  if (*p == '0') p++; else if (*p >= '1' && *p <= '9') while (p < e && *p >= '0' && *p <= '9') p++; else return 0;
  if (p < e && *p == '.') { p++; if (p >= e || *p < '0' || *p > '9') return 0; while (p < e && *p >= '0' && *p <= '9') p++; }
  if (p < e && (*p == 'e' || *p == 'E')) { p++; if (p < e && (*p == '+' || *p == '-')) p++; if (p >= e || *p < '0' || *p > '9') return 0; while (p < e && *p >= '0' && *p <= '9') p++; }
  j->p = p; return 1;
}
static int jlit(js_t *j, const char *s, size_t n) { if ((size_t)(j->e - j->p) < n || memcmp(j->p, s, n)) return 0; j->p += n; return 1; }
static int jvalue(js_t *j, int depth) {  // validates and skips one value
  if (depth > 64) return 0;
  ws(j); if (j->p >= j->e) return 0;
  switch (*j->p) {
    case '"': return jstr(j, NULL, 0) >= 0;
    case '{':
      j->p++; ws(j);
      if (j->p < j->e && *j->p == '}') { j->p++; return 1; }
      for (;;) {
        ws(j); if (j->p >= j->e || *j->p != '"' || jstr(j, NULL, 0) < 0) return 0;
        ws(j); if (j->p >= j->e || *j->p++ != ':') return 0;
        if (!jvalue(j, depth + 1)) return 0;
        ws(j); if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return 1; }
        return 0;
      }
    case '[':
      j->p++; ws(j);
      if (j->p < j->e && *j->p == ']') { j->p++; return 1; }
      for (;;) {
        if (!jvalue(j, depth + 1)) return 0;
        ws(j); if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return 1; }
        return 0;
      }
    case 't': return jlit(j, "true", 4);
    case 'f': return jlit(j, "false", 5);
    case 'n': return jlit(j, "null", 4);
    default: return jnumber(j);
  }
}
// Validates a whole document. For a top-level object it records the raw span of the last value of
// each wanted key (the last duplicate wins, as in JSON.parse). Returns 0 if the document is invalid.
typedef struct { const char *key; const char *v, *ve; } want_t;
static int jdoc(const char *s, size_t n, want_t *w, int nw) {
  js_t j = {s, s + n};
  for (int i = 0; i < nw; i++) w[i].v = NULL;
  ws(&j);
  if (j.p < j.e && *j.p == '{') {
    j.p++; ws(&j);
    if (j.p < j.e && *j.p == '}') j.p++;
    else for (;;) {
      char kb[32]; ws(&j);
      if (j.p >= j.e || *j.p != '"') return 0;
      long kl = jstr(&j, kb, sizeof kb);
      if (kl == -1) return 0;
      ws(&j); if (j.p >= j.e || *j.p++ != ':') return 0;
      ws(&j); const char *vs = j.p;
      if (!jvalue(&j, 1)) return 0;
      if (kl >= 0) for (int i = 0; i < nw; i++) if ((size_t)kl == strlen(w[i].key) && !memcmp(kb, w[i].key, kl)) { w[i].v = vs; w[i].ve = j.p; }
      ws(&j); if (j.p >= j.e) return 0;
      if (*j.p == ',') { j.p++; continue; }
      if (*j.p == '}') { j.p++; break; }
      return 0;
    }
  } else if (!jvalue(&j, 0)) return 0;
  ws(&j);
  return j.p == j.e;
}

// ---------- auth ----------
// Returns NULL and fills uid/uname on success, else the message to send with the 401.
static const char *auth(const char *h, size_t hl, int64_t *uid, char *uname, size_t *unl) {
  static const char *bad = "invalid or expired token", *payload = "invalid token payload";
  if (!h || hl < 7 || memcmp(h, "Bearer ", 7)) return "missing bearer token";
  const char *t = h + 7, *te = h + hl, *d1 = memchr(t, '.', te - t), *d2;
  if (!d1) return bad;
  d2 = memchr(d1 + 1, '.', te - d1 - 1);
  if (!d2 || memchr(d2 + 1, '.', te - d2 - 1)) return bad;
  unsigned char sig[48], mac[32], hdr[512], pl[2048];
  if (te - d2 - 1 != 43 || b64dec(d2 + 1, 43, sig) != 32) return bad;
  hmac((const unsigned char *)t, d2 - t, mac);
  unsigned diff = 0; for (int i = 0; i < 32; i++) diff |= sig[i] ^ mac[i];
  if (diff) return bad;
  if (d1 - t > 680 || d2 - d1 - 1 > 2700) return bad;
  int hn = b64dec(t, d1 - t, hdr), pn = b64dec(d1 + 1, d2 - d1 - 1, pl);
  if (hn < 0 || pn < 0) return bad;
  want_t hw[1] = {{"alg"}};
  if (!jdoc((char *)hdr, hn, hw, 1) || !hw[0].v || *hw[0].v != '"') return bad;
  char alg[8]; js_t aj = {hw[0].v, hw[0].ve}; long al = jstr(&aj, alg, sizeof alg);
  if (al != 5 || memcmp(alg, "HS256", 5)) return bad;
  want_t pw[4] = {{"exp"}, {"nbf"}, {"sub"}, {"username"}};
  if (!jdoc((char *)pl, pn, pw, 4) || !pw[0].v) return bad;  // exp is required
  for (int i = 0; i < 2; i++) if (pw[i].v) {  // exp / nbf must be numbers, checked against now
    if (*pw[i].v != '-' && (*pw[i].v < '0' || *pw[i].v > '9')) return bad;
    char nb[64]; size_t l = pw[i].ve - pw[i].v; if (l >= sizeof nb) return bad;
    memcpy(nb, pw[i].v, l); nb[l] = 0; double v = strtod(nb, NULL);
    if (i == 0 ? (double)now_s >= v : (double)now_s < v) return bad;
  }
  if (!pw[2].v || *pw[2].v != '"' || !pw[3].v || *pw[3].v != '"') return payload;
  char sb[24]; js_t sj = {pw[2].v, pw[2].ve}; long sl = jstr(&sj, sb, sizeof sb);
  if (sl <= 0 || sl > 15) return payload;
  int64_t v = 0; for (long i = 0; i < sl; i++) { if (sb[i] < '0' || sb[i] > '9') return payload; v = v * 10 + sb[i] - '0'; }
  if (v <= 0) return payload;
  js_t uj = {pw[3].v, pw[3].ve}; long ul = jstr(&uj, uname, 1024);
  if (ul < 0) return payload;
  *uid = v; *unl = ul; return NULL;
}

// ---------- database ----------
static int txn(void) {  // opens the batch transaction on first use
  if (in_txn) return 1;
  int rc = sqlite3_step(st_begin); sqlite3_reset(st_begin);
  return in_txn = rc == SQLITE_DONE;
}
// Runs a write inside the batch transaction. With several processes, a read snapshot can't be upgraded
// once another process has committed (SQLITE_BUSY_SNAPSHOT): end the read-only part of the batch, take
// the write lock with BEGIN IMMEDIATE (which waits up to busy_timeout) and retry once.
static int wstep(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  if ((rc & 0xff) == SQLITE_BUSY && !txn_writes) {
    sqlite3_reset(s);
    if (in_txn) { sqlite3_step(st_commit); sqlite3_reset(st_commit); in_txn = 0; }
    rc = sqlite3_step(st_beginimm); sqlite3_reset(st_beginimm);
    if (rc != SQLITE_DONE) return rc;
    in_txn = 1;
    rc = sqlite3_step(s);
  }
  if (rc == SQLITE_ROW || rc == SQLITE_DONE) txn_writes = 1;
  return rc;
}
static void db_fail(void) {  // a failed statement may have rolled the whole batch back
  if (in_txn && sqlite3_get_autocommit(db)) { in_txn = 0; if (txn_writes) batch_failed = 1; txn_writes = 0; }
}
static void put_post_row(sqlite3_stmt *s) {
  PUTS("{\"id\":"); putu(sqlite3_column_int64(s, 0));
  PUTS(",\"body\":\""); putjs(sqlite3_column_text(s, 1), sqlite3_column_bytes(s, 1));
  PUTS("\",\"created_at\":\""); put(sqlite3_column_text(s, 2), sqlite3_column_bytes(s, 2));
  PUTS("\",\"author\":\""); putjs(sqlite3_column_text(s, 3), sqlite3_column_bytes(s, 3));
  PUTS("\",\"like_count\":"); putu(sqlite3_column_int64(s, 4)); PUTS("}");
}

// ---------- request handling ----------
static int64_t parse_id(const char *s, size_t n) {  // positive decimal integer, else -1 (huge ids map to 0: not found)
  if (!n) return -1;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; if (v < 100000000000000000LL) v = v * 10 + s[i] - '0'; else v = 0x7fffffffffffffffLL; }
  return v > 0 ? v : -1;
}
static int jsws_at(const unsigned char *p, const unsigned char *e) {  // byte length of JS String.trim whitespace at p, or 0
  if (*p == ' ' || (*p >= 9 && *p <= 13)) return 1;
  if (e - p >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (e - p >= 3) {
    uint32_t c = p[0] << 16 | p[1] << 8 | p[2];
    if (c == 0xe19a80 || (c >= 0xe28080 && c <= 0xe2808a) || c == 0xe280a8 || c == 0xe280a9 || c == 0xe280af || c == 0xe2819f || c == 0xe38080 || c == 0xefbbbf) return 3;
  }
  return 0;
}
static int jsws_before(const unsigned char *s, const unsigned char *e) {
  if (e[-1] == ' ' || (e[-1] >= 9 && e[-1] <= 13)) return 1;
  if (e - s >= 2 && e[-2] == 0xc2 && e[-1] == 0xa0) return 2;
  if (e - s >= 3 && jsws_at(e - 3, e) == 3) return 3;
  return 0;
}

static void handle(const char *m, size_t ml, const char *path, size_t pl, const char *authh, size_t al,
                   const char *body, size_t bl, int close) {
  const char *q = memchr(path, '?', pl); if (q) pl = q - path;
  int get = ml == 3 && !memcmp(m, "GET", 3), post = ml == 4 && !memcmp(m, "POST", 4);
  int rc;
  if (get && pl == 5 && !memcmp(path, "/feed", 5)) {
    if (!txn()) { err(500, "internal server error", close); return; }
    size_t mark = alen;
    begin_resp(200, close); PUTS("{\"posts\":[");
    int i = 0;
    while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) { if (i++) PUTS(","); put_post_row(st_feed); }
    sqlite3_reset(st_feed);
    if (rc != SQLITE_DONE) { db_fail(); alen = mark; err(500, "internal server error", close); return; }
    PUTS("]}"); finish(); return;
  }
  if (pl > 7 && !memcmp(path, "/posts/", 7)) {
    const char *seg = path + 7, *sl = memchr(seg, '/', pl - 7);
    if (get && !sl) {
      int64_t id = parse_id(seg, pl - 7);
      if (id < 0) { err(400, "invalid post id", close); return; }
      if (!txn()) { err(500, "internal server error", close); return; }
      sqlite3_bind_int64(st_post, 1, id);
      size_t mark = alen;
      rc = sqlite3_step(st_post);
      if (rc == SQLITE_ROW) { begin_resp(200, close); PUTS("{\"post\":"); put_post_row(st_post); PUTS("}"); finish(); }
      else if (rc == SQLITE_DONE) err(404, "post not found", close);
      else { db_fail(); alen = mark; err(500, "internal server error", close); }
      sqlite3_reset(st_post); return;
    }
    if (post && sl && (size_t)(path + pl - sl) == 5 && !memcmp(sl, "/like", 5)) {
      int64_t uid; char un[1024]; size_t unl;
      const char *e = auth(authh, al, &uid, un, &unl);
      if (e) { err(401, e, close); return; }
      int64_t id = parse_id(seg, sl - seg);
      if (id < 0) { err(400, "invalid post id", close); return; }
      if (!txn()) { err(500, "internal server error", close); return; }
      sqlite3_bind_int64(st_like, 1, uid); sqlite3_bind_int64(st_like, 2, id);
      rc = wstep(st_like); sqlite3_reset(st_like);
      if (rc != SQLITE_DONE) { db_fail(); err(500, "internal server error", close); return; }
      int created = sqlite3_changes(db) > 0;
      if (!created) {
        sqlite3_bind_int64(st_exists, 1, id);
        rc = sqlite3_step(st_exists); sqlite3_reset(st_exists);
        if (rc == SQLITE_DONE) { err(404, "post not found", close); return; }
        if (rc != SQLITE_ROW) { db_fail(); err(500, "internal server error", close); return; }
      }
      begin_resp(created ? 201 : 200, close);
      PUTS("{\"liked\":true,\"already_liked\":"); if (created) PUTS("false"); else PUTS("true");
      PUTS(",\"post_id\":"); putu(id); PUTS("}"); finish(); return;
    }
  }
  if (post && pl == 6 && !memcmp(path, "/posts", 6)) {
    int64_t uid; char un[1024]; size_t unl;
    const char *e = auth(authh, al, &uid, un, &unl);
    if (e) { err(401, e, close); return; }
    want_t w[1] = {{"body"}};
    if (!jdoc(body, bl, w, 1)) { err(400, "malformed JSON body", close); return; }
    if (!w[0].v || *w[0].v != '"') { err(400, "body is required", close); return; }
    static char txt[MAXREQ]; js_t bj = {w[0].v, w[0].ve};
    long n = jstr(&bj, txt, sizeof txt);
    if (n < 0) { err(400, "malformed JSON body", close); return; }
    const unsigned char *s = (unsigned char *)txt, *se = s + n; int l;
    while (s < se && (l = jsws_at(s, se))) s += l;
    while (se > s && (l = jsws_before(s, se))) se -= l;
    if (s == se) { err(400, "body is required", close); return; }
    size_t chars = 0; for (const unsigned char *c = s; c < se; c++) chars += (*c & 0xc0) != 0x80;
    if (chars > 500) { err(400, "body must be at most 500 characters", close); return; }
    if (!txn()) { err(500, "internal server error", close); return; }
    sqlite3_bind_int64(st_ins, 1, uid); sqlite3_bind_text(st_ins, 2, (const char *)s, se - s, SQLITE_STATIC);
    rc = wstep(st_ins);
    if (rc != SQLITE_ROW) { sqlite3_reset(st_ins); db_fail(); err(500, "internal server error", close); return; }
    size_t mark = alen;
    begin_resp(201, close);
    PUTS("{\"post\":{\"id\":"); putu(sqlite3_column_int64(st_ins, 0));
    PUTS(",\"body\":\""); putjs(s, se - s);
    PUTS("\",\"created_at\":\""); put(sqlite3_column_text(st_ins, 1), sqlite3_column_bytes(st_ins, 1));
    PUTS("\",\"author\":\""); putjs((unsigned char *)un, unl); PUTS("\",\"like_count\":0}}"); finish();
    rc = sqlite3_step(st_ins); sqlite3_reset(st_ins);  // run RETURNING to completion so the insert finishes
    if (rc != SQLITE_DONE) { db_fail(); alen = mark; err(500, "internal server error", close); }
    return;
  }
  if (get && pl == 7 && !memcmp(path, "/health", 7)) {
    rc = sqlite3_step(st_one); sqlite3_reset(st_one);
    if (rc == SQLITE_ROW) {
      begin_resp(200, close); PUTS("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"); putu(now_s - started); PUTS("}"); finish();
    } else {
      begin_resp(503, close); PUTS("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
      const char *m2 = sqlite3_errmsg(db); putjs((const unsigned char *)m2, strlen(m2)); PUTS("\"}"); finish();
    }
    return;
  }
  err(404, "not found", close);
}

static int hdr_is(const char *p, size_t n, const char *name, size_t nl) { return n == nl && !strncasecmp(p, name, nl); }

// Serves every complete request in buf. Returns bytes consumed, or -1 to drop the connection.
static long serve(int fd, char *buf, size_t n, int *closing) {
  size_t used = 0;
  while (used < n && !*closing) {
    char *p = buf + used, *e = buf + n;
    if (alen + 2 * MAXREQ > BATCH_OUT) { arena_full = 1; break; }  // the rest waits for the next batch
    char *he = memmem(p, e - p, "\r\n\r\n", 4);
    if (!he) { if ((size_t)(e - p) > MAXREQ) return -1; break; }
    char *le = memchr(p, '\r', he + 2 - p);
    char *m = p, *sp1 = memchr(m, ' ', le - m); if (!sp1) return -1;
    char *path = sp1 + 1, *sp2 = memchr(path, ' ', le - path); if (!sp2) return -1;
    int http10 = le - sp2 - 1 == 8 && !memcmp(sp2 + 1, "HTTP/1.0", 8);
    const char *authh = NULL; size_t al = 0, clen = 0; int close = http10, expect = 0;
    for (char *h = le + 2; h < he + 2;) {
      char *hl = memchr(h, '\r', he + 2 - h), *c = memchr(h, ':', hl - h);
      if (c) {
        char *v = c + 1; while (v < hl && (*v == ' ' || *v == '\t')) v++;
        char *ve = hl; while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t kn = c - h;
        if (hdr_is(h, kn, "content-length", 14)) { clen = 0; for (char *d = v; d < ve; d++) { if (*d < '0' || *d > '9') return -1; clen = clen * 10 + *d - '0'; if (clen > MAXREQ) return -1; } }
        else if (hdr_is(h, kn, "authorization", 13)) { authh = v; al = ve - v; }
        else if (hdr_is(h, kn, "connection", 10)) { if (ve - v == 5 && !strncasecmp(v, "close", 5)) close = 1; else if (ve - v == 10 && !strncasecmp(v, "keep-alive", 10)) close = 0; }
        else if (hdr_is(h, kn, "expect", 6)) expect = ve - v == 12 && !strncasecmp(v, "100-continue", 12);
        else if (hdr_is(h, kn, "transfer-encoding", 17)) return -1;  // chunked uploads aren't supported
      }
      h = hl + 2;
    }
    char *body = he + 4;
    if ((size_t)(e - body) < clen) {
      if (expect && body == e) { static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n"; if (write(fd, cont, sizeof cont - 1) < 0) return -1; }
      break;
    }
    handle(m, sp1 - m, path, sp2 - path, authh, al, body, clen, close);
    if (close) *closing = 1;
    used = body + clen - buf;
  }
  return used;
}

// ---------- connections ----------
static void conn_close(int fd) {
  conn_t *c = &conns[fd];
  free(c->in); free(c->out); memset(c, 0, sizeof *c);
  close(fd); nconns--;
}
static void watch(int fd, uint32_t ev) { struct epoll_event e = {.events = ev, .data.fd = fd}; epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e); }

static void process(int fd, char *buf, size_t n) {
  conn_t *c = &conns[fd];
  uint32_t start = alen; int closing = 0; arena_full = 0;
  long used = serve(fd, buf, n, &closing);
  if (used < 0) { alen = start; conn_close(fd); return; }
  if (alen > start) segs[nsegs++] = (seg_t){fd, start, alen - start};
  c->closing = closing;
  size_t left = closing ? 0 : n - used;
  if (left) {
    if (buf != c->in) { c->incap = left + RBUF; c->in = malloc(c->incap); memcpy(c->in, buf + used, left); }
    else if (used) memmove(c->in, c->in + used, left);
    c->inlen = left;
    if (arena_full) deferred[ndef++] = fd;
  } else if (c->in) { free(c->in); c->in = NULL; c->inlen = c->incap = 0; }
  if (closing && alen == start) conn_close(fd);
}

static char rbuf[RBUF];
static void on_readable(int fd) {
  conn_t *c = &conns[fd];
  if (c->out) return;  // still sending an earlier response; reading resumes once it is out
  ssize_t r;
  if (c->inlen) {
    if (c->incap - c->inlen < 4096) { c->incap = c->incap * 2 + RBUF; c->in = realloc(c->in, c->incap); }
    r = read(fd, c->in + c->inlen, c->incap - c->inlen);
    if (r <= 0) { if (r < 0 && errno == EAGAIN) return; conn_close(fd); return; }
    c->inlen += r; process(fd, c->in, c->inlen);
  } else {
    r = read(fd, rbuf, RBUF);
    if (r <= 0) { if (r < 0 && errno == EAGAIN) return; conn_close(fd); return; }
    process(fd, rbuf, r);
  }
}
static void on_writable(int fd) {
  conn_t *c = &conns[fd];
  if (!c->out) return;
  ssize_t w = write(fd, c->out + c->outoff, c->outlen - c->outoff);
  if (w < 0) { if (errno == EAGAIN) return; conn_close(fd); return; }
  c->outoff += w;
  if (c->outoff < c->outlen) return;
  free(c->out); c->out = NULL; c->outlen = c->outoff = 0;
  if (c->closing) { conn_close(fd); return; }
  watch(fd, EPOLLIN);
  if (c->inlen) process(fd, c->in, c->inlen);
}

// Commits the batch, then sends every response produced in it.
static void flush_batch(void) {
  if (in_txn) {
    int rc = sqlite3_step(st_commit); sqlite3_reset(st_commit);
    if (rc != SQLITE_DONE) { sqlite3_step(st_rollback); sqlite3_reset(st_rollback); if (txn_writes) batch_failed = 1; }
    in_txn = 0; txn_writes = 0;
  }
  for (int i = 0; i < nsegs; i++) {
    seg_t *s = &segs[i]; conn_t *c = &conns[s->fd];
    if (!c->open) continue;
    if (batch_failed) { conn_close(s->fd); continue; }  // the batch wasn't committed: never claim it was
    ssize_t w = write(s->fd, arena + s->off, s->len);
    if (w < 0) { if (errno != EAGAIN) { conn_close(s->fd); continue; } w = 0; }
    if ((uint32_t)w < s->len) {
      c->outlen = s->len - w; c->outoff = 0; c->out = malloc(c->outlen); memcpy(c->out, arena + s->off + w, c->outlen);
      watch(s->fd, EPOLLOUT);
    } else if (c->closing) conn_close(s->fd);
  }
  nsegs = 0; alen = 0; batch_failed = 0;
}

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) { fprintf(stderr, "prepare: %s: %s\n", sqlite3_errmsg(db), sql); exit(1); }
  return s;
}
static void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { fprintf(stderr, "open %s: %s\n", path, sqlite3_errmsg(db)); exit(1); }
  sqlite3_busy_timeout(db, 5000);
  char *e = NULL;
  if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000;"
                   "SELECT count(*) FROM users WHERE id = 1;", NULL, NULL, &e) != SQLITE_OK) { fprintf(stderr, "pragma: %s\n", e); exit(1); }
  // The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
  // for a long time), so the WAL stays bounded at about 256 MB.
  sqlite3_exec(db, "PRAGMA wal_autocheckpoint=65536;", NULL, NULL, NULL);
  const char *cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
  char sql[512];
  snprintf(sql, sizeof sql, "%sORDER BY p.created_at DESC, p.id DESC LIMIT 20", cols); st_feed = prep(sql);
  snprintf(sql, sizeof sql, "%sWHERE p.id = ?", cols); st_post = prep(sql);
  st_ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
  st_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_exists = prep("SELECT 1 FROM posts WHERE id = ?");
  st_one = prep("SELECT 1");
  st_begin = prep("BEGIN"); st_beginimm = prep("BEGIN IMMEDIATE"); st_commit = prep("COMMIT"); st_rollback = prep("ROLLBACK");
  sqlite3_step(st_feed); sqlite3_reset(st_feed);  // takes the lock and touches the hot pages before the first request
}

static const char *g_host; static int g_port, lfd = -1, conn_hi, conn_lo, spawn_pending, spawned;
static void listen_start(void) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(g_port)};
  if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", g_host); exit(1); }
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 65535)) { perror("bind/listen"); exit(1); }
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
  lfd = fd;
}

// Every keep-alive client holds a descriptor, and LimitNOFILE caps descriptors per process. Close to
// that cap this process starts one more copy of itself on the same port (SO_REUSEPORT) and stops
// accepting until it has room again, so new clients go to the other copy. Below the cap there is one process.
static int ready_fd = -1, conn_max;
static void spawn_worker(void) {
  spawn_pending = 0; spawned = 1;
  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC)) { perror("pipe"); return; }
  pid_t p = fork();
  if (p == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    fcntl(pfd[1], F_SETFD, 0);
    char fdarg[16]; snprintf(fdarg, sizeof fdarg, "%d", pfd[1]);
    char *argv[] = {"server", "--shared", fdarg, NULL};
    execv("/proc/self/exe", argv);
    _exit(1);
  }
  close(pfd[1]);
  ready_fd = pfd[0];
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = ready_fd};
  epoll_ctl(epfd, EPOLL_CTL_ADD, ready_fd, &ev);
  fprintf(stderr, "%d connections: starting worker %d\n", nconns, (int)p);
}
static void accept_conns(int limit) {
  while (nconns < limit) {
    int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) break;
    if (c >= MAXFD) { close(c); continue; }
    conns[c].open = 1; nconns++;
    struct epoll_event ce = {.events = EPOLLIN, .data.fd = c};
    epoll_ctl(epfd, EPOLL_CTL_ADD, c, &ce);
  }
}
static void listen_stop(void) {  // take what is already queued, then leave new clients to the other worker
  accept_conns(conn_max);
  close(lfd); lfd = -1;
}

static void run(int notify_fd) {
  arena = malloc(BATCH_OUT);
  epfd = epoll_create1(EPOLL_CLOEXEC);
  listen_start();
  if (notify_fd >= 0) { if (write(notify_fd, "1", 1) < 0) {} close(notify_fd); }  // tell the parent worker we're listening
  struct epoll_event evs[512];
  for (;;) {
    int n = epoll_wait(epfd, evs, 512, -1);
    if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); exit(1); }
    now_s = time(NULL);
    for (int i = 0; i < n; i++) {
      int fd = evs[i].data.fd;
      if (fd == lfd) {
        accept_conns(conn_hi);
        if (nconns >= conn_hi) {  // nearly out of descriptors: new clients go to another worker
          if (spawned) listen_stop();
          else { epoll_ctl(epfd, EPOLL_CTL_DEL, lfd, NULL); if (!spawned) spawn_pending = 1; }
        }
        continue;
      }
      if (fd == ready_fd) {  // the new worker is listening
        char b; int up = read(ready_fd, &b, 1) == 1;
        close(ready_fd); ready_fd = -1;
        if (up) listen_stop();
        else { spawned = 0; struct epoll_event ev = {.events = EPOLLIN, .data.fd = lfd}; epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev); }  // it failed to start
        continue;
      }
      uint32_t e = evs[i].events;
      if (!conns[fd].open) continue;
      if (e & EPOLLOUT) on_writable(fd);
      else if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(fd);
    }
    flush_batch();
    while (ndef) {  // requests held back by a full arena
      int k = ndef; ndef = 0;
      static int todo[MAXFD]; memcpy(todo, deferred, k * sizeof(int));
      for (int i = 0; i < k; i++) { conn_t *c = &conns[todo[i]]; if (c->open && c->inlen && !c->out) process(todo[i], c->in, c->inlen); }
      flush_batch();
    }
    if (spawn_pending) spawn_worker();
    if (lfd < 0 && nconns < conn_lo) listen_start();
  }
}

// Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
// the length of the fsync, so a helper process does them and the event loop never waits on the disk.
static void checkpointer(const char *path) {
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) exit(1);
  sqlite3_busy_timeout(db, 100);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;", NULL, NULL, NULL);
  for (;;) {
    int log = 0, done = 0;
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
    usleep(log > 0 && done < log ? 1000 : 10000);
  }
}
static void spawn_checkpointer(void) {
  if (fork() == 0) { char *argv[] = {"server", "--ckpt", NULL}; execv("/proc/self/exe", argv); _exit(1); }
}

#ifdef PGO_TRAIN
#include "train.c"
#endif
int main(int argc, char **argv) {
#ifdef PGO_TRAIN
  train();
#endif
  const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET"), *host = getenv("HOST"), *ports = getenv("PORT");
  if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
  g_host = host ? host : "0.0.0.0";
  g_port = ports ? atoi(ports) : 3000;
  if (argc > 1 && !strcmp(argv[1], "--ckpt")) { sqlite3_initialize(); checkpointer(path); }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);  // workers are reaped automatically
  struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
  if (rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
  long lim = rl.rlim_cur > MAXFD ? MAXFD : (long)rl.rlim_cur;
  conn_hi = lim - (lim / 16 > 64 ? lim / 16 : 64);  // leave room for SQLite's files and the listener
  conn_lo = conn_hi - conn_hi / 8;
  conn_max = lim - 16;
  memset(b64t, -1, sizeof b64t);
  for (int i = 0; i < 64; i++) b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
  hmac_key(secret);
  sqlite3_initialize();  // built with SQLITE_OMIT_AUTOINIT
  started = now_s = time(NULL);
  db_open(path);
  if (argc == 1) spawn_checkpointer();
  run(argc > 2 ? atoi(argv[2]) : -1);
}
