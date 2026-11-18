#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SECRET "twelve-dollar-challenge"
#define DEFAULT_PORT 3000

#define RECV_BUF (256u * 1024u)
#define RESP_BUF (256u * 1024u)

#define RUNS 8

struct sha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
};

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr32(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

static void sha256_block(struct sha256 *s, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

static void sha256_init(struct sha256 *s) {
  s->h[0] = 0x6a09e667u;
  s->h[1] = 0xbb67ae85u;
  s->h[2] = 0x3c6ef372u;
  s->h[3] = 0xa54ff53au;
  s->h[4] = 0x510e527fu;
  s->h[5] = 0x9b05688cu;
  s->h[6] = 0x1f83d9abu;
  s->h[7] = 0x5be0cd19u;
  s->len = 0;
  s->n = 0;
}

static void sha256_update(struct sha256 *s, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  s->len += (uint64_t)len;
  if (s->n) {
    size_t need = 64 - s->n;
    if (need > len) need = len;
    memcpy(s->buf + s->n, p, need);
    s->n += need;
    p += need;
    len -= need;
    if (s->n == 64) {
      sha256_block(s, s->buf);
      s->n = 0;
    }
  }
  while (len >= 64) {
    sha256_block(s, p);
    p += 64;
    len -= 64;
  }
  if (len) {
    memcpy(s->buf + s->n, p, len);
    s->n += len;
  }
}

static void sha256_final(struct sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8u;
  uint8_t pad = 0x80;
  sha256_update(s, &pad, 1);
  uint8_t z = 0x00;
  while (s->n != 56) sha256_update(s, &z, 1);
  uint8_t L[8];
  for (int i = 0; i < 8; i++) L[7 - i] = (uint8_t)(bits >> (8u * (unsigned)i));
  sha256_update(s, L, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(s->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)s->h[i];
  }
}

static void hmac_sha256(const uint8_t *key, size_t keylen,
                        const uint8_t *msg, size_t msglen, uint8_t out[32]) {
  uint8_t k[64];
  uint8_t ipad[64], opad[64];
  memset(k, 0, sizeof(k));
  if (keylen > 64) {
    struct sha256 t;
    sha256_init(&t);
    sha256_update(&t, key, keylen);
    sha256_final(&t, k);
  } else if (keylen) {
    memcpy(k, key, keylen);
  }
  for (int i = 0; i < 64; i++) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36u);
    opad[i] = (uint8_t)(k[i] ^ 0x5cu);
  }
  struct sha256 s;
  sha256_init(&s);
  sha256_update(&s, ipad, 64);
  sha256_update(&s, msg, msglen);
  sha256_final(&s, k);
  sha256_init(&s);
  sha256_update(&s, opad, 64);
  sha256_update(&s, k, 32);
  sha256_final(&s, out);
}

static size_t b64url(const uint8_t *in, size_t n, char *out) {
  static const char tab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0, i = 0;
  while (i + 3 <= n) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    out[o++] = tab[(v >> 6) & 63];
    out[o++] = tab[v & 63];
    i += 3;
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
  } else if (n - i == 2) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    out[o++] = tab[(v >> 6) & 63];
  }
  return o;
}

static int make_jwt(const char *user_id, const char *username, char *out,
                    size_t cap, size_t *out_len) {
  static const char header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  char payload[384];
  int plen = snprintf(payload, sizeof(payload),
                      "{\"sub\":\"%s\",\"username\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
                      user_id, username, (long long)time(NULL),
                      (long long)(time(NULL) + 86400));
  if (plen <= 0 || (size_t)plen >= sizeof(payload)) return -1;

  uint8_t hmac[32];
  char b64h[128], b64p[768];
  size_t nh = b64url((const uint8_t *)header, sizeof(header) - 1, b64h);
  size_t np = b64url((const uint8_t *)payload, (size_t)plen, b64p);

  char signing[128 + 768 + 2];
  memcpy(signing, b64h, nh);
  signing[nh] = '.';
  memcpy(signing + nh + 1, b64p, np);
  size_t sl = nh + 1 + np;

  hmac_sha256((const uint8_t *)SECRET, sizeof(SECRET) - 1,
              (const uint8_t *)signing, sl, hmac);
  char b64s[64];
  size_t ns = b64url(hmac, 32, b64s);

  size_t total = sl + 1 + ns;
  if (total + 1 > cap) return -1;
  memcpy(out, signing, sl);
  out[sl] = '.';
  memcpy(out + sl + 1, b64s, ns);
  out[total] = '\0';
  if (out_len) *out_len = total;
  return 0;
}

