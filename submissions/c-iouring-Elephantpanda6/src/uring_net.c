#define _GNU_SOURCE
#include "uring_net.h"
#include "uring_rings.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif
#ifndef IORING_FILE_INDEX_ALLOC
#define IORING_FILE_INDEX_ALLOC (~0U)
#endif
#ifndef IORING_ASYNC_CANCEL_USERDATA
#define IORING_ASYNC_CANCEL_USERDATA (1U << 4)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif

#define UD_OP_MASK 0xfu
#define UD_RING_MASK 0xfu
#define UD_SLOT_MASK ((1u << 17) - 1u)
#define UD_OP_SHIFT 0u
#define UD_RING_SHIFT 4u
#define UD_SLOT_SHIFT 8u
#define UD_GEN_SHIFT 25u

#define CONN_ALIVE 0x0001u
#define CONN_READY 0x0002u
#define CONN_EOF 0x0004u
#define CONN_CLOSING 0x0008u
#define CONN_CLOSE_AFTER 0x0010u
#define CONN_CLOSE_SUBMITTED 0x0020u
#define CONN_DRAIN_QUEUED 0x0040u
#define CONN_DRAIN_INFLIGHT 0x0080u
#define CONN_SEND_INFLIGHT 0x0100u
#define CONN_CLOSE_DROP 0x0200u
#define CONN_CLOSE_RETRYING 0x0400u

#define CONN_NO_BID 0xffffu
#define ACCEPT_HEADROOM_MAX 1024u

enum net_op {
  OP_ACCEPT = 1,
  OP_RECV = 2,
  OP_SEND = 3,
  OP_DRAIN = 4,
  OP_SHUTDOWN = 5,
  OP_CLOSE = 6,
  OP_CANCEL = 7,
  OP_ORPHAN_CLOSE = 8
};

struct send_req {
  struct send_req *next;
  struct send_arena *arena;
  const uint8_t *data;
  size_t len;
  uint8_t is_static;
  uint8_t close_after;
};

struct conn {
  struct send_req *send_head;
  uint8_t *rx_buf;
  uint8_t *fast_data;

  uint32_t gen;
  uint32_t flags;
  uint32_t rx_len;
  uint32_t rx_cap;
  uint32_t fast_len;
  uint32_t send_len;
  uint32_t send_off;
  uint32_t next_ready;
  uint32_t next_drain;

  uint16_t fast_bid;
  uint16_t pad;
};

_Static_assert(sizeof(struct conn) <= 64, "conn must stay compact");

struct net_ctx {
  int listeners[MAX_LISTENERS];
  int n_listeners;

  uint32_t nofile;
  uint32_t table_size;
  uint32_t n_rings_target;
  uint32_t n_rings;
  int accept_ring;
  int ring_file_offset[MAX_RINGS];

  struct uring_ring rings[MAX_RINGS];
  struct conn *conns;
  size_t conn_count;

  uint32_t ready_head;
  uint32_t ready_tail;
  uint32_t drain_head[MAX_RINGS];
  uint32_t drain_tail[MAX_RINGS];
  uint32_t live_conns;

  struct send_arena *arena_pool;
  struct send_arena *batch_head;
  struct send_arena *batch_cur;
};

static void conn_start_close(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                             struct conn *c);
static void conn_kick_send(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                           struct conn *c);
static int conn_arm_recv(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                         struct conn *c);
static void conn_queue_drain(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                             struct conn *c);

static inline uint64_t net_tag(uint32_t op, uint32_t ridx, uint32_t slot,
                               uint32_t gen) {
  return (uint64_t)op | ((uint64_t)(ridx & UD_RING_MASK) << UD_RING_SHIFT) |
         ((uint64_t)(slot & UD_SLOT_MASK) << UD_SLOT_SHIFT) |
         ((uint64_t)gen << UD_GEN_SHIFT);
}

static inline uint32_t net_tag_op(uint64_t ud) {
  return (uint32_t)(ud & UD_OP_MASK);
}
static inline uint32_t net_tag_slot(uint64_t ud) {
  return (uint32_t)((ud >> UD_SLOT_SHIFT) & UD_SLOT_MASK);
}
static inline uint32_t net_tag_gen(uint64_t ud) {
  return (uint32_t)(ud >> UD_GEN_SHIFT);
}

static struct conn *conn_by_id(struct net_ctx *ctx, uint32_t conn_id) {
  if (!ctx || !ctx->table_size || (size_t)conn_id >= ctx->conn_count)
    return NULL;
  return &ctx->conns[conn_id];
}

static struct conn *conn_get(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                             uint32_t gen) {
  if (!ctx || ridx >= ctx->n_rings_target || slot >= ctx->table_size)
    return NULL;
  {
    struct conn *c =
        &ctx->conns[(size_t)ridx * (size_t)ctx->table_size + (size_t)slot];
    if (!(c->flags & CONN_ALIVE) || c->gen != gen)
      return NULL;
    return c;
  }
}

static inline uint32_t conn_id_of(const struct net_ctx *ctx, uint32_t ridx,
                                  uint32_t slot) {
  return ridx * ctx->table_size + slot;
}

static inline int conn_has_data(const struct conn *c) {
  return c->fast_bid != CONN_NO_BID ? c->fast_len != 0 : c->rx_len != 0;
}

static uint8_t *pbuf_data(struct uring_ring *r, uint16_t bid) {
  if (!r->pbuf_mem || bid >= PBUF_COUNT)
    return NULL;
  return r->pbuf_mem + (size_t)bid * (size_t)PBUF_SIZE;
}

static int conn_rx_reserve(struct conn *c, size_t need) {
  size_t ncap;
  uint8_t *p;
  if (need <= c->rx_cap)
    return 0;
  if (need > 0xffffffffu)
    return -1;
  ncap = c->rx_cap ? c->rx_cap : 4096u;
  while (ncap < need)
    ncap <<= 1;
  if (ncap > 0xffffffffu)
    return -1;
  p = (uint8_t *)realloc(c->rx_buf, ncap);
  if (!p)
    return -1;
  c->rx_buf = p;
  c->rx_cap = (uint32_t)ncap;
  return 0;
}

