/* The program around server.sql: an io_uring loop that fills the http_requests table, runs the statement,
 * and sends what it answered. One thread, plus a thread that runs WAL checkpoints so fsync never stalls
 * the loop. Configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT.
 *
 * Sockets are io_uring direct descriptors: they don't count against LimitNOFILE (65,535), but each
 * ring's table can't be larger than it, so a new ring starts when the current one is full (up to 8). */
#define _GNU_SOURCE
#include "app.h"
#include "liburing.h"
#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

enum { MAX_RINGS = 8, NBUF = 4096, BUFSZ = 2048 };
enum { U_ACCEPT, U_RECV, U_SEND, U_CLOSE, U_WAKE };

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *br;
  char *bufs;
  conn *conns;              /* indexed by direct-descriptor slot */
  int nopen, efd;
} uring;

static uring RINGS[MAX_RINGS];
static int NRINGS, SLOTS, LFD, ACCEPT_RING = -1, NOPEN;
static conn IDLE;           /* list head: oldest idle connection first */

static uint64_t ud(int type, int ring, uint32_t slot, uint32_t gen) {
  return (uint64_t)type << 60 | (uint64_t)ring << 56 | (uint64_t)slot << 32 | gen;
}
static struct io_uring_sqe *sqe_of(int r) {
  struct io_uring_sqe *s = io_uring_get_sqe(&RINGS[r].ring);
  if (!s) { io_uring_submit(&RINGS[r].ring); s = io_uring_get_sqe(&RINGS[r].ring); }
  return s;
}

static void idle_remove(conn *c) { if (c->next) { c->prev->next = c->next; c->next->prev = c->prev; c->next = c->prev = 0; } }
static void idle_push(conn *c) {
  idle_remove(c);
  c->prev = IDLE.prev; c->next = &IDLE;
  IDLE.prev->next = c; IDLE.prev = c;
}

/* Within 1/16 of all rings' capacity, replies carry Connection: close (off again below 1/8). */
static void update_pressure(void) {
  int cap = MAX_RINGS * SLOTS;
  if (NOPEN > cap - cap / 16) CLOSE_PRESSURE = 1;
  else if (NOPEN < cap - cap / 8) CLOSE_PRESSURE = 0;
}

static void close_conn(conn *c) {
  if (!c->open) return;
  idle_remove(c);
  struct io_uring_sqe *s = sqe_of(c->ring);
  io_uring_prep_shutdown(s, c->fd, SHUT_RDWR);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
  io_uring_sqe_set_data64(s, ud(U_CLOSE, c->ring, (uint32_t)c->fd, c->gen));
  s = sqe_of(c->ring);
  io_uring_prep_close_direct(s, (unsigned)c->fd);
  io_uring_sqe_set_data64(s, ud(U_CLOSE, c->ring, (uint32_t)c->fd, c->gen));
  free(c->in); free(c->out);
  c->in = c->out = 0;
  c->in_len = c->in_cap = c->out_len = c->out_off = c->out_cap = 0;
  c->open = c->busy = c->close_after = c->sent_continue = c->draining = c->sending = c->eof = 0;
  c->gen++;
  RINGS[c->ring].nopen--;
  NOPEN--;
  update_pressure();
}

/* Last resort when every ring is full: close idle connections, most recently used first (the furthest
 * from sending again), connections waiting for a FIN after Connection: close before those. */
static int shed_idle(int n) {
  int closed = 0;
  for (conn *c = IDLE.prev, *p; c != &IDLE && closed < n; c = p) { p = c->prev; if (c->draining) { close_conn(c); closed++; } }
  for (conn *c = IDLE.prev, *p; c != &IDLE && closed < n; c = p) { p = c->prev; close_conn(c); closed++; }
  return closed;
}

static void arm_recv(conn *c) {
  struct io_uring_sqe *s = sqe_of(c->ring);
  io_uring_prep_recv_multishot(s, c->fd, 0, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud(U_RECV, c->ring, (uint32_t)c->fd, c->gen));
}

