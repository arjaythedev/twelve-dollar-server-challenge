/* One thread, one io_uring: accepts, multishot receives into a provided-buffer ring, group-committed
 * SQLite batches, sends. HTTP parsing and every handler live in Swift (one callback, lp_handler);
 * this file only moves bytes and decides when a batch commits.
 *
 * A pass: wait for WAIT_MIN completions or WAIT_NS, drain them (requests are answered as they
 * are read), commit the SQLite transaction the answers shared, then send every response. Nothing
 * from a pass is sent before its COMMIT returns. */
#define _GNU_SOURCE
#include "shim.h"
#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef unsigned char u8;
typedef uint64_t u64;

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_SHUTDOWN, OP_CLOSE, OP_READ };

/* Single-shot accepts kept in flight, spread over the listeners. Each is re-armed only after a
 * pass sees it complete, so this bounds the accept rate: a ramp of thousands of connections a
 * second needs hundreds in flight, or the listen queues fill and clients wait out SYN retries. */
#define ACCEPT_DEPTH 512
/* SO_REUSEPORT listeners on the one port: each has its own accept queue (somaxconn deep). */
#define LISTENERS 8
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536
/* A request is a few hundred bytes, a POST with its body under 2 KB; a longer one fills its
 * buffer and is read again. BUF_COUNT also bounds the single-shot reads in flight. */
#define BUF_SIZE 2048
#define BUF_COUNT 1024
/* A pass reads up to READS_PER_PASS times but answers at most ANSWERS_PER_PASS reads; the rest
 * wait on their connection (in `partial`) on the ready list. */
#define READS_PER_PASS 16384
#define ANSWERS_PER_PASS 2048
/* Each pass waits for this many completions or WAIT_NS. Larger passes share one COMMIT. */
#define WAIT_MIN 256
#define WAIT_NS 1000000
#define POOL_MAX 2048
#define OUT_CAP 8192

typedef struct {
  unsigned short gen;
  u8 open, close_after, held, eof, closing, sending, queued, recving, ready, in_batch, answered;
  unsigned batch_mark, sent;
  lp_ob out;     /* responses not yet handed to the kernel */
  lp_ob wire;    /* the one send in flight */
  lp_ob partial; /* an incomplete request, or bytes waiting for a later pass's answers */
} conn;

typedef struct {
  u64 *a;
  unsigned cap, head, n; /* cap is a power of two */
} fifo;

static struct io_uring ring;
static lp_handler handler;
static conn *conns;
static unsigned slots, nopen, margin, depth, nreads;
static int listen_fds[LISTENERS];
static unsigned accepts[LISTENERS];
static int accepting;
static unsigned answers_left;

static struct io_uring_buf_ring *br;
static u8 *bufmem;
static unsigned br_pending;

static fifo rq;       /* readable connections waiting for a read */
static fifo ready;    /* connections with bytes read but not yet answered */
static u64 *held_a, *held_b; /* connections whose next request is a write waiting out a hold */
static unsigned held_n;
static unsigned *touched;
static unsigned touched_n;

static lp_ob pool[POOL_MAX];
static unsigned pool_n;

static inline u64 ud(unsigned op, unsigned gen, unsigned slot) { return (u64)op << 56 | (u64)gen << 32 | slot; }
static inline u64 ent(unsigned gen, unsigned slot) { return (u64)gen << 32 | slot; }

/* ---- buffers ---- */

void lp_ob_grow(lp_ob *o, unsigned need) {
  if (o->cap - o->len >= need) return;
  unsigned nc = o->cap * 2;
  if (nc < o->len + need) nc = o->len + need;
  if (nc < 256) nc = 256;
  u8 *p = realloc(o->p, nc);
  if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
  o->p = p;
  o->cap = nc;
}

static void ob_take(lp_ob *o) {
  if (pool_n) {
    *o = pool[--pool_n];
    o->len = 0;
  } else {
    o->p = malloc(OUT_CAP);
    if (!o->p) { fprintf(stderr, "out of memory\n"); abort(); }
    o->len = 0;
    o->cap = OUT_CAP;
  }
}

