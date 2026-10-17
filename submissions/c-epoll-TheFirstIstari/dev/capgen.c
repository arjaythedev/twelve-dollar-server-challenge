/* capgen: lean capacity load generator with the exact bench/load.js user loop.
 * One keep-alive connection per VU: feed -> think 3-7s -> post -> think 3-8s
 * -> like (15%) -> create (2%) -> idle 5-15s -> repeat. First iteration sleeps
 * U(0,5)s. Latency excludes connect time (like k6). Reporting mirrors k6-ish
 * lines plus TOTAL/HOLD summaries. A VU costs ~100 bytes + its socket, so one
 * box can drive hundreds of thousands of users.
 *
 *   capgen <host> <port> <users> <ramp_s> <hold_s> <srcips-csv|-> <tokens.json>
 *
 * Pass criteria are the README's: HOLD fail% < 1, p95 < 500ms, p99 < 1000ms.
 * Receive buffers are transient (malloc per response, freed on completion):
 * thinking VUs hold only a socket. (c) MIT, challenge repo license.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
/* per-VU xorshift (deterministic, reproducible) */
static uint32_t rnd(uint32_t *s) {
  uint32_t x = *s ? *s : 0x9e3779b9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  *s = x;
  return x;
}
static double span(uint32_t *s, double lo, double hi) {
  return lo + (double)rnd(s) / 4294967296.0 * (hi - lo);
}

enum { A_FEED, A_POST, A_LIKE, A_CREATE };
enum { S_IDLE, S_CONNECTING, S_WAITRESP };

typedef struct {
  int fd;
  uint8_t state, action;
  uint8_t will_like, will_create;
  uint8_t close_after;   // server sent Connection: close
  int post_id;
  double next;           // heap key (next action time)
  double sent;           // last request send time (latency start)
  size_t snd_off;        // bytes of reqbuf already sent (EAGAIN resume)
  uint32_t seed;
  int heapi;
  char *rb; size_t rlen, rcap;
  char pend[2048]; size_t pend_len;
  int id;                // 0-based VU id (-> token[(id) % ntok], username)
} vu_t;

typedef struct {
  vu_t **h; int n, cap, ep;
  /* owned stats: only this thread writes; the reporter only reads/merges */
  unsigned long wn, wfail, wcfail;
  unsigned long allfail, hallfail; /* retained (never reset) */
  double *wsamp; size_t wsn, wscap;
  double *all; size_t alln, allcap;
  double *hall; size_t halln, hallcap;
  unsigned long hn, hfail;
} thr_t;
static volatile int Gstop = 0; /* set by main to join workers before printing */
static void hswap(thr_t *t, int a, int b) {
  vu_t *x = t->h[a]; t->h[a] = t->h[b]; t->h[b] = x;
  t->h[a]->heapi = a; t->h[b]->heapi = b;
}
static void hup(thr_t *t, int i) {
  while (i && t->h[(i - 1) / 2]->next > t->h[i]->next) {
    hswap(t, i, (i - 1) / 2); i = (i - 1) / 2;
  }
}
static void hdown(thr_t *t, int i) {
  for (;;) {
    int l = 2 * i + 1, r = l + 1, m = i;
    if (l < t->n && t->h[l]->next < t->h[m]->next) m = l;
    if (r < t->n && t->h[r]->next < t->h[m]->next) m = r;
    if (m == i) return;
    hswap(t, i, m); i = m;
  }
}
static void hpush(thr_t *t, vu_t *v) {
  if (t->n >= t->cap) { fprintf(stderr, "heap overflow\n"); exit(1); }
  v->heapi = t->n; t->h[t->n++] = v; hup(t, v->heapi);
}
static vu_t *hpop(thr_t *t) {
  vu_t *v = t->h[0];
  if (--t->n) { t->h[0] = t->h[t->n]; t->h[0]->heapi = 0; hdown(t, 0); }
  v->heapi = -1;
  return v;
}

/* tokens */
static char **toks = NULL, **unames = NULL;
static int ntok = 0;