static void send_out(conn *c) {
  struct io_uring_sqe *s = sqe_of(c->ring);
  io_uring_prep_send(s, c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, ud(U_SEND, c->ring, (uint32_t)c->fd, c->gen));
  c->sending = 1;
}

static void serve_pending(void);

/* Called when c has nothing left to send. */
static void after_send(conn *c) {
  free(c->out);
  c->out = 0; c->out_len = c->out_off = c->out_cap = 0;
  if (c->busy) return;
  /* After a Connection: close reply, wait for the client's FIN rather than closing first: whoever closes
   * first keeps the TIME_WAIT, and on the server those collide with clients reusing source ports. */
  if (c->close_after && c->eof) { close_conn(c); return; }
  if (c->close_after) c->draining = 1;
  idle_push(c);
}

static void flush(conn *c) {
  if (c->sending || !c->open) return;
  if (c->out_off < c->out_len) send_out(c);
  else after_send(c);
}

/* Queues the next complete request of c (one at a time: a pipelined request waits for the previous reply). */
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

/* Runs the statement over the batch and queues the replies' sends. */
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
      flush(c);
    }
  }
}

static int ring_init(int r);

static void arm_accept(int r) {
  ACCEPT_RING = r;
  struct io_uring_sqe *s = sqe_of(r);
  io_uring_prep_multishot_accept_direct(s, LFD, 0, 0, 0);
  io_uring_sqe_set_data64(s, ud(U_ACCEPT, r, 0, 0));
}

/* The accepting ring's table is full: move accept to a ring with room, starting a new one if needed. */
static void move_accept(void) {
  int best = -1;
  for (int r = 0; r < NRINGS; r++) if (RINGS[r].nopen < SLOTS - 256 && (best < 0 || RINGS[r].nopen < RINGS[best].nopen)) best = r;
  if (best < 0 && NRINGS < MAX_RINGS && ring_init(NRINGS) == 0) best = NRINGS - 1;
  if (best < 0) { shed_idle(256); best = ACCEPT_RING; }
  arm_accept(best);
}

static void on_cqe(int r, struct io_uring_cqe *cqe) {
  uint64_t u = cqe->user_data;
  int type = (int)(u >> 60), res = cqe->res;
  uint32_t slot = (uint32_t)(u >> 32) & 0xffffff, gen = (uint32_t)u;
  uring *R = &RINGS[r];
  if (type == U_WAKE) {
    uint64_t v;
    if (read(RINGS[slot].efd, &v, sizeof v) < 0) {}
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
      struct io_uring_sqe *s = sqe_of(0);
      io_uring_prep_poll_multishot(s, RINGS[slot].efd, POLLIN);
      io_uring_sqe_set_data64(s, ud(U_WAKE, 0, slot, 0));
    }
    return;
  }
  if (type == U_ACCEPT) {
    if (res >= 0) {
      conn *c = &R->conns[res];
      c->fd = res; c->ring = (uint8_t)r; c->open = 1;
      R->nopen++; NOPEN++;
      update_pressure();
      idle_push(c);
      arm_recv(c);
    }
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
      if (res == -ENFILE || res == -EMFILE || R->nopen >= SLOTS - 256) move_accept();
      else arm_accept(r);
    }
    return;
  }
  if (type == U_CLOSE) return;
  conn *c = &R->conns[slot];
  int live = c->open && c->gen == gen;
  if (type == U_RECV) {
    if (cqe->flags & IORING_CQE_F_BUFFER) {  /* always give the buffer back, even for a closed connection */
      unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
      char *b = R->bufs + (size_t)bid * BUFSZ;
      if (live && res > 0 && !c->draining) {
        if (c->in_len + (uint32_t)res > 81920) { close_conn(c); live = 0; }
        else {
          if (c->in_len + (uint32_t)res > c->in_cap) {
            uint32_t cap = c->in_cap ? c->in_cap : 4096;
            while (cap < c->in_len + (uint32_t)res) cap *= 2;
            c->in = realloc(c->in, cap); c->in_cap = cap;
          }
          memcpy(c->in + c->in_len, b, (size_t)res);
          c->in_len += (uint32_t)res;
        }
      }
      io_uring_buf_ring_add(R->br, b, BUFSZ, (unsigned short)bid, io_uring_buf_ring_mask(NBUF), 0);
      io_uring_buf_ring_advance(R->br, 1);
    }
    if (!live) return;
    if (res == 0 || (res < 0 && res != -ENOBUFS)) {  /* FIN or error: close once nothing is in flight */
      if (c->busy || c->sending) c->close_after = c->eof = 1;
      else close_conn(c);
      return;
    }
    if (!(cqe->flags & IORING_CQE_F_MORE)) arm_recv(c);
    if (res > 0 && !c->busy && !c->sending && !c->draining) pump(c);
    return;
  }
  if (type == U_SEND) {
    if (!live) return;
    c->sending = 0;
    if (res < 0) { close_conn(c); return; }
    c->out_off += (uint32_t)res;
    if (c->out_off < c->out_len) { send_out(c); return; }
    after_send(c);
    if (c->open && c->in_len && !c->busy && !c->draining) pump(c);
  }
}