static void ob_give(lp_ob *o) {
  if (!o->p) return;
  if (pool_n < POOL_MAX && o->cap <= 4 * OUT_CAP) {
    pool[pool_n++] = *o;
  } else {
    free(o->p);
  }
  memset(o, 0, sizeof *o);
}

static void ob_free(lp_ob *o) {
  free(o->p);
  memset(o, 0, sizeof *o);
}

static void ob_append(lp_ob *o, const u8 *p, unsigned n) {
  lp_ob_grow(o, n);
  memcpy(o->p + o->len, p, n);
  o->len += n;
}

static void fifo_init(fifo *f, unsigned cap) {
  f->a = malloc(cap * sizeof *f->a);
  if (!f->a) abort();
  f->cap = cap;
  f->head = f->n = 0;
}

static void fifo_push(fifo *f, u64 v) {
  if (f->n == f->cap) {
    u64 *a = malloc(f->cap * 2 * sizeof *a);
    if (!a) abort();
    for (unsigned i = 0; i < f->n; i++) a[i] = f->a[(f->head + i) & (f->cap - 1)];
    free(f->a);
    f->a = a;
    f->head = 0;
    f->cap *= 2;
  }
  f->a[(f->head + f->n++) & (f->cap - 1)] = v;
}

static u64 fifo_pop(fifo *f) {
  u64 v = f->a[f->head++ & (f->cap - 1)];
  f->n--;
  return v;
}

/* Frees a connection's buffers and resets it, keeping its generation. */
static void conn_reset(conn *c, unsigned short gen, int open) {
  ob_give(&c->out);
  ob_give(&c->wire);
  ob_free(&c->partial);
  memset(c, 0, sizeof *c);
  c->gen = gen;
  c->open = (u8)open;
}

/* ---- provided buffers ---- */

static inline void buf_add(unsigned bid) {
  io_uring_buf_ring_add(br, bufmem + (size_t)bid * BUF_SIZE, BUF_SIZE, (unsigned short)bid, BUF_COUNT - 1, br_pending++);
}

static inline void buf_publish(void) {
  if (br_pending) {
    io_uring_buf_ring_advance(br, (int)br_pending);
    br_pending = 0;
  }
}

/* Registers buffer group 0. Ubuntu's 6.8.0-136 and later kernels invert the check on the
 * reserved words (Launchpad #2162843): they refuse all zeroes and accept a nonzero resv[0], so a
 * refused registration is tried once more that way. A correct kernel never gets there. */
static int setup_bufs(void) {
  size_t bytes = BUF_COUNT * sizeof(struct io_uring_buf);
  void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return -errno;
  bufmem = malloc((size_t)BUF_COUNT * BUF_SIZE);
  if (!bufmem) return -ENOMEM;
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (unsigned long)m;
  reg.ring_entries = BUF_COUNT;
  reg.bgid = 0;
  int rc = 0;
  for (int t = 0; t < 2; t++) {
    rc = io_uring_register_buf_ring(&ring, &reg, 0);
    if (rc != -EINVAL) break;
    reg.resv[0] = 1;
  }
  if (rc < 0) return rc;
  br = m;
  io_uring_buf_ring_init(br);
  for (unsigned i = 0; i < BUF_COUNT; i++) buf_add(i);
  buf_publish();
  return 0;
}

/* ---- submissions ---- */

/* Room for `room` entries (a linked pair must not be split across submissions). If the kernel
 * can't take the queue yet (its completion queue is overflowing) it flushes completions and
 * tries again; only if that keeps failing is the entry dropped, rather than stalling every
 * connection. */
static struct io_uring_sqe *get_sqe(unsigned room) {
  int tries = 0;
  while (io_uring_sq_space_left(&ring) < room) {
    if (io_uring_submit(&ring) < 0) io_uring_get_events(&ring);
    if (++tries == 1000) {
      fprintf(stderr, "submission queue stuck full; dropping an entry\n");
      return NULL;
    }
  }
  return io_uring_get_sqe(&ring);
}

/* The steady state: one multishot receive per connection, one completion per request. */
static void arm_recv(unsigned slot, unsigned gen) {
  struct io_uring_sqe *s = get_sqe(1);
  if (!s) return;
  io_uring_prep_recv_multishot(s, (int)slot, NULL, 0, 0);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud(OP_RECV, gen, slot));
}

