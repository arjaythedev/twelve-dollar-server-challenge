/* Linux transport derived from Pavlyhalim 7387a313d58041c76e0fe8134825ef335eeafd0d.
 * Goose owns all connection state, input and scheduling queues. Native code
 * owns kernel rings, SQLite, arenas and asynchronous send tickets. */
#include "native.c"

#define MAX_RINGS 16
#define SWEEP_NS 2000000000ULL
#define VALVE_NS 200000000ULL
#define BATCH_WAIT 64
#define BATCH_WAIT_MS 2
enum { UD_ACCEPT = 1, UD_RECV = 2, UD_SEND = 3, UD_CLOSE = 4, UD_EVFD = 5,
       UD_CANCEL = 6, UD_POLL = 7, UD_SHUTDOWN = 8 };
#define UD(op, ring, slot, gen) \
  (((uint64_t)(op) << 56) | ((uint64_t)(ring) << 48) | ((uint64_t)(slot) << 32) | (uint64_t)(gen))
#define UD_OP(u) ((unsigned)((u) >> 56))
#define UD_RING(u) ((unsigned)((u) >> 48) & 0xff)
#define UD_SLOT(u) ((unsigned)((u) >> 32) & 0xffff)
#define UD_GEN(u) ((uint32_t)(u))
#define CONN_ID(ri, slot) (((uint32_t)(ri) << 16) | (uint32_t)(slot))

struct rs {
  struct ring r;
  int efd;
  unsigned open;
  uint8_t accept_armed, accept_cancelling, populated;
  unsigned reads;
};
static struct rs R[MAX_RINGS];
static unsigned NR, NR_MAX, ring_slots, ring_bufs, ring_buf_size;
static int ring_grow_failed;

void g300_native_poll(uint32_t id, uint32_t gen) {
  unsigned ri = id >> 16, slot = id & 0xffff;
  sqe_poll_conn(ring_sqe(&R[ri].r), slot, UD(UD_POLL, ri, slot, gen));
}
void g300_native_close(uint32_t id, uint32_t gen) {
  unsigned ri = id >> 16, slot = id & 0xffff;
  struct ring *r = &R[ri].r;
  struct io_uring_sqe *s = ring_sqe(r);
  s->opcode = IORING_OP_ASYNC_CANCEL; s->fd = -1;
  s->addr = UD(UD_POLL, ri, slot, gen);
  s->flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
  s->user_data = UD(UD_CANCEL, ri, slot, gen);
  s = ring_sqe(r);
  s->opcode = IORING_OP_SHUTDOWN; s->fd = (int)slot;
  s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
  s->len = SHUT_RDWR;
  /* A failed shutdown must never be mistaken for the final close. */
  s->user_data = UD(UD_SHUTDOWN, ri, slot, gen);
  s = ring_sqe(r);
  s->opcode = IORING_OP_CLOSE; s->file_index = slot + 1;
  s->user_data = UD(UD_CLOSE, ri, slot, gen);
}
uint8_t g300_native_read(uint32_t id, uint32_t gen) {
  unsigned ri = id >> 16, slot = id & 0xffff;
  struct rs *rs = &R[ri];
  if (rs->reads >= rs->r.br_entries) return 0;
  rs->reads++;
  sqe_recv_once(ring_sqe(&rs->r), slot, UD(UD_RECV, ri, slot, gen));
  return 1;
}

/* Tickets exist only while sends are in flight. No borrowed Goose argument is
 * stored. The kernel borrows arena bytes, or immutable native error responses. */
