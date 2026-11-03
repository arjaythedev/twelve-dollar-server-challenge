// nic.c: the board's network card and storage controller, a d_cosim peripheral (C, not Verilog).
//
// It does what wire/conn.sh and socat did, inside the ngspice process: it owns the listening socket,
// frames each HTTP request (header lines up to the blank line, then Content-Length body bytes), and
// clocks the bytes into the chip's pins, up to 8 a clock. It reads the chip's output pins back into
// bytes. If the chip's first byte is '!', the rest is the response. If it is '?', the chip has written
// SQL: the controller runs it on SQLite and returns the rows as the sqlite3 CLI's -ascii mode prints
// them, and the chip writes the response from those. Between jobs it pulses rst, which power-cycles
// the chip. It makes no decision about content apart from the '!'/'?' byte the chip emits.
//
// Pins (see ../chip/top.v): in  clk ot won3..won0 wo63..wo0 done   out wt wn3..wn0 wi63..wi0 ie rst
// Up to 8 bytes a clock each way: a word of wn (won) bytes, first byte in bits 63:56, each time wt (ot)
// toggles. It samples and drives on the clock's falling edge; its outputs reach the chip one clock
// later (d_cosim delay), 5 ns before the rising edge the chip samples on. While it has no request it blocks in epoll_wait, which stops
// the simulation (and its clock) until a request arrives.
//
// Linked with -z nodelete: the board reruns its transient analysis in a loop to keep ngspice's
// saved vectors small, and d_cosim reloads this library for every run, so sockets and queued
// requests live in static state that survives. A job cut off by the end of a run is sent again.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ngspice/cmtypes.h"
#include "ngspice/cosim.h"

enum { P_CLK, P_OT, P_N3, P_W63 = P_N3 + 4, P_DONE = P_W63 + 64, N_IN };    // inputs
enum { Q_WT, Q_N3, Q_W63 = Q_N3 + 4, Q_IE = Q_W63 + 64, Q_RST, N_OUT };     // outputs

typedef struct { char *p; size_t n, cap; } buf;
static void put(buf *b, const void *s, size_t n) {
  if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->p = realloc(b->p, b->cap); }
  memcpy(b->p + b->n, s, n); b->n += n;
}
static void puts_(buf *b, const char *s) { put(b, s, strlen(s)); }

#define MAXFD 8192
typedef struct { buf in, out; int open, queued; unsigned gen; } conn;
static conn cs[MAXFD];
static int qfd[MAXFD]; static unsigned qgen[MAXFD]; static int qh, qt, qn;   // ring of fds with a full request

static int ep = -1, lfd = -1, inited;
static sqlite3 *db;
static const char *secret;
static long boot;

// job state
enum { IDLE, RST1, RST2, SETTLE, SEND, WAIT };
static int st = IDLE, cur = -1, phase;     // phase 'A' or 'B'
static unsigned curgen;
static buf req, job, resp;                   // the current request, the bytes being clocked in, the chip's output
static size_t jp; static int settle, fresh;
static unsigned char pins[N_IN], drv[N_OUT], clk_prev = 1, ot_prev, wt;
static int fall;

static void die(const char *m) { perror(m); exit(1); }

static void conn_close(int fd) {
  epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL); close(fd);
  cs[fd].open = 0; cs[fd].queued = 0; cs[fd].gen++; cs[fd].in.n = cs[fd].out.n = 0;
}

static void flush_out(int fd) {
  conn *c = &cs[fd];
  while (c->out.n) {
    ssize_t w = write(fd, c->out.p, c->out.n);
    if (w < 0) {
      if (errno == EAGAIN) { struct epoll_event e = {EPOLLIN | EPOLLOUT, {.fd = fd}}; epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e); return; }
      conn_close(fd); return;
    }
    memmove(c->out.p, c->out.p + w, c->out.n - w); c->out.n -= w;
  }
  struct epoll_event e = {EPOLLIN, {.fd = fd}}; epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e);
}

// a full request at the front of c->in? returns its length (header lines + Content-Length body), or 0
static size_t framed(conn *c) {
  char *p = c->in.p, *e = p + c->in.n, *ls = p; long cl = 0;
  for (char *q = p; q < e; q++) {
    if (*q != '\n') continue;
    size_t ll = q - ls;                              // the line, without its \n
    if (ls != p && (ll == 0 || (ll == 1 && ls[0] == '\r'))) {
      size_t h = q + 1 - p;
      return c->in.n >= h + cl ? h + cl : 0;
    }
    if (ll > 15 && !strncasecmp(ls, "content-length:", 15)) {
      cl = 0; for (char *d = ls + 15; d < q; d++) if (*d >= '0' && *d <= '9') cl = cl * 10 + (*d - '0');
    }
    ls = q + 1;
  }
  return 0;
}

static void enqueue_if_ready(int fd) {
  conn *c = &cs[fd];
  if (c->queued || !c->in.n || !framed(c)) return;
  c->queued = 1; qfd[qt] = fd; qgen[qt] = c->gen; qt = (qt + 1) % MAXFD; qn++;
}