/* One read of a queued connection. MSG_DONTWAIT: if nothing is left it ends with EAGAIN. */
static void arm_read(unsigned slot, unsigned gen) {
  struct io_uring_sqe *s = get_sqe(1);
  if (!s) return;
  io_uring_prep_recv(s, (int)slot, NULL, 0, MSG_DONTWAIT);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  io_uring_sqe_set_data64(s, ud(OP_READ, gen, slot));
}

static void arm_send(unsigned slot, conn *c) {
  struct io_uring_sqe *s = get_sqe(1);
  if (!s) return;
  io_uring_prep_send(s, (int)slot, c->wire.p + c->sent, c->wire.len - c->sent, MSG_NOSIGNAL);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, ud(OP_SEND, c->gen, slot));
}

static void arm_close(unsigned slot, conn *c) {
  c->closing = 1;
  /* The shutdown ends the multishot receive or the read in flight; the close frees the slot. */
  struct io_uring_sqe *s = get_sqe(2);
  if (!s) return;
  io_uring_prep_shutdown(s, (int)slot, SHUT_RDWR);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  io_uring_sqe_set_data64(s, ud(OP_SHUTDOWN, c->gen, slot));
  s = io_uring_get_sqe(&ring);
  io_uring_prep_close_direct(s, slot);
  io_uring_sqe_set_data64(s, ud(OP_CLOSE, c->gen, slot));
}

static void fill_accepts(void) {
  for (int li = 0; li < LISTENERS; li++) {
    while (accepts[li] < depth / LISTENERS) {
      struct io_uring_sqe *s = get_sqe(1);
      if (!s) return;
      io_uring_prep_accept_direct(s, listen_fds[li], NULL, NULL, 0, IORING_FILE_INDEX_ALLOC);
      io_uring_sqe_set_data64(s, ud(OP_ACCEPT, 0, (unsigned)li));
      accepts[li]++;
    }
  }
}

/* ---- connections ---- */

static inline void mark(unsigned slot, conn *c, unsigned out_len) {
  if (!c->in_batch) {
    c->in_batch = 1;
    c->batch_mark = out_len;
    touched[touched_n++] = slot;
  }
}

/* The client is done sending (EOF, an error). A held write or bytes on the ready list are still
 * owed answers: then it closes once they are answered. */
static void finish(unsigned slot, conn *c) {
  if (c->held || c->ready) {
    c->eof = 1;
  } else {
    c->close_after = 1;
    mark(slot, c, c->out.len);
  }
}

/* Parses and answers every complete request in data (plus any earlier partial bytes). During a
 * checkpoint hold a write stops it: that request and everything behind it stay on the
 * connection, which goes on the held list, until the hold ends. */
static void serve(unsigned slot, conn *c, const u8 *data, unsigned dlen) {
  if (c->close_after) return;
  const u8 *buf;
  unsigned blen;
  int own = c->partial.len != 0;
  if (own) {
    if (dlen) ob_append(&c->partial, data, dlen);
    buf = c->partial.p;
    blen = c->partial.len;
  } else {
    buf = data;
    blen = dlen;
  }
  unsigned off = 0;
  while (off < blen) {
    if (!c->out.p) ob_take(&c->out);
    unsigned before = c->out.len, used = 0;
    int r = handler(buf + off, blen - off, &c->out, &used);
    if (r == 1) {
      mark(slot, c, before);
      c->answered = 1;
      off += used;
    } else if (r == 0) {
      break;
    } else if (r == 2) {
      if (!c->held) {
        c->held = 1;
        held_a[held_n++] = ent(c->gen, slot);
      }
      break;
    } else {
      mark(slot, c, before);
      c->answered = 1;
      c->close_after = 1;
      off = blen;
      break;
    }
  }
  if (c->out.p && c->out.len == 0 && !c->in_batch) ob_give(&c->out);
  unsigned rest = blen - off;
  if (rest == 0) {
    if (own) ob_free(&c->partial);
  } else if (own) {
    memmove(c->partial.p, c->partial.p + off, rest);
    c->partial.len = rest;
  } else {
    ob_append(&c->partial, data + off, rest);
  }
}

