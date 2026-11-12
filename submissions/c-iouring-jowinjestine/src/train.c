/* train.c: the PGO training client (build time only). Drives the load test's request mix and
 * the test suite's error paths through an instrumented server over a few dozen keep-alive
 * connections. Plain blocking sockets; nothing here ships. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"

#define NCONN 48

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static size_t b64url(const uint8_t *s, size_t n, char *out) {
  size_t o = 0, i = 0;
  while (i + 3 <= n) {
    uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
    out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63]; out[o++] = B64[(v >> 6) & 63]; out[o++] = B64[v & 63];
    i += 3;
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)s[i] << 16;
    out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63];
  } else if (n - i == 2) {
    uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8;
    out[o++] = B64[v >> 18]; out[o++] = B64[(v >> 12) & 63]; out[o++] = B64[(v >> 6) & 63];
  }
  out[o] = 0;
  return o;
}

static void make_token(const struct hmac_key *k, long uid, const char *name, long exp, char *out) {
  char payload[256], hb[64], pb[512];
  snprintf(payload, sizeof payload, "{\"sub\":\"%ld\",\"username\":\"%s\",\"iat\":%ld,\"exp\":%ld}", uid, name, exp - 3600, exp);
  b64url((const uint8_t *)"{\"alg\":\"HS256\",\"typ\":\"JWT\"}", 27, hb);
  b64url((const uint8_t *)payload, strlen(payload), pb);
  char msg[800];
  int mn = snprintf(msg, sizeof msg, "%s.%s", hb, pb);
  uint8_t sig[32];
  hmac_sha256(k, (const uint8_t *)msg, (size_t)mn, sig);
  char sb[64];
  b64url(sig, 32, sb);
  sprintf(out, "%s.%s", msg, sb);
}

static int dial(const char *host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, host, &a.sin_addr);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); exit(1); }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct timeval tv = {.tv_sec = 30, .tv_usec = 0}; /* a lost reply fails the training; it never hangs the build */
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

struct c {
  int fd;
  char in[262144]; /* bytes received and not yet consumed: a read may bring more than one response */
  size_t n;
};

static int send_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

/* Reads one full response and returns its status code; whatever follows it in the same read
 * (the next pipelined response) stays in the buffer for the next call. -1 on EOF, error or
 * timeout. */
