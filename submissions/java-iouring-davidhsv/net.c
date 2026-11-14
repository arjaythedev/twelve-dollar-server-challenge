/* The kernel side of the server: io_uring through raw system calls (no liburing), the listening
 * sockets, and the bytes the kernel reads and writes while an operation is in flight.
 *
 * Java reads and writes these bytes in place through java.lang.foreign: a received message is
 * parsed where the kernel put it (the buffer goes back to the kernel once it is served), input
 * kept for later is parsed in its chunk, and a response is written into room reserved at the end
 * of the connection's pending output. Server.java decides everything; this file only does what it
 * is told.
 *
 * Design after rust-iouring-kensac (#264): accepted sockets are direct descriptors in a ring's
 * file table (a table is capped by RLIMIT_NOFILE, so more connections need more rings), one
 * multishot receive per connection into a provided-buffer ring, single-shot accepts over
 * several SO_REUSEPORT listeners, SHUTDOWN hard-linked to CLOSE. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <link.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "feed.h"

#define MAX_RINGS 16
#define MAX_LISTEN 8
#define SQ_ENTRIES 4096
#define CQ_ENTRIES 65536
/* Receive buffers per ring. A request is a few hundred bytes; a longer one spans buffers. What
 * matters is the count: a receive that finds no buffer ends its multishot, and a burst after a
 * stall needs one buffer per waiting connection (the count from goose-iouring-ahmadsh2007 #291,
 * which measured it). */
#define RX_COUNT 16384
#define RX_SIZE 1024
/* Chunks of this size are pooled; a 20-post feed response is about 5 KB. */
#define CHUNK_STD 8176
#define POOL_MAX 4096

/* The layout Java reads: len at offset 0, the bytes at offset 8. */
typedef struct chunk {
  uint32_t len, cap;
  uint8_t b[];
} chunk;

/* A connection's bytes. All three are null while idle, so an idle connection costs this record
 * and its kernel socket. */
typedef struct {
  chunk *out;  /* responses not handed to the kernel yet */
  chunk *wire; /* the one send in flight */
  chunk *kept; /* input waiting: a request still arriving, or one whose write is queued */
  uint32_t sent;
} cslot;

typedef struct {
  int fd;
  unsigned sq_mask, cq_mask, sq_tail;
  unsigned *sq_khead, *sq_ktail, *sq_kflags, *cq_khead, *cq_ktail;
  struct io_uring_sqe *sqes;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;
  uint16_t br_tail;
  uint8_t *rx;
  cslot *slots;
} ring_t;

static ring_t rings[MAX_RINGS];
static int nrings;
static int listeners[MAX_LISTEN];
static chunk *pool[POOL_MAX];
static int npool;

static void die(const char *what) {
  fprintf(stderr, "%s: %s\n", what, strerror(errno));
  exit(1);
}

/* ---- chunks ---- */

static chunk *chunk_new(uint32_t need) {
  if (need <= CHUNK_STD && npool > 0) {
    chunk *c = pool[--npool];
    c->len = 0;
    return c;
  }
  uint32_t cap = CHUNK_STD;
  while (cap < need) cap *= 2;
  chunk *c = malloc(sizeof(chunk) + cap);
  if (!c) die("out of memory");
  c->len = 0;
  c->cap = cap;
  return c;
}

static void chunk_free(chunk *c) {
  if (!c) return;
  if (c->cap == CHUNK_STD && npool < POOL_MAX) pool[npool++] = c;
  else free(c);
}

/* Room for n more bytes at the end of c (a new chunk if c is null): the chunk, perhaps moved. */
static chunk *chunk_room(chunk *c, int64_t n) {
  if (!c) return chunk_new((uint32_t)n);
  if (c->len + n <= c->cap) return c;
  chunk *g = chunk_new((uint32_t)(c->len + n));
  memcpy(g->b, c->b, c->len);
  g->len = c->len;
  chunk_free(c);
  return g;
}

static chunk *chunk_add(chunk *c, const uint8_t *p, int64_t n) {
  c = chunk_room(c, n);
  memcpy(c->b + c->len, p, (size_t)n);
  c->len += (uint32_t)n;
  return c;
}

