/* Closed-loop HTTP/1.1 throughput probe: T threads x C keep-alive connections, each sending
 * the next request as soon as the previous response is complete.
 *   blast <ip> <port> <threads> <conns-per-thread> <seconds> <mix> [token]
 * mix: feed | post | like | create | mixed (load.js proportions: 46% feed, 46% post,
 * 7% like, 1% create). Prints requests/s and status counts. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char *ip, *mix, *token;
static int port, nconn, seconds;
static volatile int stop;

typedef struct { int fd; char in[65536]; int in_len; unsigned rng; } conn_t;
typedef struct { long done, s2xx, s4xx, s5xx, other; } stats_t;

static unsigned rnd(unsigned *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

static int make_req(conn_t *c, char *out) {
  unsigned r = rnd(&c->rng) % 100;
  const char *kind = mix;
  if (!strcmp(mix, "mixed")) kind = r < 46 ? "feed" : r < 92 ? "post" : r < 99 ? "like" : "create";
  int id = 499000 + (int)(rnd(&c->rng) % 1000);
  if (!strcmp(kind, "feed")) return sprintf(out, "GET /feed HTTP/1.1\r\nHost: x\r\nUser-Agent: blast\r\n\r\n");
  if (!strcmp(kind, "post")) return sprintf(out, "GET /posts/%d HTTP/1.1\r\nHost: x\r\nUser-Agent: blast\r\n\r\n", id);
  if (!strcmp(kind, "like")) return sprintf(out, "POST /posts/%d/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n", id, token);
  const char *body = "{\"body\":\"blast says hi at 2026-10-24T00:00:00.000Z (VU 1, iter 1)\"}";
  return sprintf(out, "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", token, strlen(body), body);
}
static int send_all(int fd, const char *p, int n) {
  while (n > 0) { int k = (int)write(fd, p, (size_t)n); if (k <= 0) return -1; p += k; n -= k; }
  return 0;
}
static void *worker(void *arg) {
  stats_t *st = arg;
  int ep = epoll_create1(0);
  conn_t *cs = calloc((size_t)nconn, sizeof *cs);
  char req[4096];
  for (int i = 0; i < nconn; i++) {
    conn_t *c = &cs[i];
    c->rng = (unsigned)(i * 2654435761u + (unsigned)(uintptr_t)st);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); exit(1); }
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
    epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
    send_all(c->fd, req, make_req(c, req));
  }
  struct epoll_event evs[256];
  while (!stop) {
    int n = epoll_wait(ep, evs, 256, 100);
    for (int i = 0; i < n; i++) {
      conn_t *c = evs[i].data.ptr;
      int k = (int)read(c->fd, c->in + c->in_len, sizeof c->in - (size_t)c->in_len);
      if (k <= 0) { fprintf(stderr, "connection closed\n"); exit(1); }
      c->in_len += k;
      for (;;) {
        char *he = memmem(c->in, (size_t)c->in_len, "\r\n\r\n", 4);
        if (!he) break;
        char *cl = memmem(c->in, (size_t)(he - c->in), "Content-Length: ", 16);
        int body = cl ? atoi(cl + 16) : 0;
        int total = (int)(he - c->in) + 4 + body;
        if (c->in_len < total) break;
        int status = atoi(c->in + 9);
        st->done++;
        if (status >= 200 && status < 300) st->s2xx++; else if (status >= 400 && status < 500) st->s4xx++; else if (status >= 500) st->s5xx++; else st->other++;
        memmove(c->in, c->in + total, (size_t)(c->in_len - total));
        c->in_len -= total;
        if (!stop) send_all(c->fd, req, make_req(c, req));
      }
    }
  }
  return 0;
}
int main(int argc, char **argv) {
  if (argc < 7) { fprintf(stderr, "usage: blast ip port threads conns secs mix [token]\n"); return 1; }
  ip = argv[1]; port = atoi(argv[2]); int nt = atoi(argv[3]); nconn = atoi(argv[4]); seconds = atoi(argv[5]); mix = argv[6]; token = argc > 7 ? argv[7] : "";
  pthread_t th[64];
  stats_t st[64];
  memset(st, 0, sizeof st);
  for (int i = 0; i < nt; i++) pthread_create(&th[i], 0, worker, &st[i]);
  sleep(1);   /* warm-up */
  long base = 0;
  for (int i = 0; i < nt; i++) base += st[i].done;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  sleep((unsigned)seconds);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long done = 0, a = 0, b = 0, c = 0, d = 0;
  for (int i = 0; i < nt; i++) { done += st[i].done; a += st[i].s2xx; b += st[i].s4xx; c += st[i].s5xx; d += st[i].other; }
  stop = 1;
  for (int i = 0; i < nt; i++) pthread_join(th[i], 0);
  double dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
  printf("%s: %.0f req/s  (2xx %ld, 4xx %ld, 5xx %ld, other %ld)\n", mix, (double)(done - base) / dt, a, b, c, d);
  return 0;
}