static int ring_init(int r) {
  uring *R = &RINGS[r];
  struct io_uring_params p = {.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE, .cq_entries = 65536};
  int rc = io_uring_queue_init_params(4096, &R->ring, &p);
  if (rc < 0) { fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-rc)); return -1; }
  if ((rc = io_uring_register_files_sparse(&R->ring, (unsigned)SLOTS)) < 0) { fprintf(stderr, "register files: %s\n", strerror(-rc)); return -1; }
  R->br = io_uring_setup_buf_ring(&R->ring, NBUF, 0, 0, &rc);
  if (!R->br) { fprintf(stderr, "buf ring: %s\n", strerror(-rc)); return -1; }
  R->bufs = malloc((size_t)NBUF * BUFSZ);
  for (int i = 0; i < NBUF; i++) io_uring_buf_ring_add(R->br, R->bufs + (size_t)i * BUFSZ, BUFSZ, (unsigned short)i, io_uring_buf_ring_mask(NBUF), i);
  io_uring_buf_ring_advance(R->br, NBUF);
  R->conns = calloc((size_t)SLOTS, sizeof *R->conns);
  if (r > 0) {  /* ring 0 is the one the loop sleeps on: other rings wake it through an eventfd */
    R->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    io_uring_register_eventfd(&R->ring, R->efd);
    struct io_uring_sqe *s = sqe_of(0);
    io_uring_prep_poll_multishot(s, R->efd, POLLIN);
    io_uring_sqe_set_data64(s, ud(U_WAKE, 0, (uint32_t)r, 0));
  }
  NRINGS = r + 1;
  fprintf(stderr, "ring %d up (%d slots)\n", r, SLOTS);
  return 0;
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

  /* the kernel refuses a direct-descriptor table larger than RLIMIT_NOFILE */
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  SLOTS = rl.rlim_cur > 65535 ? 65535 : (int)rl.rlim_cur;
  IDLE.next = IDLE.prev = &IDLE;

  LFD = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;
  setsockopt(LFD, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(LFD, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  /* inherited by accepted sockets */
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port))};
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "HOST must be an IPv4 address\n"); return 1; }
  if (bind(LFD, (struct sockaddr *)&a, sizeof a) < 0 || listen(LFD, 65535) < 0) { perror("bind/listen"); return 1; }

  if (ring_init(0)) return 1;
  arm_accept(0);

  pthread_t t;
  pthread_create(&t, 0, checkpointer, (void *)path);
  fprintf(stderr, "server.sql is serving on %s:%s\n", host, port);

  for (;;) {
    for (int r = 1; r < NRINGS; r++) io_uring_submit(&RINGS[r].ring);
    io_uring_submit_and_wait(&RINGS[0].ring, 1);
    for (int r = 0; r < NRINGS; r++) {
      struct io_uring_cqe *cqe;
      unsigned head, n = 0;
      io_uring_for_each_cqe(&RINGS[r].ring, head, cqe) { on_cqe(r, cqe); n++; }
      io_uring_cq_advance(&RINGS[r].ring, n);
    }
    serve_pending();
  }
}
