#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <liburing.h>

#include "src/app.hpp"

namespace {

constexpr unsigned kGetEvents = IORING_ENTER_GETEVENTS;
constexpr uint64_t kOpAccept = 1;
constexpr uint64_t kOpRecv = 2;
constexpr uint64_t kOpSend = 3;
constexpr uint64_t kOpShutdown = 4;
constexpr uint64_t kOpClose = 5;

constexpr uint64_t kOpRead = 6;

constexpr uint32_t kAcceptDepth = 512;

constexpr size_t kListeners = 8;

constexpr unsigned kSqEntries = 4096;
constexpr unsigned kCqEntries = 65536;

constexpr size_t kBufSize = 2048;
constexpr uint16_t kBufCount = 1024;

constexpr size_t kReadsPerPass = 16384;
constexpr uint32_t kAnswersPerPass = 2048;
constexpr size_t kMaxRings = 16;

constexpr unsigned kWaitMin = 256;
constexpr uint32_t kWaitNs = 1000000;

using app::ByteBuf;

struct BufRing {
  uint8_t* ring = nullptr;
  uint16_t tail = 0;
  uint8_t* mem = nullptr;
};

struct Conn {
  uint16_t gen = 0;
  bool open = false;
  bool close_after = false;
  bool held = false;
  bool eof = false;
  bool closing = false;
  bool sending = false;
  bool queued = false;
  bool recving = false;
  bool ready = false;
  bool in_batch = false;
  bool answered = false;
  uint32_t batch_mark = 0;
  uint32_t sent = 0;
  ByteBuf* out = nullptr;
  ByteBuf* wire = nullptr;
  std::vector<uint8_t>* partial = nullptr;
};

static_assert(sizeof(Conn) == 48);
static_assert(sizeof(ByteBuf) == 16);

struct Ring {
  io_uring uring{};
  BufRing bufs;
  std::vector<Conn> conns;

  bool accepting = false;

  uint32_t accepts[kListeners] = {};
  uint32_t depth = 0;

  bool full = false;
  uint32_t open = 0;

  uint32_t margin = 0;

  uint32_t reads = 0;
};

struct Triple {
  uint8_t ri;
  uint32_t slot;
  uint16_t gen;
};

struct Server {
  std::vector<Ring*> rings;
  app::App* app = nullptr;
  std::vector<int> listen;
  uint32_t slots = 0;
  app::Pool pool;
  std::vector<std::pair<uint8_t, uint32_t>> touched;

  std::vector<Triple> held;

  std::deque<Triple> rq;

  std::deque<Triple> ready;

  uint32_t answers_left = 0;
  bool need_ring = false;

  std::chrono::steady_clock::time_point spill_after;
};

Server S;

inline uint64_t ud(uint64_t op, size_t ring, uint16_t gen, uint32_t slot) {
  return op << 56 | static_cast<uint64_t>(ring) << 48 | static_cast<uint64_t>(gen) << 32 | slot;
}

bool register_buf_ring(int ring_fd, void* addr) {
  struct io_uring_buf_reg reg {};
  reg.ring_addr = reinterpret_cast<uint64_t>(addr);
  reg.ring_entries = kBufCount;
  reg.bgid = 0;
  reg.flags = 0;
  int rc = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    rc = static_cast<int>(
        syscall(SYS_io_uring_register, ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1));
    if (rc == 0 || errno != EINVAL) break;
    reg.resv[0] = 1;
  }
  return rc == 0;
}

bool bufring_create(BufRing& b, int ring_fd) {
  const size_t bytes = static_cast<size_t>(kBufCount) * 16;
  void* ring = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ring == MAP_FAILED) return false;
  b.ring = static_cast<uint8_t*>(ring);
  b.mem = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(kBufCount) * kBufSize));
  if (!b.mem) return false;
  if (!register_buf_ring(ring_fd, b.ring)) return false;
  for (uint16_t i = 0; i < kBufCount; i++) {
    auto* e = reinterpret_cast<struct io_uring_buf*>(b.ring + (b.tail & (kBufCount - 1)) * 16);
    e->addr = reinterpret_cast<uint64_t>(b.mem + static_cast<size_t>(i) * kBufSize);
    e->len = static_cast<uint32_t>(kBufSize);
    e->bid = i;
    b.tail = static_cast<uint16_t>(b.tail + 1);
  }
  __atomic_store_n(reinterpret_cast<uint16_t*>(b.ring + 14), b.tail, __ATOMIC_RELEASE);
  return true;
}

