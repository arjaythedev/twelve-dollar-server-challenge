// $12 Server Challenge entry: one thread, io_uring, SQLite in-process.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"

#define MAX_RINGS 8
#define NBUF 1024
#define BUF_SZ 2048
#define BGID 0
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 16384
#define JOBS_CAP 4096
#define MAX_HDR 16384
#define MAX_REQ (64 * 1024)
#define HDR_RES 128
#define ACCEPT_MARGIN 512
#define OUT_CAP (1024 * 1024)
#ifndef CKPT_FRAMES
#define CKPT_FRAMES 4096
#endif
#ifndef SLOTS_MAX
#define SLOTS_MAX 65535
#endif

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_CANCEL };
#define UD(op, r, gen, slot) (((uint64_t)(op) << 56) | ((uint64_t)(r) << 48) | ((uint64_t)(gen) << 32) | (uint64_t)(slot))
#define UD_OP(u) ((int)((u) >> 56))
#define UD_RING(u) ((int)(((u) >> 48) & 0xff))
#define UD_GEN(u) ((uint16_t)(((u) >> 32) & 0xffff))
#define UD_SLOT(u) ((uint32_t)(u))

enum { C_FREE = 0, C_OPEN, C_CLOSING };
enum { F_SENDING = 1, F_JOB = 2, F_PENDING = 4, F_CLOSE_AFTER = 8, F_EXPECT_SENT = 16 };

typedef struct {
  uint16_t gen;
  uint8_t state, flags;
  uint32_t inlen, incap;
  uint32_t sendoff, sendlen;
  uint32_t qlen, qcap;
  char *in, *sendbuf, *q;
} conn_t;

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *br;
  char *bufs;
  conn_t *conns;
  uint32_t nslots, open;
  int accept_armed;
  int nret;
  struct { uint32_t slot; uint16_t gen; } rearm[NBUF];
  int nrearm;
} ring_t;

static ring_t R[MAX_RINGS];
static int nrings, accept_ring, listen_fd, accept_paused;
static uint16_t accept_epoch;
static uint32_t slots_per_ring;
static void arm_accept(int r);

enum { J_FEED, J_POST, J_LIKE, J_CREATE, J_HEALTH, J_ERR };
typedef struct {
  uint8_t ring, kind, close_after;
  uint16_t gen;
  uint32_t slot;
  int status;
  const char *err;
  int64_t post_id, user_id;
  char *store;
  char *uname, *body;
  uint32_t unamelen, bodylen;
  char *resp;
  uint32_t resp_off, resp_len;
} job_t;

static job_t jobs[JOBS_CAP];
static int njobs, jobs_cap_pass = 1024;

typedef struct { uint8_t ring; uint16_t gen; uint32_t slot; } pend_t;
static pend_t *pend;
static int npend, pendcap;

static time_t now_s;
static struct timespec start_ts;

// ---------------------------------------------------------------- SHA-256 / HMAC

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha_t;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, hh;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static void sha_init(sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv);
  s->len = 0, s->n = 0;
}

static void sha_update(sha_t *s, const void *data, size_t len) {
  const uint8_t *p = data;
  s->len += len;
  if (s->n) {
    size_t take = 64 - s->n < len ? 64 - s->n : len;
    memcpy(s->buf + s->n, p, take);
    s->n += take, p += take, len -= take;
    if (s->n < 64) return;
    sha_block(s->h, s->buf);
    s->n = 0;
  }
  for (; len >= 64; p += 64, len -= 64) sha_block(s->h, p);
  memcpy(s->buf, p, len);
  s->n = len;
}

static void sha_final(sha_t *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8;
  uint8_t pad = 0x80;
  sha_update(s, &pad, 1);
  pad = 0;
  while (s->n != 56) sha_update(s, &pad, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha_update(s, lb, 8);
  for (int i = 0; i < 8; i++) out[4 * i] = s->h[i] >> 24, out[4 * i + 1] = s->h[i] >> 16, out[4 * i + 2] = s->h[i] >> 8, out[4 * i + 3] = s->h[i];
}

static sha_t hmac_inner, hmac_outer;

static void hmac_setup(const char *key) {
  uint8_t k[64] = {0}, pad[64];
  size_t kl = strlen(key);
  if (kl > 64) {
    sha_t s;
    sha_init(&s);
    sha_update(&s, key, kl);
    sha_final(&s, k);
  } else {
    memcpy(k, key, kl);
  }
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  sha_init(&hmac_inner);
  sha_update(&hmac_inner, pad, 64);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  sha_init(&hmac_outer);
  sha_update(&hmac_outer, pad, 64);
}

static void hmac_sha256(const void *msg, size_t len, uint8_t out[32]) {
  sha_t s = hmac_inner;
  uint8_t ih[32];
  sha_update(&s, msg, len);
  sha_final(&s, ih);
  s = hmac_outer;
  sha_update(&s, ih, 32);
  sha_final(&s, out);
}

// ---------------------------------------------------------------- base64url / JSON

static int8_t b64t[256];

static void b64_init(void) {
  memset(b64t, -1, sizeof b64t);
  const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (int i = 0; i < 64; i++) b64t[(uint8_t)a[i]] = i;
}

static int b64url_dec(const char *s, size_t n, uint8_t *out, size_t cap) {
  if (n % 4 == 1) return -1;
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int v = b64t[(uint8_t)s[i]];
    if (v < 0) return -1;
    acc = acc << 6 | v, bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  return (int)o;
}

static const char *jws(const char *p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static const char *jstr_end(const char *p, const char *e) {
  for (p++; p < e; p++) {
    unsigned char c = *p;
    if (c == '"') return p + 1;
    if (c == '\\') {
      if (++p >= e) return NULL;
      switch (*p) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': break;
        case 'u':
          if (e - p < 5) return NULL;
          for (int i = 1; i <= 4; i++)
            if (hexv(p[i]) < 0) return NULL;
          p += 4;
          break;
        default: return NULL;
      }
    } else if (c < 0x20) {
      return NULL;
    }
  }
  return NULL;
}

static const char *jnum_end(const char *p, const char *e) {
  if (p < e && *p == '-') p++;
  if (p >= e) return NULL;
  if (*p == '0') p++;
  else if (*p >= '1' && *p <= '9') while (p < e && *p >= '0' && *p <= '9') p++;
  else return NULL;
  if (p < e && *p == '.') {
    p++;
    if (p >= e || *p < '0' || *p > '9') return NULL;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p >= e || *p < '0' || *p > '9') return NULL;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  return p;
}

static const char *jval_end(const char *p, const char *e, int depth) {
  if (p >= e || depth > 256) return NULL;
  switch (*p) {
    case '"': return jstr_end(p, e);
    case '{':
      p = jws(p + 1, e);
      if (p < e && *p == '}') return p + 1;
      for (;;) {
        if (p >= e || *p != '"' || !(p = jstr_end(p, e))) return NULL;
        p = jws(p, e);
        if (p >= e || *p != ':') return NULL;
        if (!(p = jval_end(jws(p + 1, e), e, depth + 1))) return NULL;
        p = jws(p, e);
        if (p < e && *p == ',') { p = jws(p + 1, e); continue; }
        if (p < e && *p == '}') return p + 1;
        return NULL;
      }
    case '[':
      p = jws(p + 1, e);
      if (p < e && *p == ']') return p + 1;
      for (;;) {
        if (!(p = jval_end(p, e, depth + 1))) return NULL;
        p = jws(p, e);
        if (p < e && *p == ',') { p = jws(p + 1, e); continue; }
        if (p < e && *p == ']') return p + 1;
        return NULL;
      }
    case 't': return (e - p >= 4 && !memcmp(p, "true", 4)) ? p + 4 : NULL;
    case 'f': return (e - p >= 5 && !memcmp(p, "false", 5)) ? p + 5 : NULL;
    case 'n': return (e - p >= 4 && !memcmp(p, "null", 4)) ? p + 4 : NULL;
    default: return jnum_end(p, e);
  }
}

static int utf8_put(char *o, uint32_t cp) {
  if (cp < 0x80) { o[0] = cp; return 1; }
  if (cp < 0x800) { o[0] = 0xc0 | cp >> 6; o[1] = 0x80 | (cp & 0x3f); return 2; }
  if (cp < 0x10000) { o[0] = 0xe0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3f); o[2] = 0x80 | (cp & 0x3f); return 3; }
  o[0] = 0xf0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 0x3f); o[2] = 0x80 | (cp >> 6 & 0x3f); o[3] = 0x80 | (cp & 0x3f);
  return 4;
}

