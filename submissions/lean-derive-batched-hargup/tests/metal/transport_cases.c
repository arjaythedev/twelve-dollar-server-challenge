/* Unit cases for runtime/uring.c, compiled from the real source (not a copy) with a fake
 * in-memory ring instead of the kernel, under ASan/UBSan:
 *   cc -O1 -g -fsanitize=address,undefined -Iruntime -Ibuild tests/metal/transport_cases.c
 * Covers the submission backlog at every fill level (including the boundary where the backlog
 * grows), shutdown -> close ordering, stale-generation completions, the queue of ended receives
 * (re-arm budget per pass, no duplicates, slot reuse), and the pending list. */
#define _GNU_SOURCE
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include "out.c"
#include "http.c"
#include "uring.c"

static int app_decoded;
static void app_decode(const req_t *rq, job_t *j) { (void)rq; app_decoded++; j->kind = 0; j->resp = obuf_static((const u8 *)"x", 1); }
static void app_batch(job_t *js, int n) { (void)js; (void)n; }

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u32 khead, ktail, kflags, karray[64];
static struct io_uring_sqe ksqes[64];

static ring *fake_ring_n(u32 sq_entries, u32 nslots) {
  ring *r = &rings[0];
  free(r->bl); free(r->rq); free(r->conns); free(r->bufs); free(r->br);
  memset(r, 0, sizeof *r);
  memset(ksqes, 0, sizeof ksqes);
  khead = ktail = kflags = 0;
  r->sq_khead = &khead; r->sq_ktail = &ktail; r->sq_kflags = &kflags; r->sq_array = karray;
  r->sq_entries = sq_entries; r->sq_mask = sq_entries - 1; r->sqes = ksqes;
  r->nslots = nslots;
  r->conns = calloc(r->nslots, sizeof(conn));
  r->bufs = calloc(NBUF, BUFSZ);
  r->br = calloc(NBUF, sizeof(struct io_uring_buf));
  nrings = 1;
  return r;
}
static ring *fake_ring(u32 sq_entries) { return fake_ring_n(sq_entries, 64); }
static u32 written(ring *r) { return r->sq_tail + r->bl_tail; }   /* entries written so far, anywhere */
/* the "kernel" consumes k published entries; returns their user_data in order */
static u32 consume(ring *r, u64 *out, u32 k) {
  u32 n = 0;
  while (n < k && khead != r->sq_tail) out[n++] = ksqes[khead++ & r->sq_mask].user_data;
  return n;
}
static struct io_uring_sqe *last_sqe(ring *r) {   /* the most recently written entry, wherever it went */
  return bl_empty(r) ? &r->sqes[(r->sq_tail - 1) & r->sq_mask] : &r->bl[(r->bl_tail - 1) & (r->bl_cap - 1)];
}

static void backlog_cases(void) {
  /* every fill level of the backlog across several growths, with interleaved draining: entries
   * reach the kernel exactly once and in the order they were written */
  for (u32 pre = 0; pre < 2100; pre += 7) {
    ring *r = fake_ring(4);
    u64 next = 1, expect = 1, got[8];
    for (u32 i = 0; i < 4 + pre; i++) { struct io_uring_sqe *s = sqe_get(r); s->user_data = next++; }
    CHECK(r->bl_tail - r->bl_head == pre);
    for (int i = 0; i < 3; i++) { struct io_uring_sqe *s = sqe_get(r); s->user_data = next++; }   /* may grow */
    while (expect < next) {
      bl_drain(r);
      u32 n = consume(r, got, 1 + (u32)(expect % 4));
      for (u32 i = 0; i < n; i++) CHECK(got[i] == expect++);
      if (!n) break;
    }
    CHECK(expect == next);
    CHECK(bl_empty(r) && khead == r->sq_tail);
  }
  /* the coordinator's boundary: kernel SQ full, backlog cap 1024 holding cap - 1 */
  ring *r = fake_ring(4);
  for (u32 i = 0; i < 4 + 1023; i++) sqe_get(r)->user_data = i;
  CHECK(r->bl_cap == 1024 && r->bl_tail - r->bl_head == 1023);
  struct io_uring_sqe *a = sqe_get(r); a->user_data = 5000; a->opcode = IORING_OP_SHUTDOWN;
  struct io_uring_sqe *b = sqe_get(r); b->user_data = 5001; b->opcode = IORING_OP_CLOSE;   /* grows the backlog */
  CHECK(r->bl_cap == 2048);
  CHECK(r->bl[(r->bl_tail - 2) & 2047].user_data == 5000 && r->bl[(r->bl_tail - 2) & 2047].opcode == IORING_OP_SHUTDOWN);
  CHECK(r->bl[(r->bl_tail - 1) & 2047].user_data == 5001);
}