inline void bufring_add(BufRing& b, uint16_t bid) {
  auto* e = reinterpret_cast<struct io_uring_buf*>(b.ring + (b.tail & (kBufCount - 1)) * 16);
  e->addr = reinterpret_cast<uint64_t>(b.mem + static_cast<size_t>(bid) * kBufSize);
  e->len = static_cast<uint32_t>(kBufSize);
  e->bid = bid;
  b.tail = static_cast<uint16_t>(b.tail + 1);
}

inline void bufring_publish(BufRing& b) {
  __atomic_store_n(reinterpret_cast<uint16_t*>(b.ring + 14), b.tail, __ATOMIC_RELEASE);
}

inline const uint8_t* bufring_get(const BufRing& b, uint16_t bid) {
  return b.mem + static_cast<size_t>(bid) * kBufSize;
}

io_uring_sqe* get_sqe(Ring& r, unsigned room) {
  for (unsigned tries = 0;; tries++) {
    if (io_uring_sq_space_left(&r.uring) >= room) {
      io_uring_sqe* sqe = io_uring_get_sqe(&r.uring);
      if (sqe) {
        std::memset(sqe, 0, sizeof *sqe);
        return sqe;
      }
    }
    io_uring_submit(&r.uring);
    io_uring_get_events(&r.uring);
    if (tries == 1000) {
      std::fprintf(stderr, "submission queue stuck full; dropping an entry\n");
      return nullptr;
    }
  }
}

void enter_ring(Ring& r, unsigned flags) {

  io_uring_submit(&r.uring);
  if (flags & IORING_ENTER_GETEVENTS) io_uring_get_events(&r.uring);
}

void arm_recv(Ring& r, size_t ri, uint32_t slot, uint16_t gen) {
  io_uring_sqe* sqe = get_sqe(r, 1);
  if (!sqe) return;
  io_uring_prep_recv_multishot(sqe, static_cast<int>(slot), nullptr, 0, 0);
  sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  sqe->buf_group = 0;
  sqe->user_data = ud(kOpRecv, ri, gen, slot);
}

void arm_read(Ring& r, size_t ri, uint32_t slot, uint16_t gen) {
  io_uring_sqe* sqe = get_sqe(r, 1);
  if (!sqe) return;
  io_uring_prep_recv(sqe, static_cast<int>(slot), nullptr, 0, MSG_DONTWAIT);
  sqe->flags |= IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
  sqe->buf_group = 0;
  sqe->user_data = ud(kOpRead, ri, gen, slot);
}

void arm_send(Ring& r, size_t ri, uint32_t slot, const Conn& c) {
  const ByteBuf& wire = *c.wire;
  io_uring_sqe* sqe = get_sqe(r, 1);
  if (!sqe) return;
  io_uring_prep_send(sqe, static_cast<int>(slot), wire.p + c.sent, wire.len - c.sent,
                     MSG_NOSIGNAL);
  sqe->flags |= IOSQE_FIXED_FILE;
  sqe->user_data = ud(kOpSend, ri, c.gen, slot);
}

void arm_close(Ring& r, size_t ri, uint32_t slot, Conn& c) {
  c.closing = true;

  io_uring_sqe* s = get_sqe(r, 2);
  if (!s) return;
  io_uring_prep_shutdown(s, static_cast<int>(slot), SHUT_RDWR);
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  s->user_data = ud(kOpShutdown, ri, c.gen, slot);
  io_uring_sqe* cl = get_sqe(r, 1);
  if (!cl) return;

  io_uring_prep_close_direct(cl, slot);
  cl->user_data = ud(kOpClose, ri, c.gen, slot);
}

void fill_accepts(Ring& r, size_t ri) {
  const uint32_t per = r.depth / kListeners;
  for (size_t li = 0; li < kListeners; li++) {
    while (r.accepts[li] < per) {
      io_uring_sqe* sqe = get_sqe(r, 1);
      if (!sqe) return;
      io_uring_prep_accept_direct(sqe, S.listen[li], nullptr, nullptr, 0,
                                  IORING_FILE_INDEX_ALLOC);
      sqe->user_data = ud(kOpAccept, ri, 0, static_cast<uint32_t>(li));
      r.accepts[li]++;
    }
  }
}

void arm_accept(Ring& r, size_t ri) {
  r.accepting = true;
  fill_accepts(r, ri);
}