// Decodes a validated JSON string [s, e) including quotes into out; returns length.
static uint32_t jstr_dec(const char *s, const char *e, char *out) {
  char *o = out;
  for (s++, e--; s < e; s++) {
    if (*s != '\\') { *o++ = *s; continue; }
    s++;
    switch (*s) {
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case 'n': *o++ = '\n'; break;
      case 'r': *o++ = '\r'; break;
      case 't': *o++ = '\t'; break;
      case 'u': {
        uint32_t cp = hexv(s[1]) << 12 | hexv(s[2]) << 8 | hexv(s[3]) << 4 | hexv(s[4]);
        s += 4;
        if (cp >= 0xd800 && cp < 0xdc00 && e - s >= 7 && s[1] == '\\' && s[2] == 'u') {
          uint32_t lo = hexv(s[3]) << 12 | hexv(s[4]) << 8 | hexv(s[5]) << 4 | hexv(s[6]);
          if (lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00), s += 6;
        }
        if (cp >= 0xd800 && cp < 0xe000) cp = 0xfffd;
        o += utf8_put(o, cp);
        break;
      }
      default: *o++ = *s;
    }
  }
  return (uint32_t)(o - out);
}

typedef struct { const char *k; const char *v, *ve; } jfield;

// Scans a top-level JSON document. Returns 1 for an object, 0 for another valid value, -1 if malformed.
// For each wanted key, records the span of its last value.
static int jobj_scan(const char *p, const char *e, jfield *want, int nwant) {
  for (int i = 0; i < nwant; i++) want[i].v = NULL;
  p = jws(p, e);
  if (p >= e) return -1;
  if (*p != '{') {
    const char *v = jval_end(p, e, 0);
    if (!v || jws(v, e) != e) return -1;
    return 0;
  }
  p = jws(p + 1, e);
  if (p < e && *p == '}') return jws(p + 1, e) == e ? 1 : -1;
  for (;;) {
    if (p >= e || *p != '"') return -1;
    const char *ks = p, *ke = jstr_end(p, e);
    if (!ke) return -1;
    p = jws(ke, e);
    if (p >= e || *p != ':') return -1;
    const char *vs = jws(p + 1, e), *ve = jval_end(vs, e, 1);
    if (!ve) return -1;
    if (ke - ks <= 40) {
      char kb[64];
      uint32_t kl = jstr_dec(ks, ke, kb);
      for (int i = 0; i < nwant; i++)
        if (strlen(want[i].k) == kl && !memcmp(want[i].k, kb, kl)) want[i].v = vs, want[i].ve = ve;
    }
    p = jws(ve, e);
    if (p < e && *p == ',') { p = jws(p + 1, e); continue; }
    if (p < e && *p == '}') return jws(p + 1, e) == e ? 1 : -1;
    return -1;
  }
}

// ---------------------------------------------------------------- output buffers

typedef struct { char *p; uint32_t len, cap; } ob_t;

static void ob_init(ob_t *o, uint32_t cap) { o->p = malloc(cap), o->len = HDR_RES, o->cap = cap; }

static inline void ob_need(ob_t *o, uint32_t n) {
  if (o->len + n <= o->cap) return;
  while (o->len + n > o->cap) o->cap *= 2;
  o->p = realloc(o->p, o->cap);
}

static inline void ob_put(ob_t *o, const char *s, uint32_t n) {
  ob_need(o, n);
  memcpy(o->p + o->len, s, n);
  o->len += n;
}

#define OB_LIT(o, s) ob_put(o, s, sizeof(s) - 1)

static inline void ob_i64(ob_t *o, int64_t v) {
  char t[24];
  int n = 0;
  uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;
  do t[n++] = '0' + u % 10, u /= 10; while (u);
  if (v < 0) t[n++] = '-';
  ob_need(o, n);
  while (n) o->p[o->len++] = t[--n];
}