static int read_response(struct c *c) {
  for (;;) {
    if (c->n) {
      c->in[c->n] = 0;
      char *he = strstr(c->in, "\r\n\r\n");
      if (he) {
        size_t head_len = (size_t)(he + 4 - c->in);
        char *cl = strcasestr(c->in, "content-length:");
        size_t want = cl && cl < he ? (size_t)atoi(cl + 15) : 0;
        if (c->n >= head_len + want) {
          int st = atoi(c->in + 9);
          size_t used = head_len + want;
          memmove(c->in, c->in + used, c->n - used);
          c->n -= used;
          return st;
        }
      }
    }
    if (c->n >= sizeof c->in - 1) return -1;
    ssize_t k = recv(c->fd, c->in + c->n, sizeof c->in - c->n - 1, 0);
    if (k <= 0) return -1;
    c->n += (size_t)k;
  }
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: train host port secret requests\n"); return 2; }
  const char *host = argv[1];
  int port = atoi(argv[2]);
  long total = atol(argv[4]);
  struct hmac_key key;
  hmac_key_init(&key, (const uint8_t *)argv[3], strlen(argv[3]));
  long now = (long)time(NULL);
  static struct c conns[NCONN];
  static char tokens[NCONN][1024];
  char expired[1024], badsub[1024], noname[1024];
  for (int i = 0; i < NCONN; i++) {
    conns[i].fd = dial(host, port);
    char name[32];
    snprintf(name, sizeof name, "trainer_%d", i + 1);
    make_token(&key, i + 1, name, now + 3600, tokens[i]);
  }
  make_token(&key, 1, "trainer_1", now - 3600, expired);
  {
    char payload[] = "{\"sub\":\"abc\",\"username\":\"x\",\"iat\":1,\"exp\":9999999999}";
    char hb[64], pb[512], msg[800], sb[64];
    b64url((const uint8_t *)"{\"alg\":\"HS256\",\"typ\":\"JWT\"}", 27, hb);
    b64url((const uint8_t *)payload, strlen(payload), pb);
    int mn = snprintf(msg, sizeof msg, "%s.%s", hb, pb);
    uint8_t sig[32];
    hmac_sha256(&key, (const uint8_t *)msg, (size_t)mn, sig);
    b64url(sig, 32, sb);
    sprintf(badsub, "%s.%s", msg, sb);
    char payload2[] = "{\"sub\":\"1\",\"iat\":1,\"exp\":9999999999}";
    b64url((const uint8_t *)payload2, strlen(payload2), pb);
    mn = snprintf(msg, sizeof msg, "%s.%s", hb, pb);
    hmac_sha256(&key, (const uint8_t *)msg, (size_t)mn, sig);
    b64url(sig, 32, sb);
    sprintf(noname, "%s.%s", msg, sb);
  }
  srand(7);
  long posts_max = 40000, ok = 0, bad = 0;
  char req[4096];
  for (long i = 0; i < total; i++) {
    struct c *c = &conns[i % NCONN];
    const char *tok = tokens[i % NCONN];
    int r = rand() % 1000, n, want = 200;
    long id = posts_max - rand() % 25;
    if (r < 450) {
      n = snprintf(req, sizeof req, "GET /feed HTTP/1.1\r\nHost: t\r\nUser-Agent: k6/1.0\r\n\r\n");
    } else if (r < 820) {
      n = snprintf(req, sizeof req, "GET /posts/%ld HTTP/1.1\r\nHost: t\r\nUser-Agent: k6/1.0\r\n\r\n", id);
    } else if (r < 930) {
      n = snprintf(req, sizeof req, "POST /posts/%ld/like HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n", id, tok);
      want = 0;
    } else if (r < 960) {
      char body[700];
      int bn = snprintf(body, sizeof body, "{\"body\":\"trainer says hi at %ld (iter %ld) \\\"quoted\\\" caf\\u00e9\"}", now, i);
      n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", tok, bn, body);
      want = 201;
    } else { /* the error paths */
      switch (rand() % 14) {
      case 0: n = snprintf(req, sizeof req, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n"); want = 404; break;
      case 1: n = snprintf(req, sizeof req, "GET /posts/abc HTTP/1.1\r\nHost: t\r\n\r\n"); want = 400; break;
      case 2: n = snprintf(req, sizeof req, "GET /posts/999999999 HTTP/1.1\r\nHost: t\r\n\r\n"); want = 404; break;
      case 3: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}"); want = 401; break;
      case 4: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer abc\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}"); want = 401; break;
      case 5: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}", expired); want = 401; break;
      case 6: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}", badsub); want = 401; break;
      case 7: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}", noname); want = 401; break;
      case 8: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 11\r\n\r\n{\"body\":\"\"}", tok); want = 400; break;
      case 9: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 9\r\n\r\n{bad json", tok); want = 400; break;
      case 10: n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 12\r\n\r\n{\"body\":123}", tok); want = 400; break;
      case 11: n = snprintf(req, sizeof req, "POST /posts/999999999/like HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", tok); want = 404; break;
      case 12: n = snprintf(req, sizeof req, "POST /posts/abc/like HTTP/1.1\r\nHost: t\r\n\r\n"); want = 401; break;
      default: n = snprintf(req, sizeof req, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n"); want = 200; break;
      }
    }
    if (send_all(c->fd, req, (size_t)n)) { fprintf(stderr, "send failed\n"); return 1; }
    int st = read_response(c);
    if (st < 0) { fprintf(stderr, "connection lost at request %ld\n", i); return 1; }
    if (want == 0 ? (st != 200 && st != 201) : st != want) bad++;
    else ok++;
  }
  /* a few split and pipelined requests, and a close mid-stream */
  {
    struct c *c = &conns[0];
    const char *a = "GET /fe", *b = "ed HTTP/1.1\r\nHost: t\r\n\r\n";
    send_all(c->fd, a, strlen(a));
    usleep(20000);
    send_all(c->fd, b, strlen(b));
    read_response(c);
    const char *two = "GET /health HTTP/1.1\r\nHost: t\r\n\r\nGET /health HTTP/1.1\r\nHost: t\r\n\r\n";
    send_all(c->fd, two, strlen(two));
    if (read_response(c) != 200 || read_response(c) != 200) { fprintf(stderr, "pipelined replies missing\n"); return 1; }
  }
  for (int i = 0; i < NCONN; i++) close(conns[i].fd);
  fprintf(stderr, "train: %ld ok, %ld unexpected\n", ok, bad);
  return bad > total / 100 ? 1 : 0;
}