bool ring_create(Ring& r, uint32_t slots) {
  struct io_uring_params p {};
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_CQSIZE;
  p.cq_entries = kCqEntries;
  int rc = io_uring_queue_init_params(kSqEntries, &r.uring, &p);
  if (rc < 0) {
    std::fprintf(stderr, "io_uring_queue_init: %s\n", std::strerror(-rc));
    return false;
  }
  rc = io_uring_register_files_sparse(&r.uring, slots);
  if (rc < 0) {
    std::fprintf(stderr, "io_uring_register_files_sparse(%u): %s\n", slots, std::strerror(-rc));
    return false;
  }
  if (!bufring_create(r.bufs, r.uring.ring_fd)) {
    std::fprintf(stderr, "provided-buffer ring registration failed\n");
    return false;
  }
  r.conns.resize(slots);

  uint32_t depth = slots / 16;
  if (depth < kListeners) depth = static_cast<uint32_t>(kListeners);
  if (depth > kAcceptDepth) depth = kAcceptDepth;
  depth = depth / static_cast<uint32_t>(kListeners) * static_cast<uint32_t>(kListeners);
  r.depth = depth;

  const uint32_t cand = slots / 8;
  const uint32_t lo = depth + 1, hi = 2 * kAcceptDepth;
  uint32_t margin = cand < lo ? lo : (cand > hi ? hi : cand);
  if (margin > slots / 4) margin = slots / 4;
  r.margin = margin;
  return true;
}

ByteBuf& out_of(Conn& c) {
  if (!c.out) c.out = app::take_buf(S.pool);
  return *c.out;
}

void mark_touched(size_t ri, uint32_t slot, Conn& c) {
  if (!c.in_batch) {
    c.in_batch = true;
    c.batch_mark = c.out ? c.out->len : 0;
    S.touched.emplace_back(static_cast<uint8_t>(ri), slot);
  }
}

void queue_read(size_t ri, uint32_t slot, Conn& c) {
  if (!c.queued) {
    c.queued = true;
    S.rq.push_back(Triple{static_cast<uint8_t>(ri), slot, c.gen});
  }
}

void conn_finish(size_t ri, uint32_t slot, Conn& c) {
  if (c.held || c.ready) {
    c.eof = true;
  } else {
    c.close_after = true;
    mark_touched(ri, slot, c);
  }
}

void stash(Conn& c, const uint8_t* data, size_t n) {
  if (n == 0) return;
  if (!c.partial) c.partial = new std::vector<uint8_t>();
  c.partial->insert(c.partial->end(), data, data + n);
}

void serve(size_t ri, uint32_t slot, Conn& c, const uint8_t* data, size_t datalen);

void received(size_t ri, uint32_t slot, Conn& c, const uint8_t* data, size_t n) {
  if (c.close_after) {

  } else if (c.held || c.ready || S.answers_left == 0) {
    stash(c, data, n);
    if (!c.held && !c.ready) {
      c.ready = true;
      S.ready.push_back(Triple{static_cast<uint8_t>(ri), slot, c.gen});
    }
  } else {
    S.answers_left--;
    serve(ri, slot, c, data, n);
  }
}

void serve(size_t ri, uint32_t slot, Conn& c, const uint8_t* data, size_t datalen) {
  if (c.close_after) return;
  std::vector<uint8_t> partial;
  bool used_partial = false;
  if (c.partial) {
    partial = std::move(*c.partial);
    delete c.partial;
    c.partial = nullptr;
    used_partial = !partial.empty();
  }
  const uint8_t* buf;
  size_t buflen;
  if (partial.empty()) {
    buf = data ? data : reinterpret_cast<const uint8_t*>("");
    buflen = datalen;
  } else {
    if (datalen) partial.insert(partial.end(), data, data + datalen);
    buf = partial.data();
    buflen = partial.size();
  }
  size_t off = 0;
  for (;;) {
    app::http::Request req;
    size_t used = 0;
    app::http::Parsed pr = app::http::parse(buf + off, buflen - off, req, used);
    if (pr == app::http::Parsed::Done) {
      if (req.method_len == 4 && std::memcmp(req.method, "POST", 4) == 0 &&
          S.app->db.writes_held()) {
        if (!c.held) {
          c.held = true;
          S.held.push_back(Triple{static_cast<uint8_t>(ri), slot, c.gen});
        }
        break;
      }
      mark_touched(ri, slot, c);
      c.answered = true;
      S.app->handle(req, out_of(c));
      off += used;
      if (off == buflen) break;
    } else if (pr == app::http::Parsed::Partial) {
      break;
    } else {
      mark_touched(ri, slot, c);
      c.answered = true;
      app::error(out_of(c), 400, "bad request");
      c.close_after = true;
      off = buflen;
      break;
    }
  }
  const size_t rest = buflen - off;
  if (rest != 0) {
    if (!used_partial) {
      c.partial = new std::vector<uint8_t>(buf + off, buf + buflen);
    } else {
      partial.erase(partial.begin(), partial.begin() + static_cast<ptrdiff_t>(off));
      c.partial = new std::vector<uint8_t>(std::move(partial));
    }
  }
}

