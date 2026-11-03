// server.c: single-threaded epoll HTTP/1.1 + SQLite feed API.
// One loop iteration: epoll_wait -> accept/recv/parse/handle (build replies)
// -> group-commit -> send. No response is sent before its commit.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include "conn.h"
#include "http.h"
#include "api.h"
#include "jwt.h"
#include "json.h"
#include "db.h"

// ---------- globals ----------
static db_t Gdb;
static api_t Gapi;
static char Gscratch[API_SCRATCH];
static uint64_t Gnow_s, Gmono_s;
static uint64_t Gnow_ms; // millisecond clock: sub-second pacing without extra syscalls
static char Gdate[40];
static size_t Gdate_len;
static int Glisten_fd = -1, Gep_fd = -1;
static volatile int Gstop = 0;
static wheel_t Gwheel;
static evict_t Gev = { 0, 0, 0, 32, 0, 0, 0 };
static int Gemfile_backoff = 0;   // accept paused (until this second)
static long Gsoft_fds = 65535;    // raised soft RLIMIT value
// UAF guard: conn_close records the freed object here. Any path that may
// have closed `c` resets Gclosed=NULL beforehand and checks Gclosed==c after,
// instead of touching possibly-freed memory.
static conn_t *Gclosed = NULL;

// dirty list: conns with framed responses pending send (pop-from-end phase).
// batch_resp flags responses built during the open group-commit txn, so a
// failed commit converts exactly those to 500s before anything is sent.
static conn_t **Gdirty = NULL;
static size_t Gdirty_n = 0, Gdirty_cap = 0;
static void pend_add(conn_t *c) {
  if (c->in_pend) return;
  c->in_pend = 1;
  if (Gdirty_n == Gdirty_cap) {
    size_t nc = Gdirty_cap ? Gdirty_cap * 2 : 1024;
    conn_t **nb = realloc(Gdirty, nc * sizeof *nb);
    if (!nb) { c->in_pend = 0; return; } // fail-safe: stall sweep reaps it
    Gdirty = nb; Gdirty_cap = nc;
  }
  c->pend_idx = Gdirty_n;
  Gdirty[Gdirty_n++] = c;
}
static void pend_remove(conn_t *c) {
  if (!c->in_pend) return;
  c->in_pend = 0;
  size_t i = c->pend_idx, last = Gdirty_n - 1;
  if (i != last) { Gdirty[i] = Gdirty[last]; Gdirty[i]->pend_idx = i; }
  Gdirty_n = last;
}
static void batch_note(conn_t *c) { // response framed; queue for send phase
  if (Gdb.in_txn) c->batch_resp = 1;
  pend_add(c);
}