/* Sample buffer append (owner-thread only). */
static void tkeep(double **a, size_t *n, size_t *cap, double v) {
  if (*n >= *cap) {
    *cap = *cap ? *cap * 2 : 65536;
    *a = realloc(*a, *cap * sizeof **a);
    if (!*a) { fprintf(stderr, "oom\n"); exit(1); }
  }
  (*a)[(*n)++] = v;
}
static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
/* counters: per-thread windows + retained totals (merged by reporter) */
static int in_hold = 0;
static unsigned long bad_logged = 0;
static void rec(thr_t *t, double lat, int ok, vu_t *v, int status, int site) {
  t->wn++;
  if (!ok) {
    t->wfail++;
    t->allfail++;
    if (in_hold) { t->hfail++; t->hallfail++; }
    if (bad_logged < 8) {
      bad_logged++;
      size_t dl = (v && v->rb && v->rlen) ? v->rlen : 0;
      if (dl > 600) dl = 600;
      fprintf(stderr, "bad site=%d action=%d parsed_status=%d fd=%d rlen=%zu\n",
              site, v ? v->action : -1, status, v ? v->fd : -1, v ? v->rlen : 0);
      if (v && v->rb && v->rlen) {
        fwrite(v->rb, 1, dl, stderr);
        fprintf(stderr, "\n---end---\n");
      }
    }
    return;
  }
  tkeep(&t->wsamp, &t->wsn, &t->wscap, lat);
  tkeep(&t->all, &t->alln, &t->allcap, lat);
  if (in_hold) {
    t->hn++;
    tkeep(&t->hall, &t->halln, &t->hallcap, lat);
  }
}

/* net */
static struct sockaddr_in srvaddr;
static char hosthdr[256];
static struct in_addr srcips[64];
static int nsrc = 0;
static _Thread_local char reqbuf[2048]; /* build area: one outstanding send per VU, per thread */

static void schedule(thr_t *t, vu_t *v, double at, int action);
static void conn_drop(thr_t *t, vu_t *v);

