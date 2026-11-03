/* poll(2) transport: the portable fallback for the io_uring transport, used on macOS and
 * whenever FORCE_POLL is set. One thread drives one listener and a flat pollfd array backed
 * by a connection state table. Each iteration collects every readable connection's requests
 * into one batch, serves the batch with a single application transaction, queues the
 * responses, and flushes the sockets that took the data without blocking. Connections that
 * stopped parsing at a write (or a close) resume only after that batch has committed, so a
 * pipelined request can never observe uncommitted state of an earlier request on the same
 * connection. */

#include "rt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS   4096    /* flat pollfd array + connection table bound */
#define READ_ROUNDS 4       /* reads per readable connection per iteration (4 x 16 KiB) */
#define READ_SZ     16384

enum { C_FREE = 0, C_OPEN };

typedef struct conn {
  u16 gen;
  u8 state, close_after, hold, pending, polout;
  u16 in_jobs;            /* parsed requests currently waiting in jobs[] for deliver() */
  u32 in_len, in_cap;
  u8 *in;                 /* bytes of a request that is not complete yet */
  obuf *out_head, *out_tail;
} conn;

static struct pollfd pfds[MAX_CONNS + 1];   /* [0] is the listener, [1 + slot] a connection */
static conn conns[MAX_CONNS];
static u32 slot_free[MAX_CONNS];            /* free slot stack, popped lowest index first */
static int nfree;
static u32 hi_slot;                         /* poll span: slots [0, hi_slot) */
static int lfd = -1;
static u32 nslots;
static int send_flags;                      /* MSG_NOSIGNAL where the platform has it */
static int accept_blocked;                  /* fd table or slot table exhausted */

/* ---- per-iteration request batch ------------------------------------------------------ */

#define MAX_JOBS 16384
static job_t jobs[MAX_JOBS];
static int njobs;
static u32 pend[MAX_JOBS];      /* connections holding complete requests for the next batch */
static int npend;

static const u8 RESP_400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed request\"}\r\n";

static void conn_close(u32 slot);

static void conn_hold(u32 slot) {
  conn *c = &conns[slot];
  c->hold = 1;
  if (!c->pending) { c->pending = 1; pend[npend++] = slot; }
}

/* Parse and decode as many complete requests as possible from p[0..n). Returns bytes used. */
static u32 parse_requests(u32 slot, const u8 *p, u32 n) {
  conn *c = &conns[slot];
  u32 used = 0;
  while (used < n && !c->hold && c->state == C_OPEN) {
    req_t rq;
    int k = http_parse(p + used, n - used, &rq);
    if (k == 0) break;
    if (njobs >= MAX_JOBS - 1) { conn_hold(slot); break; }
    job_t *j = &jobs[njobs++];
    memset(j, 0, sizeof *j);
    j->conn = slot;
    j->gen = c->gen;
    c->in_jobs++;
    if (k < 0) {
      j->resp = obuf_static(RESP_400, sizeof RESP_400 - 1);
      j->close_after = 1;
      used = n;
      conn_hold(slot);
      break;
    }
    j->close_after = rq.close;
    app_decode(&rq, j);
    used += (u32)k;
    if (j->is_write || j->close_after) conn_hold(slot);   /* later requests wait for this batch */
  }
  return used;
}

/* Keep the bytes of the request that is not complete yet. */
static void conn_keep(conn *c, const u8 *p, u32 n) {
  if (!n) { c->in_len = 0; return; }
  if (c->in == p) { c->in_len = n; return; }
  if (n > c->in_cap) {
    u32 cap = n < 4096 ? 4096 : n * 2;
    u8 *m = malloc(cap);
    if (!m) abort();
    if (c->in && c->in_len) memmove(m, p, n); else memcpy(m, p, n);
    free(c->in);
    c->in = m;
    c->in_cap = cap;
  } else memmove(c->in, p, n);
  c->in_len = n;
}

static void on_data(u32 slot, const u8 *p, u32 n) {
  conn *c = &conns[slot];
  if (c->in_len || c->hold) {
    if (c->in_len + n > 65536) { conn_close(slot); return; }
    if (c->in_len + n > c->in_cap) {
      u32 cap = (c->in_len + n) * 2;
      u8 *m = realloc(c->in, cap);
      if (!m) abort();
      c->in = m;
      c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, p, n);
    c->in_len += n;
    if (c->hold) return;
    u32 used = parse_requests(slot, c->in, c->in_len);
    conn_keep(c, c->in + used, c->in_len - used);
  } else {
    u32 used = parse_requests(slot, p, n);
    if (used < n) conn_keep(c, p + used, n - used);
  }
}

/* ---- connection lifecycle ------------------------------------------------------------- */

