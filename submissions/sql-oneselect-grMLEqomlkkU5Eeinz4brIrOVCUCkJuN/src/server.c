/* The program around server.sql: an epoll loop that fills the http_requests table, runs the statement,
 * and sends what it answered. One thread, plus a thread that runs WAL checkpoints so fsync never stalls
 * the loop. Configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT. */
#define _GNU_SOURCE
#include "app.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

static int EP, LFD, MAXFD, NOPEN;
static conn *CONNS;          /* indexed by fd */
static conn IDLE;            /* list head: oldest idle connection first */

static void idle_remove(conn *c) { if (c->next) { c->prev->next = c->next; c->next->prev = c->prev; c->next = c->prev = 0; } }
static void idle_push(conn *c) {
  idle_remove(c);
  c->prev = IDLE.prev; c->next = &IDLE;
  IDLE.prev->next = c; IDLE.prev = c;
}

static void close_conn(conn *c) {
  if (!c->open) return;
  idle_remove(c);
  close(c->fd);
  free(c->in); free(c->out);
  c->in = c->out = 0;
  c->in_len = c->in_cap = c->out_len = c->out_off = c->out_cap = 0;
  c->open = c->busy = c->close_after = c->sent_continue = 0;
  c->gen++;
  NOPEN--;
}

/* LimitNOFILE is 65,535, fewer than the number of users. Near the limit, close the connections that
 * have been idle longest. */
static void shed_idle(int n) {
  while (n-- > 0 && IDLE.next != &IDLE) close_conn(IDLE.next);
}

static void set_events(conn *c, uint32_t ev) {
  struct epoll_event e = {.events = ev, .data.fd = c->fd};
  epoll_ctl(EP, EPOLL_CTL_MOD, c->fd, &e);
}

/* Sends c->out. Returns 0 if the connection was closed. */
static int flush(conn *c) {
  while (c->out_off < c->out_len) {
    ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
    if (n > 0) { c->out_off += (uint32_t)n; continue; }
    if (n < 0 && errno == EAGAIN) { set_events(c, EPOLLIN | EPOLLOUT); return 1; }
    close_conn(c);
    return 0;
  }
  free(c->out);
  c->out = 0; c->out_len = c->out_off = c->out_cap = 0;
  if (c->close_after && !c->busy) { close_conn(c); return 0; }
  if (!c->busy) idle_push(c);
  return 1;
}

static void serve_pending(void);

/* Queues every complete request of c (one at a time: a pipelined request waits for the previous reply). */
static void pump(conn *c) {
  for (;;) {
    int r = queue_request(c);
    if (r == 1) {
      idle_remove(c);
      if (!c->in_len) { free(c->in); c->in = 0; c->in_cap = 0; }  /* idle connections hold no buffers */
      if (NBATCH >= BATCH_MAX) serve_pending();
      return;
    }
    if (r == 2) { serve_pending(); if (!c->open) return; continue; }
    if (r < 0) { close_conn(c); return; }
    if (c->out_len && !c->busy) flush(c);  /* a 100 Continue */
    if (c->open && !c->in_len) { free(c->in); c->in = 0; c->in_cap = 0; }
    return;
  }
}

/* Runs the statement over the batch, sends the replies, and starts on any pipelined requests. */
static void serve_pending(void) {
  static conn *done[BATCH_MAX];
  static uint32_t gens[BATCH_MAX];
  while (NBATCH) {
    int n = NBATCH;
    for (int i = 0; i < n; i++) { done[i] = BATCH[i].c; gens[i] = BATCH[i].gen; }
    run_batch();
    for (int i = 0; i < n; i++) {
      conn *c = done[i];
      if (c->gen != gens[i] || !c->open) continue;
      c->busy = 0;
      if (flush(c) && c->in_len) pump(c);
    }
  }
}