// ---------- clock ----------
static void clock_tick(void) {
  struct timespec ts, tmo;
  clock_gettime(CLOCK_REALTIME, &ts);
  clock_gettime(CLOCK_MONOTONIC, &tmo);
  Gmono_s = (uint64_t)tmo.tv_sec;
  Gnow_ms = (uint64_t)tmo.tv_sec * 1000 + (uint64_t)(tmo.tv_nsec / 1000000);
  uint64_t s = (uint64_t)ts.tv_sec;
  if (s == Gnow_s) return;
  Gnow_s = s;
  time_t t = (time_t)s;
  struct tm tm;
  gmtime_r(&t, &tm);
  static const char *dow = "SunMonTueWedThuFriSat";
  static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  Gdate_len = (size_t)snprintf(Gdate, sizeof Gdate,
      "Date: %.3s, %02d %.3s %04d %02d:%02d:%02d GMT\r\n",
      dow + tm.tm_wday * 3, tm.tm_mday, mon + tm.tm_mon * 3, tm.tm_year + 1900,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ---------- timing wheel ----------
void wheel_init(wheel_t *w) { memset(w, 0, sizeof *w); }
void wheel_arm(wheel_t *w, conn_t *c, uint32_t now_s, uint32_t in_secs) {
  wheel_disarm(w, c);
  c->deadline = now_s + in_secs;
  wheel_slot_t *s = &w->slot[c->deadline & (WHEEL_SIZE - 1)];
  c->wnext = NULL; c->wprev = s->tail;
  if (s->tail) s->tail->wnext = c; else s->head = c;
  s->tail = c;
}
void wheel_disarm(wheel_t *w, conn_t *c) {
  (void)w;
  // find slot via deadline (c may be stale if deadline changed; caller arms fresh)
  wheel_slot_t *s = &w->slot[c->deadline & (WHEEL_SIZE - 1)];
  // verify membership by scan of neighbors (cheap, no full scan)
  if (c->wprev) c->wprev->wnext = c->wnext; else if (s->head == c) s->head = c->wnext;
  if (c->wnext) c->wnext->wprev = c->wprev; else if (s->tail == c) s->tail = c->wprev;
  c->wprev = c->wnext = NULL;
}
void wheel_tick(wheel_t *w, uint32_t now_s,
                void (*on_expire)(conn_t *c, void *arg), void *arg) {
  while (w->tick < now_s) {
    w->tick++;
    wheel_slot_t *s = &w->slot[w->tick & (WHEEL_SIZE - 1)];
    conn_t *c = s->head;
    s->head = s->tail = NULL;
    while (c) {
      conn_t *nx = c->wnext;
      c->wprev = c->wnext = NULL;
      if (c->deadline <= w->tick) on_expire(c, arg);
      else wheel_arm(w, c, w->tick, c->deadline - w->tick); // wrong slot, reinsert
      c = nx;
    }
  }
}

conn_t *conn_new(int fd, uint32_t now_s) {
  conn_t *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->fd = fd;
  c->last_act = now_s;
  c->req_since = now_s;
  c->want_read = 1;
  return c;
}
void conn_free(conn_t *c) {
  free(c->in);
  free(c->out);
  free(c);
}

// ---------- listener ----------
static int make_listener(const char *host, const char *port) {
  struct addrinfo hints, *res, *rp;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
  int fd = -1;
  for (rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* NOTE: deliberately no SO_REUSEPORT. A second instance must fail to bind
       loudly, never silently split traffic. */

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
    close(fd); fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  if (listen(fd, 4096) != 0) { close(fd); return -1; }
  return fd;
}

// ---------- conn lifecycle ----------
static void conn_set_rd(conn_t *c, int on) {
  if (!!on == !!c->want_read) return;
  c->want_read = (uint8_t)!!on;
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.data.ptr = c;
  ev.events = EPOLLET | EPOLLRDHUP | (on ? EPOLLIN : 0) | (c->in_epollout ? EPOLLOUT : 0);
  epoll_ctl(Gep_fd, EPOLL_CTL_MOD, c->fd, &ev);
}
static void conn_set_wr(conn_t *c, int on) {
  if (!!on == !!c->in_epollout) return;
  c->in_epollout = (uint8_t)!!on;
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.data.ptr = c;
  ev.events = EPOLLET | EPOLLRDHUP | (c->want_read ? EPOLLIN : 0) | (on ? EPOLLOUT : 0);
  epoll_ctl(Gep_fd, EPOLL_CTL_MOD, c->fd, &ev);
}
#ifdef COUNT_CLOSES
static unsigned long close_from[16] = {0};
static const char *close_tag[16] = {
  "shed", "expire-idle", "expire-stall", "recv-eof", "recv-err", "frame-err",
  "frame-oom", "send-err", "send-eagain?", "post-send", "accept?", "500-conv?",
  "other1", "other2", "other3", "other4"};
#define COUNT_CLOSE(i) do { close_from[i]++; } while (0)
static uint64_t last_dump_ms = 0;
static void maybe_dump_closes(void) {
  if (Gnow_ms - last_dump_ms < 5000) return;
  last_dump_ms = Gnow_ms;
  fprintf(stderr, "closes:");
  for (int i = 0; i < 16; i++) if (close_from[i]) fprintf(stderr, " %s=%lu", close_tag[i], close_from[i]);
  fprintf(stderr, " open=%ld level=%d\n", Gev.open_conns, Gev.level);
}
#else
#define COUNT_CLOSE(i) do {} while (0)
#define maybe_dump_closes() do {} while (0)
#endif
static int close_site = -1; /* set before each conn_close call site */
static void conn_close(conn_t *c) {
  COUNT_CLOSE(close_site < 0 ? 15 : close_site);
  close_site = -1;
  Gclosed = c;
  epoll_ctl(Gep_fd, EPOLL_CTL_DEL, c->fd, NULL);
  wheel_disarm(&Gwheel, c);
  pend_remove(c);
  close(c->fd);
  Gev.open_conns--;
  conn_free(c);
}
static void on_expire(conn_t *c, void *arg) {
  (void)arg;
  uint32_t now = (uint32_t)Gnow_s;
  int busy = (c->in_len > 0 || c->out != NULL);
  if (busy) {
    if (now - c->req_since >= CONN_STALL_S) { close_site = 2; conn_close(c); return; }
    wheel_arm(&Gwheel, c, now, c->req_since + CONN_STALL_S - now);
    return;
  }
  if (now - c->last_act >= CONN_KEEPALIVE_S) { close_site = 1; conn_close(c); return; }
  wheel_arm(&Gwheel, c, now, c->last_act + CONN_KEEPALIVE_S - now);
}

// ---------- memory sensing (cgroup-aware) ----------
static long mem_usable_mb(void) {
  // Prefer cgroup v2 (correct inside containers); fall back to meminfo.
  FILE *f = fopen("/sys/fs/cgroup/memory.max", "r");
  if (f) {
    char b[64];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    if (n > 0 && !(n == 4 && memcmp(b, "max\n", 4) == 0)) {
      b[n] = 0;
      long long maxv = atoll(b);
      f = fopen("/sys/fs/cgroup/memory.current", "r");
      if (f) {
        n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        if (n > 0) { b[n] = 0; return (long)((maxv - atoll(b)) >> 20); }
      }
    }
  }
  f = fopen("/proc/meminfo", "r");
  if (!f) return 1024;
  char line[128];
  long avail = -1, slab = 0;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "MemAvailable: %ld kB", &avail) == 1) continue;
    if (sscanf(line, "SReclaimable: %ld kB", &slab) == 1) continue;
  }
  fclose(f);
  if (avail < 0) return 1024;
  return (avail - slab) >> 10;
}