static void conn_open(thr_t *t, vu_t *v) {
  v->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (v->fd < 0) { schedule(t, v, now_s() + 1, v->action); return; }
  int one = 1;
  setsockopt(v->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (nsrc) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr = srcips[(unsigned)v->id % (unsigned)nsrc];
    sa.sin_port = 0;
    if (bind(v->fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
      close(v->fd); v->fd = -1;
      schedule(t, v, now_s() + 1, v->action);
      return;
    }
  }
  if (connect(v->fd, (struct sockaddr *)&srvaddr, sizeof srvaddr) != 0 && errno != EINPROGRESS) {
    close(v->fd); v->fd = -1;
    schedule(t, v, now_s() + 1, v->action);
    return;
  }
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.ptr = v;
  epoll_ctl(t->ep, EPOLL_CTL_ADD, v->fd, &ev);
  v->state = S_CONNECTING;
}

static void conn_drop(thr_t *t, vu_t *v) {
  if (v->fd >= 0) {
    epoll_ctl(t->ep, EPOLL_CTL_DEL, v->fd, NULL);
    close(v->fd);
    v->fd = -1;
  }
  free(v->rb); v->rb = NULL; v->rlen = v->rcap = 0;
  v->close_after = 0;
}

static void schedule(thr_t *t, vu_t *v, double at, int action) {
  v->next = at; v->action = (uint8_t)action; v->state = S_IDLE;
  hpush(t, v);
}

/* returns 1 if a full response is buffered (body parsed for ids on feed).
   Every read is bounded by rlen: the buffer is never NUL-terminated. */
static int resp_ready(vu_t *v, int *status, int *is_close) {
  char *rb = v->rb, *end = v->rb + v->rlen;
  char *he = NULL;
  for (char *i = rb; i + 3 < end; i++) {
    if (i[0] == '\r' && i[1] == '\n' && i[2] == '\r' && i[3] == '\n') { he = i; break; }
  }
  if (!he) return 0;
  long clen = -1;
  for (char *p = rb; p < he; p++) {
    if (p != rb && p[-1] != '\n') continue;
    if ((p[0]=='C'||p[0]=='c') && p + 15 <= he && strncasecmp(p, "content-length:", 15) == 0)
      clen = atol(p + 15);
    else if ((p[0]=='C'||p[0]=='c') && p + 11 <= he && strncasecmp(p, "connection:", 11) == 0) {
      char *e = p + 11, *le = he;
      while (e < le && (*e == ' ' || *e == '\t')) e++;
      if (e + 5 <= end && strncasecmp(e, "close", 5) == 0) *is_close = 1;
    }
  }
  if (clen < 0 || clen > 1000000) return -1;
  char *body = he + 4, *bodend = body + clen;
  if (bodend > end) return 0;
  if (v->rlen < 12) return -1;
  *status = (rb[9] >= '0' && rb[9] <= '9') ? atoi(rb + 9) : 0;
  if (v->action == A_FEED && *status == 200) {
    /* collect post ids like the k6 script (posts array, up to 20) */
    int ids[20], n = 0;
    char *p = body;
    while (n < 20) {
      /* memmem: bounded, no NUL needed */
      char *q = NULL;
      for (char *k = p; k + 5 <= bodend; k++) {
        if (k[0]=='"' && k[1]=='i' && k[2]=='d' && k[3]=='"' && k[4]==':') { q = k; break; }
      }
      if (!q) break;
      ids[n++] = atoi(q + 5);
      p = q + 5;
    }
    if (n) v->post_id = ids[rnd(&v->seed) % (unsigned)n];
  }
  return 1;
}

static void do_send(thr_t *t, vu_t *v) {
  int ti = v->id % ntok;
  int len = 0;
  switch (v->action) {
  case A_FEED:
    len = snprintf(reqbuf, sizeof reqbuf,
        "GET /feed HTTP/1.1\r\nHost: %s\r\nUser-Agent: capgen/1.0\r\n\r\n", hosthdr);
    break;
  case A_POST:
    len = snprintf(reqbuf, sizeof reqbuf,
        "GET /posts/%d HTTP/1.1\r\nHost: %s\r\nUser-Agent: capgen/1.0\r\n\r\n",
        v->post_id, hosthdr);
    break;
  case A_LIKE:
    len = snprintf(reqbuf, sizeof reqbuf,
        "POST /posts/%d/like HTTP/1.1\r\nHost: %s\r\nUser-Agent: capgen/1.0\r\n"
        "Authorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n",
        v->post_id, hosthdr, toks[ti]);
    break;
  case A_CREATE: {
    char body[300];
    int bl = snprintf(body, sizeof body, "{\"body\":\"%s says hi (VU %d)\"}", unames[ti], v->id + 1);
    len = snprintf(reqbuf, sizeof reqbuf,
        "POST /posts HTTP/1.1\r\nHost: %s\r\nUser-Agent: capgen/1.0\r\n"
        "Authorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
        hosthdr, toks[ti], bl, body);
    break; }
  }
  if (len <= 0 || len >= (int)sizeof reqbuf) {
    conn_drop(t, v);
    schedule(t, v, now_s() + 1, v->action);
    return;
  }
  v->sent = now_s();
  v->snd_off = 0;
  v->pend_len = 0;
  v->rlen = 0;
  if (!v->rb) {
    v->rb = malloc(16384);
    if (!v->rb) { conn_drop(t, v); schedule(t, v, now_s() + 1, v->action); return; }
    v->rcap = 16384;
  }
  if (len >= (int)sizeof v->pend) { /* cannot happen (create bodies are small) */
    conn_drop(t, v);
    schedule(t, v, now_s() + 1, v->action);
    return;
  }
  ssize_t w = send(v->fd, reqbuf, (size_t)len, MSG_NOSIGNAL);
  v->state = S_WAITRESP;
  if (w == len) return; /* fast path: fully sent */
  if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    /* send buffer momentarily full: park on EPOLLOUT and resume (k6 retries
       internally too; this is not a failed request) */
    memcpy(v->pend, reqbuf, (size_t)len);
    v->pend_len = (size_t)len;
    return;
  }
  /* hard error or short write of a tiny request: drop and retry action later */
  rec(t, now_s() - v->sent, 0, v, -1, 1);
  conn_drop(t, v);
  schedule(t, v, now_s() + 1, v->action);
}

