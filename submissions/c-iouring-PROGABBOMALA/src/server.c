#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.c"
#include "http.h"
#include "jwt.h"
#include "btree.h"
#include "db.h"
#include "net.h"

#define MAX_BATCH 2048              /* requests answered per pass */
#define ARENA_CAP (2u << 20)        /* response memory comes in 2 MiB arenas */
#define MAX_ARENAS 256
#define RESP_MAX (128u << 10)       /* upper bound of one response */
#define HDR_ROOM 128                /* the header is written right in front of the body */
#define SCRATCH_CAP (8u << 20)      /* per-pass memory for request copies and decoded text */
#define NO_ARENA 0xffffu
#define WAIT_BATCH 128              /* completions to wait for when the loop is busy */
#define WAIT_NS 1000000L            /* ... or this long */
#define IDLE_NS 1000000000L
#define READS_PER_PASS 16384        /* single reads issued per pass */
#define CQES_PER_REAP 32768

enum { ST_DONE = 0, ST_READ, ST_WRITE };

struct task {
  uint32_t cid; /* ring << 16 | slot */
  uint16_t gen;
  uint16_t arena;
  uint8_t route, is_post, keep_alive, state;
  int64_t post_id;
  const char *auth;
  uint32_t auth_len;
  const char *body;
  uint32_t body_len;
  int64_t uid;
  const uint8_t *name;
  uint32_t name_len;
  const uint8_t *text;
  uint32_t text_len;
  char *base; /* arena region of the response */
  char *resp;
  uint32_t resp_len;
};

struct arena {
  char *base;
  uint32_t off;
  uint32_t inflight; /* sends the kernel may still read from here */
};

struct idlist {
  uint32_t *v;
  uint32_t n, cap;
};

static struct ring_ctx g_rings[MAX_RINGS];
static int g_nrings;
static int g_acc; /* the ring that accepts */
static uint32_t g_slots;
static uint32_t g_depth;
static int g_lis[NLIS];
static struct db_engine g_db;
static struct jwt_hmac_state g_jwt;
static int64_t g_started, g_now_s, g_now_ms;

static struct task g_batch[MAX_BATCH];
static int g_nbatch;
static struct arena g_arena[MAX_ARENAS];
static int g_narena, g_cur_arena = -1;
static char *g_scratch;
static uint32_t g_scratch_off;
static struct idlist g_ready, g_held, g_rq;
static uint32_t g_reads;
static struct bt_feed g_feed;

static void list_push(struct idlist *l, uint32_t id) {
  if (l->n == l->cap) {
    uint32_t cap = l->cap ? l->cap * 2 : 1024;
    uint32_t *v = realloc(l->v, cap * sizeof *v);
    if (!v) abort();
    l->v = v;
    l->cap = cap;
  }
  l->v[l->n++] = id;
}

static inline struct ring_ctx *cid_ring(uint32_t cid) { return &g_rings[cid >> 16]; }
static inline struct conn *cid_conn(uint32_t cid) { return &g_rings[cid >> 16].conns[cid & 0xffff]; }

static inline void *scratch_get(uint32_t n) {
  n = (n + 15u) & ~15u;
  if (g_scratch_off + n > SCRATCH_CAP) return NULL;
  void *p = g_scratch + g_scratch_off;
  g_scratch_off += n;
  return p;
}