// ---------- eviction ----------
static void evict_sample(void) {
  static uint32_t last_fd_check = 0, last_mem_check = 0;
  uint32_t now = (uint32_t)Gnow_s;
  long soft = Gsoft_fds;
  long l1 = soft - 5535 < EVICT_L1 ? soft - 5535 : EVICT_L1;
  long l2 = soft - 2535 < EVICT_L2 ? soft - 2535 : EVICT_L2;
  long rel = soft - 10535 < EVICT_RELEASE ? soft - 10535 : EVICT_RELEASE;
  if (l1 < 1024) l1 = 1024;
  if (l2 < l1) l2 = l1;
  if (rel < 512) rel = 512;
  if (now - last_fd_check >= 1 || Gev.level > 0) {
    last_fd_check = now;
    long total = Gev.open_conns + Gev.fd_reserve;
    int prev = Gev.level;
    if (Gev.level == 0 && total >= l1) Gev.level = 1;
    else if (Gev.level == 1 && total >= l2) Gev.level = 2;
    else if (Gev.level > 0 && total < rel) Gev.level = 0;
    if (Gev.level != prev)
      fprintf(stderr, "evict: level %d->%d (conns %ld)\n", prev, Gev.level, total);
  }
  if (now - last_mem_check >= 1) {
    last_mem_check = now;
    long usable = mem_usable_mb();
    if (usable < MEM_SHED_MB && Gev.level < 2) Gev.level = 2;
    else if (usable < MEM_STOP_MB && Gev.level < 1) Gev.level = 1;
  }
  /* Trickle shedding: a token bucket paces closes to a smooth, sustainable
     rate (default 2500 per 250ms window = 10k/s). Bursty mass-closes would
     synchronize thousands of reconnects and overflow the listen backlog,
     whose retransmit backoff then avalanches (measured at 100k users).
     The walk rotates so each call does bounded work. */
  if (Gev.level > 0) {
    uint64_t win = Gnow_ms / 250;
    if (win != Gev.shed_win) { Gev.shed_win = win; Gev.shed_budget = 2500; }
    long quota = Gev.shed_budget;
    if (quota > 512) quota = 512; // per-call bound
    if (quota > 0) {
      long closed = 0;
      unsigned scanned = 0;
      for (unsigned k = 0; k < WHEEL_SIZE && quota > 0 && scanned < 32; k++) {
        unsigned i = (Gev.walk_from + k) & (WHEEL_SIZE - 1);
        scanned++;
        conn_t *c = Gwheel.slot[i].head;
        while (c && quota > 0) {
          conn_t *nx = c->wnext;
          if (!c->closing && !c->out && c->in_len == 0 &&
              now - c->last_act >= (uint32_t)(Gev.level == 2 ? 0 : 2)) {
            /* Never shed a conn with bytes waiting in the kernel: that is an
               unprocessed request, and closing it would silently drop it (the
               client sees FIN/RST instead of a response). It gets served
               normally (likely with Connection: close) and sheds later idle. */
            int pending = 0;
            if (ioctl(c->fd, FIONREAD, &pending) != 0 || pending <= 0) {
              close_site = 0; conn_close(c);
              quota--; closed++;
            }
          }
          c = nx;
        }
      }
      Gev.walk_from = (Gev.walk_from + scanned) & (WHEEL_SIZE - 1);
      Gev.shed_budget -= closed;
      Gev.last_shed = now;
      if (closed >= 64) fprintf(stderr, "evict: shed %ld idle (level %d)\n", closed, Gev.level);
    }
  }
}
static int evict_should_close(int idle_long) {
  if (Gev.level >= 2) return 1;
  if (Gev.level == 1 && idle_long) return 1;
  return 0;
}