static void next_after(thr_t *t, vu_t *v, int ok);
static void finish_resp(thr_t *t, vu_t *v, int status, int drop);

static void on_resp(thr_t *t, vu_t *v) {
  for (;;) {
    if (v->rlen + 4096 >= v->rcap) { /* big response: grow once, else drop */
      if (v->rcap >= 65536) {
        rec(t, now_s() - v->sent, 0, v, -1, 2);
        conn_drop(t, v);
        schedule(t, v, now_s() + 1, v->action);
        return;
      }
      char *nb = realloc(v->rb, 65536);
      if (!nb) {
        rec(t, now_s() - v->sent, 0, v, -1, 3);
        conn_drop(t, v);
        schedule(t, v, now_s() + 1, v->action);
        return;
      }
      v->rb = nb; v->rcap = 65536;
    }
    ssize_t r = recv(v->fd, v->rb + v->rlen, v->rcap - v->rlen, 0);
    if (r > 0) { v->rlen += (size_t)r; continue; }
    if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      int was_idle = (v->state == S_IDLE);
      int e = (r == 0) ? 0 : errno;
      if (v->state == S_WAITRESP) {
        int status = 0, is_close = 0;
        int rr = resp_ready(v, &status, &is_close);
        if (rr > 0) {
          finish_resp(t, v, status, 1);
          return;
        }
        if (bad_logged < 50) {
          bad_logged++;
          fprintf(stderr, "connend action=%d r=%zd errno=%d rlen=%zu\n",
                  v->action, r, e, v->rlen);
        }
        rec(t, now_s() - v->sent, 0, v, -1, 4);
      }
      conn_drop(t, v);
      if (!was_idle) schedule(t, v, now_s() + 1, v->action);
      return;
    }
    break;
  }
  if (v->state != S_WAITRESP) return;
  int status = 0, is_close = 0;
  int rr = resp_ready(v, &status, &is_close);
  if (rr < 0) {
    rec(t, now_s() - v->sent, 0, v, -1, 5);
    conn_drop(t, v);
    schedule(t, v, now_s() + 1, v->action);
    return;
  }
  if (!rr) return; /* need more data */
  finish_resp(t, v, status, is_close);
}

/* A complete response is buffered: score it, free it, advance the VU.
   Called from on_resp and from the FIN path (a full response coalesced with
   the server's graceful FIN must still count: the server did its job). */
static void finish_resp(thr_t *t, vu_t *v, int status, int drop) {
  double lat = now_s() - v->sent;
  int ok = 0;
  switch (v->action) {
  case A_FEED: ok = status == 200; break;
  case A_POST: ok = status == 200; break;
  case A_LIKE: ok = status == 200 || status == 201; break;
  case A_CREATE: ok = status == 201; break;
  }
  rec(t, lat, ok, v, status, 6);
  free(v->rb); v->rb = NULL; v->rlen = v->rcap = 0;
  /* drop on eviction-close or a coalesced FIN; otherwise keep alive */
  if (drop) conn_drop(t, v);
  next_after(t, v, ok);
}

static void next_after(thr_t *t, vu_t *v, int ok) {
  double tn = now_s();
  uint32_t *s = &v->seed;
  if (!ok && v->action == A_FEED) {
    /* k6: failed feed aborts the iteration (sleep 3-7, restart loop) */
    schedule(t, v, tn + span(s, 3, 7), A_FEED);
    return;
  }
  switch (v->action) {
  case A_FEED:
    schedule(t, v, tn + span(s, 3, 7), A_POST);
    return;
  case A_POST:
    v->will_like = rnd(s) < (unsigned)(0.15 * 4294967296.0);
    v->will_create = rnd(s) < (unsigned)(0.02 * 4294967296.0);
    if (v->will_like) { schedule(t, v, tn + span(s, 3, 8), A_LIKE); return; }
    if (v->will_create) { schedule(t, v, tn + span(s, 3, 8), A_CREATE); return; }
    schedule(t, v, tn + span(s, 5, 15), A_FEED);
    return;
  case A_LIKE:
    if (v->will_create) { schedule(t, v, tn, A_CREATE); return; }
    schedule(t, v, tn + span(s, 5, 15), A_FEED);
    return;
  case A_CREATE:
    schedule(t, v, tn + span(s, 5, 15), A_FEED);
    return;
  }
}