struct conn {
  int fd;
  uint16_t port;
  size_t rlen;
  uint8_t rbuf[RECV_BUF];
};

#define HT_CAP 512
#define HT_TOMB INT64_MIN

static int64_t ht_key[HT_CAP];
static unsigned ht_seed = 12345u;

static unsigned ht_slot(int64_t key) {
  uint64_t x = (uint64_t)key * 0x9E3779B97F4A7C15ull;
  return (unsigned)(x % HT_CAP);
}

static void ht_clear(void) {
  for (unsigned i = 0; i < HT_CAP; i++) ht_key[i] = HT_TOMB;
}

static void ht_add(int64_t key) {
  if (key <= 0) return;
  unsigned i = ht_slot(key);
  while (ht_key[i] != HT_TOMB) {
    if (ht_key[i] == key) return;
    i = (i + 1u) % HT_CAP;
  }
  ht_key[i] = key;
}

static void conn_open(struct conn *c, uint16_t port) {
  c->port = port;
  c->rlen = 0;
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) {
    perror("socket");
    return;
  }
  int one = 1;
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(c->fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  struct timeval tv = {.tv_sec = 8, .tv_usec = 0};
  setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int flags = fcntl(c->fd, F_GETFL, 0);
  fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);
  int rc = connect(c->fd, (struct sockaddr *)&sa, sizeof(sa));
  if (rc < 0 && errno == EINPROGRESS) {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(c->fd, &wf);
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    rc = select(c->fd + 1, NULL, &wf, NULL, &tv);
    if (rc > 0) {
      int soerr = 0;
      socklen_t sl = sizeof(soerr);
      getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
      rc = soerr == 0 ? 0 : -1;
    } else {
      rc = -1;
    }
  }
  if (rc < 0) {
    fprintf(stderr, "train: connect 127.0.0.1:%u failed: %s\n", (unsigned)port,
            strerror(errno));
    close(c->fd);
    c->fd = -1;
    return;
  }
  fcntl(c->fd, F_SETFL, flags);
}

static void conn_close(struct conn *c) {
  if (c->fd >= 0) close(c->fd);
  c->fd = -1;
  c->rlen = 0;
}

static int send_all(struct conn *c, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  while (len) {
    ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int recv_some(struct conn *c) {
  ssize_t n = recv(c->fd, c->rbuf + c->rlen, RECV_BUF - c->rlen, 0);
  if (n > 0) {
    c->rlen += (size_t)n;
    return (int)n;
  }
  if (n == 0) return 0;
  if (errno == EINTR) return -1;
  return -1;
}

static int parse_response(struct conn *c, char *out, size_t outcap, size_t *blen) {
  char *s = (char *)c->rbuf;
  char *hdrend = NULL;
  for (;;) {
    hdrend = c->rlen ? memmem(s, c->rlen, "\r\n\r\n", 4) : NULL;
    if (hdrend) break;
    if (c->rlen > 64u * 1024u) return -1;
    if (recv_some(c) <= 0) return -1;
  }
  size_t hlen = (size_t)(hdrend - s) + 4u;

  int status = 0;
  if (c->rlen < 12 || memcmp(s, "HTTP/", 5) != 0) return -1;
  status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
  if (status < 100 || status > 599) return -1;

  size_t clen = 0;
  char *line = s;
  char *lim = s + hlen - 4;
  while (line < lim) {
    char *nl = memmem(line, (size_t)(lim - line), "\r\n", 2);
    if (!nl) break;
    size_t ll = (size_t)(nl - line);
    if (ll >= 15 && strncasecmp(line, "content-length:", 15) == 0) {
      char *vp = line + 15;
      while (vp < nl && (*vp == ' ' || *vp == '\t')) vp++;
      char *end = NULL;
      unsigned long v = strtoul(vp, &end, 10);
      if (end != vp && end <= nl) clen = (size_t)v;
    }
    line = nl + 2;
  }

  size_t need = hlen + clen;
  while (c->rlen < need) {
    if (need > RECV_BUF) return -1;
    if (recv_some(c) <= 0) return -1;
  }
  if (blen) {
    size_t copy = clen < outcap - 1 ? clen : outcap - 1;
    memcpy(out, s + hlen, copy);
    out[copy] = '\0';
    *blen = copy;
  }
  memmove(c->rbuf, c->rbuf + need, c->rlen - need);
  c->rlen -= need;
  return status;
}

static int send_req(struct conn *c, const char *head, const void *body,
                    size_t body_len) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (c->fd < 0) conn_open(c, c->port);
    if (c->fd < 0) return -1;
    if (send_all(c, head, strlen(head)) == 0 &&
        (body_len == 0 || send_all(c, body, body_len) == 0))
      return 0;
    conn_close(c);
  }
  return -1;
}