/* Bytes received: answered now if this pass has answers left and nothing older waits on the
 * connection, otherwise kept for a later pass (drain first). */
static void received(unsigned slot, conn *c, const u8 *data, unsigned len) {
  if (c->close_after) return; /* its answers are done; what follows is dropped */
  if (c->held || c->ready || answers_left == 0) {
    ob_append(&c->partial, data, len);
    if (!c->held && !c->ready) {
      c->ready = 1;
      fifo_push(&ready, ent(c->gen, slot));
    }
  } else {
    answers_left--;
    serve(slot, c, data, len);
  }
}

static inline void queue_read(unsigned slot, conn *c) {
  if (!c->queued) {
    c->queued = 1;
    fifo_push(&rq, ent(c->gen, slot));
  }
}

static void on_cqe(u64 u, int res, unsigned flags) {
  unsigned op = (unsigned)(u >> 56), gen = (unsigned)(u >> 32) & 0xffff, slot = (unsigned)u;
  int has_buf = flags & IORING_CQE_F_BUFFER;
  unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
  conn *c;
  switch (op) {
  case OP_ACCEPT:
    accepts[slot]--;
    if (res >= 0) {
      c = &conns[res];
      conn_reset(c, (unsigned short)(c->gen + 1), 1);
      arm_recv((unsigned)res, c->gen);
      nopen++;
      if (nopen + margin >= slots) accepting = 0;
    } else if (res == -ENFILE) {
      accepting = 0;
    }
    break;
  case OP_RECV:
    c = &conns[slot];
    if (c->gen != gen || !c->open || c->closing) {
      if (has_buf) buf_add(bid);
      break;
    }
    if (res > 0) {
      if (has_buf) {
        received(slot, c, bufmem + (size_t)bid * BUF_SIZE, (unsigned)res);
        buf_add(bid);
      }
      /* The multishot ended (the buffers ran out behind it): read the rest in bounded
       * single-shot reads, which re-arm it once the socket is drained. */
      if (!(flags & IORING_CQE_F_MORE)) queue_read(slot, c);
    } else if (res == -ENOBUFS) {
      /* A burst emptied the buffer ring. Not re-armed: re-arming every receive at once repeats
       * the storm. */
      queue_read(slot, c);
    } else {
      if (has_buf) buf_add(bid);
      finish(slot, c);
    }
    break;
  case OP_READ:
    nreads--;
    c = &conns[slot];
    if (c->gen != gen || !c->open || c->closing) {
      if (has_buf) buf_add(bid);
      break;
    }
    c->recving = 0;
    if (res > 0) {
      if (!has_buf) {
        queue_read(slot, c);
        break;
      }
      received(slot, c, bufmem + (size_t)bid * BUF_SIZE, (unsigned)res);
      buf_add(bid);
      if (res == BUF_SIZE) queue_read(slot, c); /* the buffer filled: there may be more */
      else arm_recv(slot, gen);                 /* drained: back to the multishot receive */
    } else if (res == -EAGAIN) {
      arm_recv(slot, gen);
    } else if (res == -ENOBUFS) {
      queue_read(slot, c);
    } else {
      if (has_buf) buf_add(bid);
      finish(slot, c);
    }
    break;
  case OP_SEND:
    c = &conns[slot];
    if (c->gen != gen || !c->sending) break;
    if (res < 0) {
      c->sending = 0;
      ob_give(&c->wire);
      ob_give(&c->out);
      c->close_after = 1;
      mark(slot, c, c->out.len);
    } else {
      c->sent += (unsigned)res;
      if (c->sent < c->wire.len) {
        arm_send(slot, c);
      } else {
        c->sending = 0;
        ob_give(&c->wire);
        if (c->out.len || c->close_after) mark(slot, c, c->out.len); /* sent after this batch commits */
        else ob_give(&c->out);
      }
    }
    break;
  case OP_CLOSE:
    c = &conns[slot];
    if (c->gen == gen && c->open) {
      conn_reset(c, c->gen, 0);
      nopen--;
    }
    break;
  default:
    break;
  }
}