struct send_ticket {
  struct arena *arena;
  const uint8_t *data;
  uint32_t remaining, connection, generation, next;
};
_Static_assert(sizeof(struct send_ticket) == 32, "send ticket layout changed");
static struct send_ticket *tickets;
static uint32_t ticket_cap, ticket_count, ticket_free = UINT32_MAX;
static uint32_t ticket_alloc(void) {
  if (ticket_free != UINT32_MAX) {
    uint32_t id = ticket_free; ticket_free = tickets[id].next; return id;
  }
  if (ticket_count == ticket_cap) {
    ticket_cap = ticket_cap ? ticket_cap * 2 : 2048;
    void *p = realloc(tickets, (size_t)ticket_cap * sizeof *tickets);
    if (!p) allocation_failed();
    tickets = p;
  }
  return ticket_count++;
}
static void ticket_submit(uint32_t id) {
  struct send_ticket *t = &tickets[id];
  unsigned ri = t->connection >> 16, slot = t->connection & 0xffff;
  sqe_send(ring_sqe(&R[ri].r), slot, t->data, t->remaining, ((uint64_t)UD_SEND << 56) | id);
}
void g300_native_send(uint32_t index, uint32_t gen, uint32_t arena,
                       uint32_t offset, uint32_t len, uint8_t error) {
  uint32_t id = ticket_alloc();
  struct send_ticket *t = &tickets[id];
  t->arena = NULL; t->connection = index; t->generation = gen;
  if (error) { t->data = static_response(error, &len); }
  else {
    if (!arena || arena >= arena_ids_cap || !arena_ids[arena]) abort();
    t->arena = arena_ids[arena];
    if (offset > t->arena->used || len > t->arena->used - offset) abort();
    t->arena->refs++;
    t->data = t->arena->data + offset;
  }
  t->remaining = len;
  ticket_submit(id);
}
static void on_send(unsigned ri, struct io_uring_cqe *cqe) {
  (void)ri;
  uint32_t id = (uint32_t)cqe->user_data;
  if (id >= ticket_count) abort();
  struct send_ticket *t = &tickets[id];
  int failed = cqe->res <= 0;
  if (cqe->res > 0 && (uint32_t)cqe->res < t->remaining) {
    t->data += cqe->res; t->remaining -= (uint32_t)cqe->res;
    if (g300_send_live(t->connection, t->generation)) { ticket_submit(id); return; }
    failed = 1;
  } else if ((cqe->res == -EINTR || cqe->res == -EAGAIN) &&
             g300_send_live(t->connection, t->generation)) {
    ticket_submit(id); return;
  }
  uint32_t index = t->connection, gen = t->generation;
  if (t->arena) arena_unref(t->arena);
  t->arena = NULL; t->data = NULL;
  t->next = ticket_free; ticket_free = id;
  g300_sent(index, gen, (uint8_t)failed);
}

/* Accepting. NLISTEN listening sockets share the port (SO_REUSEPORT); the kernel spreads new
 * connections over them, and each has its own accept queue of somaxconn (4,096 on the box), so
 * together they hold ~32k connections through a stall instead of 4k (one listener overflowed 1.8M
 * times in a run with stalls, and every overflow is a SYN the client resends after 1 s, then 3 s;
 * c-iouring-public #158 and Lean #153 run 8). Their multishot accepts are armed only on the ring
 * being filled, and on the next one once the first is within ACCEPT_NEXT slots of full, so
 * connections fill the rings in order and only as many rings are busy as the connection count needs.
 * A ring's accepts are cancelled ACCEPT_STOP slots before its direct-descriptor table is full: an
 * accept that finds the table full makes the kernel reset that client. */
#define ACCEPT_NEXT 8192
#define ACCEPT_STOP 4096 /* the kernel keeps accepting while the loop is busy or stalled: 4096 covers
                          * ~0.8 s at 5,000 new connections a second, for 32k of 523k slots */

#define NLISTEN 8
#define ALL_LISTENERS ((1u << NLISTEN) - 1)
static int LFD[NLISTEN];

/* Goose initializes the connection table when the ring is created. Touch the
 * native receive buffers when the first accept is armed. Uncreated rings cost nothing. */
static void ring_populate(struct rs *rs) {
  size_t nb = (size_t)rs->r.br_entries * rs->r.buf_size;
  prefault(rs->r.bufs, nb);
  rs->populated = 1;
}

/* A ring is made when connections first need it (ring 0 at startup, each later one ACCEPT_NEXT
 * connections before the one before it is full), so rings that are never used cost no kernel
 * memory. Newer kernels than the box's 6.8 charge ring memory to RLIMIT_MEMLOCK (8 MB under
 * systemd): made up front, eight rings with 65,536-entry completion queues did not fit, and CI's
 * runner refused the seventh. The CQ holds one completion per connection on the ring, so a burst
 * of completions never goes to the kernel's overflow list, allocated per completion just when
 * memory is shortest; on ENOMEM from io_uring_setup (nothing allocated yet) the ring is made with
 * half the entries, down to 8,192. */