size_t drain(size_t ri) {
  Ring& r = *S.rings[ri];
  size_t n = 0;
  unsigned head = 0;
  struct io_uring_cqe* cqe = nullptr;
  io_uring_for_each_cqe(&r.uring, head, cqe) {
    n++;
    const uint64_t u = cqe->user_data;
    const int res = cqe->res;
    const uint32_t flags = cqe->flags;
    const uint64_t op = u >> 56;
    const uint16_t gen = static_cast<uint16_t>((u >> 32) & 0xFFFF);
    const uint32_t slot = static_cast<uint32_t>(u);
    switch (op) {
      case kOpAccept: {
        r.accepts[slot]--;
        if (res >= 0) {
          Conn& c = r.conns[static_cast<size_t>(res)];
          const uint16_t g = static_cast<uint16_t>(c.gen + 1);
          c = Conn{};
          c.gen = g;
          c.open = true;
          arm_recv(r, ri, static_cast<uint32_t>(res), g);
          r.open++;
          if (static_cast<size_t>(r.open) + r.margin >= r.conns.size()) r.full = true;
        } else if (res == -ENFILE) {

          r.full = true;
        }
        break;
      }
      case kOpRecv: {

        const bool has_bid = (flags & IORING_CQE_F_BUFFER) != 0;
        const uint16_t bid = static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
        Conn& c = r.conns[slot];
        if (c.gen != gen || !c.open || c.closing) {
          if (has_bid) bufring_add(r.bufs, bid);
          break;
        }
        if (res > 0) {
          if (has_bid) {
            received(ri, slot, c, bufring_get(r.bufs, bid), static_cast<size_t>(res));
            bufring_add(r.bufs, bid);
          }
          if (!(flags & IORING_CQE_F_MORE)) {

            queue_read(ri, slot, c);
          }
        } else if (res == -ENOBUFS) {

          queue_read(ri, slot, c);
        } else {

          if (has_bid) bufring_add(r.bufs, bid);
          conn_finish(ri, slot, c);
        }
        break;
      }
      case kOpRead: {
        r.reads--;
        const bool has_bid = (flags & IORING_CQE_F_BUFFER) != 0;
        const uint16_t bid = static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
        Conn& c = r.conns[slot];
        if (c.gen != gen || !c.open || c.closing) {
          if (has_bid) bufring_add(r.bufs, bid);
          break;
        }
        c.recving = false;
        if (res > 0) {
          if (!has_bid) {
            queue_read(ri, slot, c);
            break;
          }
          received(ri, slot, c, bufring_get(r.bufs, bid), static_cast<size_t>(res));
          bufring_add(r.bufs, bid);
          if (static_cast<size_t>(res) == kBufSize) {

            queue_read(ri, slot, c);
          } else {

            arm_recv(r, ri, slot, gen);
          }
        } else if (res == -EAGAIN) {
          arm_recv(r, ri, slot, gen);
        } else if (res == -ENOBUFS) {
          queue_read(ri, slot, c);
        } else {
          if (has_bid) bufring_add(r.bufs, bid);
          conn_finish(ri, slot, c);
        }
        break;
      }
      case kOpSend: {
        Conn& c = r.conns[slot];
        if (c.gen != gen || !c.sending) break;
        if (res < 0) {
          c.sending = false;
          app::give_buf(S.pool, c.wire);
          app::give_buf(S.pool, c.out);
          c.close_after = true;
          mark_touched(ri, slot, c);
        } else {
          c.sent += static_cast<uint32_t>(res);
          if (c.sent < c.wire->len) {
            arm_send(r, ri, slot, c);
          } else {
            c.sending = false;
            app::give_buf(S.pool, c.wire);
            if ((c.out && c.out->len) || c.close_after) {

              mark_touched(ri, slot, c);
            } else {
              app::give_buf(S.pool, c.out);
            }
          }
        }
        break;
      }
      case kOpClose: {
        Conn& c = r.conns[slot];
        if (c.gen == gen && c.open) {
          const uint16_t g = c.gen;
          app::give_buf(S.pool, c.out);
          app::give_buf(S.pool, c.wire);
          delete c.partial;
          c.partial = nullptr;
          c = Conn{};
          c.gen = g;
          r.open--;
        }
        if (r.full && static_cast<size_t>(r.open) + 2 * r.margin < r.conns.size()) {
          r.full = false;
          r.accepting = true;
        }
        break;
      }
      default:
        break;
    }
  }
  io_uring_cq_advance(&r.uring, static_cast<unsigned>(n));
  bufring_publish(r.bufs);
  if (r.accepting) {
    if (r.full) {

      r.accepting = false;
      S.need_ring = true;
    } else {
      fill_accepts(r, ri);
    }
  }
  return n;
}