// ---------- response framing ----------
static const char *reason(int s) {
  switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}
static void frame_response(conn_t *c, const resp_t *r, int close_after) {
  char hdr[512];
  int hn = snprintf(hdr, sizeof hdr,
      "HTTP/1.1 %d %s\r\n%.*sContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
      r->status, reason(r->status), (int)Gdate_len, Gdate,
      r->body_len, close_after ? "close" : "keep-alive");
  if (hn < 0 || hn >= (int)sizeof hdr) { close_site = 5; conn_close(c); return; }
  size_t total = (size_t)hn + r->body_len;
  free(c->out); // by construction none pending; be safe
  c->out = malloc(total ? total : 1);
  if (!c->out) { close_site = 6; conn_close(c); return; }
  memcpy(c->out, hdr, (size_t)hn);
  memcpy(c->out + hn, r->body, r->body_len);
  c->out_len = total; c->out_off = 0;
  if (close_after) c->closing = 1;
  batch_note(c);
}
static const char ERR500_BODY[] = "{\"error\":\"internal server error\"}";
static void frame_500(conn_t *c) {
  resp_t r = { 500, ERR500_BODY, sizeof(ERR500_BODY) - 1 };
  frame_response(c, &r, 0);
}

// ---------- request handling ----------
static void handle_complete(conn_t *c, req_t *req, int idle_long) {
  Gapi.now_s = Gnow_s;
  Gapi.now_mono = Gmono_s;
  resp_t r = api_handle(&Gapi, req);
  frame_response(c, &r, (req->conn_close || evict_should_close(idle_long)) ? 1 : 0);
}

