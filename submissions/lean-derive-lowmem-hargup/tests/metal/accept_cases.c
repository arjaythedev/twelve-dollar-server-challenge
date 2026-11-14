/* Actual transport accounting, with only the kernel boundary replaced: io_uring syscalls,
 * ring mappings, the eventfd and its epoll registration. System headers come first, so the
 * redirections apply only to calls made by the transport. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/io_uring.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
static long mock_syscall(long number, ...);
static void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
static int mock_eventfd(unsigned count, int flags) { (void)count; (void)flags; return 5000; }
static int mock_epoll_ctl(int e, int op, int fd, struct epoll_event *ev) { (void)e; (void)op; (void)fd; (void)ev; return 0; }
#define syscall mock_syscall
#define mmap mock_mmap
#define eventfd mock_eventfd
#define epoll_ctl mock_epoll_ctl
#include "out.c"
#include "http.c"
#include "uring.c"
#undef syscall
#undef mmap
#undef eventfd
#undef epoll_ctl
static void app_decode(const req_t *rq, job_t *j, u32 s) {(void)rq;(void)j;(void)s;}
static void app_batch(job_t *j, int n) {(void)j;(void)n;}

/* ---- the kernel side -------------------------------------------------------------------- */
static ring *active;              /* the stand-alone ring of the single-ring cases (fd 99) */
static int kernel_mode;           /* buffer rings: 0 correct, 1 inverted check, 2 EPERM, 3 EINVAL */
static int reg_calls;
static u32 kaccepts[MAX_RINGS];   /* accepts the kernel holds: submitted, not yet completed */
static struct io_uring_params setup_params;
static int next_fd = 1000;
static void *maps[8 * MAX_RINGS];
static int nmaps;

static ring *ring_of(int fd) {
  if (fd == 99) return active;
  for (int i = 0; i < nrings; i++) if (rings[i].fd == fd) return &rings[i];
  return 0;
}
static long mock_syscall(long number, ...) {
  va_list ap; va_start(ap, number);
  if (number == __NR_io_uring_setup) {
    unsigned entries = va_arg(ap, unsigned);
    struct io_uring_params *p = va_arg(ap, struct io_uring_params *);
    va_end(ap);
    assert((p->flags & IORING_SETUP_CQSIZE) && p->cq_entries == CQ_ENTRIES);
    p->sq_entries = entries;
    p->sq_off = (struct io_sqring_offsets){.head = 0, .tail = 4, .ring_mask = 8, .ring_entries = 12, .flags = 16, .array = 64};
    p->cq_off = (struct io_cqring_offsets){.head = 32, .tail = 36, .ring_mask = 40, .ring_entries = 44, .cqes = 64 + entries * 4};
    setup_params = *p;
    return next_fd++;
  }
  int fd = va_arg(ap, int);
  if (number == __NR_io_uring_register) {
    unsigned op = va_arg(ap, unsigned);
    struct io_uring_buf_reg *reg = va_arg(ap, void *);
    va_end(ap);
    if (op != IORING_REGISTER_PBUF_RING) { assert(op == IORING_REGISTER_FILES2 || op == IORING_REGISTER_EVENTFD); return 0; }
    reg_calls++;
    int zero = !reg->resv[0] && !reg->resv[1] && !reg->resv[2];
    if (kernel_mode == 0 ? zero : kernel_mode == 1 ? !zero : 0) return 0;
    errno = kernel_mode == 2 ? EPERM : EINVAL;
    return -1;
  }
  assert(number == __NR_io_uring_enter);
  unsigned n = va_arg(ap, unsigned);
  va_end(ap);
  ring *r = ring_of(fd);
  assert(r);
  for (unsigned i = 0; i < n; i++) {
    struct io_uring_sqe *s = &r->sqes[r->sq_array ? r->sq_array[*r->sq_khead & r->sq_mask] : *r->sq_khead & r->sq_mask];
    if (s->opcode == IORING_OP_ACCEPT) {
      assert(s->ioprio == 0 && s->file_index == IORING_FILE_INDEX_ALLOC && s->fd == listen_fd);
      if (r != active) kaccepts[r->idx]++;
    }
    (*r->sq_khead)++;
  }
  return n;
}
static void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  (void)addr; (void)prot; (void)flags;
  assert(nmaps < (int)(sizeof maps / sizeof *maps));
  u8 *m = calloc(1, len);
  assert(m);
  maps[nmaps++] = m;
  if (fd >= 0 && off == IORING_OFF_SQ_RING) {
    *(u32 *)(m + setup_params.sq_off.ring_mask) = setup_params.sq_entries - 1;
    *(u32 *)(m + setup_params.sq_off.ring_entries) = setup_params.sq_entries;
    *(u32 *)(m + setup_params.cq_off.ring_mask) = setup_params.cq_entries - 1;
  }
  return m;
}