bool spill();

void end_batch() {
  const bool committed = S.app->db.commit();
  S.app->wrote = false;

  S.app->db.tick();
  for (const auto& t : S.touched) {
    Ring& r = *S.rings[t.first];
    Conn& c = r.conns[t.second];
    if (!committed && c.answered) {

      ByteBuf& out = out_of(c);
      out.len = c.batch_mark;
      app::error(out, 500, "internal server error");
      c.close_after = true;
    }
    c.in_batch = false;
    c.answered = false;
    if (!c.open || c.closing || c.sending) continue;
    if (c.out && c.out->len) {
      c.wire = c.out;
      c.out = nullptr;
      c.sent = 0;
      c.sending = true;
      arm_send(r, t.first, t.second, c);
    } else if (c.close_after) {
      arm_close(r, t.first, t.second, c);
    }
  }
  S.touched.clear();
  if (S.need_ring && std::chrono::steady_clock::now() >= S.spill_after) {

    S.need_ring = !spill();
    if (S.need_ring) S.spill_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  }
}

void release_held() {
  std::vector<Triple> list = std::move(S.held);
  S.held.clear();
  for (const Triple& t : list) {
    Ring& r = *S.rings[t.ri];
    Conn& c = r.conns[t.slot];
    if (c.gen == t.gen && c.open && !c.closing) {
      c.held = false;
      serve(t.ri, t.slot, c, nullptr, 0);
      if (c.eof && !c.held) {
        c.close_after = true;
        mark_touched(t.ri, t.slot, c);
      }
    }
  }
}

void run_ready() {
  while (S.answers_left > 0) {
    if (S.ready.empty()) break;
    const Triple t = S.ready.front();
    S.ready.pop_front();
    Ring& r = *S.rings[t.ri];
    Conn& c = r.conns[t.slot];
    if (c.gen != t.gen || !c.open || c.closing) continue;
    c.ready = false;
    if (c.held) continue;
    S.answers_left--;
    serve(t.ri, t.slot, c, nullptr, 0);
    if (c.eof && !c.held) {
      c.close_after = true;
      mark_touched(t.ri, t.slot, c);
    }
  }
}

size_t issue_reads(size_t budget) {
  size_t issued = 0;
  const size_t n = S.rq.size();
  for (size_t i = 0; i < n; i++) {
    if (issued == budget) break;
    if (S.rq.empty()) break;
    const Triple t = S.rq.front();
    S.rq.pop_front();
    Ring& r = *S.rings[t.ri];
    Conn& c = r.conns[t.slot];
    if (c.gen != t.gen || !c.open || c.closing) continue;
    if (c.recving || r.reads >= kBufCount) {
      S.rq.push_back(t);
      continue;
    }
    c.queued = false;
    c.recving = true;
    r.reads++;
    issued++;
    arm_read(r, t.ri, t.slot, t.gen);
  }
  return issued;
}

size_t read_rounds() {
  size_t n = 0;
  size_t budget = kReadsPerPass;
  while (budget > 0 && !S.rq.empty()) {
    const size_t k = issue_reads(budget);
    if (k == 0) break;
    budget -= k;
    for (Ring* r : S.rings) {
      if (io_uring_sq_ready(&r->uring) > 0) enter_ring(*r, kGetEvents);
    }
    for (size_t ri = 0; ri < S.rings.size(); ri++) n += drain(ri);
  }
  return n;
}

