/* train.c: build-time load client for profile-guided optimisation. Opens keep-alive
 * connections and sends the load test's request mix plus the error paths the test suite
 * checks, against a synthetic database the server generates. Not part of the server.
 *
 *   train <host> <port> <secret> <users> <posts> <requests> <connections>
 */
#define _GNU_SOURCE
#include "sha256.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static size_t b64url_enc(char *o, const uint8_t *p, size_t n) {
  size_t k = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
    o[k++] = b64url[v >> 18]; o[k++] = b64url[(v >> 12) & 63]; o[k++] = b64url[(v >> 6) & 63]; o[k++] = b64url[v & 63];
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)p[i] << 16;
    o[k++] = b64url[v >> 18]; o[k++] = b64url[(v >> 12) & 63];
  } else if (n - i == 2) {
    uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
    o[k++] = b64url[v >> 18]; o[k++] = b64url[(v >> 12) & 63]; o[k++] = b64url[(v >> 6) & 63];
  }
  return k;
}

static size_t make_token(char *out, const struct hmac_key *k, long uid, long exp) {
  char payload[256];
  int pn = snprintf(payload, sizeof payload, "{\"sub\":\"%ld\",\"username\":\"user_%ld\",\"iat\":%ld,\"exp\":%ld}", uid, uid, exp - 3600, exp);
  size_t n = b64url_enc(out, (const uint8_t *)"{\"alg\":\"HS256\",\"typ\":\"JWT\"}", 27);
  out[n++] = '.';
  n += b64url_enc(out + n, (const uint8_t *)payload, (size_t)pn);
  uint8_t mac[32];
  hmac_sha256(k, (const uint8_t *)out, n, mac);
  out[n++] = '.';
  n += b64url_enc(out + n, mac, 32);
  out[n] = 0;
  return n;
}

static int connect_to(const char *host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  inet_pton(AF_INET, host, &a.sin_addr);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); exit(1); }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return fd;
}

static int send_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return -1;
    p += w; n -= (size_t)w;
  }
  return 0;
}

/* Reads one response; returns the status code or -1. */
static int read_response(int fd, char *buf, size_t cap, char **body_out, size_t *body_len) {
  size_t n = 0;
  for (;;) {
    ssize_t r = recv(fd, buf + n, cap - n - 1, 0);
    if (r <= 0) return -1;
    n += (size_t)r;
    buf[n] = 0;
    char *end = strstr(buf, "\r\n\r\n");
    if (!end) continue;
    char *cl = strcasestr(buf, "content-length:");
    size_t need = cl ? strtoul(cl + 15, NULL, 10) : 0;
    size_t head = (size_t)(end + 4 - buf);
    while (n < head + need) {
      r = recv(fd, buf + n, cap - n - 1, 0);
      if (r <= 0) return -1;
      n += (size_t)r;
    }
    if (body_out) { *body_out = buf + head; *body_len = need; }
    return atoi(buf + 9);
  }
}

int main(int argc, char **argv) {
  if (argc < 8) { fprintf(stderr, "usage: %s host port secret users posts requests connections\n", argv[0]); return 2; }
  const char *host = argv[1];
  int port = atoi(argv[2]);
  long users = atol(argv[4]), posts = atol(argv[5]), total = atol(argv[6]);
  int nconn = atoi(argv[7]);
  struct hmac_key key;
  hmac_key_init(&key, (const uint8_t *)argv[3], strlen(argv[3]));
  long now = (long)time(NULL);
  srand(12345);
  int *fds = calloc((size_t)nconn, sizeof *fds);
  for (int i = 0; i < nconn; i++) fds[i] = connect_to(host, port);
  static char req[8192], resp[65536], tok[1024];
  long ok = 0, bad = 0;
  for (long i = 0; i < total; i++) {
    int fd = fds[i % nconn];
    int r = rand() % 1000;
    int n;
    long pid = 1 + rand() % posts, uid = 1 + rand() % users;
    int expect = 200;
    if (r < 440) {
      n = snprintf(req, sizeof req, "GET /feed HTTP/1.1\r\nHost: x\r\nUser-Agent: train\r\nAccept: */*\r\n\r\n");
    } else if (r < 880) {
      /* the load test reads posts it just saw in the feed: mostly the newest ones */
      long p = (rand() % 4) ? posts - rand() % 20 : pid;
      n = snprintf(req, sizeof req, "GET /posts/%ld HTTP/1.1\r\nHost: x\r\nUser-Agent: train\r\nAccept: */*\r\n\r\n", p);
    } else if (r < 950) {
      make_token(tok, &key, uid, now + 3600);
      n = snprintf(req, sizeof req, "POST /posts/%ld/like HTTP/1.1\r\nHost: x\r\nUser-Agent: train\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n", pid, tok);
      expect = 201;
    } else if (r < 965) {
      make_token(tok, &key, uid, now + 3600);
      char body[200];
      int bn = snprintf(body, sizeof body, "{\"body\":\"user_%ld says hi at %ld (train %ld)\"}", uid, now, i);
      n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: x\r\nUser-Agent: train\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", tok, bn, body);
      expect = 201;
    } else if (r < 975) {
      n = snprintf(req, sizeof req, "GET /posts/abc HTTP/1.1\r\nHost: x\r\n\r\n");
      expect = 400;
    } else if (r < 985) {
      n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}");
      expect = 401;
    } else if (r < 990) {
      make_token(tok, &key, uid, now - 10);
      n = snprintf(req, sizeof req, "POST /posts/1/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", tok);
      expect = 401;
    } else if (r < 995) {
      make_token(tok, &key, uid, now + 3600);
      n = snprintf(req, sizeof req, "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 9\r\n\r\n{bad json", tok);
      expect = 400;
    } else {
      n = snprintf(req, sizeof req, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    }
    if (send_all(fd, req, (size_t)n) < 0) { fprintf(stderr, "send failed at %ld\n", i); return 1; }
    int st = read_response(fd, resp, sizeof resp, NULL, NULL);
    if (st == expect || (expect == 201 && st == 200) || (expect == 200 && st == 404)) ok++; else bad++;
    if (st < 0) { fprintf(stderr, "connection lost at %ld\n", i); return 1; }
  }
  fprintf(stderr, "train: %ld requests, %ld unexpected statuses\n", ok + bad, bad);
  return bad > total / 100 ? 1 : 0;
}