static void drain(void) {
  unsigned head, n = 0;
  struct io_uring_cqe *cqe;
  io_uring_for_each_cqe(&ring, head, cqe) {
    n++;
    on_cqe(cqe->user_data, cqe->res, cqe->flags);
  }
  io_uring_cq_advance(&ring, n);
  buf_publish();
  if (accepting) {
    if (nopen + margin >= slots) accepting = 0;
    else fill_accepts();
  } else if (nopen + 2 * margin < slots) {
    accepting = 1;
    fill_accepts();
  }
}

static void send_500(lp_ob *o) {
  static const char body[] = "{\"error\":\"internal server error\"}";
  char head[128];
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n",
                   (int)(sizeof body - 1));
  ob_append(o, (const u8 *)head, (unsigned)n);
  ob_append(o, (const u8 *)body, (unsigned)(sizeof body - 1));
}

/* Commits the batch, then hands every response it produced to the kernel. */
static void end_batch(void) {
  int committed = td_commit() == 0;
  /* No transaction is open here: the only point where a checkpoint hold starts or ends. */
  td_tick();
  for (unsigned i = 0; i < touched_n; i++) {
    unsigned slot = touched[i];
    conn *c = &conns[slot];
    if (!committed && c->answered) {
      /* No response from a rolled-back batch may claim success. */
      if (!c->out.p) ob_take(&c->out);
      c->out.len = c->batch_mark;
      send_500(&c->out);
      c->close_after = 1;
    }
    c->in_batch = 0;
    c->answered = 0;
    if (!c->open || c->closing || c->sending) continue;
    if (c->out.len) {
      c->wire = c->out;
      memset(&c->out, 0, sizeof c->out);
      c->sent = 0;
      c->sending = 1;
      arm_send(slot, c);
    } else if (c->close_after) {
      arm_close(slot, c);
    }
  }
  touched_n = 0;
}

/* The hold is over: serves the writes it held (and whatever arrived behind them) in this pass,
 * from the bytes kept on each connection. */
static void release_held(void) {
  u64 *list = held_a;
  unsigned n = held_n;
  held_a = held_b;
  held_b = list;
  held_n = 0;
  for (unsigned i = 0; i < n; i++) {
    unsigned slot = (unsigned)list[i], gen = (unsigned)(list[i] >> 32);
    conn *c = &conns[slot];
    if (c->gen == gen && c->open && !c->closing) {
      c->held = 0;
      serve(slot, c, NULL, 0);
      if (c->eof && !c->held) {
        c->close_after = 1;
        mark(slot, c, c->out.len);
      }
    }
  }
}

/* Answers connections on the ready list, oldest first, while this pass has answers left. */
static void run_ready(void) {
  while (answers_left > 0 && ready.n) {
    u64 e = fifo_pop(&ready);
    unsigned slot = (unsigned)e, gen = (unsigned)(e >> 32);
    conn *c = &conns[slot];
    if (c->gen != gen || !c->open || c->closing) continue;
    c->ready = 0;
    if (c->held) continue; /* release_held answers it, with everything behind the held write */
    answers_left--;
    serve(slot, c, NULL, 0);
    if (c->eof && !c->held) {
      c->close_after = 1;
      mark(slot, c, c->out.len);
    }
  }
}

/* Issues one read per queued connection, up to budget and never more than the ring has free
 * receive buffers, so the kernel never runs out of them. The rest stay queued, in order. */
static unsigned issue_reads(unsigned budget) {
  unsigned issued = 0, count = rq.n;
  for (unsigned i = 0; i < count && issued < budget; i++) {
    u64 e = fifo_pop(&rq);
    unsigned slot = (unsigned)e, gen = (unsigned)(e >> 32);
    conn *c = &conns[slot];
    if (c->gen != gen || !c->open || c->closing) continue;
    if (c->recving || nreads >= BUF_COUNT) {
      fifo_push(&rq, e);
      continue;
    }
    c->queued = 0;
    c->recving = 1;
    nreads++;
    issued++;
    arm_read(slot, gen);
  }
  return issued;
}

/* Reads the queued connections in rounds. A MSG_DONTWAIT read completes inside the submit, so
 * each round is one non-blocking enter, and a request is answered in the pass that saw it
 * readable. */