static void close_cases(void) {
  ring *r = fake_ring(8);
  conn *c = &r->conns[5];
  c->gen = 7; c->state = C_OPEN; c->recv = R_MULTI;
  u32 t0 = r->sq_tail;
  conn_close(r, 5);
  CHECK(c->state == C_CLOSING);
  CHECK(r->sq_tail == t0 + 1);                                   /* one entry: the shutdown only */
  struct io_uring_sqe *s = last_sqe(r);
  CHECK(s->opcode == IORING_OP_SHUTDOWN && !(s->flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS)));
  /* a close completion before the shutdown's would be an ordering violation: ignored */
  struct io_uring_cqe e = {.user_data = ud(OP_CLOSE, 0, 7, 5), .res = 0};
  handle_cqe(r, &e);
  CHECK(c->state == C_CLOSING);
  /* stale generation: dropped */
  e = (struct io_uring_cqe){.user_data = ud(OP_SHUTDOWN, 0, 6, 5), .res = 0};
  handle_cqe(r, &e);
  CHECK(c->state == C_CLOSING && r->sq_tail == t0 + 1);
  /* the shutdown completes (even with an error): now, and only now, the close is submitted */
  e = (struct io_uring_cqe){.user_data = ud(OP_SHUTDOWN, 0, 7, 5), .res = -ENOTCONN};
  handle_cqe(r, &e);
  CHECK(c->state == C_SHUT && r->sq_tail == t0 + 2);
  s = last_sqe(r);
  CHECK(s->opcode == IORING_OP_CLOSE && s->file_index == 6 && s->user_data == ud(OP_CLOSE, 0, 7, 5));
  e = (struct io_uring_cqe){.user_data = ud(OP_CLOSE, 0, 7, 5), .res = 0};
  handle_cqe(r, &e);
  CHECK(c->state == C_FREE && c->recv == R_IDLE && !c->pending);
  /* a connection with a send in flight closes after it */
  c = &r->conns[9]; c->gen = 1; c->state = C_OPEN; c->sending = 1;
  t0 = r->sq_tail;
  conn_close(r, 9);
  CHECK(c->state == C_OPEN && c->close_after && r->sq_tail == t0);
}

static void accept_on(ring *r, u32 slot) {
  struct io_uring_cqe e = {.user_data = ud(OP_ACCEPT, 0, 0, 0), .res = (int)slot, .flags = IORING_CQE_F_MORE};
  handle_cqe(r, &e);
}
static struct io_uring_cqe recv_cqe(u8 op, u16 gen, u32 slot, int res, u32 flags, u16 bid) {
  return (struct io_uring_cqe){.user_data = ud(op, 0, gen, slot), .res = res, .flags = flags | (u32)bid << IORING_CQE_BUFFER_SHIFT};
}