static void conn_release_fast(struct net_ctx *ctx, uint32_t ridx,
                              struct conn *c, int keep_leftover) {
  struct uring_ring *r = &ctx->rings[ridx];
  if (c->fast_bid == CONN_NO_BID)
    return;

  if (keep_leftover && (c->flags & CONN_ALIVE) && !(c->flags & CONN_CLOSING) &&
      c->fast_len) {
    if (conn_rx_reserve(c, (size_t)c->rx_len + (size_t)c->fast_len) == 0) {
      memcpy(c->rx_buf + c->rx_len, c->fast_data, c->fast_len);
      c->rx_len += c->fast_len;
    }
  }
  ur_recycle_pbuf(r, c->fast_bid);
  c->fast_bid = CONN_NO_BID;
  c->fast_data = NULL;
  c->fast_len = 0;
}

static void conn_mark_ready(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                            struct conn *c) {
  uint32_t id, val;
  if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return;
  if (c->flags & CONN_READY)
    return;

  id = conn_id_of(ctx, ridx, slot);
  val = id + 1u;
  c->flags |= CONN_READY;
  c->next_ready = 0;
  if (ctx->ready_tail) {
    struct conn *tail = conn_by_id(ctx, ctx->ready_tail - 1u);
    if (tail)
      tail->next_ready = val;
    else
      ctx->ready_head = val;
  } else {
    ctx->ready_head = val;
  }
  ctx->ready_tail = val;
}

static void conn_unlink_ready(struct net_ctx *ctx, uint32_t id,
                              struct conn *c) {
  uint32_t prev, cur;
  if (!(c->flags & CONN_READY))
    return;

  prev = 0;
  cur = ctx->ready_head;
  while (cur) {
    struct conn *cc = conn_by_id(ctx, cur - 1u);
    uint32_t next;
    if (!cc)
      break;
    next = cc->next_ready;
    if (cur - 1u == id) {
      if (prev) {
        struct conn *p = conn_by_id(ctx, prev - 1u);
        if (p)
          p->next_ready = next;
      } else {
        ctx->ready_head = next;
      }
      if (ctx->ready_tail == cur)
        ctx->ready_tail = prev;
      c->next_ready = 0;
      break;
    }
    prev = cur;
    cur = next;
  }
  c->flags &= ~CONN_READY;
}

static void conn_drain_push_front(struct net_ctx *ctx, uint32_t ridx,
                                  struct conn *c, uint32_t id) {
  uint32_t val = id + 1u;
  c->next_drain = ctx->drain_head[ridx];
  ctx->drain_head[ridx] = val;
  if (!ctx->drain_tail[ridx])
    ctx->drain_tail[ridx] = val;
  c->flags |= CONN_DRAIN_QUEUED;
}

static void conn_queue_drain(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                             struct conn *c) {
  uint32_t id, val;
  if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING) ||
      (c->flags & CONN_EOF))
    return;
  if (c->flags & (CONN_DRAIN_QUEUED | CONN_DRAIN_INFLIGHT))
    return;

  id = conn_id_of(ctx, ridx, slot);
  val = id + 1u;
  c->next_drain = 0;
  c->flags |= CONN_DRAIN_QUEUED;
  if (ctx->drain_tail[ridx]) {
    struct conn *tail = conn_by_id(ctx, ctx->drain_tail[ridx] - 1u);
    if (tail)
      tail->next_drain = val;
    else
      ctx->drain_head[ridx] = val;
  } else {
    ctx->drain_head[ridx] = val;
  }
  ctx->drain_tail[ridx] = val;
}

static void conn_unlink_drain(struct net_ctx *ctx, uint32_t ridx, uint32_t id,
                              struct conn *c) {
  uint32_t prev, cur;
  if (!(c->flags & CONN_DRAIN_QUEUED))
    return;

  prev = 0;
  cur = ctx->drain_head[ridx];
  while (cur) {
    struct conn *cc = conn_by_id(ctx, cur - 1u);
    uint32_t next;
    if (!cc)
      break;
    next = cc->next_drain;
    if (cur - 1u == id) {
      if (prev) {
        struct conn *p = conn_by_id(ctx, prev - 1u);
        if (p)
          p->next_drain = next;
      } else {
        ctx->drain_head[ridx] = next;
      }
      if (ctx->drain_tail[ridx] == cur)
        ctx->drain_tail[ridx] = prev;
      c->next_drain = 0;
      break;
    }
    prev = cur;
    cur = next;
  }
  c->flags &= ~CONN_DRAIN_QUEUED;
}

static void send_req_free(struct net_ctx *ctx, struct send_req *n) {
  if (!n)
    return;
  if (n->arena) {
    if (n->arena->refcnt)
      n->arena->refcnt--;
    if (n->arena->refcnt == 0)
      ur_arena_release(&ctx->arena_pool, n->arena);
  }
  free(n);
}

static void conn_drop_sends(struct net_ctx *ctx, struct conn *c) {
  struct send_req *n = c->send_head;
  c->send_head = NULL;
  c->send_len = 0;
  c->send_off = 0;
  while (n) {
    struct send_req *next = n->next;
    send_req_free(ctx, n);
    n = next;
  }
}

static struct io_uring_sqe *net_get_sqe(struct uring_ring *r) {
  struct io_uring_sqe *sqe = ur_get_sqe(r);
  if (sqe)
    return sqe;
  (void)ur_submit_wait(r, 0, 0);
  return ur_get_sqe(r);
}

static struct io_uring_sqe *net_zero_sqe(struct uring_ring *r) {
  struct io_uring_sqe *sqe = net_get_sqe(r);
  if (sqe)
    memset(sqe, 0, sizeof(*sqe));
  return sqe;
}

static long net_uring_register(int fd, unsigned int opcode, void *arg,
                               unsigned int nr_args) {
#if defined(__NR_io_uring_register)
  return syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
#elif defined(SYS_io_uring_register)
  return syscall(SYS_io_uring_register, fd, opcode, arg, nr_args);
#else
  (void)fd;
  (void)opcode;
  (void)arg;
  (void)nr_args;
  errno = ENOSYS;
  return -1;
#endif
}