static void read_rounds(void) {
  unsigned budget = READS_PER_PASS;
  while (budget > 0 && rq.n) {
    unsigned k = issue_reads(budget);
    if (k == 0) break;
    budget -= k;
    int rc = io_uring_submit_and_get_events(&ring);
    if (rc < 0 && rc != -EINTR && rc != -EBUSY && rc != -EAGAIN) fprintf(stderr, "io_uring_enter: %s\n", strerror(-rc));
    drain();
  }
}

/* ---- setup ---- */

static int listener(struct in_addr ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return -1;
  /* Accepted sockets inherit TCP_NODELAY. */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr = ip;
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
  if (listen(fd, 65535) < 0) return -1;
  return fd;
}

/* Raises the soft descriptor limit to the hard one. A ring's file table may be as large as it. */
static unsigned descriptor_slots(void) {
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  return rl.rlim_cur < (1u << 20) ? (unsigned)rl.rlim_cur : (1u << 20);
}

static unsigned clamp(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

int lp_run(const char *host, int port, lp_handler h) {
  handler = h;
  signal(SIGPIPE, SIG_IGN);
  struct in_addr ip;
  if (inet_pton(AF_INET, host, &ip) != 1) {
    fprintf(stderr, "HOST must be an IPv4 address\n");
    return -1;
  }
  slots = descriptor_slots();
  for (int i = 0; i < LISTENERS; i++) {
    listen_fds[i] = listener(ip, port);
    if (listen_fds[i] < 0) {
      fprintf(stderr, "listen %s:%d: %s\n", host, port, strerror(errno));
      return -1;
    }
  }
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
  p.cq_entries = CQ_ENTRIES;
  int rc = io_uring_queue_init_params(SQ_ENTRIES, &ring, &p);
  if (rc < 0) {
    /* Kernels before 6.1 lack DEFER_TASKRUN: the same loop works with the default task work. */
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = CQ_ENTRIES;
    rc = io_uring_queue_init_params(SQ_ENTRIES, &ring, &p);
  }
  if (rc < 0) {
    fprintf(stderr, "io_uring: %s\n", strerror(-rc));
    return -1;
  }
  if ((rc = io_uring_register_files_sparse(&ring, slots)) < 0) {
    fprintf(stderr, "register files: %s\n", strerror(-rc));
    return -1;
  }
  if ((rc = setup_bufs()) < 0) {
    fprintf(stderr, "buffer ring: %s\n", strerror(-rc));
    return -1;
  }
  conns = calloc(slots, sizeof *conns);
  touched = malloc(slots * sizeof *touched);
  held_a = malloc((size_t)slots * 2 * sizeof *held_a);
  held_b = malloc((size_t)slots * 2 * sizeof *held_b);
  if (!conns || !touched || !held_a || !held_b) {
    fprintf(stderr, "out of memory\n");
    return -1;
  }
  fifo_init(&rq, 1024);
  fifo_init(&ready, 1024);
  /* In flight at once: at most a sixteenth of the table, and always less than the margin. */
  depth = clamp(slots / 16, LISTENERS, ACCEPT_DEPTH) / LISTENERS * LISTENERS;
  margin = clamp(slots / 8, depth + 1, 2 * ACCEPT_DEPTH);
  if (margin > slots / 4) margin = slots / 4;
  accepting = 1;
  fill_accepts();
  fprintf(stderr, "listening on %s:%d; io_uring, %u direct descriptors\n", host, port, slots);

  struct __kernel_timespec ts = {0, WAIT_NS};
  for (;;) {
    /* Submit what the last batch queued and wait for completions. Deferred task work
     * (completions) only runs inside an enter with GETEVENTS. With reads or answers still
     * queued there is work now: the wait is skipped. */
    int busy = rq.n || ready.n;
    struct io_uring_cqe *cqe;
    rc = busy ? io_uring_submit_and_get_events(&ring) : io_uring_submit_and_wait_timeout(&ring, &cqe, WAIT_MIN, &ts, NULL);
    if (rc < 0 && rc != -EINTR && rc != -ETIME && rc != -EBUSY && rc != -EAGAIN) fprintf(stderr, "io_uring_enter: %s\n", strerror(-rc));
    answers_left = ANSWERS_PER_PASS;
    drain();
    run_ready();
    read_rounds();
    if (held_n && !td_writes_held()) release_held();
    end_batch();
  }
}