static char *arena_reserve(uint16_t *idx) {
  if (g_cur_arena >= 0 && g_arena[g_cur_arena].off + RESP_MAX <= ARENA_CAP) {
    *idx = (uint16_t)g_cur_arena;
    return g_arena[g_cur_arena].base + g_arena[g_cur_arena].off;
  }
  for (int i = 0; i < g_narena; i++) {
    if (g_arena[i].inflight == 0 && g_arena[i].off == 0) {
      g_cur_arena = i;
      *idx = (uint16_t)i;
      return g_arena[i].base;
    }
  }
  if (g_narena == MAX_ARENAS) return NULL;
  char *m = mmap(0, ARENA_CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return NULL;
  g_arena[g_narena].base = m;
  g_arena[g_narena].off = 0;
  g_arena[g_narena].inflight = 0;
  g_cur_arena = g_narena++;
  *idx = (uint16_t)g_cur_arena;
  return m;
}

static void arenas_recycle(void) {
  for (int i = 0; i < g_narena; i++)
    if (g_arena[i].inflight == 0) g_arena[i].off = 0;
  g_cur_arena = -1;
}

static const char *status_text(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

static char *resp_begin(struct task *t) {
  t->base = arena_reserve(&t->arena);
  if (!t->base) {
    t->state = ST_DONE;
    t->resp = NULL;
    return NULL;
  }
  return t->base + HDR_ROOM;
}

static void resp_header(struct task *t, char *end, int code) {
  char *body = t->base + HDR_ROOM;
  char hdr[HDR_ROOM];
  char *h = PUT(hdr, "HTTP/1.1 ");
  const char *st = status_text(code);
  h = put_lit(h, st, strlen(st));
  h = PUT(h, "\r\nContent-Type: application/json\r\nContent-Length: ");
  h = put_u64(h, (uint64_t)(end - body));
  if (!t->keep_alive) h = PUT(h, "\r\nConnection: close");
  h = PUT(h, "\r\n\r\n");
  size_t hl = (size_t)(h - hdr);
  memcpy(body - hl, hdr, hl);
  t->resp = body - hl;
  t->resp_len = (uint32_t)(hl + (size_t)(end - body));
}

static void resp_end(struct task *t, char *end, int code) {
  resp_header(t, end, code);
  g_arena[t->arena].off += ((uint32_t)(end - t->base) + 31u) & ~15u;
  t->state = ST_DONE;
}

static void resp_error(struct task *t, int code, const char *msg) {
  char *p = resp_begin(t);
  if (!p) return;
  p = PUT(p, "{\"error\":\"");
  p = put_lit(p, msg, strlen(msg));
  p = PUT(p, "\"}");
  resp_end(t, p, code);
}

static void resp_rewrite_500(struct task *t) {
  if (!t->base) return;
  char *p = t->base + HDR_ROOM;
  p = PUT(p, "{\"error\":\"internal server error\"}");
  resp_header(t, p, 500);
}

static inline char *emit_post(char *p, int64_t id, const uint8_t *body, uint32_t body_len, const uint8_t *stamp,
                              uint32_t stamp_len, const uint8_t *name, uint32_t name_len, uint64_t likes) {
  p = PUT(p, "{\"id\":");
  p = put_i64(p, id);
  p = PUT(p, ",\"body\":\"");
  p = put_jstr(p, body, body_len);
  p = PUT(p, "\",\"created_at\":\"");
  p = put_jstr(p, stamp, stamp_len);
  p = PUT(p, "\",\"author\":\"");
  p = put_jstr(p, name, name_len);
  p = PUT(p, "\",\"like_count\":");
  p = put_u64(p, likes);
  *p++ = '}';
  return p;
}

/* Output bytes a post object can need at most. */
static inline size_t post_room(uint32_t body_len, uint32_t stamp_len, uint32_t name_len) {
  return 6u * ((size_t)body_len + stamp_len + name_len) + 160;
}

static void conn_arm_recv(struct ring_ctx *r, uint32_t slot) {
  struct conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_RECV;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->ioprio = IORING_RECV_MULTISHOT;
  s->buf_group = 0;
  s->user_data = ud_make(OP_RECV, (unsigned)r->idx, c->gen, 0, slot);
  c->flags |= CF_RECV;
}

static void conn_arm_read(struct ring_ctx *r, uint32_t slot) {
  struct conn *c = &r->conns[slot];
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_RECV;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  s->buf_group = 0;
  s->msg_flags = MSG_DONTWAIT;
  s->user_data = ud_make(OP_READ, (unsigned)r->idx, c->gen, 0, slot);
  c->flags |= CF_READING;
  r->reads++;
}

static inline void conn_queue_read(struct ring_ctx *r, uint32_t slot) {
  struct conn *c = &r->conns[slot];
  if (c->flags & (CF_RQ | CF_READING | CF_RECV)) return;
  c->flags |= CF_RQ;
  list_push(&g_rq, ((uint32_t)r->idx << 16) | slot);
}

static void conn_close(struct ring_ctx *r, uint32_t slot) {
  struct conn *c = &r->conns[slot];
  if (c->state != C_OPEN) return;
  c->state = C_CLOSING;
  c->flags = 0;
  free(c->in);
  c->in = NULL;
  c->in_len = c->in_cap = 0;
  struct io_uring_sqe *s = sqe_get(r);
  s->opcode = IORING_OP_SHUTDOWN;
  s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
  s->len = SHUT_RDWR;
  s->user_data = ud_make(OP_MISC, (unsigned)r->idx, c->gen, 0, slot);
  s = sqe_get(r);
  s->opcode = IORING_OP_CLOSE;
  s->file_index = slot + 1;
  s->user_data = ud_make(OP_CLOSE, (unsigned)r->idx, c->gen, 0, slot);
}

static int conn_stash(struct ring_ctx *r, uint32_t slot, const char *p, uint32_t n) {
  struct conn *c = &r->conns[slot];
  if ((uint64_t)c->in_len + n > MAX_HEAD + MAX_BODY) {
    conn_close(r, slot);
    return 0;
  }
  if (c->in_len + n > c->in_cap) {
    uint32_t cap = c->in_cap ? c->in_cap : 512;
    while (cap < c->in_len + n) cap *= 2;
    char *m = realloc(c->in, cap);
    if (!m) {
      conn_close(r, slot);
      return 0;
    }
    c->in = m;
    c->in_cap = cap;
  }
  memcpy(c->in + c->in_len, p, n);
  c->in_len += n;
  return 1;
}

static inline void conn_ready(struct ring_ctx *r, uint32_t slot) {
  struct conn *c = &r->conns[slot];
  if (c->flags & CF_READY) return;
  c->flags |= CF_READY;
  list_push(&g_ready, ((uint32_t)r->idx << 16) | slot);
}

static inline int is_write_route(const struct http_req *q) {
  return q->route == ROUTE_CREATE_POST || q->route == ROUTE_LIKE_POST;
}

/* 0 = no scratch memory left in this pass. */
static int task_add(struct ring_ctx *r, uint32_t slot, const struct http_req *q) {
  struct conn *c = &r->conns[slot];
  const char *auth = NULL, *body = NULL;
  if (q->is_post) { /* a GET needs nothing of its bytes later */
    char *m = scratch_get(q->auth_len + q->body_len + 1);
    if (!m) return 0;
    if (q->auth) {
      memcpy(m, q->auth, q->auth_len);
      auth = m;
    }
    memcpy(m + q->auth_len, q->body, q->body_len);
    body = m + q->auth_len;
  }
  struct task *t = &g_batch[g_nbatch++];
  t->cid = ((uint32_t)r->idx << 16) | slot;
  t->gen = c->gen;
  t->arena = NO_ARENA;
  t->route = q->route;
  t->is_post = q->is_post;
  t->keep_alive = q->keep_alive;
  t->state = ST_DONE;
  t->post_id = q->post_id;
  t->auth = auth;
  t->auth_len = auth ? q->auth_len : 0;
  t->body = body;
  t->body_len = body ? q->body_len : 0;
  t->base = NULL;
  t->resp = NULL;
  t->resp_len = 0;
  c->flags |= CF_BUSY;
  return 1;
}

static void on_data(struct ring_ctx *r, uint32_t slot, const char *p, uint32_t n) {
  struct conn *c = &r->conns[slot];
  if (c->in_len || (c->flags & (CF_BUSY | CF_HELD))) {
    if (conn_stash(r, slot, p, n) && !(c->flags & (CF_BUSY | CF_HELD))) conn_ready(r, slot);
    return;
  }
  struct http_req q;
  int k = http_parse(p, n, &q);
  if (k < 0) {
    conn_close(r, slot);
    return;
  }
  if (k == 0) {
    conn_stash(r, slot, p, n);
    return;
  }
  if (is_write_route(&q) && g_db.writes_held) {
    if (conn_stash(r, slot, p, n)) {
      c->flags |= CF_HELD;
      list_push(&g_held, ((uint32_t)r->idx << 16) | slot);
    }
    return;
  }
  if (g_nbatch == MAX_BATCH || !task_add(r, slot, &q)) { /* read now, answered by a later pass */
    if (conn_stash(r, slot, p, n)) conn_ready(r, slot);
    return;
  }
  if ((uint32_t)k < n) conn_stash(r, slot, p + k, n - (uint32_t)k); /* pipelined: after this response */
}

static void process_ready(void) {
  uint32_t n = g_ready.n;
  if (!n) return;
  uint32_t *v = g_ready.v;
  struct idlist keep = {0, 0, 0};
  g_ready.v = NULL;
  g_ready.n = g_ready.cap = 0;
  for (uint32_t i = 0; i < n; i++) {
    struct ring_ctx *r = cid_ring(v[i]);
    uint32_t slot = v[i] & 0xffff;
    struct conn *c = &r->conns[slot];
    if (c->state != C_OPEN || !(c->flags & CF_READY)) continue;
    if (g_nbatch == MAX_BATCH) { /* the rest keeps its place in the queue */
      for (; i < n; i++) list_push(&keep, v[i]);
      break;
    }
    c->flags &= (uint8_t)~CF_READY;
    if (!c->in_len || (c->flags & (CF_BUSY | CF_HELD))) continue;
    struct http_req q;
    int k = http_parse(c->in, c->in_len, &q);
    if (k < 0) {
      conn_close(r, slot);
      continue;
    }
    if (k == 0) continue;
    if (is_write_route(&q) && g_db.writes_held) {
      c->flags |= CF_HELD;
      list_push(&g_held, v[i]);
      continue;
    }
    if (!task_add(r, slot, &q)) {
      c->flags |= CF_READY;
      list_push(&keep, v[i]);
      continue;
    }
    c->in_len -= (uint32_t)k;
    if (c->in_len) {
      memmove(c->in, c->in + k, c->in_len);
    } else {
      free(c->in);
      c->in = NULL;
      c->in_cap = 0;
    }
  }
  free(v);
  for (uint32_t i = 0; i < keep.n; i++) list_push(&g_ready, keep.v[i]);
  free(keep.v);
}

static void release_held(void) {
  for (uint32_t i = 0; i < g_held.n; i++) {
    struct ring_ctx *r = cid_ring(g_held.v[i]);
    uint32_t slot = g_held.v[i] & 0xffff;
    struct conn *c = &r->conns[slot];
    if (c->state != C_OPEN || !(c->flags & CF_HELD)) continue;
    c->flags &= (uint8_t)~CF_HELD;
    conn_ready(r, slot);
  }
  g_held.n = 0;
}

static inline uint32_t ring_free(const struct ring_ctx *r) { return r->nslots - r->open - r->acc_inflight; }

static void accept_arm(struct ring_ctx *r) {
  uint32_t budget = ring_free(r);
  uint32_t per = g_depth / NLIS;
  for (unsigned i = 0; i < NLIS && budget; i++) {
    while (r->acc_lis[i] < per && budget) {
      struct io_uring_sqe *s = sqe_get(r);
      s->opcode = IORING_OP_ACCEPT;
      s->fd = g_lis[i];
      s->file_index = IORING_FILE_INDEX_ALLOC;
      s->user_data = ud_make(OP_ACCEPT, (unsigned)r->idx, 0, i, 0);
      r->acc_lis[i]++;
      r->acc_inflight++;
      budget--;
    }
  }
}

static void accept_policy(void) {
  static int64_t retry_at;
  struct ring_ctx *a = &g_rings[g_acc];
  if (ring_free(a) + a->acc_inflight < g_depth) {
    int best = g_acc;
    for (int i = 0; i < g_nrings; i++)
      if (ring_free(&g_rings[i]) > ring_free(&g_rings[best])) best = i;
    if (ring_free(&g_rings[best]) < 2 * g_depth && g_nrings < MAX_RINGS && g_now_ms >= retry_at) {
      if (ring_init(&g_rings[g_nrings], g_nrings, g_slots, 8192)) {
        best = g_nrings++;
        fprintf(stderr, "ring %d opened\n", best);
      } else {
        fprintf(stderr, "ring %d: %s (will retry)\n", g_nrings, strerror(errno));
        retry_at = g_now_ms + 1000;
      }
    }
    g_acc = best;
    a = &g_rings[best];
  }
  accept_arm(a);
}

static void on_cqe(struct ring_ctx *r, const struct io_uring_cqe *e) {
  uint64_t u = e->user_data;
  uint32_t slot = UD_SLOT(u);
  int res = e->res;
  switch (UD_OP(u)) {
    case OP_RECV: {
      struct conn *c = &r->conns[slot];
      int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
      uint16_t bid = (uint16_t)(e->flags >> IORING_CQE_BUFFER_SHIFT);
      if (c->gen != UD_GEN(u) || c->state != C_OPEN) {
        if (has_buf) pbuf_put(r, bid);
        return;
      }
      if (!(e->flags & IORING_CQE_F_MORE)) c->flags &= (uint8_t)~CF_RECV;
      if (res > 0 && has_buf) {
        on_data(r, slot, (const char *)(r->bufs + (size_t)bid * PBUF_SIZE), (uint32_t)res);
        pbuf_put(r, bid);
      } else {
        if (has_buf) pbuf_put(r, bid);
        if (res != -ENOBUFS) { /* end of stream or a socket error */
          conn_close(r, slot);
          return;
        }
      }

      if (c->state == C_OPEN && !(c->flags & CF_RECV)) conn_queue_read(r, slot);
      return;
    }
    case OP_READ: {
      if (r->reads) r->reads--;
      struct conn *c = &r->conns[slot];
      int has_buf = (e->flags & IORING_CQE_F_BUFFER) != 0;
      uint16_t bid = (uint16_t)(e->flags >> IORING_CQE_BUFFER_SHIFT);
      if (c->gen != UD_GEN(u) || c->state != C_OPEN) {
        if (has_buf) pbuf_put(r, bid);
        return;
      }
      c->flags &= (uint8_t)~CF_READING;
      if (res > 0 && has_buf) {
        on_data(r, slot, (const char *)(r->bufs + (size_t)bid * PBUF_SIZE), (uint32_t)res);
        pbuf_put(r, bid);
        if (c->state != C_OPEN) return;
        if ((uint32_t)res == PBUF_SIZE) conn_queue_read(r, slot); /* there may be more */
        else conn_arm_recv(r, slot);                                /* drained */
        return;
      }
      if (has_buf) pbuf_put(r, bid);
      if (res == -EAGAIN) conn_arm_recv(r, slot);
      else if (res == -ENOBUFS) conn_queue_read(r, slot);
      else conn_close(r, slot);
      return;
    }
    case OP_SEND: {
      uint16_t ai = UD_AUX(u);
      if (ai != NO_ARENA && g_arena[ai].inflight) g_arena[ai].inflight--;
      struct conn *c = &r->conns[slot];
      if (c->gen != UD_GEN(u) || c->state != C_OPEN) return;
      if (res < 0 || (uint32_t)res != c->send_len || (c->flags & CF_CLOSE)) {
        conn_close(r, slot);
        return;
      }
      c->flags &= (uint8_t)~CF_BUSY;
      if (c->in_len) conn_ready(r, slot);
      return;
    }
    case OP_ACCEPT: {
      unsigned li = UD_AUX(u);
      if (r->acc_lis[li]) r->acc_lis[li]--;
      if (r->acc_inflight) r->acc_inflight--;
      if (res >= 0 && (uint32_t)res < r->nslots) {
        struct conn *c = &r->conns[res];
        if (c->state == C_FREE) r->open++;
        free(c->in);
        c->in = NULL;
        c->in_len = c->in_cap = 0;
        c->gen++;
        c->state = C_OPEN;
        c->flags = 0;
        conn_arm_recv(r, (uint32_t)res);
      }
      return; /* accept_policy queues the replacement */
    }
    case OP_CLOSE: {
      struct conn *c = &r->conns[slot];
      if (c->gen == UD_GEN(u) && c->state == C_CLOSING) {
        c->state = C_FREE;
        if (r->open) r->open--;
      }
      return;
    }
    default:
      return;
  }
}

static void reap(struct ring_ctx *r) {
  uint32_t head = *r->cq_khead;
  uint32_t tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
  if (tail - head > CQES_PER_REAP) tail = head + CQES_PER_REAP;
  r->got = tail - head;
  if (head == tail) return;
  while (head != tail) {
    on_cqe(r, &r->cqes[head & r->cq_mask]);
    head++;
  }
  __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
  pbuf_publish(r);
}

static uint32_t issue_reads(void) {
  uint32_t n = g_rq.n, issued = 0;
  if (!n) return 0;
  uint32_t *v = g_rq.v;
  g_rq.v = NULL;
  g_rq.n = g_rq.cap = 0;
  for (uint32_t i = 0; i < n; i++) {
    struct ring_ctx *r = cid_ring(v[i]);
    uint32_t slot = v[i] & 0xffff;
    struct conn *c = &r->conns[slot];
    if (c->state != C_OPEN || !(c->flags & CF_RQ)) continue;
    if (r->reads >= READ_ROUND || g_reads >= READS_PER_PASS) {
      list_push(&g_rq, v[i]);
      continue;
    }
    c->flags &= (uint8_t)~CF_RQ;
    conn_arm_read(r, slot);
    g_reads++;
    issued++;
  }
  free(v);
  return issued;
}

static int authenticate(struct task *t) {
  uint32_t cap = t->auth_len < JWT_WORK ? t->auth_len + 16 : JWT_WORK;
  uint8_t *work = scratch_get(cap);
  if (!work) {
    resp_error(t, 500, "internal server error");
    return 0;
  }
  struct jwt_claims cl;
  int rc = jwt_verify(&g_jwt, t->auth, t->auth_len, g_now_s, &cl, work, cap);
  if (rc == JWT_OK) {
    t->uid = cl.uid;
    t->name = cl.name;
    t->name_len = cl.name_len;
    return 1;
  }
  resp_error(t, 401, rc == JWT_MISSING ? "missing bearer token" : rc == JWT_INVALID ? "invalid or expired token"
                                                                                    : "invalid token payload");
  return 0;
}

static int decode_create(struct task *t) {
  struct jmember m[1] = {{"body", 4, 0, 0, 0, 0}};
  int rc = t->body_len ? json_object((const uint8_t *)t->body, t->body_len, m, 1) : 1;
  if (rc < 0) {
    resp_error(t, 400, "malformed JSON body");
    return 0;
  }
  if (rc > 0 || m[0].type != JV_STRING) {
    resp_error(t, 400, "body is required");
    return 0;
  }
  const uint8_t *s = m[0].v;
  uint32_t n = m[0].vlen;
  if (m[0].esc) {
    uint8_t *d = scratch_get(n);
    if (!d) {
      resp_error(t, 500, "internal server error");
      return 0;
    }
    n = json_unescape(s, n, d);
    s = d;
  }
  js_trim(&s, &n);
  if (n == 0) {
    resp_error(t, 400, "body is required");
    return 0;
  }
  if (u8_chars(s, n) > 500) {
    resp_error(t, 400, "body must be at most 500 characters");
    return 0;
  }
  t->text = s;
  t->text_len = n;
  return 1;
}

static void prepare(struct task *t) {
  switch (t->route) {
    case ROUTE_HEALTH:
    case ROUTE_FEED:
    case ROUTE_GET_POST:
      t->state = ST_READ;
      return;
    case ROUTE_CREATE_POST:
      if (authenticate(t) && decode_create(t)) t->state = ST_WRITE;
      return;
    case ROUTE_LIKE_POST:
      if (authenticate(t)) t->state = ST_WRITE;
      return;
    case ROUTE_BAD_POST_ID: /* authenticated routes check the token before the id */
      if (t->is_post && !authenticate(t)) return;
      resp_error(t, 400, "invalid post id");
      return;
    default:
      resp_error(t, 404, "not found");
  }
}

static int sql_emit_row(char **pp, char *limit, sqlite3_stmt *st) {
  const uint8_t *body = sqlite3_column_text(st, 1);
  const uint8_t *stamp = sqlite3_column_text(st, 2);
  const uint8_t *name = sqlite3_column_text(st, 3);
  if (!body || !stamp || !name) return -1;
  uint32_t bl = (uint32_t)sqlite3_column_bytes(st, 1), sl = (uint32_t)sqlite3_column_bytes(st, 2),
           nl = (uint32_t)sqlite3_column_bytes(st, 3);
  if ((size_t)(limit - *pp) < post_room(bl, sl, nl)) return -1;
  *pp = emit_post(*pp, sqlite3_column_int64(st, 0), body, bl, stamp, sl, name, nl,
                  (uint64_t)sqlite3_column_int64(st, 4));
  return 0;
}

static void exec_feed(struct task *t) {
  char *body = resp_begin(t);
  if (!body) return;
  char *limit = t->base + RESP_MAX - 32;
  char *p = PUT(body, "{\"posts\":[");
  int ok = 0;
  if (bt_feed(&g_db.bt, &g_feed) == 0) {
    ok = 1;
    for (int i = 0; i < g_feed.n; i++) {
      struct bt_post *o = &g_feed.row[i];
      if ((size_t)(limit - p) < post_room(o->body_len, o->stamp_len, o->name_len)) {
        ok = 0;
        break;
      }
      if (i) *p++ = ',';
      p = emit_post(p, o->id, o->body, o->body_len, o->stamp, o->stamp_len, o->name, o->name_len, o->likes);
    }
  }
  if (!ok) { /* the reference query, for anything the direct read declined */
    p = body + 10;
    ok = 1;
    int rc, i = 0;
    while ((rc = sqlite3_step(g_db.st_feed_sql)) == SQLITE_ROW) {
      if (i++) *p++ = ',';
      if (sql_emit_row(&p, limit, g_db.st_feed_sql)) {
        ok = 0;
        break;
      }
    }
    sqlite3_reset(g_db.st_feed_sql);
    if (rc != SQLITE_DONE) ok = 0;
  }
  if (!ok) {
    p = PUT(body, "{\"error\":\"internal server error\"}");
    resp_end(t, p, 500);
    return;
  }
  p = PUT(p, "]}");
  resp_end(t, p, 200);
}

static void exec_get_post(struct task *t) {
  char *body = resp_begin(t);
  if (!body) return;
  char *limit = t->base + RESP_MAX - 32;
  char *p = PUT(body, "{\"post\":");
  struct bt_post o;
  int rc = bt_get_post(&g_db.bt, t->post_id, &o);
  if (rc == 1 && (size_t)(limit - p) >= post_room(o.body_len, o.stamp_len, o.name_len)) {
    p = emit_post(p, o.id, o.body, o.body_len, o.stamp, o.stamp_len, o.name, o.name_len, o.likes);
  } else if (rc != 0) {
    sqlite3_bind_int64(g_db.st_post_sql, 1, t->post_id);
    int s = sqlite3_step(g_db.st_post_sql);
    rc = s == SQLITE_ROW ? (sql_emit_row(&p, limit, g_db.st_post_sql) ? -1 : 1) : s == SQLITE_DONE ? 0 : -1;
    sqlite3_reset(g_db.st_post_sql);
  }
  if (rc == 1) {
    *p++ = '}';
    resp_end(t, p, 200);
  } else if (rc == 0) {
    p = PUT(body, "{\"error\":\"post not found\"}");
    resp_end(t, p, 404);
  } else {
    p = PUT(body, "{\"error\":\"internal server error\"}");
    resp_end(t, p, 500);
  }
}

static void exec_health(struct task *t) {
  char *body = resp_begin(t);
  if (!body) return;
  char *p;
  int rc = sqlite3_step(g_db.st_health);
  sqlite3_reset(g_db.st_health);
  if (rc == SQLITE_ROW) {
    p = PUT(body, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    p = put_i64(p, g_now_s > g_started ? g_now_s - g_started : 0);
    *p++ = '}';
    resp_end(t, p, 200);
  } else {
    const char *msg = sqlite3_errmsg(g_db.db);
    size_t ml = strlen(msg);
    if (ml > 256) ml = 256;
    p = PUT(body, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    p = put_jstr(p, (const uint8_t *)msg, ml);
    p = PUT(p, "\"}");
    resp_end(t, p, 503);
  }
}

static void exec_create(struct task *t) {
  sqlite3_stmt *st = g_db.st_ins_post;
  sqlite3_bind_int64(st, 1, t->uid);
  sqlite3_bind_text(st, 2, (const char *)t->text, (int)t->text_len, SQLITE_STATIC);
  int64_t id = 0;
  uint8_t stamp[ROW_STAMP_MAX];
  int sl = 0;
  int ok = 0;
  if (sqlite3_step(st) == SQLITE_ROW) {
    id = sqlite3_column_int64(st, 0);
    const uint8_t *ct = sqlite3_column_text(st, 1);
    sl = sqlite3_column_bytes(st, 1);
    if (ct && sl > 0 && sl <= ROW_STAMP_MAX) {
      memcpy(stamp, ct, (size_t)sl);
      ok = 1;
    }
  }
  if (sqlite3_reset(st) != SQLITE_OK) ok = 0;
  if (!ok) {
    db_check_broken(&g_db);
    resp_error(t, 500, "internal server error");
    return;
  }
  char *body = resp_begin(t);
  if (!body) return;
  char *p = PUT(body, "{\"post\":");
  if ((size_t)(t->base + RESP_MAX - 32 - p) < post_room(t->text_len, (uint32_t)sl, t->name_len)) {
    g_db.broken = 1; /* cannot report this insert, so the pass must not commit it */
    p = PUT(body, "{\"error\":\"internal server error\"}");
    resp_end(t, p, 500);
    return;
  }
  p = emit_post(p, id, t->text, t->text_len, stamp, (uint32_t)sl, t->name, t->name_len, 0);
  *p++ = '}';
  resp_end(t, p, 201);
  t->state = ST_WRITE; /* still depends on the COMMIT */
}

static void exec_like(struct task *t) {
  int ex = g_db.bt.open ? bt_post_exists(&g_db.bt, t->post_id) : -1;
  if (ex < 0) {
    sqlite3_bind_int64(g_db.st_post_exists, 1, t->post_id);
    int s = sqlite3_step(g_db.st_post_exists);
    sqlite3_reset(g_db.st_post_exists);
    ex = s == SQLITE_ROW ? 1 : s == SQLITE_DONE ? 0 : -1;
  }
  if (ex == 0) {
    resp_error(t, 404, "post not found");
    return;
  }
  int changes = 0;
  if (ex > 0) {
    sqlite3_stmt *st = g_db.st_ins_like;
    sqlite3_bind_int64(st, 1, t->uid);
    sqlite3_bind_int64(st, 2, t->post_id);
    int s = sqlite3_step(st);
    changes = sqlite3_changes(g_db.db);
    if (sqlite3_reset(st) != SQLITE_OK || s != SQLITE_DONE) ex = -1;
  }
  if (ex < 0) {
    db_check_broken(&g_db);
    resp_error(t, 500, "internal server error");
    return;
  }
  char *body = resp_begin(t);
  if (!body) return;
  char *p = PUT(body, "{\"liked\":true,\"already_liked\":");
  p = changes ? PUT(p, "false") : PUT(p, "true");
  p = PUT(p, ",\"post_id\":");
  p = put_i64(p, t->post_id);
  *p++ = '}';
  resp_end(t, p, changes ? 201 : 200);
  t->state = ST_WRITE;
}

static void run_batch(void) {
  int reads = 0, writes = 0;
  for (int i = 0; i < g_nbatch; i++) {
    struct task *t = &g_batch[i];
    prepare(t);
    reads += t->state == ST_READ;
    writes += t->state == ST_WRITE;
  }
  if (!reads && !writes) return;

  if (db_txn_begin(&g_db) != 0) {
    for (int i = 0; i < g_nbatch; i++)
      if (g_batch[i].state != ST_DONE) resp_error(&g_batch[i], 500, "internal server error");
    return;
  }
  if (reads) {
    for (int i = 0; i < g_nbatch; i++) {
      struct task *t = &g_batch[i];
      if (t->state != ST_READ) continue;
      if (t->route == ROUTE_FEED) exec_feed(t);
      else if (t->route == ROUTE_GET_POST) exec_get_post(t);
      else exec_health(t);
    }
  }
  if (writes) {
    for (int i = 0; i < g_nbatch; i++) {
      struct task *t = &g_batch[i];
      if (t->state != ST_WRITE) continue;
      if (g_db.broken) resp_error(t, 500, "internal server error");
      else if (t->route == ROUTE_CREATE_POST) exec_create(t);
      else exec_like(t);
    }
  }
  if (db_txn_end(&g_db) != 0) {
    for (int i = 0; i < g_nbatch; i++)
      if (g_batch[i].state == ST_WRITE) resp_rewrite_500(&g_batch[i]);
  }
}

static void send_batch(void) {
  for (int i = 0; i < g_nbatch; i++) {
    struct task *t = &g_batch[i];
    struct ring_ctx *r = cid_ring(t->cid);
    uint32_t slot = t->cid & 0xffff;
    struct conn *c = &r->conns[slot];
    if (c->gen != t->gen || c->state != C_OPEN) continue;
    if (!t->resp) {
      conn_close(r, slot);
      continue;
    }
    struct io_uring_sqe *s = sqe_get(r);
    s->opcode = IORING_OP_SEND;
    s->fd = (int)slot;
    s->flags = IOSQE_FIXED_FILE;
    s->addr = (uint64_t)(uintptr_t)t->resp;
    s->len = t->resp_len;
    s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL; /* the kernel finishes a short send itself */
    s->user_data = ud_make(OP_SEND, (unsigned)r->idx, c->gen, t->arena, slot);
    c->send_len = t->resp_len;
    if (!t->keep_alive) c->flags |= CF_CLOSE;
    g_arena[t->arena].inflight++;
  }
}

static void on_signal(int sig) {
  (void)sig;
  _exit(0);
}

static void clock_update(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME_COARSE, &ts);
  g_now_s = ts.tv_sec;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  g_now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  const char *host = getenv("HOST");
  if (!host || !*host) host = "0.0.0.0";
  const char *port_s = getenv("PORT");
  int port = port_s && *port_s ? atoi(port_s) : 3000;
  const char *db_path = getenv("SQLITE_PATH");
  if (!db_path || !*db_path) db_path = "seed/feed.db";
  const char *secret = getenv("JWT_SECRET");
  if (!secret || !*secret) secret = "twelve-dollar-challenge";

  clock_update();
  g_started = g_now_s;
  jwt_hmac_init(&g_jwt, secret);
  g_scratch = mmap(0, SCRATCH_CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_scratch == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  if (!db_init(&g_db, db_path)) {
    fprintf(stderr, "fatal: database setup failed\n");
    return 1;
  }

  g_slots = ring_slot_limit();
  g_depth = g_slots / 4 < ACCEPT_DEPTH ? (g_slots / 4 / NLIS) * NLIS : ACCEPT_DEPTH;
  if (g_depth < NLIS) g_depth = NLIS;
  for (int i = 0; i < NLIS; i++) {
    g_lis[i] = open_listener(host, port);
    if (g_lis[i] < 0) {
      fprintf(stderr, "fatal: cannot listen on %s:%d: %s\n", host, port, strerror(errno));
      return 1;
    }
  }

  static const uint32_t cq_try[] = {CQ_ENTRIES, 32768, 16384, 8192};
  for (int i = 0; i < MAX_RINGS; i++) {
    int ok = 0;
    for (unsigned k = 0; k < 4 && !ok; k++) ok = ring_init(&g_rings[i], i, g_slots, cq_try[k]);
    if (!ok) {
      if (i == 0) {
        fprintf(stderr, "fatal: io_uring setup failed: %s\n", strerror(errno));
        return 1;
      }
      fprintf(stderr, "ring %d: %s\n", i, strerror(errno));
      break;
    }
    g_nrings = i + 1;
  }
  accept_policy();
  fprintf(stderr, "listening on %s:%d: %d listeners, %d rings of %u direct descriptors\n", host, port, NLIS,
          g_nrings, g_slots);

  int last = 0;   /* requests answered by the previous pass */
  unsigned turn = 0;
  for (;;) {
    /* wait */

    int busy = g_ready.n > 0 || g_rq.n > 0;
    int spread = 0; /* connections or accepts on a ring that cannot wake ring 0 */
    for (int i = 1; i < g_nrings; i++) {
      struct ring_ctx *r = &g_rings[i];
      if (ring_unsubmitted(r) || ring_has_work(r) || r->got >= 16) ring_flush(r);
      spread |= r->open || r->acc_inflight;
    }
    struct ring_ctx *r0 = &g_rings[0];
    if (busy) {
      if (ring_unsubmitted(r0) || ring_has_work(r0) || r0->got >= 16) ring_flush(r0);
    } else {
      int timed = spread || last > 0 || g_db.writes_held || db_ckpt_active(&g_db);
      ring_wait(r0, last > 0 ? WAIT_BATCH : 1, timed ? WAIT_NS : IDLE_NS);
    }

    /* collect */
    clock_update();
    g_nbatch = 0;
    g_scratch_off = 0;
    process_ready(); /* requests read by earlier passes go first */
    turn++;
    for (int k = 0; k < g_nrings; k++) reap(&g_rings[(k + turn) % (unsigned)g_nrings]);

    g_reads = 0;
    for (int round = 0; round < 8 && g_rq.n; round++) {
      if (!issue_reads()) break;
      for (int i = 0; i < g_nrings; i++) {
        struct ring_ctx *r = &g_rings[i];
        if (!ring_unsubmitted(r)) continue;
        ring_flush(r);
        reap(r);
      }
    }
    process_ready();

    /* execute, then answer */
    if (g_nbatch) {
      arenas_recycle();
      run_batch();
      send_batch();
    }
    last = g_nbatch;

    if (db_tick(&g_db, g_now_ms)) release_held();
    accept_policy();
  }
  return 0;
}