bool spill() {
  for (Ring* r : S.rings)
    if (r->accepting) return true;
  for (size_t ri = 0; ri < S.rings.size(); ri++) {
    if (!S.rings[ri]->full) {
      arm_accept(*S.rings[ri], ri);
      return true;
    }
  }
  if (S.rings.size() >= kMaxRings) {
    std::fprintf(stderr, "connection limit reached (%zu rings)\n", S.rings.size());
    return false;
  }
  auto* r = new Ring;
  if (!ring_create(*r, S.slots)) {
    std::fprintf(stderr, "cannot add ring\n");
    delete r;
    return false;
  }
  const size_t ri = S.rings.size();
  arm_accept(*r, ri);
  S.rings.push_back(r);
  std::fprintf(stderr, "ring %zu added (%u slots each)\n", ri, S.slots);
  return true;
}

[[noreturn]] void run() {
  for (;;) {

    const bool busy = !S.rq.empty() || !S.ready.empty();
    for (size_t k = S.rings.size(); k-- > 0;) {
      Ring& r = *S.rings[k];
      if (k == 0 && !busy) {
        struct __kernel_timespec ts {};
        ts.tv_sec = 0;
        ts.tv_nsec = kWaitNs;
        struct io_uring_cqe* cqe = nullptr;
        const int rc = io_uring_submit_and_wait_timeout(&r.uring, &cqe, kWaitMin, &ts, nullptr);
        if (rc < 0) {
          const int e = -rc;
          if (e != EINTR && e != ETIME && e != EBUSY && e != EAGAIN)
            std::fprintf(stderr, "io_uring_enter: %s\n", std::strerror(e));
        }
      } else {
        enter_ring(r, kGetEvents);
      }
    }
    S.answers_left = kAnswersPerPass;
    size_t n = 0;
    for (size_t ri = 0; ri < S.rings.size(); ri++) n += drain(ri);

    run_ready();
    n += read_rounds();
    (void)n;
    if (!S.held.empty() && !S.app->db.writes_held()) release_held();
    end_batch();
  }
}

std::vector<int> listen_sockets(const char* host, uint16_t port) {
  struct in_addr addr {};
  if (inet_pton(AF_INET, host, &addr) != 1) {
    std::fprintf(stderr, "HOST must be an IPv4 address\n");
    std::exit(1);
  }
  std::vector<int> fds;
  for (size_t i = 0; i < kListeners; i++) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      std::perror("socket");
      std::exit(1);
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) {
      std::perror("SO_REUSEPORT");
      std::exit(1);
    }

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr = addr;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0) {
      std::perror("bind");
      std::exit(1);
    }
    if (listen(fd, 65535) < 0) {
      std::perror("listen");
      std::exit(1);
    }
    fds.push_back(fd);
  }
  return fds;
}

uint32_t descriptor_slots() {
  struct rlimit rl {};
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  rlim_t n = rl.rlim_cur;
  if (n > (1u << 20)) n = 1u << 20;
  return static_cast<uint32_t>(n);
}

const char* env_or(const char* k, const char* d) {
  const char* v = std::getenv(k);
  return v ? v : d;
}

}

int main() {
  const char* path = env_or("SQLITE_PATH", "feed.db");
  const char* secret = env_or("JWT_SECRET", "");
  const char* host = env_or("HOST", "127.0.0.1");
  const char* port_s = env_or("PORT", "3000");
  std::signal(SIGPIPE, SIG_IGN);

  app::Db db;
  std::string err;
  if (!db.open(path, err)) {
    std::fprintf(stderr, "database: %s\n", err.c_str());
    return 1;
  }
  S.app = new app::App(std::move(db), reinterpret_cast<const uint8_t*>(secret),
                       std::strlen(secret));
  S.slots = descriptor_slots();
  S.spill_after = std::chrono::steady_clock::now();
  S.listen = listen_sockets(host, static_cast<uint16_t>(std::atoi(port_s)));

  auto* ring = new Ring;
  if (!ring_create(*ring, S.slots)) {
    std::fprintf(stderr, "io_uring setup failed\n");
    return 1;
  }
  S.rings.push_back(ring);
  arm_accept(*ring, 0);
  std::fprintf(stderr, "listening on %s:%s; io_uring, %u direct descriptors per ring\n", host,
               port_s, S.slots);
  run();
}