static void net(int timeout) {
  struct epoll_event ev[256];
  int n = epoll_wait(ep, ev, 256, timeout);
  for (int i = 0; i < n; i++) {
    int fd = ev[i].data.fd;
    if (fd == lfd) {
      for (;;) {
        int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) break;
        if (c >= MAXFD) { close(c); continue; }
        int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        cs[c].open = 1; cs[c].queued = 0; cs[c].in.n = cs[c].out.n = 0;
        struct epoll_event e = {EPOLLIN, {.fd = c}}; epoll_ctl(ep, EPOLL_CTL_ADD, c, &e);
      }
      continue;
    }
    if (ev[i].events & EPOLLOUT) flush_out(fd);
    if (!cs[fd].open) continue;
    if (ev[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
      char b[65536]; ssize_t r;
      while ((r = read(fd, b, sizeof b)) > 0) put(&cs[fd].in, b, r);
      if (r == 0 || (r < 0 && errno != EAGAIN)) { conn_close(fd); continue; }
      enqueue_if_ready(fd);
    }
  }
}

// run the chip's SQL the way `sqlite3 -batch -ascii -cmd '.timeout 10000'` does: rows as columns joined
// by 0x1F, each row ended by 0x1E, on "stdout"; errors on "stderr"; rc 1 after an error
static int run_sql(const char *sql, buf *out, buf *err) {
  int rc = 0, line = 1; const char *tail;
  while (*sql) {
    sqlite3_stmt *s = NULL;
    const char *start = sql;
    if (sqlite3_prepare_v2(db, sql, -1, &s, &tail) != SQLITE_OK) {
      char m[512]; snprintf(m, sizeof m, "Parse error near line %d: %s\n", line, sqlite3_errmsg(db)); puts_(err, m);
      return 1;
    }
    for (const char *q = start; q < tail; q++) line += *q == '\n';
    sql = tail;
    if (!s) continue;                               // whitespace or a comment
    int r;
    while ((r = sqlite3_step(s)) == SQLITE_ROW) {
      int nc = sqlite3_column_count(s);
      for (int i = 0; i < nc; i++) {
        if (i) put(out, "\x1f", 1);
        const unsigned char *t = sqlite3_column_text(s, i);
        if (t) put(out, t, sqlite3_column_bytes(s, i));
      }
      put(out, "\x1e", 1);
    }
    if (r != SQLITE_DONE) {
      char m[512]; snprintf(m, sizeof m, "Runtime error near line %d: %s (%d)\n", line, sqlite3_errmsg(db), sqlite3_extended_errcode(db));
      puts_(err, m); rc = 1;
    }
    sqlite3_finalize(s);
    if (rc) return rc;
  }
  return rc;
}

static void reply(const char *p, size_t n) {
  if (n < 5 || memcmp(p, "HTTP/", 5)) {   // the chip should always answer with a status line
    fprintf(stderr, "nic: odd answer (%zu B, phase %c) to %.*s:", n, phase, (int)strcspn(req.p, "\r\n"), req.p);
    for (size_t i = 0; i < n && i < 48; i++) fprintf(stderr, " %02x", (unsigned char)p[i]);
    fprintf(stderr, "\n");
  }
  if (!cs[cur].open || cs[cur].gen != curgen) return;
  put(&cs[cur].out, p, n); flush_out(cur);
}

static void next_request(void) {
  conn *c = &cs[cur];
  size_t n = framed(c);
  req.n = 0; put(&req, c->in.p, n);
  memmove(c->in.p, c->in.p + n, c->in.n - n); c->in.n -= n;
  char h[640]; int hn = snprintf(h, sizeof h, "A\n%ld\n%ld\n%s\n", (long)time(NULL), boot, secret);
  job.n = 0; put(&job, h, hn); put(&job, req.p, req.n);
  phase = 'A';
}

// the chip has finished a job: resp holds its output
static void finished(void) {
  if (phase == 'A' && resp.n && resp.p[0] == '?') {
    char *nl = memchr(resp.p, '\n', resp.n);
    size_t kl = nl ? (size_t)(nl - resp.p - 1) : resp.n - 1;
    buf sql = {0}, out = {0}, err = {0};
    if (nl) put(&sql, nl + 1, resp.p + resp.n - nl - 1);
    put(&sql, "", 1);
    int rc = run_sql(sql.p, &out, &err);
    job.n = 0; puts_(&job, "B\n"); put(&job, resp.p + 1, kl);
    char r[16]; int rn = snprintf(r, sizeof r, "\n%d\n", rc); put(&job, r, rn);
    put(&job, out.p, out.n); put(&job, "\x1d", 1); put(&job, err.p, err.n);
    free(sql.p); free(out.p); free(err.p);
    phase = 'B'; st = RST1; return;
  }
  if (phase == 'A') { if (resp.n > 1) reply(resp.p + 1, resp.n - 1); }
  else if (resp.n) reply(resp.p, resp.n);
  cs[cur].queued = 0;
  if (cs[cur].open && cs[cur].gen == curgen) enqueue_if_ready(cur);
  cur = -1; st = IDLE;
}

