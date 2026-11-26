/* train.c: build-time only, the PGO training client (build.sh runs it against the instrumented server on a synthetic
 * scratch database; it is never part of the scored server). Keep-alive connections, one thread each, looping over the
 * load test's user loop without think time: GET /feed, GET /posts/:id of a post from that feed, a like (15 %), a new
 * post (2 %). Every 64th loop also sends one of the test suite's error cases (bad or missing token, missing post, bad
 * body, unknown route, health), so those paths are profiled too, at roughly their (rare) weight.
 * Tokens are HS256 JWTs signed here with the training run's own secret (SHA-256 / HMAC below, from FIPS 180-4).
 *   train <host> <port> <jwt secret> <users> <requests> <connections>
 * Exit 0 if fewer than 1 % of the answers were unexpected. Same design as the Graal entry's train/Train.kt. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- SHA-256, HMAC, base64url */
typedef struct { uint32_t h[8]; uint8_t b[64]; uint64_t n; } sha;
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
  0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
  0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
  0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
  0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(sha *s, const uint8_t *p) {
  uint32_t w[64], a[8];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ w[i-15] >> 3, s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ w[i-2] >> 10;
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  memcpy(a, s->h, sizeof a);
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
    uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
    memmove(a + 1, a, 7 * sizeof a[0]);
    a[4] += t1; a[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_init(sha *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, sizeof iv); s->n = 0;
}
static void sha_add(sha *s, const uint8_t *p, size_t n) {
  while (n--) { s->b[s->n++ % 64] = *p++; if (s->n % 64 == 0) sha_block(s, s->b); }
}
static void sha_end(sha *s, uint8_t out[32]) {
  uint64_t bits = s->n * 8;
  uint8_t pad = 0x80, z = 0;
  sha_add(s, &pad, 1);
  while (s->n % 64 != 56) sha_add(s, &z, 1);
  for (int i = 7; i >= 0; i--) { uint8_t c = (uint8_t)(bits >> (8 * i)); sha_add(s, &c, 1); }
  for (int i = 0; i < 8; i++) { out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16; out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i]; }
}
static void hmac(const char *key, const uint8_t *msg, size_t n, uint8_t out[32]) {
  uint8_t k[64] = {0}, ip[64], op[64], in[32];
  size_t kl = strlen(key);
  if (kl > 64) { sha s; sha_init(&s); sha_add(&s, (const uint8_t *)key, kl); sha_end(&s, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
  sha s; sha_init(&s); sha_add(&s, ip, 64); sha_add(&s, msg, n); sha_end(&s, in);
  sha_init(&s); sha_add(&s, op, 64); sha_add(&s, in, 32); sha_end(&s, out);
}
static int b64url(const uint8_t *p, int n, char *o) {
  static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  int k = 0;
  for (int i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i+1] << 8 : 0) | (i + 2 < n ? p[i+2] : 0);
    o[k++] = A[v >> 18 & 63]; o[k++] = A[v >> 12 & 63];
    if (i + 1 < n) o[k++] = A[v >> 6 & 63];
    if (i + 2 < n) o[k++] = A[v & 63];
  }
  o[k] = 0;
  return k;
}
static void token(const char *secret, int uid, long exp, char *out) {
  char p[256], h[64], ph[512];
  b64url((const uint8_t *)"{\"alg\":\"HS256\",\"typ\":\"JWT\"}", 27, h);
  int pn = snprintf(p, sizeof p, "{\"sub\":\"%d\",\"username\":\"user_%d\",\"iat\":%ld,\"exp\":%ld}", uid, uid, exp - 3600, exp);
  char pb[512]; b64url((const uint8_t *)p, pn, pb);
  int n = snprintf(ph, sizeof ph, "%s.%s", h, pb);
  uint8_t mac[32]; char mb[64];
  hmac(secret, (const uint8_t *)ph, (size_t)n, mac);
  b64url(mac, 32, mb);
  sprintf(out, "%s.%s", ph, mb);
}

/* ---------------------------------------------------------------- client */
static const char *g_host, *g_secret;
static int g_port, g_users;
static long g_total, g_exp;
static atomic_long done, fails;

typedef struct { int fd; char buf[1 << 16]; int body, blen; } conn;

static int dial(conn *c) {
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
  inet_pton(AF_INET, g_host, &a.sin_addr);
  for (;;) {
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {30, 0};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) == 0) return 0;
    close(c->fd);
    if (atomic_fetch_add(&fails, 1) > 1000) return -1;
    usleep(100000);
  }
}