static void ring_register_listener_reserve(struct net_ctx *ctx,
                                           uint32_t ridx) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_files_update upd;
  uint32_t fd;
  long ret;

  ctx->ring_file_offset[ridx] = 0;
  if (ctx->n_listeners <= 0 || !r->active)
    return;

  fd = (uint32_t)ctx->listeners[0];
  memset(&upd, 0, sizeof(upd));
  upd.offset = 0;
  upd.resv = 0;
  upd.fds = (uint64_t)(uintptr_t)&fd;
  ret = net_uring_register(r->ring_fd, IORING_REGISTER_FILES_UPDATE, &upd, 1);
  if (ret == 0)
    ctx->ring_file_offset[ridx] = 1;
}

static inline uint32_t conn_fd_index(const struct net_ctx *ctx, uint32_t ridx,
                                     uint32_t slot) {
  return slot + (uint32_t)ctx->ring_file_offset[ridx];
}

static int accept_arm_one(struct net_ctx *ctx, uint32_t ridx,
                          uint32_t listener_idx) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_sqe *sqe;

  if (!r->active || listener_idx >= (uint32_t)ctx->n_listeners)
    return -1;
  sqe = net_zero_sqe(r);
  if (!sqe)
    return -1;

  sqe->opcode = IORING_OP_ACCEPT;
  sqe->fd = ctx->listeners[listener_idx];
  sqe->addr = 0;
  sqe->addr2 = 0;
  sqe->file_index = IORING_FILE_INDEX_ALLOC;
  sqe->ioprio = IORING_ACCEPT_MULTISHOT;
  sqe->user_data = net_tag(OP_ACCEPT, ridx, listener_idx, 0);
  return 0;
}

static int accept_arm_all(struct net_ctx *ctx, uint32_t ridx) {
  int armed = 0;
  int i;
  for (i = 0; i < ctx->n_listeners; i++) {
    if (accept_arm_one(ctx, ridx, (uint32_t)i) != 0)
      break;
    armed++;
  }
  return armed;
}

static void accept_cancel_all(struct net_ctx *ctx, uint32_t ridx) {
  struct uring_ring *r = &ctx->rings[ridx];
  int i;
  if (!r->active)
    return;

  for (i = 0; i < ctx->n_listeners; i++) {
    struct io_uring_sqe *sqe = net_zero_sqe(r);
    if (!sqe)
      return;
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = net_tag(OP_ACCEPT, ridx, (uint32_t)i, 0);
    sqe->cancel_flags = IORING_ASYNC_CANCEL_USERDATA;
    sqe->user_data = net_tag(OP_CANCEL, ridx, (uint32_t)i, 0);
  }
}

static void accept_maybe_rotate(struct net_ctx *ctx) {
  uint32_t cur = (uint32_t)ctx->accept_ring;
  uint32_t cap, head, next;
  struct uring_ring *r;

  if (cur >= ctx->n_rings)
    return;
  r = &ctx->rings[cur];
  if (!r->active || r->table_size <= (uint32_t)ctx->ring_file_offset[cur])
    return;

  cap = r->table_size - (uint32_t)ctx->ring_file_offset[cur];
  head = cap / 8u;
  if (head > ACCEPT_HEADROOM_MAX)
    head = ACCEPT_HEADROOM_MAX;
  if (head == 0)
    head = 1;
  if (r->active_conns + head < cap)
    return;

  next = cur + 1u;
  if (next >= ctx->n_rings_target)
    return;

  if (!ctx->rings[next].active) {
    if (ur_ring_init(&ctx->rings[next], ctx->table_size) != 0)
      return;
    ctx->rings[next].active = 1;
    if (ctx->n_rings <= next)
      ctx->n_rings = next + 1u;
    ring_register_listener_reserve(ctx, next);
  }

  (void)accept_arm_all(ctx, next);
  ctx->accept_ring = (int)next;
  accept_cancel_all(ctx, cur);
}

static void orphan_close(struct net_ctx *ctx, uint32_t ridx, uint32_t fd_index) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_sqe *sqe;

  if (!r->active || fd_index == 0)
    return;
  sqe = net_zero_sqe(r);
  if (!sqe)
    return;
  sqe->opcode = IORING_OP_CLOSE;
  sqe->fd = 0;
  sqe->file_index = fd_index;
  sqe->user_data = net_tag(OP_ORPHAN_CLOSE, ridx, 0, 0);
}

static int conn_submit_close_pair(struct net_ctx *ctx, uint32_t ridx,
                                  uint32_t slot, struct conn *c) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_sqe *shut, *close_sqe;
  uint32_t fd_index = conn_fd_index(ctx, ridx, slot);
  uint64_t tag = net_tag(OP_CLOSE, ridx, slot, c->gen);

  if (r->sq_pending + 2u > RING_SQ_ENTRIES)
    (void)ur_submit_wait(r, 0, 0);
  if (r->sq_pending + 2u > RING_SQ_ENTRIES)
    return -1;

  shut = ur_get_sqe(r);
  if (!shut)
    return -1;
  close_sqe = ur_get_sqe(r);
  if (!close_sqe) {
    memset(shut, 0, sizeof(*shut));
    shut->opcode = IORING_OP_SHUTDOWN;
    shut->flags = IOSQE_FIXED_FILE;
    shut->fd = (int)fd_index;
    shut->len = SHUT_WR;
    shut->user_data = net_tag(OP_SHUTDOWN, ridx, slot, c->gen);
    return -1;
  }

  memset(shut, 0, sizeof(*shut));
  shut->opcode = IORING_OP_SHUTDOWN;
  shut->flags = IOSQE_FIXED_FILE | IOSQE_IO_LINK;
  shut->fd = (int)fd_index;
  shut->len = SHUT_WR;
  shut->user_data = net_tag(OP_SHUTDOWN, ridx, slot, c->gen);

  memset(close_sqe, 0, sizeof(*close_sqe));
  close_sqe->opcode = IORING_OP_CLOSE;
  close_sqe->fd = 0;
  close_sqe->file_index = fd_index;
  close_sqe->user_data = tag;
  return 0;
}