static int ring_create(unsigned i) {
  const char *err = "";
  unsigned cq = 65536;
  while (ring_init(&R[i].r, 4096, cq, ring_slots, ring_bufs, ring_buf_size, i == 0, &err) < 0) {
    if (errno != ENOMEM || strcmp(err, "io_uring_setup") || cq <= 8192) {
      fprintf(stderr, "ring %u: %s: %s\n", i, err, strerror(errno));
      return -1;
    }
    cq /= 2;
  }
  g300_ring_created(i);
  if (i > 0) {
    R[i].efd = ring_register_eventfd(&R[i].r);
    if (R[i].efd < 0) { perror("register eventfd"); return -1; }
    sqe_poll_multishot(ring_sqe(&R[0].r), R[i].efd, UD(UD_EVFD, i, 0, 0));
  }
  return 0;
}

static void arm_accept(struct rs *rs, unsigned ri) {
  if (!rs->populated) ring_populate(rs);
  for (unsigned l = 0; l < NLISTEN; l++)
    if (!(rs->accept_armed & (1u << l))) {
      sqe_accept_multishot(ring_sqe(&rs->r), LFD[l], UD(UD_ACCEPT, ri, l, 0));
      rs->accept_armed |= (uint8_t)(1u << l);
    }
}

static void accept_policy(void) {
  if (NR < NR_MAX && !ring_grow_failed && R[NR - 1].open + ACCEPT_NEXT >= R[NR - 1].r.nslots) {
    if (ring_create(NR) == 0) NR++;
    else ring_grow_failed = 1; /* stay at NR rings; the last one's accepts stop ACCEPT_STOP short of full */
  }
  unsigned lowest = NR;
  for (unsigned i = 0; i < NR; i++)
    if (R[i].open + ACCEPT_STOP < R[i].r.nslots) { lowest = i; break; }
  for (unsigned i = 0; i < NR; i++) {
    struct rs *rs = &R[i];
    int room = rs->open + ACCEPT_STOP < rs->r.nslots;
    int want = room && (i == lowest || (i == lowest + 1 && R[lowest].open + ACCEPT_NEXT >= R[lowest].r.nslots));
    if (want && rs->accept_armed != ALL_LISTENERS) {
      arm_accept(rs, i);
    } else if (!room) {
      for (unsigned l = 0; l < NLISTEN; l++)
        if ((rs->accept_armed & ~rs->accept_cancelling) & (1u << l)) {
          sqe_cancel(ring_sqe(&rs->r), UD(UD_ACCEPT, i, l, 0), UD(UD_CANCEL, i, 0, 0));
          rs->accept_cancelling |= (uint8_t)(1u << l);
        }
    }
  }
}

/* The memory valve. Past ~300k users the box runs out of memory and the kernel kills the server:
 * each connection costs ~4 KB of kernel memory, almost all of it the socket itself, which no
 * setting shrinks. When the memory the kernel could still hand out runs low, a share of responses
 * carries "Connection: close". The client reads its answer and dials again for
 * its next request, after its think time, so no request fails, and for those seconds the socket does
 * not exist. The share follows the headroom: none above VALVE_HI_MB, every response at VALVE_LO_MB.
 * Headroom is MemAvailable minus SReclaimable, as c-iouring-public (#158) measured on the box: the
 * sockets' own dentries and inodes are counted as reclaimable but are pinned while the socket is
 * open. Free memory plus page cache read 160-180 MB when a VM froze for lack of memory.
 * It rises at once and falls by at most VALVE_RELEASE a second: memory comes back seconds after the
 * closes, and dropping straight to zero would let it run out again (it swung 45% -> 0% that way).
 * Sampled every 200 ms; while the headroom falls the share follows where it will be in VALVE_AHEAD_S
 * at the current rate (near the wall it went from 400 MB to the OOM kill in seconds).
 * The cost is a TCP handshake per closed response, and while the old socket finishes closing the
 * client's new one is already open, so near the wall a wave of closes adds sockets before it removes
 * any. With 1,000 MB locked in the test VM, the valve on closed 17k connections in 10 s and the
 * server was OOM-killed at ~59k; the valve off held to ~144k. c-iouring-public (#158) found the same
 * (reconnects cost more than they saved), so it is off unless VALVE=1. */