static void do_recv(conn_t *c) {
  char tmp[8192];
  for (;;) {
    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
    if (n > 0) {
      size_t need = c->in_len + (size_t)n;
      if (need > c->in_cap) {
        size_t nc = c->in_cap ? c->in_cap * 2 : 4096;
        while (nc < need) nc *= 2;
        if (nc > HTTP_MAX_TOTAL + 8192) { close_site = 5; conn_close(c); return; }
        char *nb = realloc(c->in, nc);
        if (!nb) { close_site = 6; conn_close(c); return; }
        c->in = nb; c->in_cap = nc;
      }
      memcpy(c->in + c->in_len, tmp, (size_t)n);
      c->in_len = need;
      continue; // edge-triggered: drain until EAGAIN
    }
    if (n == 0) { close_site = 3; conn_close(c); return; } // orderly shutdown
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    close_site = 4; conn_close(c); return; // RST / error
  }
  if (c->in_len == 0) return;
  // parse (possibly pipelined) requests
  for (int iter = 0; iter < 8; iter++) {
    if (c->out) return; // previous response still draining; wait (safety)
    req_t req;
    memset(&req, 0, sizeof req);
    int rc = http_parse(c->in, c->in_len, &req);
    if (rc == 0) {
      if (req.headers_done && req.expect_continue && !c->exp_sent) {
        static const char CONT[] = "HTTP/1.1 100 Continue\r\n\r\n";
        ssize_t w = send(c->fd, CONT, sizeof(CONT) - 1, MSG_NOSIGNAL);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { conn_close(c); return; }
        if (w == (ssize_t)(sizeof(CONT) - 1)) c->exp_sent = 1;
      }
      break; // need more data
    }
    if (rc < 0) { close_site = 5; conn_close(c); return; } // framing error / too large
    int idle_long = (c->last_act + 2 < (uint32_t)Gnow_s);
    c->last_act = (uint32_t)Gnow_s;
    c->req_since = (uint32_t)Gnow_s;
    c->exp_sent = 0;
    wheel_arm(&Gwheel, c, (uint32_t)Gnow_s, CONN_KEEPALIVE_S);
    Gclosed = NULL;
    handle_complete(c, &req, idle_long);
    if (Gclosed == c) return; // frame path closed it (malloc fail)
    size_t cons = req.consumed;
    if (cons < c->in_len) memmove(c->in, c->in + cons, c->in_len - cons);
    c->in_len -= cons;
    if (c->in_len == 0) break;
  }
  if (c->in && c->in_len == 0) { free(c->in); c->in = NULL; c->in_cap = 0; }
  c->last_act = (uint32_t)Gnow_s;
  wheel_arm(&Gwheel, c, (uint32_t)Gnow_s, CONN_KEEPALIVE_S);
}

// ---------- send ----------
static void flush_send(conn_t *c) {
  while (c->out && c->out_off < c->out_len) {
    ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
    if (n > 0) { c->out_off += (size_t)n; continue; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      conn_set_wr(c, 1);
      conn_set_rd(c, 0); // strict per-conn serialization: no new reads while sending
      return;
    }
    close_site = 7; conn_close(c); return;
  }
  free(c->out); c->out = NULL; c->out_len = c->out_off = 0;
  conn_set_wr(c, 0);
  if (c->closing) { close_site = 9; conn_close(c); return; }
  conn_set_rd(c, 1);
  c->req_since = (uint32_t)Gnow_s;
  wheel_arm(&Gwheel, c, (uint32_t)Gnow_s, CONN_KEEPALIVE_S);
}

// ---------- accept ----------
static void do_accept(void) {
  for (int i = 0; i < 8192; i++) {
    if (Gev.level >= 2) return; // shedding: leave backlog until pressure drops
    int fd = accept4(Glisten_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EMFILE || errno == ENFILE) {
        Gemfile_backoff = (int)(uint32_t)Gnow_s + 1; // retry in ~1s, no spin
        if (Gev.level < 2) Gev.level = 2;
        return;
      }
      return; // other errors: drop, keep serving
    }
    if ((long)fd >= Gsoft_fds) { close(fd); continue; } // beyond our table
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    conn_t *c = conn_new(fd, (uint32_t)Gnow_s);
    if (!c) { close(fd); return; }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.data.ptr = c;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (epoll_ctl(Gep_fd, EPOLL_CTL_ADD, fd, &ev) != 0) { close(fd); conn_free(c); return; }
    wheel_arm(&Gwheel, c, (uint32_t)Gnow_s, CONN_KEEPALIVE_S);
    Gev.open_conns++;
  }
}

// ---------- main loop ----------
static void on_term(int sig) { (void)sig; Gstop = 1; }