static int conn_submit_close_only(struct net_ctx *ctx, uint32_t ridx,
                                  uint32_t slot, struct conn *c) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_sqe *sqe = net_zero_sqe(r);
  if (!sqe)
    return -1;
  sqe->opcode = IORING_OP_CLOSE;
  sqe->fd = 0;
  sqe->file_index = conn_fd_index(ctx, ridx, slot);
  sqe->user_data = net_tag(OP_CLOSE, ridx, slot, c->gen);
  return 0;
}

static void conn_start_close(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                             struct conn *c) {
  uint32_t id;

  if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSE_SUBMITTED))
    return;
  if (c->flags & CONN_SEND_INFLIGHT) {
    c->flags |= CONN_CLOSE_AFTER;
    return;
  }
  if (c->flags & CONN_CLOSE_DROP)
    conn_drop_sends(ctx, c);
  if (c->send_head) {
    c->flags |= CONN_CLOSE_AFTER;
    return;
  }

  if (c->fast_bid != CONN_NO_BID)
    conn_release_fast(ctx, ridx, c, 0);

  id = conn_id_of(ctx, ridx, slot);
  conn_unlink_ready(ctx, id, c);
  conn_unlink_drain(ctx, ridx, id, c);
  c->flags |= CONN_CLOSING;

  if (conn_submit_close_pair(ctx, ridx, slot, c) == 0) {
    c->flags |= CONN_CLOSE_SUBMITTED;
  } else {

    struct uring_ring *r = &ctx->rings[ridx];
    (void)ur_submit_wait(r, 0, 0);
    if (conn_submit_close_pair(ctx, ridx, slot, c) == 0)
      c->flags |= CONN_CLOSE_SUBMITTED;
  }
}

static int conn_submit_send_sqe(struct net_ctx *ctx, uint32_t ridx,
                                uint32_t slot, struct conn *c,
                                struct send_req *n, uint32_t off) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_sqe *sqe = net_zero_sqe(r);
  if (!sqe)
    return -1;
  sqe->opcode = IORING_OP_SEND;
  sqe->flags = IOSQE_FIXED_FILE;
  sqe->fd = (int)conn_fd_index(ctx, ridx, slot);
  sqe->addr = (uint64_t)(uintptr_t)(n->data + off);
  sqe->len = (uint32_t)(n->len - (size_t)off);
  sqe->msg_flags = MSG_NOSIGNAL;
  sqe->user_data = net_tag(OP_SEND, ridx, slot, c->gen);
  return 0;
}

static void conn_kick_send(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                           struct conn *c) {
  while (!(c->flags & CONN_SEND_INFLIGHT) && c->send_head) {
    struct send_req *n = c->send_head;
    if (n->len == 0) {
      c->send_head = n->next;
      send_req_free(ctx, n);
      continue;
    }
    if (conn_submit_send_sqe(ctx, ridx, slot, c, n, 0) != 0) {

      return;
    }
    c->flags |= CONN_SEND_INFLIGHT;
    c->send_len = (uint32_t)n->len;
    c->send_off = 0;
    return;
  }

  if (!c->send_head && (c->flags & (CONN_CLOSE_AFTER | CONN_CLOSING)))
    conn_start_close(ctx, ridx, slot, c);
}

static struct io_uring_sqe *prep_recv(struct net_ctx *ctx, uint32_t ridx,
                                      uint32_t slot, struct conn *c,
                                      uint32_t op, int multishot) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct io_uring_sqe *sqe = net_zero_sqe(r);
  if (!sqe)
    return NULL;

  sqe->opcode = IORING_OP_RECV;
  sqe->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  sqe->fd = (int)conn_fd_index(ctx, ridx, slot);
  sqe->addr = 0;
  sqe->len = 0;
  sqe->buf_group = PBUF_BGID;
  if (multishot)
    sqe->ioprio = IORING_RECV_MULTISHOT;
  else
    sqe->msg_flags = MSG_DONTWAIT;
  sqe->user_data = net_tag(op, ridx, slot, c->gen);
  return sqe;
}

static int conn_arm_recv(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                         struct conn *c) {
  if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING) ||
      (c->flags & CONN_EOF))
    return 0;
  if (!prep_recv(ctx, ridx, slot, c, OP_RECV, 1))
    return -1;
  return 0;
}

static void conn_rearm_after_drain(struct net_ctx *ctx, uint32_t ridx,
                                   uint32_t slot, struct conn *c) {
  c->flags &= ~CONN_DRAIN_INFLIGHT;
  if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING) ||
      (c->flags & CONN_EOF))
    return;
  if (conn_arm_recv(ctx, ridx, slot, c) != 0)
    conn_queue_drain(ctx, ridx, slot, c);
}

static void conn_deliver(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                         struct conn *c, uint8_t *data, size_t len,
                         uint16_t bid) {
  if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING)) {
    ur_recycle_pbuf(&ctx->rings[ridx], bid);
    return;
  }

  if (c->fast_bid != CONN_NO_BID)
    conn_release_fast(ctx, ridx, c, 1);

  if (c->rx_len) {
    if (conn_rx_reserve(c, (size_t)c->rx_len + len) != 0) {
      ur_recycle_pbuf(&ctx->rings[ridx], bid);
      return;
    }
    memcpy(c->rx_buf + c->rx_len, data, len);
    c->rx_len += (uint32_t)len;
    ur_recycle_pbuf(&ctx->rings[ridx], bid);
  } else {

    c->fast_bid = bid;
    c->fast_data = data;
    c->fast_len = (uint32_t)len;
  }
  conn_mark_ready(ctx, ridx, slot, c);
}