/* ---- buffer registration and one ring's reservations ------------------------------------ */
static void registration_cases(void) {
  for (kernel_mode = 0; kernel_mode < 4; kernel_mode++) {
    struct io_uring_buf_reg reg = {0};
    reg_calls = 0;
    int rc = register_buffers(99, &reg);
    assert((rc == 0) == (kernel_mode < 2));
    assert(reg_calls == ((kernel_mode == 1 || kernel_mode == 3) ? 2 : 1));
    assert(!reg.resv[0] && !reg.resv[1] && !reg.resv[2]);   /* the retry leaves no trace */
    if (rc) assert(errno == (kernel_mode == 2 ? EPERM : EINVAL));
  }
  kernel_mode = 0;
}
static void single_ring_cases(void) {
  for (u32 nslots = 1; nslots <= 1024; nslots = nslots * 2) {
    for (u32 entries = 2; entries <= 32; entries *= 4) {
      u32 head = 0, tail = 0;
      struct io_uring_sqe sq[32] = {0};
      ring r = {.fd = 99, .sq_khead = &head, .sq_ktail = &tail, .sq_entries = entries,
                .sq_mask = entries - 1, .sqes = sq, .nslots = nslots};
      r.conns = calloc(nslots, sizeof(conn));
      active = &r;
      for (int cycle = 0; cycle < 3; cycle++) {
        while (r.nopen < nslots) {
          fill_accepts(&r);
          assert(r.accepts > 0 && r.accepts <= accept_depth(&r));
          assert(r.nopen + r.accepts <= nslots);
          u32 count = r.accepts;
          /* Complete a failed accept first: the reservation must be released. */
          struct io_uring_cqe bad = {.user_data = ud(OP_ACCEPT, 0, 0, 0), .res = -EINTR};
          handle_cqe(&r, &bad);
          assert(r.accepts == count - 1);
          fill_accepts(&r);
          count = r.accepts;
          for (u32 i = 0; i < count; i++) {
            struct io_uring_cqe e = {.user_data = ud(OP_ACCEPT, 0, 0, 0), .res = (int)r.nopen};
            handle_cqe(&r, &e);
            assert(r.nopen + r.accepts <= nslots);
          }
        }
        fill_accepts(&r);
        assert(r.accepts == 0);   /* full means no accepted-and-dropped socket */
        while (r.to_submit) ring_flush(&r);
        /* All slots close, then a second burst can reuse them. */
        for (u32 i = 0; i < nslots; i++) {
          r.conns[i].state = C_CLOSING;
          struct io_uring_cqe e = {.user_data = ud(OP_CLOSE, 0, r.conns[i].gen, i), .res = 0};
          handle_cqe(&r, &e);
        }
        assert(!r.nopen);
      }
      free(r.conns);
      active = 0;
    }
  }
}

/* ---- many rings: the real accept_balance and ring_init, driven at random ------------------ */
static u64 seed = 0x9e3779b97f4a7c15ull;
static u32 rnd(u32 n) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (u32)(seed % n); }
typedef struct { int ri; u32 slot; u16 gen; } closing;
static closing *closes;
static int ncloses;