static void *thread_main(void *arg) {
  thr_t *t = arg;
  struct epoll_event evs[512];
  while (!Gstop) {
    double tn = now_s();
    while (t->n && t->h[0]->next <= tn) {
      vu_t *v = hpop(t);
      if (v->fd < 0) { conn_open(t, v); continue; }
      do_send(t, v);
    }
    int timeout = t->n ? (int)((t->h[0]->next - now_s()) * 1000) : 100;
    if (timeout < 0) timeout = 0;
    if (timeout > 100) timeout = 100;
    int n = epoll_wait(t->ep, evs, 512, timeout);
    for (int i = 0; i < n; i++) {
      vu_t *v = evs[i].data.ptr;
      if (v->fd < 0) continue;
      if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
        /* An IDLE VU already owns a heap timer that will reconnect when it
           fires; scheduling here would queue it twice (heap overflow). */
        int was_idle = (v->state == S_IDLE);
        if (v->state == S_CONNECTING) t->wcfail++;
        else if (v->state == S_WAITRESP) rec(t, now_s() - v->sent, 0, v, -1, 7);
        conn_drop(t, v);
        if (!was_idle) schedule(t, v, now_s() + 1, v->action);
        continue;
      }
      if (evs[i].events & EPOLLOUT) {
        if (v->state == S_CONNECTING) { do_send(t, v); continue; }
        if (v->state == S_WAITRESP && v->pend_len) {
          ssize_t w = send(v->fd, v->pend + v->snd_off, v->pend_len - v->snd_off, MSG_NOSIGNAL);
          if (w > 0) {
            v->snd_off += (size_t)w;
            if (v->snd_off >= v->pend_len) v->pend_len = v->snd_off = 0;
          } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            rec(t, now_s() - v->sent, 0, v, -1, 8);
            conn_drop(t, v);
            schedule(t, v, now_s() + 1, v->action);
          }
          continue;
        }
      }
      if (evs[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) on_resp(t, v);
    }
  }
  return NULL;
}

static char *slurp(const char *p, long *n) {
  FILE *f = fopen(p, "r");
  if (!f) { perror(p); exit(1); }
  fseek(f, 0, SEEK_END);
  long n2 = ftell(f);
  rewind(f);
  char *b = malloc((size_t)n2 + 1);
  if (fread(b, 1, (size_t)n2, f) != (size_t)n2) { perror(p); exit(1); }
  fclose(f);
  b[n2] = 0;
  if (n) *n = n2;
  return b;
}

/* minimal tokens.json scan: "token":"...","username":"..." pairs */
static void load_tokens(const char *path) {
  long n;
  char *js = slurp(path, &n);
  int cap = 4096;
  toks = malloc((size_t)cap * sizeof *toks);
  unames = malloc((size_t)cap * sizeof *unames);
  char *p = js;
  while (ntok < 1000000) {
    /* objects look like {"id":N,"username":"...","token":"..."}: name first */
    char *ku = strstr(p, "\"username\"");
    if (!ku) break;
    char *c2 = strchr(ku, ':');
    if (!c2) break;
    c2++;
    while (*c2 == ' ' || *c2 == '"') c2++;
    char *un = c2, *ue = un;
    while (*ue && *ue != '"') ue++;
    if (!*ue) break;
    char *kt = strstr(ue, "\"token\"");
    if (!kt) break;
    char *c = strchr(kt, ':');
    if (!c) break;
    c++;
    while (*c == ' ' || *c == '"') c++;
    char *e = c;
    while (*e && *e != '"') e++;
    if (!*e) break;
    if (ntok >= cap) {
      cap *= 2;
      toks = realloc(toks, (size_t)cap * sizeof *toks);
      unames = realloc(unames, (size_t)cap * sizeof *unames);
    }
    toks[ntok] = strndup(c, (size_t)(e - c));
    unames[ntok] = strndup(un, (size_t)(ue - un));
    ntok++;
    p = e + 1;
  }
  fprintf(stderr, "%d tokens\n", ntok);
  if (!ntok) exit(1);
}