static void handle_accept(struct net_ctx *ctx, uint32_t ridx, uint64_t ud,
                          const struct io_uring_cqe *cqe) {
  uint32_t listener_idx = net_tag_slot(ud);
  int32_t res = cqe->res;
  uint32_t off = (uint32_t)ctx->ring_file_offset[ridx];
  uint32_t slot, cap;
  struct uring_ring *r = &ctx->rings[ridx];
  struct conn *c;

  if (res < 0) {
    if (res == -ECANCELED)
      return;
    if ((uint32_t)ctx->accept_ring == ridx &&
        (res == -EAGAIN || res == -ECONNABORTED))
      (void)accept_arm_one(ctx, ridx, listener_idx);
    return;
  }

  if (off == 0 && res == 0) {
    ctx->ring_file_offset[ridx] = 1;
    goto maybe_rearm;
  }

  if ((uint32_t)res < off || (uint32_t)res >= r->table_size) {
    orphan_close(ctx, ridx, (uint32_t)res);
    goto maybe_rearm;
  }
  slot = (uint32_t)res - off;
  cap = r->table_size - off;
  if (slot >= cap || r->active_conns >= cap) {
    orphan_close(ctx, ridx, (uint32_t)res);
    goto maybe_rearm;
  }

  c = &ctx->conns[(size_t)ridx * (size_t)ctx->table_size + (size_t)slot];
  if (c->flags & CONN_ALIVE) {
    orphan_close(ctx, ridx, (uint32_t)res);
    goto maybe_rearm;
  }

  c->gen++;
  c->flags = CONN_ALIVE;
  c->rx_buf = NULL;
  c->rx_len = 0;
  c->rx_cap = 0;
  c->fast_bid = CONN_NO_BID;
  c->fast_data = NULL;
  c->fast_len = 0;
  c->send_head = NULL;
  c->send_len = 0;
  c->send_off = 0;
  c->next_ready = 0;
  c->next_drain = 0;

  r->active_conns++;
  ctx->live_conns++;

  if (conn_arm_recv(ctx, ridx, slot, c) != 0)
    conn_queue_drain(ctx, ridx, slot, c);

  accept_maybe_rotate(ctx);

maybe_rearm:
  if (!(cqe->flags & IORING_CQE_F_MORE) && (uint32_t)ctx->accept_ring == ridx)
    (void)accept_arm_one(ctx, ridx, listener_idx);
}

static void handle_recv(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                        uint32_t gen, const struct io_uring_cqe *cqe,
                        int has_buf, uint16_t bid) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct conn *c = conn_get(ctx, ridx, slot, gen);
  int32_t res = cqe->res;

  if (!c) {
    if (has_buf)
      ur_recycle_pbuf(r, bid);
    return;
  }
  if (c->flags & CONN_CLOSING) {
    if (has_buf)
      ur_recycle_pbuf(r, bid);
    return;
  }

  if (res > 0) {
    if (has_buf) {
      uint8_t *data = pbuf_data(r, bid);
      if (data)
        conn_deliver(ctx, ridx, slot, c, data, (size_t)res, bid);
      else
        ur_recycle_pbuf(r, bid);
    }
    if (cqe->flags & IORING_CQE_F_MORE)
      return;

    conn_queue_drain(ctx, ridx, slot, c);
    return;
  }

  if (has_buf)
    ur_recycle_pbuf(r, bid);

  if (res < 0) {
    if (res == -ENOBUFS || res == -EAGAIN)
      conn_queue_drain(ctx, ridx, slot, c);
    else if (res != -ECANCELED)
      c->flags |= CONN_EOF;
    return;
  }

  if (res == 0) {
    c->flags |= CONN_EOF;
    return;
  }
}

static void handle_drain(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                         uint32_t gen, const struct io_uring_cqe *cqe,
                         int has_buf, uint16_t bid) {
  struct uring_ring *r = &ctx->rings[ridx];
  struct conn *c = conn_get(ctx, ridx, slot, gen);
  int32_t res = cqe->res;

  if (!c) {
    if (has_buf)
      ur_recycle_pbuf(r, bid);
    return;
  }

  c->flags &= ~CONN_DRAIN_INFLIGHT;

  if (c->flags & CONN_CLOSING) {
    if (has_buf)
      ur_recycle_pbuf(r, bid);
    return;
  }

  if (res > 0) {
    if (has_buf) {
      uint8_t *data = pbuf_data(r, bid);
      if (data)
        conn_deliver(ctx, ridx, slot, c, data, (size_t)res, bid);
      else
        ur_recycle_pbuf(r, bid);
    }
    if ((uint32_t)res >= PBUF_SIZE)
      conn_queue_drain(ctx, ridx, slot, c);
    else
      conn_rearm_after_drain(ctx, ridx, slot, c);
    return;
  }

  if (has_buf)
    ur_recycle_pbuf(r, bid);

  if (res == -EAGAIN) {
    conn_rearm_after_drain(ctx, ridx, slot, c);
  } else if (res == -ENOBUFS) {
    conn_queue_drain(ctx, ridx, slot, c);
  } else if (res == -ECANCELED) {

  } else if (res == 0) {
    c->flags |= CONN_EOF;
  } else {
    c->flags |= CONN_EOF;
  }
}

static void handle_send(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                        uint32_t gen, const struct io_uring_cqe *cqe) {
  struct conn *c = conn_get(ctx, ridx, slot, gen);
  struct send_req *n;
  int32_t res = cqe->res;

  if (!c || !(c->flags & CONN_SEND_INFLIGHT))
    return;
  n = c->send_head;
  if (!n) {
    c->flags &= ~CONN_SEND_INFLIGHT;
    return;
  }

  if (res <= 0) {

    c->flags &= ~CONN_SEND_INFLIGHT;
    conn_drop_sends(ctx, c);
    c->flags |= CONN_CLOSE_AFTER;
    conn_start_close(ctx, ridx, slot, c);
    return;
  }

  c->send_off += (uint32_t)res;
  if (c->send_off < n->len) {
    if (conn_submit_send_sqe(ctx, ridx, slot, c, n, c->send_off) != 0) {
      c->flags &= ~CONN_SEND_INFLIGHT;
      conn_drop_sends(ctx, c);
      c->flags |= CONN_CLOSE_AFTER;
      conn_start_close(ctx, ridx, slot, c);
    }
    return;
  }

  c->flags &= ~CONN_SEND_INFLIGHT;
  c->send_head = n->next;
  c->send_len = 0;
  c->send_off = 0;
  send_req_free(ctx, n);

  if (c->flags & CONN_CLOSING) {
    conn_start_close(ctx, ridx, slot, c);
    return;
  }
  if (!c->send_head && (c->flags & CONN_CLOSE_AFTER)) {
    conn_start_close(ctx, ridx, slot, c);
    return;
  }
  conn_kick_send(ctx, ridx, slot, c);
}