#define VALVE_HI_MB 400
#define VALVE_LO_MB 200
#define VALVE_RELEASE (UINT32_MAX / 200) /* per 200 ms sample: 2.5 points a second */
#define VALVE_AHEAD_S 2.0
static int meminfo_fd = -1;
static int valve_on; /* VALVE=1 turns it on */
static uint32_t valve_p;   /* probability that a response closes, in 1/2^32 */
static uint32_t valve_rng = 2463534242u;
static long headroom_mb = -1;
static double headroom_slope; /* MB/s, smoothed */

static long meminfo_kb(const char *buf, const char *key) {
  const char *p = strstr(buf, key);
  return p ? atol(p + strlen(key)) : 0;
}

static void valve_update(void) {
  char buf[4096];
  ssize_t n = meminfo_fd < 0 ? -1 : pread(meminfo_fd, buf, sizeof buf - 1, 0);
  if (n <= 0) return;
  buf[n] = 0;
  long prev = headroom_mb;
  headroom_mb = (meminfo_kb(buf, "MemAvailable:") - meminfo_kb(buf, "SReclaimable:")) >> 10;
  if (prev >= 0) headroom_slope = 0.7 * headroom_slope + 0.3 * (double)(headroom_mb - prev) * (1e9 / VALVE_NS);
  double h = (double)headroom_mb;
  if (headroom_slope < 0) h += headroom_slope * VALVE_AHEAD_S;
  uint32_t want;
  if (!valve_on || h >= VALVE_HI_MB) want = 0;
  else if (h <= VALVE_LO_MB) want = UINT32_MAX;
  else want = (uint32_t)((VALVE_HI_MB - h) / (VALVE_HI_MB - VALVE_LO_MB) * UINT32_MAX);
  if (want >= valve_p) valve_p = want;
  else valve_p = valve_p - want > VALVE_RELEASE ? valve_p - VALVE_RELEASE : want;
}

static inline int valve_close(void) {
  if (!valve_p) return 0;
  valve_rng ^= valve_rng << 13; /* xorshift32 */
  valve_rng ^= valve_rng >> 17;
  valve_rng ^= valve_rng << 5;
  return valve_rng < valve_p;
}

uint8_t g300_native_valve(void) {
  return (uint8_t)valve_close();
}

static void on_accept(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  if (!(cqe->flags & IORING_CQE_F_MORE)) { /* this listener's accept on this ring has ended */
    uint8_t bit = (uint8_t)(1u << UD_SLOT(cqe->user_data));
    rs->accept_armed &= (uint8_t)~bit;
    rs->accept_cancelling &= (uint8_t)~bit;
  }
  if (cqe->res < 0) {
    if (cqe->res != -ENFILE && cqe->res != -EMFILE && cqe->res != -ECANCELED)
      fprintf(stderr, "accept ring %u: %s\n", ri, strerror(-cqe->res));
    return;
  }
  unsigned slot = (unsigned)cqe->res;
  if (slot >= rs->r.nslots) { fprintf(stderr, "accept: slot %u out of range\n", slot); return; }
  if (!g300_accept(CONN_ID(ri, slot))) { fprintf(stderr, "accept: slot %u still open\n", slot); return; }
  rs->open++;
}


static void on_poll(unsigned ri, struct io_uring_cqe *cqe) {
  g300_poll(CONN_ID(ri, UD_SLOT(cqe->user_data)), UD_GEN(cqe->user_data), cqe->res,
            (cqe->flags & IORING_CQE_F_MORE) != 0);
}
static void on_recv(unsigned ri, struct io_uring_cqe *cqe) {
  struct rs *rs = &R[ri];
  int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
  unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  if (has_buf) rs->r.bufs_out++;
  if (!rs->reads) abort();
  rs->reads--;
  if (cqe->res > 0 && (!has_buf || (unsigned)cqe->res > rs->r.buf_size)) abort();
  sl_u8 data = {has_buf ? ring_buf_ptr(&rs->r, bid) : NULL, cqe->res > 0 ? cqe->res : 0};
  g300_receive(CONN_ID(ri, UD_SLOT(cqe->user_data)), UD_GEN(cqe->user_data), cqe->res,
               data, (unsigned)cqe->res == rs->r.buf_size);
  if (has_buf) ring_buf_return(&rs->r, bid);
}
static void on_close(unsigned ri, struct io_uring_cqe *cqe) {
  if (g300_closed(CONN_ID(ri, UD_SLOT(cqe->user_data)), UD_GEN(cqe->user_data), cqe->res)) {
    if (!R[ri].open) abort();
    R[ri].open--;
  }
}