static void drive(struct co_info *p, int i, int v) {
  if (drv[i] == v) return;
  drv[i] = v; Digital_t d = {v ? ONE : ZERO, STRONG}; p->out_fn(p, i, &d);
}

static void in_fn(struct co_info *p, unsigned int i, Digital_t *d) {
  if (i >= N_IN || d->state == UNKNOWN) return;
  unsigned char v = d->state == ONE;
  if (i == P_CLK) { if (clk_prev && !v) fall = 1; clk_prev = v; }
  pins[i] = v;
}

static void step(struct co_info *p) {
  if (fresh) { fresh = 0; for (int i = 0; i < N_OUT; i++) drive(p, i, 0); }
  if (!fall) return;
  fall = 0;
  // the chip's output: won bytes on wo each time ot toggles
  if (st == SEND || st == WAIT) {
    if (pins[P_OT] != ot_prev) {
      int n = 0; for (int k = 0; k < 4; k++) n = n << 1 | pins[P_N3 + k];
      for (int i = 0; i < n && i < 8; i++) {
        unsigned char b = 0; for (int k = 0; k < 8; k++) b = b << 1 | pins[P_W63 + 8 * i + k];
        put(&resp, &b, 1);
      }
      ot_prev = pins[P_OT];
    }
  }
  switch (st) {
  case IDLE:
    while (cur < 0) {
      net(qn ? 0 : -1);
      while (qn && cur < 0) {
        int fd = qfd[qh]; unsigned g = qgen[qh]; qh = (qh + 1) % MAXFD; qn--;
        if (cs[fd].open && cs[fd].gen == g && cs[fd].queued) { cur = fd; curgen = g; }
      }
    }
    next_request(); st = RST1;
    /* fall through */
  case RST1: wt = 0; drive(p, Q_WT, 0); drive(p, Q_IE, 0); drive(p, Q_RST, 1); st = RST2; break;
  case RST2: drive(p, Q_RST, 0); st = SETTLE; settle = 2; break;
  // the fresh chip's pins reach us a clock after the reset; start from what they say then
  case SETTLE: if (--settle == 0) { st = SEND; jp = 0; resp.n = 0; ot_prev = pins[P_OT]; } break;
  case SEND:
    if (jp < job.n) {
      int n = job.n - jp < 8 ? (int)(job.n - jp) : 8;
      for (int i = 0; i < 8; i++) {
        unsigned char b = i < n ? job.p[jp + i] : 0;
        for (int k = 0; k < 8; k++) drive(p, Q_W63 + 8 * i + k, b >> (7 - k) & 1);
      }
      for (int k = 0; k < 4; k++) drive(p, Q_N3 + k, n >> (3 - k) & 1);
      jp += n; wt ^= 1; drive(p, Q_WT, wt);
    } else { drive(p, Q_IE, 1); st = WAIT; }
    break;
  case WAIT:
    drive(p, Q_IE, 0);
    if (pins[P_DONE]) finished();
    break;
  }
}

static void setup_once(void) {
  const char *host = getenv("HOST") ? getenv("HOST") : "127.0.0.1", *port = getenv("PORT") ? getenv("PORT") : "3000";
  const char *path = getenv("SQLITE_PATH");
  secret = getenv("JWT_SECRET");
  if (!path || !secret) { fprintf(stderr, "nic: SQLITE_PATH and JWT_SECRET must be set\n"); exit(1); }
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { fprintf(stderr, "nic: %s\n", sqlite3_errmsg(db)); exit(1); }
  sqlite3_busy_timeout(db, 10000);
  boot = time(NULL);
  lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(atoi(port))};
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "nic: bad HOST %s\n", host); exit(1); }
  if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) die("nic: bind");
  if (listen(lfd, 1024) < 0) die("nic: listen");
  if ((ep = epoll_create1(EPOLL_CLOEXEC)) < 0) die("nic: epoll");
  struct epoll_event e = {EPOLLIN, {.fd = lfd}}; epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &e);
  fprintf(stderr, "nic: listening on %s:%s\n", host, port);
}

void Cosim_setup(struct co_info *p) {
  if (!inited) { setup_once(); inited = 1; }
  // a new transient run: the pins start low and the chip is fresh, so a job in flight starts over
  // after ngspice's reset, d_cosim can still hold an output's last value from the previous run while the
  // node starts at 0; claiming every output is 1 makes the first drive() of each send an explicit 0
  memset(drv, 1, sizeof drv); memset(pins, 0, sizeof pins); fresh = 1; clk_prev = 1; fall = 0;
  if (st != IDLE) st = RST1;   // a new slice cut a job short: the chip is fresh, so start the job over
  p->in_count = N_IN; p->out_count = N_OUT; p->inout_count = 0;
  p->step = step; p->in_fn = in_fn; p->cleanup = NULL; p->method = After_input;
}