static void run_loop(void) {
  struct epoll_event evts[512];
  while (!Gstop) {
    Gdb.batch_failed = 0; // new batch: no poison yet
    clock_tick();
    maybe_dump_closes();
    wheel_tick(&Gwheel, (uint32_t)Gnow_s, on_expire, NULL);
    evict_sample();
    int n = epoll_wait(Gep_fd, evts, 512, 100);
    for (int i = 0; i < n; i++) {
      if (evts[i].data.ptr == NULL) { // listener
        if ((evts[i].events & EPOLLIN) && (int)(uint32_t)Gnow_s >= Gemfile_backoff)
          do_accept();
        continue;
      }
      conn_t *c = evts[i].data.ptr;
      uint32_t e = evts[i].events;
      if (e & (EPOLLERR | EPOLLHUP)) { close_site = 4; conn_close(c); continue; }
      if ((e & EPOLLRDHUP) && !(e & EPOLLIN)) { close_site = 4; conn_close(c); continue; }
      if (e & EPOLLOUT) {
        Gclosed = NULL;
        flush_send(c);
        if (Gclosed == c) continue; // closed inside; do not touch c
      }
      if (e & EPOLLIN) {
        Gclosed = NULL;
        do_recv(c);
        if (Gclosed == c) continue;
      }
    }
    // ---- group commit: no response of this batch goes out before COMMIT ----
    // A mid-batch rollback (step failure) poisons the batch just like a
    // failed COMMIT: every response built under the rolled-back txn becomes a
    // 500, so a 201 is never sent for a row that isn't committed (rule 6).
    if (Gdb.in_txn || Gdb.batch_failed) {
      int ok = 0;
      if (Gdb.in_txn) {
        if (db_commit(&Gdb) == 0) ok = 1;
        else db_rollback(&Gdb);
      }
      if (!ok) {
        for (size_t i = Gdirty_n; i-- > 0;) { // backwards: close-safe
          conn_t *c = Gdirty[i];
          if (c->batch_resp) {
            c->batch_resp = 0;
            Gclosed = NULL;
            frame_500(c); // re-queues (already queued: no-op); may close on OOM
            if (Gclosed == c) continue;
          }
        }
      } else {
        for (size_t i = 0; i < Gdirty_n; i++) Gdirty[i]->batch_resp = 0;
      }
      Gdb.batch_failed = 0;
    }
    // ---- send phase ----
    while (Gdirty_n > 0) {
      conn_t *c = Gdirty[Gdirty_n - 1];
      Gdirty_n--;
      c->in_pend = 0;
      c->batch_resp = 0;
      Gclosed = NULL;
      flush_send(c); // EAGAIN re-arms EPOLLOUT (completed via events); errors close
    }
  }
}

// ---------- PGO training (build.sh only; synthetic DB, no network) ----------
static const char *TRAIN_SCHEMA =
  "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
  " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
  "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
  " body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),"
  " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
  "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id),"
  " post_id INTEGER NOT NULL REFERENCES posts(id),"
  " created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
  " PRIMARY KEY (user_id, post_id));"
  "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
  "CREATE INDEX posts_user_id_idx ON posts (user_id);"
  "CREATE INDEX likes_post_id_idx ON likes (post_id);";

static int pgo_train(const char *path) {
  unlink(path);
  { // db_open prepares statements eagerly: create file + schema first
    sqlite3 *tmp = NULL;
    if (sqlite3_open_v2(path, &tmp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) return 1;
    char *err = NULL;
    int rc = sqlite3_exec(tmp, TRAIN_SCHEMA, NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "train schema: %s\n", err ? err : "?");
    sqlite3_free(err);
    sqlite3_close(tmp);
    if (rc != SQLITE_OK) return 1;
  }
  db_t d;
  if (db_open(&d, path) != 0) return 1;
  sqlite3_exec(d.db, "BEGIN", NULL, NULL, NULL);
  sqlite3_stmt *iu, *ip, *il;
  sqlite3_prepare_v3(d.db, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1,
                     SQLITE_PREPARE_PERSISTENT, &iu, NULL);
  sqlite3_prepare_v3(d.db, "INSERT INTO posts (id, user_id, body) VALUES (?1, ?2, ?3)", -1,
                     SQLITE_PREPARE_PERSISTENT, &ip, NULL);
  sqlite3_prepare_v3(d.db, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2)", -1,
                     SQLITE_PREPARE_PERSISTENT, &il, NULL);
  char name[64], body[128];
  for (int u = 1; u <= 500; u++) {
    int nl = snprintf(name, sizeof name, "trainer_%d", u);
    sqlite3_bind_int(iu, 1, u);
    sqlite3_bind_text(iu, 2, name, nl, SQLITE_TRANSIENT);
    sqlite3_step(iu); sqlite3_reset(iu);
  }
  for (int p = 1; p <= 5000; p++) {
    int bl = snprintf(body, sizeof body, "training post %d with some filler text", p);
    sqlite3_bind_int(ip, 1, p);
    sqlite3_bind_int(ip, 2, (p % 500) + 1);
    sqlite3_bind_text(ip, 3, body, bl, SQLITE_TRANSIENT);
    sqlite3_step(ip); sqlite3_reset(ip);
  }
  for (int l = 0; l < 50000; l++) {
    sqlite3_bind_int(il, 1, (l % 500) + 1);
    sqlite3_bind_int(il, 2, (l % 5000) + 1);
    sqlite3_step(il); sqlite3_reset(il);
  }
  sqlite3_exec(d.db, "COMMIT", NULL, NULL, NULL);
  // exercise the exact prepared statements the server uses
  for (int i = 0; i < 3000; i++) {
    while (sqlite3_step(d.st_feed) == SQLITE_ROW) {}
    sqlite3_reset(d.st_feed);
    sqlite3_bind_int64(d.st_post, 1, (i % 5000) + 1);
    sqlite3_step(d.st_post); sqlite3_reset(d.st_post); sqlite3_clear_bindings(d.st_post);
    if (db_begin(&d) == 0) {
      sqlite3_bind_int64(d.st_create, 1, (i % 500) + 1);
      sqlite3_bind_text(d.st_create, 2, "pgo training write", -1, SQLITE_STATIC);
      sqlite3_step(d.st_create); sqlite3_reset(d.st_create); sqlite3_clear_bindings(d.st_create);
      sqlite3_bind_int64(d.st_like, 1, (i % 500) + 1);
      sqlite3_bind_int64(d.st_like, 2, (i % 5000) + 1);
      sqlite3_step(d.st_like); sqlite3_reset(d.st_like); sqlite3_clear_bindings(d.st_like);
      if (i % 64 == 63) db_commit(&d);
    }
  }
  db_commit(&d);
  sqlite3_finalize(iu); sqlite3_finalize(ip); sqlite3_finalize(il);
  db_close(&d);
  printf("pgo-train done: %s\n", path);
  return 0;
}