static void on_readable(conn *c) {
  static char rbuf[65536];
  for (;;) {
    ssize_t n = read(c->fd, rbuf, sizeof rbuf);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { if (!c->busy) close_conn(c); else c->close_after = 1; return; }
    if (n < 0) break;
    if (c->in_len + (uint32_t)n > 81920) { close_conn(c); return; }
    if (c->in_len + (uint32_t)n > c->in_cap) {
      uint32_t cap = c->in_cap ? c->in_cap : 4096;
      while (cap < c->in_len + (uint32_t)n) cap *= 2;
      c->in = realloc(c->in, cap); c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, rbuf, (size_t)n);
    c->in_len += (uint32_t)n;
    if ((size_t)n < sizeof rbuf) break;
  }
  if (!c->busy) pump(c);
}

static void on_accept(void) {
  for (;;) {
    int fd = accept4(LFD, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE) { shed_idle(1024); continue; }
      if (errno == EINTR || errno == ECONNABORTED) continue;
      return;  /* EAGAIN */
    }
    if (fd >= MAXFD) { close(fd); continue; }
    if (NOPEN >= MAXFD - 1024) shed_idle(1024);
    conn *c = &CONNS[fd];
    c->fd = fd; c->open = 1;
    NOPEN++;
    struct epoll_event e = {.events = EPOLLIN, .data.fd = fd};
    epoll_ctl(EP, EPOLL_CTL_ADD, fd, &e);
    idle_push(c);
  }
}

/* WAL checkpoints on their own connection and thread: the loop never runs one (wal_autocheckpoint=0). */
static void *checkpointer(void *path) {
  sqlite3 *db;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, 0) != SQLITE_OK) return 0;
  sqlite3_exec(db, "PRAGMA journal_size_limit=67108864; SELECT 1 FROM posts LIMIT 1;", 0, 0, 0);
  for (;;) {
    usleep(200000);
    int log = 0, ckpt = 0;
    sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, &log, &ckpt);
  }
  return 0;
}

int main(void) {
  const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
  const char *host = getenv("HOST") ? getenv("HOST") : "127.0.0.1", *port = getenv("PORT") ? getenv("PORT") : "3000";
  if (!path || !secret) { fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n"); return 1; }
  signal(SIGPIPE, SIG_IGN);

  /* server.sql lives next to the binary's source: bin/server -> src/server.sql */
  char exe[4096], sql[4200];
  ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (el < 0) { perror("readlink"); return 1; }
  exe[el] = 0;
  snprintf(sql, sizeof sql, "%s/../src/server.sql", dirname(exe));
  if (app_init(path, sql, secret)) return 1;

  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  MAXFD = rl.rlim_cur > 1 << 20 ? 1 << 20 : (int)rl.rlim_cur;
  CONNS = calloc((size_t)MAXFD, sizeof *CONNS);
  IDLE.next = IDLE.prev = &IDLE;

  LFD = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int one = 1;
  setsockopt(LFD, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(LFD, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  /* inherited by accepted sockets */
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port))};
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "HOST must be an IPv4 address\n"); return 1; }
  if (bind(LFD, (struct sockaddr *)&a, sizeof a) < 0 || listen(LFD, 65535) < 0) { perror("bind/listen"); return 1; }

  EP = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event e = {.events = EPOLLIN, .data.fd = LFD};
  epoll_ctl(EP, EPOLL_CTL_ADD, LFD, &e);

  pthread_t t;
  pthread_create(&t, 0, checkpointer, (void *)path);
  fprintf(stderr, "server.sql is serving on %s:%s\n", host, port);

  static struct epoll_event evs[1024];
  for (;;) {
    int n = epoll_wait(EP, evs, 1024, -1);
    for (int i = 0; i < n; i++) {
      int fd = evs[i].data.fd;
      if (fd == LFD) { on_accept(); continue; }
      conn *c = &CONNS[fd];
      if (!c->open) continue;
      if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(c);
      if (c->open && (evs[i].events & EPOLLOUT)) { if (flush(c) && c->out_len == 0) set_events(c, EPOLLIN); }
    }
    serve_pending();
  }
}