/* ---- rings ---- */

static int sys_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, void *arg, size_t sz) {
  return (int)syscall(SYS_io_uring_enter, fd, to_submit, min_complete, flags, arg, sz);
}

/* Hands the queued operations to the kernel and runs the completions that are due, without
 * waiting. */
static void flush_now(ring_t *r) {
  unsigned pending = r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  if (sys_enter(r->fd, pending, 0, IORING_ENTER_GETEVENTS, 0, 0) < 0 && errno != EINTR && errno != EBUSY &&
      errno != EAGAIN)
    perror("io_uring_enter");
}

/* The next free submission entry, with room for `room` in a row (a linked pair must reach the
 * kernel in one submission). */
static struct io_uring_sqe *sqe_get(ring_t *r, unsigned room) {
  for (int tries = 0; r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE) + room > SQ_ENTRIES; tries++) {
    if (tries == 1000) die("submission queue stuck full");
    flush_now(r);
  }
  struct io_uring_sqe *e = &r->sqes[r->sq_tail++ & r->sq_mask];
  memset(e, 0, sizeof *e);
  return e;
}

/* Registers receive-buffer group 0. Ubuntu's 6.8.0-136 and later kernels invert the check on
 * the reserved words (Launchpad #2162843) and refuse all zeroes, so a refused registration is
 * tried once more with resv[0] = 1 (found by rust-iouring-kensac #264). */
static int buf_ring_register(ring_t *r) {
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof reg);
  reg.ring_addr = (uint64_t)(uintptr_t)r->br;
  reg.ring_entries = RX_COUNT;
  reg.bgid = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0) return 0;
    if (errno != EINVAL) return -1;
    reg.resv[0] = 1;
  }
  return -1;
}

static void rx_give(ring_t *r, unsigned bid) {
  struct io_uring_buf *b = &r->br->bufs[r->br_tail & (RX_COUNT - 1)];
  b->addr = (uint64_t)(uintptr_t)(r->rx + (size_t)bid * RX_SIZE);
  b->len = RX_SIZE;
  b->bid = (uint16_t)bid;
  r->br_tail++;
  __atomic_store_n(&r->br->tail, r->br_tail, __ATOMIC_RELEASE);
}