static int do_req(struct conn *c, const char *head, const void *body,
                  size_t body_len, int expect, char *resp, size_t respcap,
                  size_t *resp_len) {
  if (send_req(c, head, body, body_len) != 0) return -1;
  int status = parse_response(c, resp, respcap, resp_len);
  if (status < 0) {
    conn_close(c);
    return -1;
  }
  if (expect > 0) {
    if (status != expect) {
      fprintf(stderr, "train: %.*s -> HTTP %d (expected %d)\n",
              (int)(strlen(head) > 48 ? 48 : strlen(head)), head, status, expect);
      return -1;
    }
  } else if (status < 200 || status > 299) {
    fprintf(stderr, "train: %.*s -> HTTP %d (expected 2xx)\n",
            (int)(strlen(head) > 48 ? 48 : strlen(head)), head, status);
    return -1;
  }
  return status;
}

static int do_get_code(struct conn *c, const char *path, int expect, char *resp,
                       size_t cap, size_t *rlen) {
  char head[512];
  snprintf(head, sizeof(head),
           "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: application/json\r\n"
           "Connection: keep-alive\r\n\r\n",
           path);
  return do_req(c, head, NULL, 0, expect, resp, cap, rlen);
}

static int do_get(struct conn *c, const char *path, char *resp, size_t cap,
                  size_t *rlen) {
  return do_get_code(c, path, -1, resp, cap, rlen);
}

static int64_t json_last_id(const char *j) {
  int64_t id = -1;
  const char *p = j;
  while ((p = strstr(p, "\"id\":")) != NULL) {
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end != p) {
      id = (int64_t)v;
      p = end;
    } else {
      p++;
    }
  }
  return id;
}