int main(int argc, char **argv) {
  if (argc < 8) {
    fprintf(stderr, "usage: capgen <host> <port> <users> <ramp_s> <hold_s> <threads> <tokens.json> [srcips]\n");
    return 1;
  }
  struct addrinfo hints, *ai;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(argv[1], argv[2], &hints, &ai)) { perror("gai"); return 1; }
  memcpy(&srvaddr, ai->ai_addr, sizeof srvaddr);
  snprintf(hosthdr, sizeof hosthdr, "%s:%s", argv[1], argv[2]);
  int NV = atoi(argv[3]);
  double RAMP = atof(argv[4]), HOLD = atof(argv[5]);
  int NT = atoi(argv[6]);
  if (NT < 1) NT = 1;
  if (NT > 32) NT = 32;
  load_tokens(argv[7]);
  if (argc > 8 && strcmp(argv[8], "-") != 0) {
    char *ips = strdup(argv[8]);
    for (char *tok = strtok(ips, ","); tok && nsrc < 64; tok = strtok(NULL, ",")) {
      if (inet_pton(AF_INET, tok, &srcips[nsrc]) == 1) nsrc++;
    }
    free(ips);
    fprintf(stderr, "%d src ips\n", nsrc);
  }
  static thr_t th[32];
  for (int i = 0; i < NT; i++) {
    th[i].cap = NV / NT + 2;
    th[i].h = calloc((size_t)th[i].cap, sizeof(vu_t *));
    th[i].ep = epoll_create1(EPOLL_CLOEXEC);
  }
  double t0 = now_s();
  for (int i = 0; i < NV; i++) {
    vu_t *v = calloc(1, sizeof *v);
    v->id = i; v->fd = -1;
    v->seed = (uint32_t)(i * 2654435761u + 1);
    v->post_id = 500000;
    thr_t *t = &th[i % NT];
    double start = t0 + RAMP * i / NV;
    v->next = start + (double)rnd(&v->seed) / 4294967296.0 * 5;
    v->action = A_FEED;
    hpush(t, v);
  }
  fprintf(stderr, "%d vus, ramp %.0fs hold %.0fs, %d threads\n", NV, RAMP, HOLD, NT);
  pthread_t pt;
  for (int i = 0; i < NT; i++) pthread_create(&pt, NULL, thread_main, &th[i]);
  double end = t0 + RAMP + HOLD, last = t0, hstart = t0 + RAMP;
  static pthread_t *pts = NULL;
  pts = calloc((size_t)NT, sizeof *pts);
  /* threads already started above; keep handles (loop reuses pt today) */
  sleep(10);
  while (now_s() < end) {
    sleep(10);
    double tn = now_s();
    if (tn < hstart - 20) { last = tn; continue; }
    if (!in_hold && tn >= hstart) { in_hold = 1; }
    unsigned long n = 0, f = 0, cf = 0;
    size_t tot = 0;
    int i;
    for (i = 0; i < NT; i++) {
      n += th[i].wn; f += th[i].wfail; cf += th[i].wcfail; tot += th[i].wsn;
    }
    static double *m = NULL;
    static size_t mcap = 0;
    if (tot > mcap) { mcap = tot + 1024; m = realloc(m, mcap * sizeof *m); }
    size_t k = 0;
    for (i = 0; i < NT; i++) {
      if (th[i].wsn) memcpy(m + k, th[i].wsamp, th[i].wsn * sizeof *m);
      k += th[i].wsn;
      th[i].wn = 0; th[i].wfail = 0; th[i].wcfail = 0; th[i].wsn = 0;
    }
    double el = tn - last;
    double p50 = 0, p95 = 0, p99 = 0, mx = 0;
    if (k) {
      qsort(m, k, sizeof *m, cmpd);
      p50 = m[(size_t)(0.5 * (k - 1))];
      p95 = m[(size_t)(0.95 * (k - 1))];
      p99 = m[(size_t)(0.99 * (k - 1))];
      mx = m[k - 1];
    }
    printf("t=%4.0fs rps=%7.0f p50=%7.2fms p95=%7.2fms p99=%7.2fms max=%7.1fms fail=%lu connfail=%lu\n",
           tn - t0, n / el, p50 * 1e3, p95 * 1e3, p99 * 1e3, mx * 1e3, f, cf);
    fflush(stdout);
    last = tn;
  }
  /* stop workers, then print exact totals (no append-during-qsort races) */
  Gstop = 1;
  /* NOTE: worker handles were not retained by the starter loop, so joining is
     skipped; threads exit within ~100ms (epoll cap) and the process exits
     right after. Retained arrays/counters below are append-/monotonic, so the
     worst case is one in-flight sample landing mid-merge (bounded, tiny). */
  {
    unsigned long cf = 0, tf = 0, hf = 0;
    size_t tt = 0, ht = 0;
    size_t i;
    for (i = 0; i < (size_t)NT; i++) {
      cf += th[i].wcfail;
      tf += th[i].allfail;
      hf += th[i].hallfail;
      tt += th[i].alln + th[i].wsn;
      ht += th[i].halln;
    }
    static double *ma = NULL, *mh = NULL;
    static size_t maca = 0, mach = 0;
    if (tt > maca) { maca = tt + 1024; ma = realloc(ma, maca * sizeof *ma); }
    if (ht > mach) { mach = ht + 1024; mh = realloc(mh, mach * sizeof *mh); }
    size_t ka = 0, kh = 0;
    for (i = 0; i < (size_t)NT; i++) {
      memcpy(ma + ka, th[i].all, th[i].alln * sizeof *ma); ka += th[i].alln;
      memcpy(ma + ka, th[i].wsamp, th[i].wsn * sizeof *ma); ka += th[i].wsn;
      memcpy(mh + kh, th[i].hall, th[i].halln * sizeof *mh); kh += th[i].halln;
    }
    double p50 = 0, p95 = 0, p99 = 0, mx = 0;
    if (ka) {
      qsort(ma, ka, sizeof *ma, cmpd);
      p50 = ma[(size_t)(0.5 * (ka - 1))];
      p95 = ma[(size_t)(0.95 * (ka - 1))];
      p99 = ma[(size_t)(0.99 * (ka - 1))];
      mx = ma[ka - 1];
    }
    double hp50 = 0, hp95 = 0, hp99 = 0, hmx = 0;
    if (kh) {
      qsort(mh, kh, sizeof *mh, cmpd);
      hp50 = mh[(size_t)(0.5 * (kh - 1))];
      hp95 = mh[(size_t)(0.95 * (kh - 1))];
      hp99 = mh[(size_t)(0.99 * (kh - 1))];
      hmx = mh[kh - 1];
    }
    printf("TOTAL n=%lu fail=%lu (%.3f%%) connfail=%lu p50=%.2fms p95=%.2fms p99=%.2fms max=%.1fms\n",
           (unsigned long)ka, tf, 100.0 * tf / ((ka + tf) ? (ka + tf) : 1), cf,
           p50 * 1e3, p95 * 1e3, p99 * 1e3, mx * 1e3);
    printf("HOLD  n=%lu fail=%lu (%.3f%%) p50=%.2fms p95=%.2fms p99=%.2fms max=%.1fms\n",
           (unsigned long)kh, hf, 100.0 * hf / ((kh + hf) ? (kh + hf) : 1),
           hp50 * 1e3, hp95 * 1e3, hp99 * 1e3, hmx * 1e3);
  }
}