/* A new ring with `nslots` direct descriptors: its index, or -1. */
API int32_t net_ring_add(int64_t nslots) {
  if (nrings == MAX_RINGS) return -1;
  ring_t *r = &rings[nrings];
  memset(r, 0, sizeof *r);
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
            IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
  p.cq_entries = CQ_ENTRIES;
  int fd = (int)syscall(SYS_io_uring_setup, SQ_ENTRIES, &p);
  if (fd < 0) { perror("io_uring_setup"); return -1; }
  size_t sq_len = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  size_t cq_len = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  size_t ring_len = sq_len > cq_len ? sq_len : cq_len;
  size_t br_len = RX_COUNT * sizeof(struct io_uring_buf);
  uint8_t *sq = mmap(0, ring_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  void *sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
                    IORING_OFF_SQES);
  void *br = mmap(0, br_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void *rx = mmap(0, (size_t)RX_COUNT * RX_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  cslot *slots = calloc((size_t)nslots, sizeof(cslot));
  int ok = sq != MAP_FAILED && sqes != MAP_FAILED && br != MAP_FAILED && rx != MAP_FAILED && slots &&
           (p.features & IORING_FEAT_SINGLE_MMAP);
  if (ok) {
    r->fd = fd;
    r->sq_khead = (unsigned *)(sq + p.sq_off.head);
    r->sq_ktail = (unsigned *)(sq + p.sq_off.tail);
    r->sq_kflags = (unsigned *)(sq + p.sq_off.flags);
    r->sq_mask = *(unsigned *)(sq + p.sq_off.ring_mask);
    r->cq_khead = (unsigned *)(sq + p.cq_off.head);
    r->cq_ktail = (unsigned *)(sq + p.cq_off.tail);
    r->cq_mask = *(unsigned *)(sq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)(sq + p.cq_off.cqes);
    r->sqes = sqes;
    unsigned *array = (unsigned *)(sq + p.sq_off.array);
    for (unsigned i = 0; i < p.sq_entries; i++) array[i] = i;
    r->br = br;
    r->rx = rx;
    r->slots = slots;
    struct io_uring_rsrc_register rr;
    memset(&rr, 0, sizeof rr);
    rr.nr = (unsigned)nslots;
    rr.flags = IORING_RSRC_REGISTER_SPARSE;
    ok = syscall(SYS_io_uring_register, fd, IORING_REGISTER_FILES2, &rr, sizeof rr) == 0 && buf_ring_register(r) == 0;
    if (!ok) perror("io_uring_register");
  } else {
    perror("io_uring ring memory");
  }
  if (!ok) {
    if (sq != MAP_FAILED) munmap(sq, ring_len);
    if (sqes != MAP_FAILED) munmap(sqes, p.sq_entries * sizeof(struct io_uring_sqe));
    if (br != MAP_FAILED) munmap(br, br_len);
    if (rx != MAP_FAILED) munmap(rx, (size_t)RX_COUNT * RX_SIZE);
    free(slots);
    close(fd);
    return -1;
  }
  for (unsigned i = 0; i < RX_COUNT; i++) rx_give(r, i);
  return nrings++;
}

/* Where the ring's receive buffers start: buffer `bid` is RX_SIZE bytes at base + bid * RX_SIZE. */
API uint64_t net_rx_base(int32_t ri) { return (uint64_t)(uintptr_t)rings[ri].rx; }

/* Submits what is queued and runs the completions that are due; with `wait` > 0 it waits for
 * that many completions or `ns` nanoseconds (0: it does not sleep, it only lets the kernel run
 * up to `wait` completions). Deferred task work (the receives) only runs inside an enter that
 * asks for events. */
API void net_enter(int32_t ri, int32_t wait, int64_t ns) {
  ring_t *r = &rings[ri];
  unsigned pending = r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
  int rc;
  if (wait > 0) {
    struct __kernel_timespec ts = {.tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000};
    struct io_uring_getevents_arg arg = {.ts = (uint64_t)(uintptr_t)&ts};
    rc = sys_enter(r->fd, pending, (unsigned)wait, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
  } else {
    rc = sys_enter(r->fd, pending, 0, IORING_ENTER_GETEVENTS, 0, 0);
  }
  if (rc < 0 && errno != ETIME && errno != EINTR && errno != EBUSY && errno != EAGAIN) perror("io_uring_enter");
}

/* A ring that is not waited on: submits what is queued and runs the completions that are due,
 * without sleeping. The kernel says in the ring's flags whether deferred work is pending, so a
 * ring with nothing queued and nothing pending costs no system call. Newer kernels run only a
 * small batch of deferred receives in an enter that asks to wait for none; when the flag is
 * still up afterwards, a second enter asks for many completions with a zero timeout, which lets
 * the kernel run them all and returns at once. */
#define SWEEP_MAX 4096
API void net_sweep(int32_t ri) {
  ring_t *r = &rings[ri];
  unsigned pending = r->sq_tail - __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
  unsigned busy = IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW;
  if (pending == 0 && !(__atomic_load_n(r->sq_kflags, __ATOMIC_RELAXED) & busy)) return;
  net_enter(ri, 0, 0);
  if (__atomic_load_n(r->sq_kflags, __ATOMIC_RELAXED) & IORING_SQ_TASKRUN) net_enter(ri, SWEEP_MAX, 0);
}

/* Copies the ring's completions to `out` ({ u64 user_data, i32 res, u32 flags }, the kernel's
 * own layout, room for CQ_ENTRIES) and returns how many. */
API int64_t net_reap(int32_t ri, struct io_uring_cqe *out) {
  ring_t *r = &rings[ri];
  unsigned head = *r->cq_khead;
  unsigned tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
  unsigned n = tail - head;
  if (n == 0) return 0;
  unsigned at = head & r->cq_mask;
  unsigned first = r->cq_mask + 1 - at;
  if (first > n) first = n;
  memcpy(out, &r->cqes[at], (size_t)first * sizeof(struct io_uring_cqe));
  memcpy(out + first, r->cqes, (size_t)(n - first) * sizeof(struct io_uring_cqe));
  __atomic_store_n(r->cq_khead, tail, __ATOMIC_RELEASE);
  return n;
}

/* ---- operations ---- */

API void net_accept(int32_t ri, int32_t listener, uint64_t ud) {
  struct io_uring_sqe *e = sqe_get(&rings[ri], 1);
  e->opcode = IORING_OP_ACCEPT;
  e->fd = listeners[listener];
  e->file_index = IORING_FILE_INDEX_ALLOC;
  e->user_data = ud;
}

/* One multishot receive: a completion per message, each with a buffer picked when data came. */
API void net_recv(int32_t ri, int32_t slot, uint64_t ud) {
  struct io_uring_sqe *e = sqe_get(&rings[ri], 1);
  e->opcode = IORING_OP_RECV;
  e->fd = slot;
  e->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  e->ioprio = IORING_RECV_MULTISHOT;
  e->buf_group = 0;
  e->user_data = ud;
}

static void send_wire(ring_t *r, int32_t slot, uint64_t ud) {
  cslot *s = &r->slots[slot];
  struct io_uring_sqe *e = sqe_get(r, 1);
  e->opcode = IORING_OP_SEND;
  e->fd = slot;
  e->flags = IOSQE_FIXED_FILE;
  e->addr = (uint64_t)(uintptr_t)(s->wire->b + s->sent);
  e->len = s->wire->len - s->sent;
  e->msg_flags = MSG_NOSIGNAL;
  e->user_data = ud;
}

/* Starts sending the connection's pending output. */
API void net_send(int32_t ri, int32_t slot, uint64_t ud) {
  ring_t *r = &rings[ri];
  cslot *s = &r->slots[slot];
  s->wire = s->out;
  s->out = 0;
  s->sent = 0;
  send_wire(r, slot, ud);
}

/* A send completed with n bytes: 0 when the whole chunk is out (it is freed), 1 when the rest
 * was queued again. */
API int32_t net_sent(int32_t ri, int32_t slot, int32_t n, uint64_t ud) {
  ring_t *r = &rings[ri];
  cslot *s = &r->slots[slot];
  s->sent += (uint32_t)n;
  if (s->sent < s->wire->len) {
    send_wire(r, slot, ud);
    return 1;
  }
  chunk_free(s->wire);
  s->wire = 0;
  return 0;
}

/* SHUTDOWN ends the multishot receive; the CLOSE linked behind it frees the slot. */
API void net_close(int32_t ri, int32_t slot, uint64_t ud_shut, uint64_t ud_close) {
  ring_t *r = &rings[ri];
  struct io_uring_sqe *e = sqe_get(r, 2);
  e->opcode = IORING_OP_SHUTDOWN;
  e->fd = slot;
  e->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  e->len = SHUT_RDWR;
  e->user_data = ud_shut;
  e = sqe_get(r, 1);
  e->opcode = IORING_OP_CLOSE;
  e->file_index = (uint32_t)slot + 1;
  e->user_data = ud_close;
}

/* ---- bytes ---- */

/* Gives a receive buffer back to the kernel: its bytes are not read again. */
API void net_rx_drop(int32_t ri, int32_t bid) { rx_give(&rings[ri], (unsigned)bid); }

/* Appends a received message to the connection's kept input and gives its buffer back; returns
 * the kept length. */
API int64_t net_rx_keep(int32_t ri, int32_t slot, int32_t bid, int32_t len) {
  ring_t *r = &rings[ri];
  cslot *s = &r->slots[slot];
  s->kept = chunk_add(s->kept, r->rx + (size_t)bid * RX_SIZE, len);
  rx_give(r, (unsigned)bid);
  return s->kept->len;
}

API void net_keep(int32_t ri, int32_t slot, const uint8_t *p, int64_t n) {
  cslot *s = &rings[ri].slots[slot];
  s->kept = chunk_add(s->kept, p, n);
}

/* Takes the kept input away from the connection: the chunk (len at offset 0, bytes at 8), which
 * the caller frees with net_chunk_free once it is served, or 0. */
API uint64_t net_kept_take(int32_t ri, int32_t slot) {
  cslot *s = &rings[ri].slots[slot];
  chunk *c = s->kept;
  s->kept = 0;
  return (uint64_t)(uintptr_t)c;
}

API void net_chunk_free(uint64_t c) { chunk_free((chunk *)(uintptr_t)c); }

/* Appends n bytes to the connection's pending output and returns where they go: the caller
 * writes all n of them there before the output is sent or touched again. */
API uint64_t net_out_reserve(int32_t ri, int32_t slot, int64_t n) {
  cslot *s = &rings[ri].slots[slot];
  s->out = chunk_room(s->out, n);
  uint8_t *at = s->out->b + s->out->len;
  s->out->len += (uint32_t)n;
  return (uint64_t)(uintptr_t)at;
}

API int64_t net_out_len(int32_t ri, int32_t slot) {
  cslot *s = &rings[ri].slots[slot];
  return s->out ? s->out->len : 0;
}

API void net_out_cut(int32_t ri, int32_t slot, int64_t len) {
  cslot *s = &rings[ri].slots[slot];
  if (s->out && (uint32_t)len < s->out->len) s->out->len = (uint32_t)len;
}

/* The slot is closed: frees everything it holds. */
API void net_free(int32_t ri, int32_t slot) {
  cslot *s = &rings[ri].slots[slot];
  chunk_free(s->out);
  chunk_free(s->wire);
  chunk_free(s->kept);
  memset(s, 0, sizeof *s);
}

API void net_drop_output(int32_t ri, int32_t slot) {
  cslot *s = &rings[ri].slots[slot];
  chunk_free(s->out);
  chunk_free(s->wire);
  s->out = 0;
  s->wire = 0;
}

API void net_drop_kept(int32_t ri, int32_t slot) {
  cslot *s = &rings[ri].slots[slot];
  chunk_free(s->kept);
  s->kept = 0;
}

/* ---- process ---- */

/* `n` SO_REUSEPORT listeners on host:port. Each has its own accept queue (capped at
 * net.core.somaxconn), so together they hold n times as many pending connections through a
 * ramp. Sends pass MSG_NOSIGNAL, so SIGPIPE is left to the JVM. 0, or -errno. */
API int32_t net_listen(const uint8_t *host, int64_t hlen, int64_t port, int32_t n) {
  char ip[64];
  if (hlen <= 0 || hlen >= (int64_t)sizeof ip || n > MAX_LISTEN) return -EINVAL;
  memcpy(ip, host, (size_t)hlen);
  ip[hlen] = 0;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -EINVAL;
  for (int i = 0; i < n; i++) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return -errno;
    /* accepted sockets inherit it */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) return -errno;
    if (listen(fd, 65535) < 0) return -errno;
    listeners[i] = fd;
  }
  return 0;
}

/* Raises the soft descriptor limit to the hard one and returns it: the largest file table a
 * ring may register. */
API int64_t net_slot_limit(void) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 1024;
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  return rl.rlim_cur > 1048576 ? 1048576 : (int64_t)rl.rlim_cur;
}

/* The wall clock in nanoseconds since the epoch, for a token's exp. */
API int64_t net_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Locks the code of this library and of the JVM in memory: with no swap, reclaim under memory
 * pressure takes file-backed pages first, and the loop must not wait for its own code to be read
 * back (from c-iouring-pavlyhalim #245). The JIT's code is anonymous memory and is not affected.
 * Returns the bytes locked. */
static int lock_object(struct dl_phdr_info *info, size_t size, void *arg) {
  (void)size;
  const char *name = info->dlpi_name ? info->dlpi_name : "";
  if (!strstr(name, "libfeed.so") && !strstr(name, "libjvm.so")) return 0;
  long page = sysconf(_SC_PAGESIZE);
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
    uintptr_t start = (info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)(page - 1);
    uintptr_t end = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
    if (mlock((void *)start, end - start) == 0) *(int64_t *)arg += (int64_t)(end - start);
  }
  return 0;
}

API int64_t net_lock_code(void) {
  int64_t locked = 0;
  dl_iterate_phdr(lock_object, &locked);
  return locked;
}