static int run_one(uint16_t port, int run_index) {
  struct conn c;
  memset(&c, 0, sizeof(c));
  c.fd = -1;
  conn_open(&c, port);
  if (c.fd < 0) return -1;

  static const char *users[] = {"1", "2", "3", "7", "13"};
  static const char *unames[] = {"trainer", "alice", "bob", "carol", "dave"};
  char resp[RESP_BUF];
  char token[2048];
  char payload[256];
  char head[2048];
  char path[96];
  size_t rl = 0;
  int created = 0;
  int pl;

  if (do_get(&c, "/health", resp, sizeof(resp), &rl) < 0) goto fail;
  if (do_get(&c, "/feed", resp, sizeof(resp), &rl) < 0) goto fail;

  for (int i = 1; i <= 100; i++) {
    snprintf(path, sizeof(path), "/posts/%d", i);
    if (do_get(&c, path, resp, sizeof(resp), &rl) < 0) goto fail;
    if (((i + run_index) % 37) == 0) {

      if (do_get_code(&c, "/posts/0", 400, resp, sizeof(resp), &rl) < 0) goto fail;
      if (do_get_code(&c, "/nothing", 404, resp, sizeof(resp), &rl) < 0) goto fail;
    }
  }

  ht_clear();

  for (int k = 0; k < 6; k++) {
    int u = (run_index + k) % 5;
    if (make_jwt(users[u], unames[u], token, sizeof(token), NULL) != 0) goto fail;

    pl = snprintf(payload, sizeof(payload),
                  "{\"body\":\"pgo train run %d post %d hello world\"}",
                  run_index, k);
    if (pl <= 0) goto fail;

    snprintf(head, sizeof(head),
             "POST /posts HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Authorization: Bearer %s\r\n"
             "Content-Type: application/json\r\nContent-Length: %d\r\n"
             "Connection: keep-alive\r\n\r\n",
             token, pl);
    if (do_req(&c, head, payload, (size_t)pl, 201, resp, sizeof(resp), &rl) < 0) goto fail;

    int64_t nid = json_last_id(resp);
    if (nid > 0) {
      ht_add(nid);
      created++;
    }
  }

  for (int k = 0; k < 8; k++) {
    int u = (run_index + 2 + k) % 5;
    if (make_jwt(users[u], unames[u], token, sizeof(token), NULL) != 0) goto fail;

    int64_t target = -1;
    if (created > 0) {
      unsigned want = (((unsigned)run_index * 7u + (unsigned)k * 13u) %
                       (unsigned)created) + 1u;

      unsigned seen = 0;
      for (unsigned i = 0; i < HT_CAP; i++) {
        if (ht_key[i] != HT_TOMB) {
          seen++;
          if (seen == want) {
            target = ht_key[i];
            break;
          }
        }
      }
      if (target <= 0) target = 1 + (k % 50);
    } else {
      target = 1 + (k % 20);
    }
    snprintf(path, sizeof(path), "/posts/%lld/like", (long long)target);
    snprintf(head, sizeof(head),
             "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Authorization: Bearer %s\r\nContent-Length: 0\r\n"
             "Connection: keep-alive\r\n\r\n",
             path, token);

    if (do_req(&c, head, NULL, 0, 0, resp, sizeof(resp), &rl) < 0) goto fail;
  }

  {
    if (make_jwt("1", "trainer", token, sizeof(token), NULL) != 0) goto fail;
    snprintf(head, sizeof(head),
             "POST /posts/1/like HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Authorization: Bearer %s\r\nContent-Length: 0\r\n"
             "Connection: keep-alive\r\n\r\n",
             token);
    if (do_req(&c, head, NULL, 0, 0, resp, sizeof(resp), &rl) < 0) goto fail;
  }

  {
    snprintf(head, sizeof(head),
             "POST /posts HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Content-Type: application/json\r\nContent-Length: 2\r\n"
             "Connection: keep-alive\r\n\r\n{}");
    int st = do_req(&c, head, NULL, 0, 401, resp, sizeof(resp), &rl);
    if (st < 0) goto fail;
    if (st != 401) {
      fprintf(stderr, "train: unauthenticated POST /posts -> %d (expected 401)\n", st);
      goto fail;
    }
  }

  conn_close(&c);
  return 0;

fail:
  conn_close(&c);
  return -1;
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  uint16_t port = DEFAULT_PORT;
  if (argc >= 2) {
    long v = strtol(argv[1], NULL, 10);
    if (v <= 0 || v > 65535) {
      fprintf(stderr, "usage: %s [port]\n", argv[0]);
      return 2;
    }
    port = (uint16_t)v;
  }

  ht_clear();

  {
    char probe[2048];
    if (make_jwt("1", "trainer", probe, sizeof(probe), NULL) != 0) {
      fprintf(stderr, "train: JWT generation failed\n");
      return 1;
    }
  }

  for (int r = 0; r < RUNS; r++) {
    if (run_one(port, r) != 0) {
      fprintf(stderr, "train: run %d failed\n", r);
      return 1;
    }
  }
  fprintf(stderr, "train: %d keep-alive rounds against 127.0.0.1:%u complete\n",
          RUNS, (unsigned)port);
  return 0;
}