static void reset_rings(void) {
  for (int i = 0; i < MAX_RINGS; i++) {
    for (u32 s = 0; s < rings[i].nslots; s++) conn_reset(&rings[i].conns[s]);
    free(rings[i].conns);
    memset(&rings[i], 0, sizeof rings[i]);
    kaccepts[i] = 0;
  }
  while (nmaps) free(maps[--nmaps]);
  nrings = acc_ring = 0;
  ncloses = 0;
}
static u32 total(int what) {   /* 0: open, 1: reserved by accepts, 2: kernel-held accepts */
  u32 n = 0;
  for (int i = 0; i < nrings; i++) n += what == 0 ? rings[i].nopen : what == 1 ? rings[i].accepts : kaccepts[i];
  return n;
}
static void flush_all(void) { for (int i = 0; i < nrings; i++) ring_flush(&rings[i]); }
/* The kernel completes one held accept on ring ri; a success takes any free table slot. */
static void complete_accept(int ri, int fail) {
  ring *r = &rings[ri];
  assert(kaccepts[ri]);
  kaccepts[ri]--;
  int res = fail ? -EINTR : -1;
  if (!fail) {
    for (u32 s = 0; s < r->nslots && res < 0; s++) if (r->conns[s].state == C_FREE) res = (int)s;
    assert(res >= 0);   /* a reserved slot exists: the kernel never drops an accepted socket */
  }
  struct io_uring_cqe e = {.user_data = ud(OP_ACCEPT, (u8)ri, 0, 0), .res = res};
  handle_cqe(r, &e);
}
static int random_accepting(void) {
  if (!total(2)) return -1;
  int i = (int)rnd((u32)nrings);
  while (!kaccepts[i]) i = (i + 1) % nrings;
  return i;
}
static void start_close(int ri, u32 slot) {
  ring *r = &rings[ri];
  conn_close(r, slot);
  assert(r->conns[slot].state == C_CLOSING);
  closes[ncloses++] = (closing){ri, slot, r->conns[slot].gen};
}
static void start_random_close(void) {
  if (!total(0)) return;
  int ri = (int)rnd((u32)nrings);
  while (!rings[ri].nopen) ri = (ri + 1) % nrings;
  ring *r = &rings[ri];
  u32 s = rnd(r->nslots);
  for (u32 k = 0; k < r->nslots; k++, s = (s + 1) % r->nslots)
    if (r->conns[s].state == C_OPEN) { start_close(ri, s); return; }
}
static void finish_close(int k) {
  closing c = closes[k];
  closes[k] = closes[--ncloses];
  struct io_uring_cqe e = {.user_data = ud(OP_CLOSE, (u8)c.ri, c.gen, c.slot), .res = 0};
  handle_cqe(&rings[c.ri], &e);
}
/* One loop iteration's accounting step, checked against the transport's invariants. */
static void balance_and_check(int full_scan) {
  int before = nrings;
  u32 used[MAX_RINGS];
  for (int i = 0; i < before; i++) used[i] = rings[i].nopen + rings[i].accepts;
  accept_balance();
  u32 margin = accept_depth(&rings[0]) * 2;
  if (nrings != before) {
    /* A ring opens only when every existing ring is within its margin. */
    assert(nrings == before + 1);
    for (int i = 0; i < before; i++) assert(used[i] + margin >= rings[i].nslots);
  }
  int room = nrings < MAX_RINGS;
  for (int i = 0; i < nrings; i++) {
    ring *r = &rings[i];
    assert(r->nopen + r->accepts <= r->nslots);
    assert(r->accepts >= kaccepts[i]);
    if (r->nopen < r->nslots) room = 1;
    if (full_scan) {
      u32 held = 0;
      for (u32 s = 0; s < r->nslots; s++) held += r->conns[s].state != C_FREE;
      assert(held == r->nopen);
    }
  }
  /* Liveness: while any slot is free somewhere, some accept is outstanding. */
  if (room) assert(total(1) > 0);
}
static void ramp(u32 cap, int ring_target, int full_scan) {
  for (int guard = 0; total(0) < cap && nrings < ring_target; guard++) {
    assert(guard < 1000000);
    flush_all();
    u32 burst = 1 + rnd(2 * accept_depth(&rings[0]));
    for (u32 k = 0; k < burst; k++) {
      int ri = random_accepting();
      if (ri < 0) break;
      complete_accept(ri, rnd(32) == 0);
    }
    balance_and_check(full_scan);
  }
}
static void multi_ring_cases(void) {
  const u32 sizes[] = {1, 3, 16, 64, 300};
  closes = calloc(MAX_RINGS * 300, sizeof *closes);
  for (unsigned z = 0; z < sizeof sizes / sizeof *sizes; z++) {
    u32 nslots = sizes[z], cap = nslots * MAX_RINGS;
    int full_scan = nslots <= 64;
    kernel_mode = (int)(z & 1);   /* alternate correct and inverted buffer-ring kernels */
    reset_rings();
    ring_slots = nslots;
    nrings = 1;
    ring_init(&rings[0], 0, nslots, 0, 0);
    fill_accepts(&rings[0]);
    /* Ramp to three rings, then empty the first: it must be refilled before a ring opens. */
    ramp(cap, 3, full_scan);
    for (u32 s = 0; s < rings[0].nslots; s++) if (rings[0].conns[s].state == C_OPEN) start_close(0, s);
    while (ncloses) finish_close(ncloses - 1);
    /* Ramp on: rings open on demand, and every one of the 16 tables fills completely. */
    ramp(cap, MAX_RINGS + 1, full_scan);
    assert(nrings == MAX_RINGS && total(0) == cap);
    flush_all();
    balance_and_check(1);
    assert(total(1) == 0 && total(2) == 0);   /* full tables leave new sockets on the listener */
    /* One slot frees in a ring that is not accepting: that ring takes the next socket. */
    int ri = (acc_ring + 1) % MAX_RINGS;
    start_close(ri, rnd(nslots));
    balance_and_check(1);
    assert(total(1) == 0);   /* still held until the close completes */
    finish_close(0);
    balance_and_check(1);
    assert(acc_ring == ri && rings[ri].accepts == 1 && total(1) == 1);
    flush_all();
    complete_accept(ri, 0);
    balance_and_check(1);
    assert(total(0) == cap && total(1) == 0);
    /* Churn: accepts, failures, closes and close completions interleave. */
    for (int step = 0; step < 20000; step++) {
      flush_all();
      u32 events = 1 + rnd(8);
      for (u32 k = 0; k < events; k++) {
        u32 pick = rnd(10);
        int a = random_accepting();
        if (pick < 4 && a >= 0) complete_accept(a, rnd(16) == 0);
        else if (pick < 7) start_random_close();
        else if (ncloses) finish_close((int)rnd((u32)ncloses));
      }
      balance_and_check(full_scan || step % 97 == 0);
    }
    /* Drain everything, then refill: every slot of every table is reused. */
    for (int i = 0; i < nrings; i++)
      for (u32 s = 0; s < rings[i].nslots; s++) if (rings[i].conns[s].state == C_OPEN) start_close(i, s);
    while (ncloses) finish_close(ncloses - 1);
    balance_and_check(1);
    assert(total(0) == 0);
    ramp(cap, MAX_RINGS + 1, full_scan);
    assert(nrings == MAX_RINGS && total(0) == cap);
  }
  reset_rings();
  free(closes);
  kernel_mode = 0;
}

int main(void) {
  registration_cases();
  single_ring_cases();
  multi_ring_cases();
  puts("accept: bounded reservations, full tables, errors, tiny SQs, slot reuse; "
       "16-ring balance: on-demand opening, full-table backpressure, noncurrent-ring reuse, churn; "
       "buffer registration fallback passed");
}