/* A ring's eventfd fired. Its only job is to end ring 0's wait: the ring's completions are read
 * every batch anyway. The counter is not drained: a multishot poll posts one completion per
 * wake-up of the eventfd, not per level, so reading it back would be a system call for nothing
 * (and the 64-bit counter cannot overflow at a few thousand signals a second). */
static void on_evfd(struct io_uring_cqe *cqe) {
  unsigned ri = UD_RING(cqe->user_data);
  struct rs *rs = &R[ri];
  if (!(cqe->flags & IORING_CQE_F_MORE)) sqe_poll_multishot(ring_sqe(&R[0].r), rs->efd, UD(UD_EVFD, ri, 0, 0));
}

static unsigned reap(unsigned ri) {
  struct ring *r = &R[ri].r;
  unsigned head = LOAD_RLX(r->cq_head), tail = LOAD_ACQ(r->cq_tail), n = 0;
  while (head != tail) {
    struct io_uring_cqe *cqe = &r->cqes[head & r->cq_mask];
    switch (UD_OP(cqe->user_data)) {
    case UD_RECV: on_recv(ri, cqe); break;
    case UD_POLL: on_poll(ri, cqe); break;
    case UD_SEND: on_send(ri, cqe); break;
    case UD_ACCEPT: on_accept(ri, cqe); break;
    case UD_CLOSE: on_close(ri, cqe); break;
    case UD_EVFD: on_evfd(cqe); break;
    default: break;
    }
    head++;
    n++;
    if ((n & 255) == 0) {
      /* hand back the CQ slots and the receive buffers already consumed now, not at the end of the
       * batch: a burst bigger than the buffer ring makes the kernel end multishot receives with
       * ENOBUFS, every one of them then needs re-arming, and the longer turn lets a bigger burst in */
      STORE_REL(r->cq_head, head);
      ring_buf_publish(r);
    }
  }
  STORE_REL(r->cq_head, head);
  return n;
}

static long env_long(const char *name, long def) {
  const char *v = getenv(name);
  return v && *v ? atol(v) : def;
}