static void conn_finalize(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                          struct conn *c) {
  struct uring_ring *r = &ctx->rings[ridx];
  uint32_t id = conn_id_of(ctx, ridx, slot);

  if (c->fast_bid != CONN_NO_BID)
    conn_release_fast(ctx, ridx, c, 0);
  if (c->flags & CONN_READY)
    conn_unlink_ready(ctx, id, c);
  if (c->flags & CONN_DRAIN_QUEUED)
    conn_unlink_drain(ctx, ridx, id, c);
  conn_drop_sends(ctx, c);
  free(c->rx_buf);
  c->rx_buf = NULL;
  c->rx_len = 0;
  c->rx_cap = 0;
  c->fast_data = NULL;
  c->fast_len = 0;
  c->fast_bid = CONN_NO_BID;
  c->next_ready = 0;
  c->next_drain = 0;
  c->flags = 0;
  c->gen++;

  if (r->active_conns)
    r->active_conns--;
  if (ctx->live_conns)
    ctx->live_conns--;
}

static void handle_close(struct net_ctx *ctx, uint32_t ridx, uint32_t slot,
                         uint32_t gen, const struct io_uring_cqe *cqe) {
  struct conn *c = conn_get(ctx, ridx, slot, gen);
  if (!c)
    return;

  if (cqe->res < 0 && cqe->res != -EBADF) {
    if (!(c->flags & CONN_CLOSE_RETRYING)) {
      c->flags |= CONN_CLOSE_RETRYING;
      if (conn_submit_close_only(ctx, ridx, slot, c) == 0)
        return;
    }
  }
  conn_finalize(ctx, ridx, slot, c);
}

static void process_cqe(struct net_ctx *ctx, uint32_t ridx,
                        const struct io_uring_cqe *cqe) {
  uint64_t ud = cqe->user_data;
  uint32_t op = net_tag_op(ud);
  uint32_t slot = net_tag_slot(ud);
  uint32_t gen = net_tag_gen(ud);
  int has_buf = 0;
  uint16_t bid = 0;

  if (cqe->flags & IORING_CQE_F_BUFFER) {
    bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    if (bid < PBUF_COUNT) {
      has_buf = 1;
      if (ctx->rings[ridx].avail_pbufs)
        ctx->rings[ridx].avail_pbufs--;
    }
  }

  switch (op) {
  case OP_ACCEPT:
    if (has_buf)
      ur_recycle_pbuf(&ctx->rings[ridx], bid);
    handle_accept(ctx, ridx, ud, cqe);
    break;
  case OP_RECV:
    handle_recv(ctx, ridx, slot, gen, cqe, has_buf, bid);
    break;
  case OP_DRAIN:
    handle_drain(ctx, ridx, slot, gen, cqe, has_buf, bid);
    break;
  case OP_SEND:
    if (has_buf)
      ur_recycle_pbuf(&ctx->rings[ridx], bid);
    handle_send(ctx, ridx, slot, gen, cqe);
    break;
  case OP_SHUTDOWN:
  case OP_CANCEL:
  case OP_ORPHAN_CLOSE:
    if (has_buf)
      ur_recycle_pbuf(&ctx->rings[ridx], bid);
    break;
  case OP_CLOSE:
    if (has_buf)
      ur_recycle_pbuf(&ctx->rings[ridx], bid);
    handle_close(ctx, ridx, slot, gen, cqe);
    break;
  default:
    if (has_buf)
      ur_recycle_pbuf(&ctx->rings[ridx], bid);
    break;
  }
}

static int drain_ring_cqes(struct net_ctx *ctx, uint32_t ridx) {
  struct uring_ring *r = &ctx->rings[ridx];
  int processed = 0;

  if (!r->active)
    return 0;

  for (;;) {
    uint32_t head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
    struct io_uring_cqe cqe;
    if (head == tail)
      break;
    cqe = r->cqes[head & *r->cq_mask];
    __atomic_store_n(r->cq_head, head + 1u, __ATOMIC_RELEASE);
    process_cqe(ctx, ridx, &cqe);
    processed++;
  }
  return processed;
}

static int have_drain_work(const struct net_ctx *ctx) {
  uint32_t i;
  for (i = 0; i < ctx->n_rings; i++) {
    if (ctx->drain_head[i])
      return 1;
  }
  return 0;
}

static void service_drain_queues(struct net_ctx *ctx) {
  uint32_t ridx;
  for (ridx = 0; ridx < ctx->n_rings; ridx++) {
    struct uring_ring *r = &ctx->rings[ridx];
    uint32_t budget;
    uint32_t issued = 0;

    if (!r->active || !ctx->drain_head[ridx])
      continue;
    budget = r->avail_pbufs;
    if (budget == 0)
      continue;

    while (ctx->drain_head[ridx] && issued < budget) {
      uint32_t head = ctx->drain_head[ridx];
      struct conn *c = conn_by_id(ctx, head - 1u);
      uint32_t slot;
      uint32_t id;
      struct io_uring_sqe *sqe;

      if (!c)
        break;

      ctx->drain_head[ridx] = c->next_drain;
      if (!ctx->drain_head[ridx])
        ctx->drain_tail[ridx] = 0;
      c->next_drain = 0;
      c->flags &= ~CONN_DRAIN_QUEUED;

      id = head - 1u;
      slot = id % ctx->table_size;

      if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING) ||
          (c->flags & CONN_DRAIN_INFLIGHT))
        continue;

      sqe = prep_recv(ctx, ridx, slot, c, OP_DRAIN, 0);
      if (!sqe) {
        conn_drain_push_front(ctx, ridx, c, id);
        break;
      }
      c->flags |= CONN_DRAIN_INFLIGHT;
      issued++;
    }
  }
}