static void conn_reset(conn *c) {
  free(c->in);
  c->in = 0;
  c->in_len = c->in_cap = 0;
  for (obuf *b = c->out_head; b;) { obuf *nx = b->next; obuf_release(b); b = nx; }
  c->out_head = c->out_tail = 0;
  /* pending is cleared too: a slot recycled after a close must not inherit a stale hold,
   * or conn_hold would skip queueing it and the connection would wait forever. */
  c->close_after = c->hold = c->pending = c->polout = 0;
  c->in_jobs = 0;
}

static void close_now(u32 slot) {
  conn *c = &conns[slot];
  int fd = pfds[slot + 1].fd;
  if (fd >= 0) close(fd);
  pfds[slot + 1].fd = -1;
  pfds[slot + 1].events = 0;
  pfds[slot + 1].revents = 0;
  conn_reset(c);
  c->state = C_FREE;
  slot_free[nfree++] = slot;
  while (hi_slot && conns[hi_slot - 1].state == C_FREE) hi_slot--;
  if (accept_blocked) { accept_blocked = 0; pfds[0].events = POLLIN; }
}

/* Close a connection; a queued response is flushed first. */
static void conn_close(u32 slot) {
  conn *c = &conns[slot];
  if (c->state != C_OPEN) return;
  if (c->out_head) { c->close_after = 1; return; }
  close_now(slot);
}

static void set_polout(u32 slot, conn *c) {
  if (!c->polout) { c->polout = 1; pfds[slot + 1].events = (short)(pfds[slot + 1].events | POLLOUT); }
}

static void clr_polout(u32 slot, conn *c) {
  if (c->polout) { c->polout = 0; pfds[slot + 1].events = (short)(pfds[slot + 1].events & ~POLLOUT); }
}

static void conn_send(u32 slot, obuf *b) {
  conn *c = &conns[slot];
  if (c->state != C_OPEN) { obuf_release(b); return; }
  if (b->refs > 1) { b->refs--; b = obuf_clone(b); }
  b->next = 0;
  if (c->out_tail) c->out_tail->next = b; else c->out_head = b;
  c->out_tail = b;
}

/* Send as much of the response queue as the socket takes; the rest waits for POLLOUT. */
static void flush_conn(u32 slot) {
  conn *c = &conns[slot];
  int fd = pfds[slot + 1].fd;
  while (c->out_head) {
    obuf *b = c->out_head;
    ssize_t r = send(fd, b->data, b->len, send_flags);
    if (r > 0) {
      if ((u32)r < b->len) {           /* short send: continue with the rest */
        b->data += r;
        b->len -= (u32)r;
        set_polout(slot, c);
        return;
      }
      c->out_head = b->next;
      if (!c->out_head) c->out_tail = 0;
      obuf_release(b);
      continue;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) { set_polout(slot, c); return; }
    }
    for (obuf *x = c->out_head; x;) { obuf *nx = x->next; obuf_release(x); x = nx; }
    c->out_head = c->out_tail = 0;
    c->close_after = 0;
    conn_close(slot);
    return;
  }
  clr_polout(slot, c);
  if (c->close_after) { c->close_after = 0; close_now(slot); }
}

/* Read until the socket would block, a budget runs out, or the connection ends. */
static void read_conn(u32 slot, u8 *buf) {
  conn *c = &conns[slot];
  int fd = pfds[slot + 1].fd;
  for (int k = 0; k < READ_ROUNDS; k++) {
    ssize_t r = read(fd, buf, READ_SZ);
    if (LIKELY(r > 0)) {
      on_data(slot, buf, (u32)r);
      if (UNLIKELY(c->state != C_OPEN)) return;
      continue;
    }
    if (r == 0) {                      /* EOF: if requests from this connection are still
                                        * waiting in jobs[] or pend[], defer close until
                                        * deliver()/release_holds() finishes them and drop
                                        * POLLIN so the readable EOF socket cannot spin. */
      if (c->in_jobs || c->hold || c->pending) {
        c->close_after = 1;
        pfds[slot + 1].events = (short)(pfds[slot + 1].events & ~POLLIN);
      } else {
        conn_close(slot);
      }
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    conn_close(slot);                  /* ECONNRESET and friends */
    return;
  }
}

/* ---- batch delivery ------------------------------------------------------------------- */

static void deliver(void) {
  for (int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    conn *c = &conns[j->conn];
    if (c->gen == j->gen && c->in_jobs) c->in_jobs--;
    if (c->gen != j->gen || c->state != C_OPEN) { if (j->resp) obuf_release(j->resp); continue; }
    if (j->resp) conn_send(j->conn, j->resp);
    if (j->close_after || (c->close_after && !c->in_jobs && !c->hold && !c->pending)) conn_close(j->conn);
  }
  njobs = 0;
}

static void release_holds(void) {
  /* connections that stopped parsing at a write (or close) continue after their batch committed */
  static u32 snap[MAX_JOBS];
  int n = npend;
  memcpy(snap, pend, sizeof(u32) * (size_t)n);
  npend = 0;
  for (int i = 0; i < n; i++) {
    u32 slot = snap[i];
    conn *c = &conns[slot];
    c->pending = 0;
    c->hold = 0;
    if (c->state != C_OPEN) continue;
    if (c->in_len) {
      u32 used = parse_requests(slot, c->in, c->in_len);
      conn_keep(c, c->in + used, c->in_len - used);
    }
    if (c->close_after && !c->in_jobs && !c->hold && !c->pending) conn_close(slot);
  }
}

/* ---- setup ----------------------------------------------------------------------------- */

static int listen_socket(const char *host, int port) {
  struct sockaddr_storage ss;
  socklen_t sl;
  memset(&ss, 0, sizeof ss);
  struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
  int fam;
  if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) { fam = AF_INET; a4->sin_family = AF_INET; a4->sin_port = htons((u16)port); sl = sizeof *a4; }
  else if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) { fam = AF_INET6; a6->sin6_family = AF_INET6; a6->sin6_port = htons((u16)port); sl = sizeof *a6; }
  else { fprintf(stderr, "fatal: HOST %s is not an IP address\n", host); exit(1); }
  int fd = socket(fam, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  if (bind(fd, (struct sockaddr *)&ss, sl) < 0) { perror("bind"); exit(1); }
  if (listen(fd, 4096) < 0) { perror("listen"); exit(1); }
  return fd;
}