static void ob_jstr(ob_t *o, const char *s, uint32_t n) {
  ob_need(o, n * 6 + 2);
  char *d = o->p + o->len;
  *d++ = '"';
  for (uint32_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') { *d++ = c; continue; }
    *d++ = '\\';
    switch (c) {
      case '"': *d++ = '"'; break;
      case '\\': *d++ = '\\'; break;
      case '\n': *d++ = 'n'; break;
      case '\r': *d++ = 'r'; break;
      case '\t': *d++ = 't'; break;
      case '\b': *d++ = 'b'; break;
      case '\f': *d++ = 'f'; break;
      default: *d++ = 'u', *d++ = '0', *d++ = '0', *d++ = "0123456789abcdef"[c >> 4], *d++ = "0123456789abcdef"[c & 15];
    }
  }
  *d++ = '"';
  o->len = (uint32_t)(d - o->p);
}

static void ob_post(ob_t *o, int64_t id, const char *body, int bl, const char *ca, int cl, const char *au, int al, int64_t likes) {
  OB_LIT(o, "{\"id\":");
  ob_i64(o, id);
  OB_LIT(o, ",\"body\":");
  ob_jstr(o, body, bl);
  OB_LIT(o, ",\"created_at\":");
  ob_jstr(o, ca, cl);
  OB_LIT(o, ",\"author\":");
  ob_jstr(o, au, al);
  OB_LIT(o, ",\"like_count\":");
  ob_i64(o, likes);
  OB_LIT(o, "}");
}

static const char *status_line(int st) {
  switch (st) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
    default: return "HTTP/1.1 500 Internal Server Error\r\n";
  }
}

static void job_finish(job_t *j, ob_t *o, int st) {
  char h[HDR_RES];
  uint32_t body = o->len - HDR_RES;
  int hl = snprintf(h, sizeof h, "%sContent-Type: application/json\r\nContent-Length: %u\r\n%s\r\n", status_line(st), body,
                    j->close_after ? "Connection: close\r\n" : "");
  memcpy(o->p + HDR_RES - hl, h, hl);
  j->resp = o->p, j->resp_off = HDR_RES - hl, j->resp_len = body + hl;
}

static void job_error(job_t *j, int st, const char *msg) {
  ob_t o;
  ob_init(&o, 256);
  OB_LIT(&o, "{\"error\":\"");
  ob_put(&o, msg, strlen(msg));
  OB_LIT(&o, "\"}");
  job_finish(j, &o, st);
}

// ---------------------------------------------------------------- SQLite

static sqlite3 *db;
static sqlite3_stmt *st_ids, *st_rows, *st_likes, *st_feed, *st_post, *st_exists, *st_like, *st_create, *st_one;
static sqlite3_stmt *st_begin, *st_begin_w, *st_commit, *st_rollback;
static int wal_frames;

static sqlite3_stmt *prep(const char *sql) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &s, NULL) != SQLITE_OK) {
    fprintf(stderr, "prepare failed: %s: %s\n", sql, sqlite3_errmsg(db));
    exit(1);
  }
  return s;
}

static int wal_cb(void *a, sqlite3 *d, const char *n, int frames) {
  (void)a, (void)d, (void)n;
  wal_frames = frames;
  return SQLITE_OK;
}

static int exec(const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }

static int run(sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  sqlite3_reset(s);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static void db_open(const char *path) {
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    fprintf(stderr, "open %s: %s\n", path, sqlite3_errmsg(db));
    exit(1);
  }
  exec("PRAGMA locking_mode=EXCLUSIVE");
  sqlite3_stmt *jm = prep("PRAGMA journal_mode=WAL");
  if (sqlite3_step(jm) != SQLITE_ROW || strcasecmp((const char *)sqlite3_column_text(jm, 0), "wal")) {
    fprintf(stderr, "could not enable WAL mode\n");
    exit(1);
  }
  sqlite3_finalize(jm);
  exec("PRAGMA synchronous=NORMAL");
  exec("PRAGMA cache_size=-65536");
  exec("PRAGMA mmap_size=0");
  exec("PRAGMA temp_store=MEMORY");
  exec("PRAGMA wal_autocheckpoint=0");
  exec("PRAGMA journal_size_limit=67108864");
  sqlite3_wal_hook(db, wal_cb, NULL);
  st_ids = prep("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
  st_rows = prep("SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2");
  st_likes = prep("SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id");
  st_feed = prep("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  st_post = prep("SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1");
  st_exists = prep("SELECT 1 FROM posts WHERE id = ?1");
  st_like = prep("INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
  st_create = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  st_one = prep("SELECT 1");
  st_begin = prep("BEGIN");
  st_begin_w = prep("BEGIN IMMEDIATE");
  st_commit = prep("COMMIT");
  st_rollback = prep("ROLLBACK");
}

#define TXT(s, i) (const char *)sqlite3_column_text(s, i), sqlite3_column_bytes(s, i)

static ob_t scratch;

static int do_feed(ob_t *o) {
  int64_t ids[20];
  int n = 0, rc;
  while (n < 20 && (rc = sqlite3_step(st_ids)) == SQLITE_ROW) ids[n++] = sqlite3_column_int64(st_ids, 0);
  sqlite3_reset(st_ids);
  OB_LIT(o, "{\"posts\":[");
  if (n == 0) goto done;
  int64_t mn = ids[0], mx = ids[0];
  for (int k = 1; k < n; k++) mn = ids[k] < mn ? ids[k] : mn, mx = ids[k] > mx ? ids[k] : mx;
  if (mx - mn < 256) {
    static int16_t pos[256];
    static int64_t cnt[256];
    uint32_t soff[20], slen[20];
    int span = (int)(mx - mn) + 1, found = 0;
    for (int i = 0; i < span; i++) pos[i] = -1, cnt[i] = 0;
    for (int k = 0; k < n; k++) pos[ids[k] - mn] = k;
    sqlite3_bind_int64(st_likes, 1, mn);
    sqlite3_bind_int64(st_likes, 2, mx);
    while ((rc = sqlite3_step(st_likes)) == SQLITE_ROW) {
      int64_t pid = sqlite3_column_int64(st_likes, 0);
      if (pid >= mn && pid <= mx) cnt[pid - mn] = sqlite3_column_int64(st_likes, 1);
    }
    sqlite3_reset(st_likes);
    if (rc != SQLITE_DONE) return -1;
    scratch.len = 0;
    sqlite3_bind_int64(st_rows, 1, mn);
    sqlite3_bind_int64(st_rows, 2, mx);
    while ((rc = sqlite3_step(st_rows)) == SQLITE_ROW) {
      int64_t id = sqlite3_column_int64(st_rows, 0);
      if (id < mn || id > mx || pos[id - mn] < 0) continue;
      int k = pos[id - mn];
      soff[k] = scratch.len;
      ob_post(&scratch, id, TXT(st_rows, 1), TXT(st_rows, 2), TXT(st_rows, 3), cnt[id - mn]);
      slen[k] = scratch.len - soff[k];
      found++;
    }
    sqlite3_reset(st_rows);
    if (rc != SQLITE_DONE) return -1;
    if (found == n) {
      for (int k = 0; k < n; k++) {
        if (k) OB_LIT(o, ",");
        ob_put(o, scratch.p + soff[k], slen[k]);
      }
      goto done;
    }
  }
  n = 0;
  while ((rc = sqlite3_step(st_feed)) == SQLITE_ROW) {
    if (n++) OB_LIT(o, ",");
    ob_post(o, sqlite3_column_int64(st_feed, 0), TXT(st_feed, 1), TXT(st_feed, 2), TXT(st_feed, 3), sqlite3_column_int64(st_feed, 4));
  }
  sqlite3_reset(st_feed);
  if (rc != SQLITE_DONE) return -1;
done:
  OB_LIT(o, "]}");
  return 0;
}

static void run_read(job_t *j) {
  ob_t o;
  int rc;
  switch (j->kind) {
    case J_FEED:
      ob_init(&o, 8192);
      if (do_feed(&o) < 0) { free(o.p); job_error(j, 500, "internal server error"); return; }
      job_finish(j, &o, 200);
      return;
    case J_POST:
      sqlite3_bind_int64(st_post, 1, j->post_id);
      rc = sqlite3_step(st_post);
      if (rc == SQLITE_ROW) {
        ob_init(&o, 1024);
        OB_LIT(&o, "{\"post\":");
        ob_post(&o, sqlite3_column_int64(st_post, 0), TXT(st_post, 1), TXT(st_post, 2), TXT(st_post, 3), sqlite3_column_int64(st_post, 4));
        OB_LIT(&o, "}");
        sqlite3_reset(st_post);
        job_finish(j, &o, 200);
      } else {
        sqlite3_reset(st_post);
        job_error(j, rc == SQLITE_DONE ? 404 : 500, rc == SQLITE_DONE ? "post not found" : "internal server error");
      }
      return;
    case J_HEALTH: {
      rc = sqlite3_step(st_one);
      sqlite3_reset(st_one);
      ob_init(&o, 256);
      if (rc == SQLITE_ROW) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        OB_LIT(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        ob_i64(&o, t.tv_sec - start_ts.tv_sec);
        OB_LIT(&o, "}");
        job_finish(j, &o, 200);
      } else {
        const char *m = sqlite3_errmsg(db);
        OB_LIT(&o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
        ob_jstr(&o, m, strlen(m));
        OB_LIT(&o, "}");
        job_finish(j, &o, 503);
      }
      return;
    }
  }
}

static void run_write(job_t *j) {
  ob_t o;
  int rc;
  if (j->kind == J_LIKE) {
    sqlite3_bind_int64(st_exists, 1, j->post_id);
    rc = sqlite3_step(st_exists);
    sqlite3_reset(st_exists);
    if (rc == SQLITE_DONE) { job_error(j, 404, "post not found"); return; }
    if (rc != SQLITE_ROW) { job_error(j, 500, "internal server error"); return; }
    sqlite3_bind_int64(st_like, 1, j->user_id);
    sqlite3_bind_int64(st_like, 2, j->post_id);
    rc = sqlite3_step(st_like);
    sqlite3_reset(st_like);
    if (rc != SQLITE_DONE) { job_error(j, 500, "internal server error"); return; }
    int fresh = sqlite3_changes(db) > 0;
    ob_init(&o, 256);
    OB_LIT(&o, "{\"liked\":true,\"already_liked\":");
    if (fresh) OB_LIT(&o, "false"); else OB_LIT(&o, "true");
    OB_LIT(&o, ",\"post_id\":");
    ob_i64(&o, j->post_id);
    OB_LIT(&o, "}");
    job_finish(j, &o, fresh ? 201 : 200);
    return;
  }
  sqlite3_bind_int64(st_create, 1, j->user_id);
  sqlite3_bind_text(st_create, 2, j->body, j->bodylen, SQLITE_STATIC);
  rc = sqlite3_step(st_create);
  if (rc != SQLITE_ROW) { sqlite3_reset(st_create); job_error(j, 500, "internal server error"); return; }
  ob_init(&o, 1024 + j->bodylen * 6 + j->unamelen * 6);
  OB_LIT(&o, "{\"post\":");
  ob_post(&o, sqlite3_column_int64(st_create, 0), j->body, j->bodylen, TXT(st_create, 1), j->uname, j->unamelen, 0);
  OB_LIT(&o, "}");
  rc = sqlite3_step(st_create);
  sqlite3_reset(st_create);
  if (rc != SQLITE_DONE) { free(o.p); job_error(j, 500, "internal server error"); return; }
  job_finish(j, &o, 201);
}

// ---------------------------------------------------------------- io_uring plumbing

static struct io_uring_sqe *sqe_get(int r) {
  for (int tries = 0; tries < 1000; tries++) {
    struct io_uring_sqe *s = io_uring_get_sqe(&R[r].ring);
    if (s) return s;
    io_uring_submit(&R[r].ring);
  }
  fprintf(stderr, "submission queue stuck on ring %d\n", r);
  abort();
}

static void arm_recv(int r, uint32_t slot) {
  struct io_uring_sqe *s = sqe_get(r);
  io_uring_prep_recv_multishot(s, slot, NULL, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = BGID;
  io_uring_sqe_set_data64(s, UD(OP_RECV, r, R[r].conns[slot].gen, slot));
}

static void submit_send(int r, uint32_t slot) {
  conn_t *c = &R[r].conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  io_uring_prep_send(s, slot, c->sendbuf + c->sendoff, c->sendlen, MSG_WAITALL | MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, UD(OP_SEND, r, c->gen, slot));
}

static void conn_release(int r, uint32_t slot) {
  conn_t *c = &R[r].conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  io_uring_prep_close_direct(s, slot);
  io_uring_sqe_set_data64(s, UD(OP_CLOSE, r, 0, slot));
  free(c->in), free(c->q), free(c->sendbuf);
  c->in = c->q = c->sendbuf = NULL;
  c->inlen = c->incap = c->qlen = c->qcap = 0;
  c->state = C_FREE, c->flags = 0;
  R[r].open--;
  if (accept_paused && R[r].nslots - R[r].open >= 2 * ACCEPT_MARGIN) arm_accept(r);
}

static void conn_close(int r, uint32_t slot) {
  conn_t *c = &R[r].conns[slot];
  if (c->state != C_OPEN) return;
  c->state = C_CLOSING;
  int now = !(c->flags & F_SENDING);
  struct io_uring_sqe *s = sqe_get(r);
  io_uring_prep_shutdown(s, slot, SHUT_RDWR);
  // The shutdown ends the multishot recv; the close must not run before it, or the socket stays open.
  s->flags |= IOSQE_FIXED_FILE | (now ? IOSQE_IO_HARDLINK : 0);
  io_uring_sqe_set_data64(s, UD(OP_CLOSE, r, 0, slot));
  if (now) conn_release(r, slot);
}

static void conn_send(int r, uint32_t slot, uint16_t gen, char *base, uint32_t off, uint32_t len) {
  conn_t *c = &R[r].conns[slot];
  if (c->gen != gen || c->state != C_OPEN) { free(base); return; }
  if (c->flags & F_SENDING) {
    if (c->qlen + len > OUT_CAP) { free(base); conn_close(r, slot); return; }
    if (c->qlen + len > c->qcap) {
      c->qcap = (c->qlen + len) * 2;
      c->q = realloc(c->q, c->qcap);
    }
    memcpy(c->q + c->qlen, base + off, len);
    c->qlen += len;
    free(base);
    return;
  }
  c->sendbuf = base, c->sendoff = off, c->sendlen = len;
  c->flags |= F_SENDING;
  submit_send(r, slot);
}

static void on_send(int r, uint32_t slot, uint16_t gen, int res) {
  conn_t *c = &R[r].conns[slot];
  if (c->gen != gen || c->state == C_FREE || !(c->flags & F_SENDING)) return;
  if (res > 0 && (uint32_t)res < c->sendlen && c->state == C_OPEN) {
    c->sendoff += res, c->sendlen -= res;
    submit_send(r, slot);
    return;
  }
  free(c->sendbuf);
  c->sendbuf = NULL;
  c->flags &= ~F_SENDING;
  if (c->state == C_CLOSING) { conn_release(r, slot); return; }
  if (res < 0) { conn_close(r, slot); return; }
  if (c->qlen) {
    c->sendbuf = c->q, c->sendoff = 0, c->sendlen = c->qlen;
    c->q = NULL, c->qlen = c->qcap = 0;
    c->flags |= F_SENDING;
    submit_send(r, slot);
  } else if (c->flags & F_CLOSE_AFTER) {
    conn_close(r, slot);
  }
}

// ---------------------------------------------------------------- HTTP request handling

static job_t *job_new(int r, uint32_t slot) {
  job_t *j = &jobs[njobs++];
  memset(j, 0, sizeof *j);
  j->ring = r, j->slot = slot, j->gen = R[r].conns[slot].gen;
  return j;
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

enum { AUTH_OK, AUTH_MISSING, AUTH_INVALID, AUTH_PAYLOAD };

static int verify_jwt(const char *a, size_t al, job_t *j) {
  if (al < 7 || memcmp(a, "Bearer ", 7)) return AUTH_MISSING;
  const char *t = a + 7, *e = a + al;
  const char *d1 = memchr(t, '.', e - t);
  if (!d1) return AUTH_INVALID;
  const char *d2 = memchr(d1 + 1, '.', e - d1 - 1);
  if (!d2 || memchr(d2 + 1, '.', e - d2 - 1)) return AUTH_INVALID;
  uint8_t sig[48], mac[32];
  char hdr[512], pl[1024];
  if (b64url_dec(d2 + 1, e - d2 - 1, sig, sizeof sig) != 32) return AUTH_INVALID;
  hmac_sha256(t, d2 - t, mac);
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= mac[i] ^ sig[i];
  if (diff) return AUTH_INVALID;
  int hl = b64url_dec(t, d1 - t, (uint8_t *)hdr, sizeof hdr);
  int pll = b64url_dec(d1 + 1, d2 - d1 - 1, (uint8_t *)pl, sizeof pl);
  if (hl < 0 || pll < 0) return AUTH_INVALID;
  jfield hf[1] = {{"alg", 0, 0}};
  if (jobj_scan(hdr, hdr + hl, hf, 1) != 1 || !hf[0].v || hf[0].ve - hf[0].v != 7 || memcmp(hf[0].v, "\"HS256\"", 7)) return AUTH_INVALID;
  jfield pf[3] = {{"exp", 0, 0}, {"sub", 0, 0}, {"username", 0, 0}};
  if (jobj_scan(pl, pl + pll, pf, 3) != 1) return AUTH_INVALID;
  if (!pf[0].v || (*pf[0].v != '-' && (*pf[0].v < '0' || *pf[0].v > '9'))) return AUTH_INVALID;
  char nb[64];
  size_t nl = pf[0].ve - pf[0].v;
  if (nl >= sizeof nb) return AUTH_INVALID;
  memcpy(nb, pf[0].v, nl), nb[nl] = 0;
  if (strtod(nb, NULL) <= (double)now_s) return AUTH_INVALID;
  if (!pf[1].v || *pf[1].v != '"' || !pf[2].v || *pf[2].v != '"') return AUTH_PAYLOAD;
  char sb[64];
  if (pf[1].ve - pf[1].v > 40) return AUTH_PAYLOAD;
  uint32_t sl = jstr_dec(pf[1].v, pf[1].ve, sb);
  if (parse_id(sb, sl, &j->user_id) < 0) return AUTH_PAYLOAD;
  j->store = malloc((pf[2].ve - pf[2].v) + 1);
  j->uname = j->store;
  j->unamelen = jstr_dec(pf[2].v, pf[2].ve, j->uname);
  return AUTH_OK;
}

static int is_ws_cp(uint32_t c) {
  return (c >= 9 && c <= 13) || c == 0x20 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
         c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

static uint32_t utf8_cp(const unsigned char *p, const unsigned char *e, int *len) {
  if (p[0] < 0x80 || p + 1 >= e) { *len = 1; return p[0]; }
  if ((p[0] & 0xe0) == 0xc0) { *len = 2; return (p[0] & 0x1f) << 6 | (p[1] & 0x3f); }
  if ((p[0] & 0xf0) == 0xe0 && p + 2 < e) { *len = 3; return (p[0] & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f); }
  if ((p[0] & 0xf8) == 0xf0 && p + 3 < e) { *len = 4; return (p[0] & 0x07) << 18 | (p[1] & 0x3f) << 12 | (p[2] & 0x3f) << 6 | (p[3] & 0x3f); }
  *len = 1;
  return p[0];
}

// Returns NULL on success, else the error message (status 400).
static const char *parse_body(const char *b, size_t n, job_t *j) {
  jfield f[1] = {{"body", 0, 0}};
  int k = jobj_scan(b, b + n, f, 1);
  if (k < 0) return "malformed JSON body";
  if (k == 0 || !f[0].v || *f[0].v != '"') return "body is required";
  char *dst = malloc(j->unamelen + (f[0].ve - f[0].v) + 1);
  memcpy(dst, j->uname, j->unamelen);
  free(j->store);
  j->store = dst, j->uname = dst;
  char *s = dst + j->unamelen;
  uint32_t len = jstr_dec(f[0].v, f[0].ve, s);
  const unsigned char *p = (unsigned char *)s, *e = p + len;
  int l;
  while (p < e && is_ws_cp(utf8_cp(p, e, &l))) p += l;
  while (e > p) {
    const unsigned char *q = e - 1;
    while (q > p && (*q & 0xc0) == 0x80) q--;
    if (!is_ws_cp(utf8_cp(q, e, &l))) break;
    e = q;
  }
  uint32_t units = 0;
  for (const unsigned char *q = p; q < e; q++) units += ((*q & 0xc0) != 0x80) + (*q >= 0xf0);
  if (units == 0) return "body is required";
  if (units > 500) return "body must be at most 500 characters";
  j->body = (char *)p, j->bodylen = (uint32_t)(e - p);
  return NULL;
}

static void conn_send_now(int r, uint32_t slot, const char *s, uint32_t n) {
  char *b = malloc(n);
  memcpy(b, s, n);
  conn_send(r, slot, R[r].conns[slot].gen, b, 0, n);
}

static int ci_contains(const char *s, const char *e, const char *w) {
  size_t wl = strlen(w);
  for (; e - s >= (ptrdiff_t)wl; s++)
    if (!strncasecmp(s, w, wl)) return 1;
  return 0;
}

// Returns bytes consumed (>0), 0 if incomplete, -1 to close the connection.
static int handle_request(int r, uint32_t slot, const char *d, size_t n) {
  conn_t *c = &R[r].conns[slot];
  const char *he = memmem(d, n, "\r\n\r\n", 4);
  if (!he) return n > MAX_HDR ? -1 : 0;
  const char *le = memchr(d, '\r', he + 2 - d);
  const char *sp1 = memchr(d, ' ', le - d);
  if (!sp1) return -1;
  const char *sp2 = memchr(sp1 + 1, ' ', le - sp1 - 1);
  if (!sp2) sp2 = le;
  const char *m = d, *tgt = sp1 + 1;
  size_t ml = sp1 - d, tl = sp2 - tgt;
  const char *qm = memchr(tgt, '?', tl);
  if (qm) tl = qm - tgt;
  size_t cl = 0;
  const char *auth = NULL;
  size_t authl = 0;
  int http10 = le - sp2 == 9 && !memcmp(sp2 + 1, "HTTP/1.0", 8);
  int close_after = http10, expect = 0;
  for (const char *p = le + 2; p < he;) {
    const char *eol = memmem(p, he + 2 - p, "\r\n", 2);
    const char *colon = memchr(p, ':', eol - p);
    if (colon) {
      size_t kl = colon - p;
      const char *v = colon + 1, *ve = eol;
      while (v < ve && (*v == ' ' || *v == '\t')) v++;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      if (kl == 14 && !strncasecmp(p, "content-length", 14)) {
        cl = 0;
        for (const char *q = v; q < ve; q++) {
          if (*q < '0' || *q > '9') return -1;
          cl = cl * 10 + (*q - '0');
          if (cl > MAX_REQ) return -1;
        }
      } else if (kl == 13 && !strncasecmp(p, "authorization", 13)) {
        auth = v, authl = ve - v;
      } else if (kl == 10 && !strncasecmp(p, "connection", 10)) {
        if (ci_contains(v, ve, "close")) close_after = 1;
        else if (ci_contains(v, ve, "keep-alive")) close_after = 0;
      } else if (kl == 6 && !strncasecmp(p, "expect", 6)) {
        expect = 1;
      } else if (kl == 17 && !strncasecmp(p, "transfer-encoding", 17)) {
        return -1;
      }
    }
    p = eol + 2;
  }
  size_t total = (he + 4 - d) + cl;
  if (n < total) {
    if (expect && !(c->flags & F_EXPECT_SENT)) {
      c->flags |= F_EXPECT_SENT;
      conn_send_now(r, slot, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    return 0;
  }
  c->flags &= ~F_EXPECT_SENT;
  const char *body = he + 4;
  job_t *j = job_new(r, slot);
  j->close_after = close_after;
  c->flags |= F_JOB;
  j->kind = J_ERR, j->status = 404, j->err = "not found";
  if (ml == 3 && !memcmp(m, "GET", 3)) {
    if (tl == 5 && !memcmp(tgt, "/feed", 5)) j->kind = J_FEED;
    else if (tl == 7 && !memcmp(tgt, "/health", 7)) j->kind = J_HEALTH;
    else if (tl > 7 && !memcmp(tgt, "/posts/", 7) && !memchr(tgt + 7, '/', tl - 7)) {
      if (parse_id(tgt + 7, tl - 7, &j->post_id) < 0) j->status = 400, j->err = "invalid post id";
      else j->kind = J_POST;
    }
  } else if (ml == 4 && !memcmp(m, "POST", 4)) {
    int is_create = tl == 6 && !memcmp(tgt, "/posts", 6);
    int is_like = tl >= 12 && !memcmp(tgt, "/posts/", 7) && !memcmp(tgt + tl - 5, "/like", 5) && !memchr(tgt + 7, '/', tl - 12);
    if (is_create || is_like) {
      int a = auth ? verify_jwt(auth, authl, j) : AUTH_MISSING;
      if (a == AUTH_MISSING) j->status = 401, j->err = "missing bearer token";
      else if (a == AUTH_INVALID) j->status = 401, j->err = "invalid or expired token";
      else if (a == AUTH_PAYLOAD) j->status = 401, j->err = "invalid token payload";
      else if (is_like) {
        if (parse_id(tgt + 7, tl - 12, &j->post_id) < 0) j->status = 400, j->err = "invalid post id";
        else j->kind = J_LIKE;
      } else {
        const char *err = parse_body(body, cl, j);
        if (err) j->status = 400, j->err = err;
        else j->kind = J_CREATE;
      }
    }
  }
  return (int)total;
}

static void pend_add(int r, uint32_t slot) {
  conn_t *c = &R[r].conns[slot];
  if (c->flags & F_PENDING) return;
  c->flags |= F_PENDING;
  if (npend == pendcap) {
    pendcap = pendcap ? pendcap * 2 : 1024;
    pend = realloc(pend, pendcap * sizeof *pend);
  }
  pend[npend++] = (pend_t){r, c->gen, slot};
}

// Returns -1 when the buffered input passes the cap; the caller closes the connection.
static int in_append(conn_t *c, const char *d, uint32_t n) {
  if (c->inlen + n > MAX_REQ + MAX_HDR) return -1;
  if (c->inlen + n > c->incap) {
    c->incap = (c->inlen + n) * 2;
    if (c->incap < 512) c->incap = 512;
    c->in = realloc(c->in, c->incap);
  }
  memcpy(c->in + c->inlen, d, n);
  c->inlen += n;
  return 0;
}

// Parses at most one request from the connection's buffered input.
static void try_buffered(int r, uint32_t slot) {
  conn_t *c = &R[r].conns[slot];
  if (c->state != C_OPEN || !c->inlen) return;
  if ((c->flags & F_JOB) || njobs >= jobs_cap_pass) { pend_add(r, slot); return; }
  int k = handle_request(r, slot, c->in, c->inlen);
  if (k < 0 || (k == 0 && c->inlen > MAX_REQ + MAX_HDR)) { conn_close(r, slot); return; }
  if (k == 0) return;
  c->inlen -= k;
  if (c->inlen) {
    memmove(c->in, c->in + k, c->inlen);
    pend_add(r, slot);
  } else {
    free(c->in);
    c->in = NULL, c->incap = 0;
  }
}

static void on_data(int r, uint32_t slot, const char *d, uint32_t n) {
  conn_t *c = &R[r].conns[slot];
  if (c->inlen || (c->flags & F_JOB) || njobs >= jobs_cap_pass) {
    if (in_append(c, d, n) < 0) { conn_close(r, slot); return; }
    try_buffered(r, slot);
    return;
  }
  int k = handle_request(r, slot, d, n);
  if (k < 0) { conn_close(r, slot); return; }
  if ((uint32_t)k < n) {
    if (in_append(c, d + k, n - k) < 0) { conn_close(r, slot); return; }
    if (k) pend_add(r, slot);
  }
}

// ---------------------------------------------------------------- rings, accept, completions

// Each arm gets a new epoch, so a late -ECANCELED from an older arm cannot disturb the current one.
static void arm_accept(int r) {
  struct io_uring_sqe *s = sqe_get(r);
  io_uring_prep_multishot_accept_direct(s, listen_fd, NULL, NULL, 0);
  io_uring_sqe_set_data64(s, UD(OP_ACCEPT, r, ++accept_epoch, 0));
  R[r].accept_armed = 1;
  accept_ring = r;
  accept_paused = 0;
}

static int ring_create(void) {
  if (nrings == MAX_RINGS) return -1;
  int r = nrings;
  ring_t *g = &R[r];
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
  p.cq_entries = CQ_ENTRIES;
  int ret = io_uring_queue_init_params(SQ_ENTRIES, &g->ring, &p);
  if (ret < 0) { fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret)); return -1; }
  io_uring_register_ring_fd(&g->ring);
  g->nslots = slots_per_ring;
  if ((ret = io_uring_register_files_sparse(&g->ring, g->nslots)) < 0) {
    fprintf(stderr, "register_files_sparse(%u): %s\n", g->nslots, strerror(-ret));
    return -1;
  }
  g->br = io_uring_setup_buf_ring(&g->ring, NBUF, BGID, 0, &ret);
  if (!g->br) { fprintf(stderr, "setup_buf_ring: %s\n", strerror(-ret)); return -1; }
  g->bufs = aligned_alloc(4096, (size_t)NBUF * BUF_SZ);
  for (int i = 0; i < NBUF; i++) io_uring_buf_ring_add(g->br, g->bufs + (size_t)i * BUF_SZ, BUF_SZ, i, io_uring_buf_ring_mask(NBUF), i);
  io_uring_buf_ring_advance(g->br, NBUF);
  g->conns = calloc(g->nslots, sizeof(conn_t));
  nrings++;
  fprintf(stderr, "ring %d up (%u slots)\n", r, g->nslots);
  return r;
}

static int pick_accept_ring(void) {
  int best = -1;
  uint32_t bf = 0;
  for (int r = 0; r < nrings; r++) {
    uint32_t f = R[r].nslots - R[r].open;
    if (f > bf) bf = f, best = r;
  }
  if (bf < ACCEPT_MARGIN * 4) {
    int n = ring_create();
    if (n >= 0) return n;
  }
  return best;
}

static void move_accept(void) {
  int old = accept_ring;
  if (R[old].accept_armed) {
    struct io_uring_sqe *s = sqe_get(old);
    io_uring_prep_cancel64(s, UD(OP_ACCEPT, old, accept_epoch, 0), 0);
    io_uring_sqe_set_data64(s, UD(OP_CANCEL, old, 0, 0));
    R[old].accept_armed = 0;
  }
  int r = pick_accept_ring();
  if (r >= 0 && R[r].nslots - R[r].open >= ACCEPT_MARGIN) arm_accept(r);
  else accept_paused = 1;
}

static void return_buf(int r, int bid) {
  ring_t *g = &R[r];
  io_uring_buf_ring_add(g->br, g->bufs + (size_t)bid * BUF_SZ, BUF_SZ, bid, io_uring_buf_ring_mask(NBUF), g->nret++);
}

static void handle_cqe(int r, struct io_uring_cqe *cqe) {
  uint64_t ud = cqe->user_data;
  int res = cqe->res;
  uint32_t slot = UD_SLOT(ud);
  uint16_t gen = UD_GEN(ud);
  ring_t *g = &R[r];
  switch (UD_OP(ud)) {
    case OP_ACCEPT: {
      int cur = gen == accept_epoch && accept_ring == r && g->accept_armed;
      if (res >= 0) {
        conn_t *c = &g->conns[res];
        c->gen++;
        c->state = C_OPEN, c->flags = 0;
        g->open++;
        arm_recv(r, res);
        if (cur && g->nslots - g->open < ACCEPT_MARGIN) { move_accept(); return; }
      } else if ((res == -ENFILE || res == -EMFILE) && cur) {
        g->accept_armed = 0;
        move_accept();
        return;
      }
      if (!(cqe->flags & IORING_CQE_F_MORE) && cur) {
        g->accept_armed = 0;
        if (res != -ECANCELED) arm_accept(r);
      }
      return;
    }
    case OP_RECV: {
      conn_t *c = &g->conns[slot];
      int live = c->gen == gen && c->state == C_OPEN;
      if (cqe->flags & IORING_CQE_F_BUFFER) {
        int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        if (live && res > 0) on_data(r, slot, g->bufs + (size_t)bid * BUF_SZ, res);
        return_buf(r, bid);
      }
      if (!live || (cqe->flags & IORING_CQE_F_MORE)) return;
      // A client that half-closes right after its request still gets the response.
      if (res == 0 && (c->flags & (F_JOB | F_SENDING))) c->flags |= F_CLOSE_AFTER;
      else if (res == 0 || (res < 0 && res != -ENOBUFS)) conn_close(r, slot);
      else if (g->nrearm < NBUF) g->rearm[g->nrearm].slot = slot, g->rearm[g->nrearm++].gen = gen;
      else arm_recv(r, slot);
      return;
    }
    case OP_SEND: on_send(r, slot, gen, res); return;
    default: return;
  }
}

static int drain_ring(int r) {
  ring_t *g = &R[r];
  struct io_uring_cqe *cqe;
  unsigned head, n = 0;
  io_uring_for_each_cqe(&g->ring, head, cqe) {
    handle_cqe(r, cqe);
    n++;
  }
  io_uring_cq_advance(&g->ring, n);
  if (g->nret) io_uring_buf_ring_advance(g->br, g->nret), g->nret = 0;
  for (int i = 0; i < g->nrearm; i++) {
    conn_t *c = &g->conns[g->rearm[i].slot];
    if (c->state == C_OPEN && c->gen == g->rearm[i].gen) arm_recv(r, g->rearm[i].slot);
  }
  g->nrearm = 0;
  return n;
}

// ---------------------------------------------------------------- batch

static void run_batch(void) {
  int nw = 0;
  for (int i = 0; i < njobs; i++) nw += jobs[i].kind == J_LIKE || jobs[i].kind == J_CREATE;
  if (nw) {
    int ok = run(st_begin_w) == SQLITE_OK;
    for (int i = 0; i < njobs && ok; i++)
      if (jobs[i].kind == J_LIKE || jobs[i].kind == J_CREATE) run_write(&jobs[i]);
    if (!ok || run(st_commit) != SQLITE_OK) {
      if (ok) run(st_rollback);
      for (int i = 0; i < njobs; i++)
        if (jobs[i].kind == J_LIKE || jobs[i].kind == J_CREATE) {
          free(jobs[i].resp);
          job_error(&jobs[i], 500, "internal server error");
        }
    }
  }
  int began = 0;
  for (int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    if (j->kind == J_FEED || j->kind == J_POST || j->kind == J_HEALTH) {
      if (!began) began = run(st_begin) == SQLITE_OK ? 1 : -1;
      run_read(j);
    } else if (j->kind == J_ERR) {
      job_error(j, j->status, j->err);
    }
  }
  if (began == 1) run(st_commit);
  for (int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    conn_t *c = &R[j->ring].conns[j->slot];
    if (c->gen == j->gen) c->flags &= ~F_JOB;
    conn_send(j->ring, j->slot, j->gen, j->resp, j->resp_off, j->resp_len);
    // Set only now: earlier, a previous response's completion would close before this one is queued.
    if (j->close_after && c->gen == j->gen && c->state == C_OPEN) {
      c->flags |= F_CLOSE_AFTER;
      if (!(c->flags & F_SENDING)) conn_close(j->ring, j->slot);
    }
    free(j->store);
  }
  njobs = 0;
}

static void process_pending(void) {
  if (!npend) return;
  int n = npend;
  pend_t *t = pend;
  pend = NULL, npend = pendcap = 0;
  for (int i = 0; i < n; i++) {
    conn_t *c = &R[t[i].ring].conns[t[i].slot];
    if (c->gen != t[i].gen) continue;
    c->flags &= ~F_PENDING;
    try_buffered(t[i].ring, t[i].slot);
  }
  free(t);
}

static int setup_listener(const char *host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", host); exit(1); }
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 65535) < 0) { perror("bind/listen"); exit(1); }
  return fd;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  clock_gettime(CLOCK_MONOTONIC, &start_ts);
  const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET"), *host = getenv("HOST"), *port = getenv("PORT");
  if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
  sqlite3_initialize();
  b64_init();
  hmac_setup(secret);
  db_open(path);
  scratch.p = malloc(65536), scratch.cap = 65536, scratch.len = 0;
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  slots_per_ring = rl.rlim_cur < SLOTS_MAX ? (uint32_t)rl.rlim_cur : SLOTS_MAX;
  listen_fd = setup_listener(host ? host : "0.0.0.0", port ? atoi(port) : 3000);
  if (ring_create() < 0) return 1;
  arm_accept(0);
  fprintf(stderr, "listening on %s:%s\n", host ? host : "0.0.0.0", port ? port : "3000");
  for (;;) {
    now_s = time(NULL);
    process_pending();
    int got = 0;
    for (int r = 0; r < nrings; r++) {
      io_uring_submit_and_get_events(&R[r].ring);
      got += drain_ring(r);
    }
    if (njobs) run_batch(), got = 1;
    if (wal_frames >= CKPT_FRAMES) {
      for (int r = 0; r < nrings; r++) io_uring_submit(&R[r].ring);
      sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_RESTART, NULL, NULL);
      wal_frames = 0;
    }
    if (!got && !npend) {
      struct io_uring_cqe *cqe;
      struct __kernel_timespec ts = {.tv_sec = nrings > 1 ? 0 : 1, .tv_nsec = nrings > 1 ? 1000000 : 0};
      io_uring_submit_and_wait_timeout(&R[accept_ring].ring, &cqe, 1, &ts, NULL);
    }
  }
}