static void answer_ready(struct net_ctx *ctx, int max_answers,
                         net_read_cb on_ready, void *user, int *answered_out) {
  int answered = 0;
  if (max_answers <= 0 || !on_ready)
    goto out;

  while (answered < max_answers) {
    uint32_t head = ctx->ready_head;
    struct conn *c;
    uint32_t id, ridx, next;
    if (!head)
      break;

    c = conn_by_id(ctx, head - 1u);
    if (!c) {
      ctx->ready_head = 0;
      ctx->ready_tail = 0;
      break;
    }

    next = c->next_ready;
    ctx->ready_head = next;
    if (!next)
      ctx->ready_tail = 0;
    c->next_ready = 0;
    c->flags &= ~CONN_READY;

    if (!(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
      continue;
    if (!conn_has_data(c))
      continue;

    id = head - 1u;
    ridx = id / ctx->table_size;
    if (ridx >= ctx->n_rings) {
      continue;
    }

    if (c->fast_bid != CONN_NO_BID && c->rx_len != 0)
      conn_release_fast(ctx, ridx, c, 1);

    on_ready(ctx, id, user);
    answered++;

    if (c->fast_bid != CONN_NO_BID)
      conn_release_fast(ctx, ridx, c, 1);
  }

out:
  *answered_out = answered;
}

struct net_ctx *net_init(const char *host, uint16_t port, int pre_conns) {
  struct net_ctx *ctx;
  uint32_t nofile, table_size, need;
  uint64_t eff, slots;
  int n;

  nofile = ur_raise_nofile();
  if (nofile <= 128u)
    return NULL;
  table_size = nofile - 128u;
  if (table_size > 65400u)
    table_size = 65400u;
  if (table_size < 2u)
    return NULL;

  ctx = (struct net_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;

  ctx->nofile = nofile;
  ctx->table_size = table_size;
  ctx->accept_ring = 0;

  n = ur_create_listeners(host, port, ctx->listeners, MAX_LISTENERS);
  if (n <= 0)
    goto fail;
  ctx->n_listeners = n;
  if (ctx->n_listeners > MAX_LISTENERS)
    ctx->n_listeners = MAX_LISTENERS;

  eff = (uint64_t)(table_size - 1u);
  if (eff == 0)
    eff = 1;
  slots = pre_conns > 0 ? (uint64_t)(uint32_t)pre_conns : 1u;
  need = (uint32_t)((slots + eff - 1u) / eff);
  if (need < 1u)
    need = 1u;
  if (need > MAX_RINGS)
    need = MAX_RINGS;
  ctx->n_rings_target = need;
  ctx->n_rings = 0;
  ctx->conn_count = (size_t)need * (size_t)table_size;
  ctx->conns = (struct conn *)calloc(ctx->conn_count, sizeof(struct conn));
  if (!ctx->conns)
    goto fail;

  if (ur_ring_init(&ctx->rings[0], table_size) != 0)
    goto fail;
  ctx->rings[0].active = 1;
  ctx->n_rings = 1;
  ring_register_listener_reserve(ctx, 0);
  if (accept_arm_all(ctx, 0) <= 0)
    goto fail;
  (void)ur_submit_wait(&ctx->rings[0], 0, 0);
  return ctx;

fail:
  net_destroy(ctx);
  return NULL;
}

void net_destroy(struct net_ctx *ctx) {
  uint32_t i;
  size_t j;

  if (!ctx)
    return;

  if (ctx->conns) {
    for (j = 0; j < ctx->conn_count; j++) {
      struct conn *c = &ctx->conns[j];
      conn_drop_sends(ctx, c);
      free(c->rx_buf);
      c->rx_buf = NULL;
    }
  }

  {
    struct send_arena *a = ctx->batch_head;
    ctx->batch_head = NULL;
    ctx->batch_cur = NULL;
    while (a) {
      struct send_arena *next = a->next;
      a->next = NULL;
      if (a->refcnt)
        a->refcnt--;
      if (a->refcnt == 0)
        ur_arena_release(&ctx->arena_pool, a);
      a = next;
    }
  }

  for (i = 0; i < ctx->n_rings; i++) {
    if (ctx->rings[i].active)
      ur_ring_destroy(&ctx->rings[i]);
  }
  for (i = 0; i < (uint32_t)ctx->n_listeners; i++) {
    if (ctx->listeners[i] >= 0)
      close(ctx->listeners[i]);
  }
  ur_arena_free_pool(ctx->arena_pool);
  free(ctx->conns);
  free(ctx);
}

int net_poll_pass(struct net_ctx *ctx, int wait_us, int min_cqes,
                  int max_answers, net_read_cb on_ready, void *user) {
  int processed = 0;
  int answered = 0;
  uint32_t i;

  if (!ctx)
    return 0;

  for (i = 1; i < ctx->n_rings; i++) {
    struct uring_ring *r = &ctx->rings[i];
    if (!r->active)
      continue;
    (void)ur_submit_wait(r, 0, 0);
    processed += drain_ring_cqes(ctx, i);
  }

  if (ctx->rings[0].active) {
    (void)ur_submit_wait(&ctx->rings[0], 0, 0);
    processed += drain_ring_cqes(ctx, 0);

    if (processed < min_cqes && !ctx->ready_head &&
        !have_drain_work(ctx) && wait_us > 0) {
      uint32_t wait_nr = (uint32_t)(min_cqes - processed);
      if (wait_nr > RING_CQ_ENTRIES)
        wait_nr = RING_CQ_ENTRIES;
      if (wait_nr == 0)
        wait_nr = 1;
      (void)ur_submit_wait(&ctx->rings[0], wait_nr, wait_us);
      processed += drain_ring_cqes(ctx, 0);
    }
  }

  service_drain_queues(ctx);

  answer_ready(ctx, max_answers, on_ready, user, &answered);
  return answered;
}

void net_conn_buf(struct net_ctx *ctx, uint32_t conn_id, const uint8_t **data,
                  size_t *len) {
  struct conn *c = conn_by_id(ctx, conn_id);
  if (data)
    *data = NULL;
  if (len)
    *len = 0;
  if (!c || !(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return;

  if (c->fast_bid != CONN_NO_BID) {
    if (data)
      *data = c->fast_data;
    if (len)
      *len = c->fast_len;
    return;
  }
  if (data)
    *data = c->rx_buf;
  if (len)
    *len = c->rx_len;
}

void net_conn_consume(struct net_ctx *ctx, uint32_t conn_id, size_t n) {
  struct conn *c = conn_by_id(ctx, conn_id);
  if (!c || !(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return;

  if (c->fast_bid != CONN_NO_BID) {
    if (n > c->fast_len)
      n = c->fast_len;
    c->fast_data += n;
    c->fast_len -= (uint32_t)n;
    return;
  }

  if (n > c->rx_len)
    n = c->rx_len;
  if (n && n < c->rx_len)
    memmove(c->rx_buf, c->rx_buf + n, c->rx_len - n);
  c->rx_len -= (uint32_t)n;
}

int net_conn_is_eof(struct net_ctx *ctx, uint32_t conn_id) {
  struct conn *c = conn_by_id(ctx, conn_id);
  if (!c || !(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return 1;
  return (c->flags & CONN_EOF) && !conn_has_data(c);
}

int net_conn_is_dead(struct net_ctx *ctx, uint32_t conn_id) {
  struct conn *c = conn_by_id(ctx, conn_id);
  if (!c || !(c->flags & CONN_ALIVE))
    return 1;
  return (c->flags & CONN_CLOSING) != 0;
}

static struct send_arena *batch_find_arena(struct net_ctx *ctx,
                                           const uint8_t *data, size_t len) {
  struct send_arena *a;
  for (a = ctx->batch_head; a; a = a->next) {
    if (!a->base)
      continue;
    if (data >= a->base && data < a->base + a->used) {
      size_t avail = (size_t)(a->base + a->used - data);
      if (len <= avail)
        return a;
    }
  }
  return NULL;
}

uint8_t *net_batch_alloc(struct net_ctx *ctx, size_t size) {
  struct send_arena *a;
  size_t aligned;
  uint8_t *p;

  if (!ctx || size == 0)
    return NULL;
  if (size > ARENA_SIZE)
    return NULL;
  aligned = (size + 15u) & ~(size_t)15u;

  a = ctx->batch_cur;
  if (!a || a->used + aligned > a->cap) {
    a = ur_arena_acquire(&ctx->arena_pool, ARENA_SIZE);
    if (!a)
      return NULL;
    a->used = 0;
    a->refcnt = 1;
    a->next = ctx->batch_head;
    ctx->batch_head = a;
    ctx->batch_cur = a;
  }
  if (a->used + aligned > a->cap)
    return NULL;

  p = a->base + a->used;
  a->used += aligned;
  return p;
}

int net_conn_send(struct net_ctx *ctx, uint32_t conn_id, const uint8_t *data,
                  size_t len, int is_static, int close_after) {
  struct conn *c = conn_by_id(ctx, conn_id);
  struct send_req *n;
  uint32_t ridx, slot;

  if (!ctx || !c || !(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return -1;
  if (len > 0xffffffffu)
    return -1;

  n = (struct send_req *)calloc(1, sizeof(*n));
  if (!n)
    return -1;
  n->data = data;
  n->len = len;
  n->is_static = (uint8_t)(is_static != 0);
  n->close_after = (uint8_t)(close_after != 0);

  if (!is_static && len) {
    struct send_arena *a = batch_find_arena(ctx, data, len);
    if (a) {
      a->refcnt++;
      n->arena = a;
    }
  }

  ridx = conn_id / ctx->table_size;
  slot = conn_id % ctx->table_size;

  if (close_after)
    c->flags |= CONN_CLOSE_AFTER;

  if (!c->send_head) {
    c->send_head = n;
  } else {
    struct send_req *t = c->send_head;
    while (t->next)
      t = t->next;
    t->next = n;
  }

  if (!(c->flags & CONN_SEND_INFLIGHT))
    conn_kick_send(ctx, ridx, slot, c);
  return 0;
}

void net_conn_close(struct net_ctx *ctx, uint32_t conn_id) {
  struct conn *c = conn_by_id(ctx, conn_id);
  uint32_t ridx, slot, id;

  if (!ctx || !c || !(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return;

  ridx = conn_id / ctx->table_size;
  slot = conn_id % ctx->table_size;
  id = conn_id;

  c->flags |= CONN_CLOSING | CONN_CLOSE_DROP;
  conn_unlink_ready(ctx, id, c);
  conn_unlink_drain(ctx, ridx, id, c);

  if (c->flags & CONN_SEND_INFLIGHT) {
    c->flags |= CONN_CLOSE_AFTER;
    return;
  }
  conn_drop_sends(ctx, c);
  conn_start_close(ctx, ridx, slot, c);
}

void net_conn_requeue(struct net_ctx *ctx, uint32_t conn_id) {
  struct conn *c = conn_by_id(ctx, conn_id);

  if (!c || !(c->flags & CONN_ALIVE) || (c->flags & CONN_CLOSING))
    return;
  if (!conn_has_data(c))
    return;

  conn_mark_ready(ctx, conn_id / ctx->table_size, conn_id % ctx->table_size, c);
}

void net_end_batch(struct net_ctx *ctx) {
  struct send_arena *a;
  uint32_t i;

  if (!ctx)
    return;

  a = ctx->batch_head;
  ctx->batch_head = NULL;
  ctx->batch_cur = NULL;
  while (a) {
    struct send_arena *next = a->next;
    a->next = NULL;
    if (a->refcnt)
      a->refcnt--;
    if (a->refcnt == 0)
      ur_arena_release(&ctx->arena_pool, a);
    a = next;
  }

  for (i = 0; i < ctx->n_rings; i++) {
    if (ctx->rings[i].active)
      (void)ur_submit_wait(&ctx->rings[i], 0, 0);
  }
}

int net_has_backlog(struct net_ctx *ctx) {
  if (!ctx)
    return 0;
  return ctx->ready_head != 0;
}