static void receive_cases(void) {
  ring *r = fake_ring(64);
  accept_on(r, 3);
  conn *c = &r->conns[3];
  u16 g = c->gen;
  CHECK(c->state == C_OPEN && c->recv == R_MULTI);
  /* bytes with F_MORE: delivered, the receive stays armed */
  memcpy(r->bufs + 2 * BUFSZ, "GET /x HTTP/1.1\r\n\r\n", 19);
  struct io_uring_cqe e = recv_cqe(OP_RECV, g, 3, 19, IORING_CQE_F_BUFFER | IORING_CQE_F_MORE, 2);
  handle_cqe(r, &e);
  CHECK(c->recv == R_MULTI && app_decoded == 1 && r->rq_head == r->rq_tail);
  /* ENOBUFS: queued once */
  e = recv_cqe(OP_RECV, g, 3, -ENOBUFS, 0, 0);
  handle_cqe(r, &e);
  CHECK(c->recv == R_QUEUED && r->rq_tail - r->rq_head == 1);
  /* a second ending cannot belong to a queued connection: dropped, no duplicate entry */
  handle_cqe(r, &e);
  CHECK(c->recv == R_QUEUED && r->rq_tail - r->rq_head == 1);
  /* a stale completion (older generation) changes nothing */
  e = recv_cqe(OP_RECV, (u16)(g - 1), 3, -ENOBUFS, 0, 0);
  handle_cqe(r, &e);
  CHECK(r->rq_tail - r->rq_head == 1);
  /* re-armed: one multishot receive of this generation */
  u32 t0 = written(r);
  rearm_queued(r);
  CHECK(c->recv == R_MULTI && written(r) == t0 + 1 && r->rq_head == r->rq_tail);
  CHECK(last_sqe(r)->ioprio == IORING_RECV_MULTISHOT && last_sqe(r)->user_data == ud(OP_RECV, 0, g, 3));
  /* bytes without F_MORE: delivered, then queued */
  memcpy(r->bufs + 4 * BUFSZ, "GET /y HTTP/1.1\r\n\r\n", 19);
  e = recv_cqe(OP_RECV, g, 3, 19, IORING_CQE_F_BUFFER, 4);
  handle_cqe(r, &e);
  CHECK(app_decoded == 2 && c->recv == R_QUEUED && r->rq_tail - r->rq_head == 1);
  /* closed and the slot reused before the pass ends: the stale entry is skipped */
  c->state = C_FREE;
  accept_on(r, 3);
  CHECK(c->gen == (u16)(g + 1) && c->recv == R_MULTI);
  t0 = written(r);
  rearm_queued(r);
  CHECK(written(r) == t0 && r->rq_head == r->rq_tail);   /* the new connection keeps its single receive */
  /* EOF closes (shutdown first) */
  e = recv_cqe(OP_RECV, c->gen, 3, 0, 0, 0);
  handle_cqe(r, &e);
  CHECK(c->state == C_CLOSING && last_sqe(r)->opcode == IORING_OP_SHUTDOWN);
  njobs = 0;
  /* the per-pass budget: NBUF + 5 ended receives are re-armed NBUF now and 5 in the next pass,
   * oldest first */
  r = fake_ring_n(64, NBUF + 8);
  for (u32 i = 0; i < NBUF + 5; i++) { accept_on(r, i); struct io_uring_cqe x = recv_cqe(OP_RECV, r->conns[i].gen, i, -ENOBUFS, 0, 0); handle_cqe(r, &x); }
  t0 = written(r);
  rearm_queued(r);
  CHECK(written(r) == t0 + NBUF && r->rq_tail - r->rq_head == 5);
  CHECK(r->conns[NBUF - 1].recv == R_MULTI && r->conns[NBUF].recv == R_QUEUED);
  rearm_queued(r);
  CHECK(written(r) == t0 + NBUF + 5 && r->rq_head == r->rq_tail && r->conns[NBUF + 4].recv == R_MULTI);
}

static void pending_cases(void) {
  ring *r = fake_ring(64);
  /* held connection closed and slot reused before release: the stale entry is skipped, and the
   * new connection can be held again (its pending flag was cleared by conn_reset) */
  accept_on(r, 1);
  conn *c = &r->conns[1];
  conn_hold(r, 1);
  CHECK(c->pending && npend == 1);
  conn_reset(c); c->state = C_FREE;
  accept_on(r, 1);
  CHECK(!c->pending && !c->hold);
  conn_hold(r, 1);
  CHECK(npend == 2);
  release_holds();
  CHECK(!c->pending && !c->hold && npend == 0);
  /* the list grows past its first allocation */
  for (u32 i = 0; i < 10000; i++) pend_push((u64)i);
  CHECK(npend == 10000 && pend_cap >= 10000);
  npend = 0;
}

int main(void) {
  backlog_cases();
  close_cases();
  receive_cases();
  pending_cases();
  printf("transport cases: %s (%d failures)\n", fails ? "FAIL" : "pass", fails);
  return fails != 0;
}