/* Sends one request, reads the whole answer; returns the status (body at c->buf + c->body), or -1. */
static int call(conn *c, const char *req, int n) {
  if (send(c->fd, req, (size_t)n, MSG_NOSIGNAL) != n) return -1;
  int got = 0, hend = -1;
  while (hend < 0) {
    int r = (int)recv(c->fd, c->buf + got, sizeof c->buf - 1 - (size_t)got, 0);
    if (r <= 0) return -1;
    got += r; c->buf[got] = 0;
    char *e = strstr(c->buf, "\r\n\r\n");
    if (e) hend = (int)(e - c->buf) + 4;
  }
  int status = atoi(c->buf + 9), cl = 0, close_ = 0;
  for (char *p = c->buf; p < c->buf + hend; p++) {
    if (*p != '\n') continue;
    if (!strncasecmp(p + 1, "content-length:", 15)) cl = atoi(p + 16);
    if (!strncasecmp(p + 1, "connection: close", 17)) close_ = 1;
  }
  while (got < hend + cl) {
    int r = (int)recv(c->fd, c->buf + got, sizeof c->buf - 1 - (size_t)got, 0);
    if (r <= 0) return -1;
    got += r;
  }
  c->buf[hend + cl] = 0;
  c->body = hend; c->blen = cl;
  if (close_) { close(c->fd); if (dial(c)) return -1; }
  return status;
}

static void expect(int st, int a, int b) {
  atomic_fetch_add(&done, 1);
  if (st != a && st != b) atomic_fetch_add(&fails, 1);
}

static int req(char *o, const char *method, const char *path, const char *tok, const char *body) {
  int n = sprintf(o, "%s %s HTTP/1.1\r\nHost: t\r\n", method, path);
  if (tok) n += sprintf(o + n, "Authorization: Bearer %s\r\n", tok);
  if (body) n += sprintf(o + n, "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
  else n += sprintf(o + n, "\r\n");
  return n;
}

static void *worker(void *arg) {
  int id = (int)(intptr_t)arg;
  static __thread conn c;
  char tok[512], bad[512], r[2048], path[64], body[256];
  unsigned seed = (unsigned)id * 2654435761u + 1;
  if (dial(&c)) return 0;
  token(g_secret, 1 + id % g_users, g_exp, tok);
  strcpy(bad, tok); bad[strlen(bad) - 2] = 'A'; bad[strlen(bad) - 1] = 'A';
  for (long loop = 0; atomic_load(&done) < g_total; loop++) {
    int st = call(&c, r, req(r, "GET", "/feed", 0, 0));
    expect(st, 200, 200);
    if (st < 0) { close(c.fd); if (dial(&c)) return 0; continue; }
    /* a random post id from the feed */
    long ids[20]; int nid = 0;
    for (char *p = c.buf + c.body; nid < 20 && (p = strstr(p, "{\"id\":")); p += 6) ids[nid++] = atol(p + 6);
    long pid = nid ? ids[rand_r(&seed) % nid] : 1;
    snprintf(path, sizeof path, "/posts/%ld", pid);
    expect(call(&c, r, req(r, "GET", path, 0, 0)), 200, 200);
    if (rand_r(&seed) % 100 < 15) {
      snprintf(path, sizeof path, "/posts/%ld/like", pid);
      expect(call(&c, r, req(r, "POST", path, tok, 0)), 200, 201);
    }
    if (rand_r(&seed) % 100 < 2) {
      snprintf(body, sizeof body, "{\"body\":\"user_%d says \\\"hi\\\" (loop %ld) caf\xc3\xa9 \\u00e9\"}", id, loop);
      expect(call(&c, r, req(r, "POST", "/posts", tok, body)), 201, 201);
    }
    if (loop % 64 == 63) switch ((loop / 64) % 8) {
      case 0: expect(call(&c, r, req(r, "GET", "/posts/999999999", 0, 0)), 404, 404); break;
      case 1: expect(call(&c, r, req(r, "POST", "/posts", 0, "{\"body\":\"x\"}")), 401, 401); break;
      case 2: expect(call(&c, r, req(r, "POST", "/posts", bad, "{\"body\":\"x\"}")), 401, 401); break;
      case 3: expect(call(&c, r, req(r, "POST", "/posts", tok, "{\"body\":\"\"}")), 400, 400); break;
      case 4: expect(call(&c, r, req(r, "POST", "/posts", tok, "{\"nobody\":1}")), 400, 400); break;
      case 5: expect(call(&c, r, req(r, "GET", "/nope", 0, 0)), 404, 404); break;
      case 6: expect(call(&c, r, req(r, "POST", "/posts/999999999/like", tok, 0)), 404, 404); break;
      default: expect(call(&c, r, req(r, "GET", "/health", 0, 0)), 200, 200); break;
    }
  }
  close(c.fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 7) { fprintf(stderr, "usage: train <host> <port> <secret> <users> <requests> <connections>\n"); return 2; }
  g_host = argv[1]; g_port = atoi(argv[2]); g_secret = argv[3]; g_users = atoi(argv[4]); g_total = atol(argv[5]);
  int nconn = atoi(argv[6]);
  g_exp = (long)time(0) + 7200;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pthread_t th[1024];
  if (nconn > 1024) nconn = 1024;
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512 * 1024);
  for (int i = 0; i < nconn; i++) pthread_create(&th[i], &at, worker, (void *)(intptr_t)i);
  for (int i = 0; i < nconn; i++) pthread_join(th[i], 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long d = atomic_load(&done), f = atomic_load(&fails);
  printf("train: %ld requests in %.1f s over %d connections, %ld unexpected\n", d,
         (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, nconn, f);
  return f * 100 > d ? 1 : 0;
}