static void do_accepts(void) {
  for (;;) {
    if (!nfree) {                      /* table full: park the rest in the backlog */
      accept_blocked = 1;
      pfds[0].events = 0;
      return;
    }
    int fd = accept(lfd, 0, 0);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      static i64 last_err;
      if (errno == EMFILE || errno == ENFILE) {   /* retry on a timer, never spin */
        accept_blocked = 1;
        pfds[0].events = 0;
      }
      if (now_s != last_err) { last_err = now_s; fprintf(stderr, "accept: %s\n", strerror(errno)); }
      return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    u32 slot = slot_free[--nfree];
    conn *c = &conns[slot];
    if (c->state != C_FREE) conn_reset(c);
    c->gen++;
    c->state = C_OPEN;
    pfds[slot + 1].fd = fd;
    pfds[slot + 1].events = POLLIN;
    pfds[slot + 1].revents = 0;
    if (slot + 1 > hi_slot) hi_slot = slot + 1;
  }
}

static void transport_init(const char *host, int port, u32 want_conns) {
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);     /* best effort */
  nslots = want_conns < MAX_CONNS ? want_conns : MAX_CONNS;
  if (nslots < 1) nslots = 1;
  for (u32 i = nslots; i-- > 0;) slot_free[nfree++] = i;
  for (u32 i = 0; i <= MAX_CONNS; i++) pfds[i].fd = -1;
#ifdef MSG_NOSIGNAL
  send_flags = MSG_NOSIGNAL;
#endif
  lfd = listen_socket(host, port);
  pfds[0].fd = lfd;
  pfds[0].events = POLLIN;
}

static void transport_run(void) {
  u8 buf[READ_SZ];
  for (;;) {
    now_s = (i64)time(0);
    int was_blocked = accept_blocked;
    if (was_blocked) { accept_blocked = 0; pfds[0].events = POLLIN; }
    int n = poll(pfds, (nfds_t)(hi_slot + 1), was_blocked ? 1000 : -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      exit(1);
    }
    if (pfds[0].revents & POLLIN) do_accepts();
    njobs = 0;
    for (u32 i = 0; i < hi_slot; i++) {
      conn *c = &conns[i];
      if (c->state != C_OPEN) continue;
      short re = pfds[i + 1].revents;
      if (re & (POLLIN | POLLHUP | POLLERR)) read_conn(i, buf);
      if (c->state == C_OPEN && (re & POLLOUT) && c->out_head) flush_conn(i);
    }
    if (njobs) { app_batch(jobs, njobs); deliver(); }
    /* Held connections resume after their batch committed. Unlike the io_uring transport,
     * poll has no completion events to wake the loop, so draining here (instead of leaving
     * re-held connections for the next wakeup) keeps a pipelined tail from stalling on an
     * otherwise quiet socket. Each round either serves a batch or ends the loop. */
    for (;;) {
      release_holds();
      if (!njobs) break;
      app_batch(jobs, njobs);
      deliver();
    }
    for (u32 i = 0; i < hi_slot; i++) {
      conn *c = &conns[i];
      if (c->state == C_OPEN && c->out_head && !c->polout) flush_conn(i);
    }
  }
}