// ---------- main ----------
int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--pgo-train") == 0) return pgo_train(argv[2]);

  const char *db_path = getenv("SQLITE_PATH");
  const char *secret = getenv("JWT_SECRET");
  const char *host = getenv("HOST");
  const char *port = getenv("PORT");
  if (!db_path || !db_path[0] || !secret || !secret[0]) {
    fprintf(stderr, "need SQLITE_PATH and JWT_SECRET in the environment\n");
    return 1;
  }
  if (!host || !host[0]) host = "127.0.0.1";
  if (!port || !port[0]) port = "3000";

  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) Gsoft_fds = (long)rl.rlim_cur;
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);

  if (db_open(&Gdb, db_path) != 0) return 1;
  clock_tick();
  Gapi.db = &Gdb;
  Gapi.jwt_secret = (const uint8_t *)secret;
  Gapi.secret_len = strlen(secret);
  Gapi.now_s = Gnow_s;
  Gapi.now_mono = Gmono_s;
  Gapi.boot_mono = Gmono_s;
  Gapi.scratch = Gscratch;

  Glisten_fd = make_listener(host, port);
  if (Glisten_fd < 0) { fprintf(stderr, "listen %s:%s failed: %s\n", host, port, strerror(errno)); return 1; }
  Gep_fd = epoll_create1(EPOLL_CLOEXEC);
  if (Gep_fd < 0) { fprintf(stderr, "epoll_create1: %s\n", strerror(errno)); return 1; }
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.data.ptr = NULL; // listener marker
  // NOTE: the listener is level-triggered on purpose. do_accept() may stop
  // early under L2 pressure; with edge-triggering a quiet backlog would never
  // wake us again once pressure drops. Level-triggering re-fires while the
  // backlog is non-empty, so accepts always resume.
  ev.events = EPOLLIN;
  if (epoll_ctl(Gep_fd, EPOLL_CTL_ADD, Glisten_fd, &ev) != 0) {
    fprintf(stderr, "epoll add listener: %s\n", strerror(errno));
    return 1;
  }
  wheel_init(&Gwheel);
  Gev.fd_reserve = 32;
  fprintf(stderr, "serving %s:%s db=%s fds=%ld\n", host, port, db_path, Gsoft_fds);

  run_loop();

  if (Gdb.in_txn) db_commit(&Gdb); // best effort on shutdown
  db_close(&Gdb);
  return 0;
}