/* Build time only: a synthetic database for PGO training (random data, none of the seed). */
static int make_train_db(const char *path) {
  long users = env_long("TRAIN_USERS", 5000), posts = env_long("TRAIN_POSTS", 40000), likes = env_long("TRAIN_LIKES", 160000);
  sqlite3_initialize();
  unlink(path);
  sqlite3 *c;
  if (sqlite3_open_v2(path, &c, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) return 1;
  const char *schema =
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
      "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
      "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
      "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), PRIMARY KEY (user_id, post_id));"
      "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
      "CREATE INDEX posts_user_id_idx ON posts (user_id);"
      "CREATE INDEX likes_post_id_idx ON likes (post_id);"
      "BEGIN;";
  if (sqlite3_exec(c, schema, NULL, NULL, NULL) != SQLITE_OK) { fprintf(stderr, "%s\n", sqlite3_errmsg(c)); return 1; }
  sqlite3_stmt *su, *sp, *sl;
  sqlite3_prepare_v2(c, "INSERT INTO users (id, username) VALUES (?1, ?2)", -1, &su, NULL);
  sqlite3_prepare_v2(c, "INSERT INTO posts (id, user_id, body, created_at) VALUES (?1, ?2, ?3, ?4)", -1, &sp, NULL);
  sqlite3_prepare_v2(c, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)", -1, &sl, NULL);
  srand(4242);
  char buf[600];
  for (long i = 1; i <= users; i++) {
    snprintf(buf, sizeof buf, "user_%ld", i);
    sqlite3_bind_int64(su, 1, i);
    sqlite3_bind_text(su, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(su);
    sqlite3_reset(su);
  }
  static const char *words[] = {"server", "latency", "cheap", "sqlite", "index", "cache", "keepalive", "droplet", "batch", "kernel"};
  for (long i = 1; i <= posts; i++) {
    int n = 0, nw = 5 + rand() % 30;
    for (int w = 0; w < nw && n < 480; w++) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s", w ? " " : "", words[rand() % 10]);
    long t = 1704067200L + i * 60;
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
    char created[40];
    snprintf(created, sizeof created, "%s.%03dZ", ts, rand() % 1000);
    sqlite3_bind_int64(sp, 1, i);
    sqlite3_bind_int64(sp, 2, 1 + rand() % users);
    sqlite3_bind_text(sp, 3, buf, n, SQLITE_TRANSIENT);
    sqlite3_bind_text(sp, 4, created, -1, SQLITE_TRANSIENT);
    sqlite3_step(sp);
    sqlite3_reset(sp);
  }
  for (long i = 0; i < likes; i++) {
    sqlite3_bind_int64(sl, 1, 1 + rand() % users);
    sqlite3_bind_int64(sl, 2, 1 + rand() % posts);
    sqlite3_step(sl);
    sqlite3_reset(sl);
  }
  sqlite3_finalize(su); sqlite3_finalize(sp); sqlite3_finalize(sl);
  if (sqlite3_exec(c, "COMMIT; ANALYZE; PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL) != SQLITE_OK) return 1;
  sqlite3_close(c);
  return 0;
}

/* exit() rather than the default action, so the listeners close at once and, in the
 * instrumented build, the profile is written */
static void on_term(int sig) {
  (void)sig;
  exit(0);
}

/* Keeps the server's code and data in RAM. They are file-backed, so when memory runs short the
 * kernel evicts them like any cached file (with no swap, file pages are all reclaim can take, and
 * Ubuntu's multi-generational LRU gives code pages no extra pass), and the event loop then stalls
 * on major faults that read its own code back from disk while every connection's next request
 * waits in the kernel. The binary is static, so its own mappings are all of it; locking the
 * writable one turns its untouched pages into private memory. Best effort: if RLIMIT_MEMLOCK
 * (8 MB under systemd) is too small, the server runs unlocked. Returns bytes locked. */
static size_t lock_code(void) {
  char exe[512];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  FILE *f = fopen("/proc/self/maps", "r");
  if (n <= 0 || !f) {
    if (f) fclose(f);
    return 0;
  }
  exe[n] = 0;
  char line[1024];
  size_t locked = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long lo, hi;
    char perms[8];
    int path = 0;
    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &path) < 3 || !path) continue;
    line[strcspn(line, "\n")] = 0;
    if (strcmp(line + path, exe)) continue;
    if (mlock((void *)lo, hi - lo) == 0) locked += hi - lo;
    else fprintf(stderr, "mlock %s: %s\n", perms, strerror(errno));
  }
  fclose(f);
  return locked;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  setvbuf(stderr, NULL, _IOLBF, 0);
  if (getenv("MAKE_TRAIN_DB")) return make_train_db(getenv("MAKE_TRAIN_DB"));
  goose_init(0, NULL);
  const char *host = getenv("HOST"), *port = getenv("PORT"), *db_path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3000";
  if (!db_path || !*db_path || !secret || !*secret) {
    fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
    return 1;
  }

  /* file descriptors: raise the soft limit; each ring's direct table may hold at most that many */
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    getrlimit(RLIMIT_NOFILE, &rl);
  }
  unsigned nslots = rl.rlim_cur > 65535 ? 65535 : (unsigned)rl.rlim_cur;
  if (nslots > 256) nslots -= 64; /* leave room for the process's ordinary descriptors */
  long want_conns = env_long("MAX_CONNS", 500000);
  NR_MAX = (unsigned)((want_conns + nslots - 1) / nslots);
  if (NR_MAX < 1) NR_MAX = 1;
  if (NR_MAX > MAX_RINGS) NR_MAX = MAX_RINGS;
  /* 4,096 buffers of 1 KB per ring: a request is 150-600 bytes, and the count, not the size, is what
   * a burst runs out of (a larger request spans buffers and goes through pend) */
  unsigned nbufs = (unsigned)env_long("RECV_BUFS", 4096), bufsz = (unsigned)env_long("RECV_BUF_SIZE", 1024);
  long long mmap_bytes = env_long("MMAP_MB", 0) << 20;
  /* 8 MB of SQLite page cache: memory the kernel can't reclaim, so every MB is ~240 sockets at the
   * wall. Misses are 0.053 a request at 8 MB (0.0017 at 32 MB), each a read from the page cache */
  int cache_kib = (int)env_long("CACHE_KIB", 8192);

  if (app_init(&A, db_path, secret, mmap_bytes, cache_kib) < 0) return 1;

  int backlog = (int)env_long("BACKLOG", 4096);
  {
    const char *err = "";
    for (unsigned l = 0; l < NLISTEN; l++) {
      LFD[l] = listen_socket(host, atoi(port), backlog, &err);
      if (LFD[l] < 0) {
        fprintf(stderr, "listen %s:%s: %s: %s\n", host, port, err, strerror(errno));
        return 1;
      }
    }
  }
  ring_slots = nslots;
  ring_bufs = nbufs;
  ring_buf_size = bufsz;
  if (ring_create(0) < 0) return 1;
  NR = 1;
  (void)lock_code();
  prewarm_batch_memory();
  prefault_stack(256 << 10);
  accept_policy();

  valve_on = env_long("VALVE", 0) != 0;
  if (valve_on) {
    meminfo_fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    valve_update();
  }
  uint64_t t0 = now_ns(), last_sweep = t0, last_valve = t0;
  unsigned last_cqes = 0;
  for (;;) {
    /* 1. publish the other rings' SQEs (sends, re-arms, closes) without waiting */
    for (unsigned i = 1; i < NR; i++)
      if (ring_pending_submit(&R[i].r)) ring_submit(&R[i].r, 0, 0, NULL, 0);

    /* 2. wait on ring 0. The other rings' eventfds are polled from ring 0, so a completion
     * anywhere ends the wait. */
    unsigned min = last_cqes >= 8 ? BATCH_WAIT : 1;
    unsigned ms = (last_cqes >= 8 || g300_busy()) ? BATCH_WAIT_MS : 1000;
    long rc = ring_wait(&R[0].r, min, ms);
    if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY) {
      fprintf(stderr, "io_uring_enter: %s\n", strerror((int)-rc));
      return 1;
    }

    /* 3. the batch */
    uint64_t t = now_ns();
    uint32_t now_s = (uint32_t)(t / 1000000000ULL);
    A.now = (int64_t)time(NULL);
    int writes_ok = db_writes_allowed(&A.db);
    /* Rings 1..N do not defer their task work, so the kernel has already posted their
     * completions into their queues while this thread was in the wait above: they are read
     * straight from shared memory, without entering those rings (that was 4.7 system calls
     * per batch with all 8 rings busy). */
    g300_turn(now_s, (uint8_t)writes_ok);
    unsigned ncq = reap(0);
    for (unsigned i = 1; i < NR; i++) ncq += reap(i);
    /* Reads for the connections the polls just reported, issued and completed now: a MSG_DONTWAIT
     * read finishes during the submit, so its request is answered in the batch that saw it ready.
     * Issued at the end of the batch instead, it waited for the next one, whose wait for 64
     * completions then held it up to 2 ms more (p95 4.7 against 2.7 ms at 20k users). */
    if (g300_issue_reads()) {
      for (unsigned i = 0; i < NR; i++)
        if (ring_pending_submit(&R[i].r)) ring_submit(&R[i].r, 0, 0, NULL, 0);
      for (unsigned i = 0; i < NR; i++) ncq += reap(i);
    }
    g300_run_lists();
    int failed = db_commit(&A.db) < 0;
    g300_flush((uint8_t)failed);
    for (unsigned i = 0; i < NR; i++) {
      struct rs *rs = &R[i];
      ring_buf_publish(&rs->r);
    }
    g300_rearm();
    accept_policy();
    last_cqes = ncq;

    /* 4. housekeeping */
    if (t - last_sweep > SWEEP_NS) {
      g300_sweep();
      last_sweep = t;
    }
    if (valve_on && t - last_valve > VALVE_NS) { valve_update(); last_valve = t; }
  }
}
