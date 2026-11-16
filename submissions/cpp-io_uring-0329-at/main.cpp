#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "src/app.hpp"
#include "src/io/ring.hpp"
#include "src/net/connection.hpp"

namespace {

constexpr uint32_t kMaxRings = 2;   /* rings are grown on demand up to this */
constexpr uint32_t kAcceptDepth = 2048;
constexpr size_t kHeldPerPass = 1024;
constexpr unsigned kSqEntries = 4096;
constexpr unsigned kCqEntries = 65536;
constexpr size_t kBufSize = 2048;
constexpr uint16_t kBufCount = 4096;
constexpr uint32_t kWaitNs = 1000000;
constexpr size_t kDrainBudget = 512;
constexpr uint32_t kIdleSeconds = 90;

constexpr uint64_t kEvAccept = 1;
constexpr uint64_t kEvRecv = 2;
constexpr uint64_t kEvSend = 3;
constexpr uint64_t kEvShutdown = 4;
constexpr uint64_t kEvClose = 5;
constexpr uint64_t kEvRead = 6;
constexpr uint64_t kEvEvfd = 7;

inline uint64_t ev(uint64_t op, uint32_t wi, uint16_t gen, uint32_t idx) {
  return op << 56 | static_cast<uint64_t>(wi) << 48 | static_cast<uint64_t>(gen) << 32 | idx;
}

class Worker {
 public:
  enum class Init { Ok, RingFail, BufFail };

  Init init(uint32_t index, uint32_t slots, int listener, unsigned cq) {
    index_ = index;
    listener_ = listener;
    accepting_ = true;
    if (!ring_.init(kSqEntries, cq, slots)) {
      std::fprintf(stderr, "ring %u setup failed (slots=%u, cq=%u)\n", index, slots, cq);
      return Init::RingFail;
    }
    if (!bufs_.init(ring_.fd(), kBufCount, kBufSize)) {
      std::fprintf(stderr, "ring %u buffer ring failed (slots=%u, cq=%u)\n", index, slots, cq);
      return Init::BufFail;
    }
    table_.resize(slots);
    return Init::Ok;
  }

  uint32_t index() const { return index_; }
  io::Ring& ring() { return ring_; }
  io::BufRing& bufs() { return bufs_; }
  net::SlotTable& table() { return table_; }
  std::vector<uint32_t>& dirty() { return dirty_; }
  std::deque<uint32_t>& pending() { return pending_; }
  std::deque<uint32_t>& held() { return held_; }
  int listener() const { return listener_; }
  uint32_t& accepts() { return accepts_; }
  int& efd() { return efd_; }
  uint32_t& open() { return open_; }
  uint32_t& reads() { return reads_; }
  bool& accepting() { return accepting_; }
  bool& full() { return full_; }

  void close_listener() {
    if (listener_ >= 0) {
      ::close(listener_);
      listener_ = -1;
    }
    accepting_ = false;
  }

 private:
  uint32_t index_ = 0;
  io::Ring ring_;
  io::BufRing bufs_;
  net::SlotTable table_;
  std::vector<uint32_t> dirty_;
  std::deque<uint32_t> pending_;
  std::deque<uint32_t> held_;
  int listener_ = -1;
  int efd_ = -1;
  uint32_t accepts_ = 0;
  uint32_t open_ = 0;
  uint32_t reads_ = 0;
  bool accepting_ = false;
  bool full_ = false;
};

class Server {
 public:
  Server(app::App* app, uint32_t slots, unsigned cq) : app_(app), slots_(slots), cq_(cq) {}

  bool init(const char* host, uint16_t port) {
    host_ = host;
    port_ = port;
    app::preload_pool(pool_);
    return add_ring();
  }

  /* Rings are created on demand: one is enough until its descriptor table is close to full, and
   * every extra ring costs an io_uring_enter per pass on a 1-vCPU box (6 rings measured 15.9k
   * req/s against 22.5k with 2, same binary otherwise).  Growth keeps the low-connection path
   * as cheap as a single ring and still gives the 300k-connection path a second CQ, buffer ring
   * and listen queue. */
  bool add_ring() {
    if (workers_.size() >= kMaxRings) return false;
    struct in_addr addr {};
    if (inet_pton(AF_INET, host_, &addr) != 1) {
      std::fprintf(stderr, "HOST must be an IPv4 address\n");
      return false;
    }
    const uint32_t i = static_cast<uint32_t>(workers_.size());
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) { ::close(fd); return false; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    a.sin_addr = addr;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0 ||
        listen(fd, 65535) < 0) {
      ::close(fd);
      return false;
    }
    auto w = std::make_unique<Worker>();
    unsigned cq = cq_;
    for (;;) {
      Worker::Init r = w->init(i, slots_, fd, cq);
      if (r == Worker::Init::Ok) break;
      if (r == Worker::Init::BufFail || cq <= 8192) { ::close(fd); return false; }
      cq /= 2;
    }
    workers_.push_back(std::move(w));
    listeners_.push_back(fd);
    arm_accept(*workers_.back(), i);
    if (i > 0) {
      int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (efd >= 0 && workers_[i]->ring().register_eventfd(efd) == 0) {
        workers_[i]->efd() = efd;
        io_uring_sqe* sqe = workers_[0]->ring().sqe(1);
        if (sqe) {
          io_uring_prep_poll_multishot(sqe, efd, POLLIN);
          sqe->user_data = ev(kEvEvfd, i, 0, 0);
        }
      } else if (efd >= 0) {
        ::close(efd);
      }
    }
    std::fprintf(stderr, "ring %u up: %u rings x %u slots\n", i,
                 static_cast<unsigned>(workers_.size()), slots_);
    return true;
  }

  /* Add a ring once the newest one is three quarters full, so the next accepts have a home. */
  void maybe_grow() {
    if (workers_.size() >= kMaxRings) return;
    Worker& w = *workers_.back();
    if (w.open() * 4 >= slots_ * 3) add_ring();
  }

  static uint32_t now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec);
  }

  static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
  }

  /* Idle-connection sweep, a slice at a time: walking all 6 rings' slot tables costs 25-70 ms
   * on the loop (measured), which is a p99 cliff on a 1 vCPU box. 1/32 of one ring per pass. */
  void sweep_slice() {
    if (workers_.empty() || sweep_ring_ >= workers_.size()) return;
    Worker& w = *workers_[sweep_ring_];
    auto& tbl = w.table();
    const size_t total = tbl.size();
    const size_t step = (total + 31) / 32;
    size_t end = sweep_off_ + step;
    if (end > total) end = total;
    for (size_t i = sweep_off_; i < end; i++) {
      net::Connection& c = tbl.at(i);
      if (c.open && !c.closing && now_sec_ - c.last_active > kIdleSeconds) {
        arm_close(w, sweep_ring_, static_cast<uint32_t>(i), c);
      }
    }
    sweep_off_ = end;
    if (end >= total) {
      sweep_off_ = 0;
      sweep_ring_ = (sweep_ring_ + 1) % static_cast<uint32_t>(workers_.size());
    }
  }

  /* Answer the requests the WAL gate held. Hold-once: a request that was held is executed here
   * unconditionally, so its latency is bounded by one extra pass instead of a new 400 ms window.
   * kHeldPerPass bounds the work a single pass does. */
  void retry_held() {
    size_t budget = kHeldPerPass;
    for (uint32_t wi = 0; wi < workers_.size() && budget; wi++) {
      Worker& w = *workers_[wi];
      size_t n = w.held().size();
      while (n-- > 0 && budget) {
        const uint32_t idx = w.held().front();
        w.held().pop_front();
        net::Connection& c = w.table().at(idx);
        if (!c.held) continue;
        c.held = false;
        if (!c.open || c.closing) continue;
        budget--;
        serve(w, wi, idx, nullptr, 0);
      }
    }
  }

  [[noreturn]] void run() {
    size_t last_cqes = 0;
    uint64_t pass = 0;
    const bool stats = std::getenv("STATS") != nullptr;
    uint64_t t_wait = 0, t_drain = 0, t_read = 0, t_commit = 0, t_sweep = 0;
    uint64_t stat_at = now_ns(), slow_total = 0, held_gate = 0;
    uint64_t cqes_sum = 0, requests_sum = 0, prev_responded = 0, reqs = 0, last_reqs = 1;
    uint64_t last_sweep = now_ns();
    now_sec_ = now_sec();
    for (;;) {
      const uint64_t p0 = now_ns();
      writes_ok_ = !app_->db.writes_held();
      if (!writes_ok_) held_gate++;
      /* The gate can open between two passes: the requests it held must be replayed here, or
       * they would wait for a close that never happens again. */
      if (writes_ok_) retry_held();
      /* Only block when there is nothing left to do: an unconditional 1 ms wait per pass is
       * what pins a loop at ~70 % CPU with a backlog it can never clear (kensac/mojo both
       * enter with min_complete 0 whenever work is queued). */
      bool busy = last_cqes >= 8;
      for (auto& wp : workers_) {
        if (!wp->pending().empty() || !wp->dirty().empty()) {
          busy = true;
          break;
        }
      }
      for (size_t k = workers_.size(); k-- > 0;) {
        Worker& w = *workers_[k];
        if (k == 0) {
          if (busy && (last_cqes != 0 || last_reqs != 0)) {
            w.ring().wait(1, 0);
          } else {
            w.ring().wait(last_cqes >= 8 ? 64 : 1, busy ? 50000 : kWaitNs);
          }
        } else {
          w.ring().enter();
        }
      }
      const uint64_t p1 = now_ns();
      last_cqes = 0;
      for (size_t i = 0; i < workers_.size(); i++)
        last_cqes += drain(*workers_[i], static_cast<uint32_t>(i), kDrainBudget);
      const uint64_t p2 = now_ns();
      for (size_t i = 0; i < workers_.size(); i++) run_reads(*workers_[i], static_cast<uint32_t>(i));
      maybe_grow();
      const uint64_t p3 = now_ns();
      commit_and_flush();
      const uint64_t p4 = now_ns();
      if (!writes_ok_) writes_ok_ = !app_->db.writes_held();
      /* Time-driven, not pass-driven: the pass rate is not bounded from below under load, and a
       * pass-cadence sweep would then walk the whole slot table many times a second. */
      if (p4 - last_sweep >= 50000000ULL) {
        last_sweep = p4;
        now_sec_ = now_sec();
        sweep_slice();
      }
      const uint64_t p5 = now_ns();
      t_wait += p1 - p0;
      t_drain += p2 - p1;
      t_read += p3 - p2;
      t_commit += p4 - p3;
      t_sweep += p5 - p4;
      cqes_sum += last_cqes;
      reqs = responded_ - prev_responded;
      prev_responded = responded_;
      last_reqs = reqs;
      requests_sum += reqs;
      pass++;
      if (stats) {
        const uint64_t span = p5 - p0;
        if (span > 20000000ULL && slow_total < 40) {
          slow_total++;
          std::fprintf(stderr,
                       "slow pass %.1fms cqes=%zu: wait %.1f drain %.1f reads %.1f commit %.1f sweep %.1f\n",
                       span / 1e6, last_cqes, (p1 - p0) / 1e6, (p2 - p1) / 1e6, (p3 - p2) / 1e6,
                       (p4 - p3) / 1e6, (p5 - p4) / 1e6);
        }
        if (p5 - stat_at >= 1000000000ULL) {
          std::fprintf(stderr,
                       "stats: pass=%llu/s cqes=%llu req=%llu gated=%llu/%llu wait=%.1f%% drain=%.1f%% "
                       "read=%.1f%% commit=%.1f%% sweep=%.1f%% pool_miss=%lu pool=%zu\n",
                       (unsigned long long)(pass - stat_pass), (unsigned long long)cqes_sum,
                       (unsigned long long)requests_sum, (unsigned long long)held_gate,
                       (unsigned long long)(pass - stat_pass), 100.0 * t_wait / (p5 - stat_at),
                       100.0 * t_drain / (p5 - stat_at), 100.0 * t_read / (p5 - stat_at),
                       100.0 * t_commit / (p5 - stat_at), 100.0 * t_sweep / (p5 - stat_at),
                       app::pool_misses(), pool_.size());
          stat_at = p5;
          stat_pass = pass;
          t_wait = t_drain = t_read = t_commit = t_sweep = 0;
          cqes_sum = requests_sum = held_gate = 0;
        }
      }
    }
  }

 private:


  app::ByteBuf& out_of(net::Connection& c) {
    if (!c.out) c.out = app::take_buf(pool_);
    return *c.out;
  }

  static bool is_write(const app::http::Request& req) {
    return req.method_len == 4 && std::memcmp(req.method, "POST", 4) == 0;
  }

  void mark(Worker& w, uint32_t idx) {
    net::Connection& c = w.table().at(idx);
    if (!c.in_batch) {
      c.in_batch = true;
      w.dirty().push_back(idx);
    }
  }

  void arm_accept(Worker& w, uint32_t wi) {
    while (w.accepts() < kAcceptDepth / workers_.size() && w.listener() >= 0) {
      io_uring_sqe* sqe = w.ring().sqe(1);
      if (!sqe) return;
      io_uring_prep_accept_direct(sqe, w.listener(), nullptr, nullptr, 0,
                                  IORING_FILE_INDEX_ALLOC);
      sqe->user_data = ev(kEvAccept, wi, 0, 0);
      w.accepts()++;
    }
  }

  void arm_recv(Worker& w, uint32_t wi, uint32_t idx, uint16_t gen) {
    io_uring_sqe* sqe = w.ring().sqe(1);
    if (!sqe) return;
    io_uring_prep_recv_multishot(sqe, static_cast<int>(idx), nullptr, 0, 0);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;
    sqe->user_data = ev(kEvRecv, wi, gen, idx);
    w.table().at(idx).read = net::ReadState::Idle;
  }

  void arm_read(Worker& w, uint32_t wi, uint32_t idx, uint16_t gen) {
    io_uring_sqe* sqe = w.ring().sqe(1);
    if (!sqe) return;
    io_uring_prep_recv(sqe, static_cast<int>(idx), nullptr, 0, MSG_DONTWAIT);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;
    sqe->user_data = ev(kEvRead, wi, gen, idx);
    w.table().at(idx).read = net::ReadState::Reading;
  }

  void arm_send(Worker& w, uint32_t wi, uint32_t idx, net::Connection& c) {
    io_uring_sqe* sqe = w.ring().sqe(1);
    if (!sqe) return;
    io_uring_prep_send(sqe, static_cast<int>(idx), c.wire->p + c.sent, c.wire->len - c.sent,
                       MSG_NOSIGNAL);
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->user_data = ev(kEvSend, wi, c.gen, idx);
    c.write = net::WriteState::Sending;
  }

  void arm_close(Worker& w, uint32_t wi, uint32_t idx, net::Connection& c) {
    c.closing = true;
    io_uring_sqe* a = w.ring().sqe(2);
    if (!a) return;
    io_uring_prep_shutdown(a, static_cast<int>(idx), SHUT_RDWR);
    a->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
    a->user_data = ev(kEvShutdown, wi, c.gen, idx);
    io_uring_sqe* b = w.ring().sqe(1);
    if (!b) return;
    io_uring_prep_close_direct(b, idx);
    b->user_data = ev(kEvClose, wi, c.gen, idx);
  }

  void serve(Worker& w, uint32_t wi, uint32_t idx, const uint8_t* data, size_t n) {
    net::Connection& c = w.table().at(idx);
    if (c.close_after) return;
    std::vector<uint8_t> part;
    bool joined = false;
    if (c.part) {
      part = std::move(*c.part);
      delete c.part;
      c.part = nullptr;
      joined = !part.empty();
    }
    const uint8_t* buf;
    size_t len;
    if (part.empty()) {
      buf = data ? data : reinterpret_cast<const uint8_t*>("");
      len = n;
    } else {
      if (n) part.insert(part.end(), data, data + n);
      buf = part.data();
      len = part.size();
    }
    size_t off = 0;
    while (off < len) {
      app::http::Request req;
      size_t used = 0;
      app::http::Parsed pr = app::http::parse(buf + off, len - off, req, used);
      if (pr == app::http::Parsed::Done) {
        if (!writes_ok_ && is_write(req)) {
          /* The WAL gate is closed: keep this request (and anything behind it) on the
           * connection and answer it in a later pass. Nothing is written, so no response. */
          if (!c.held) {
            c.held = true;
            w.held().push_back(idx);
          }
          break;
        }
        mark(w, idx);
        c.answered = true;
        app_->handle(req, out_of(c), writes_ok_);
        off += used;
      } else if (pr == app::http::Parsed::Partial) {
        break;
      } else {
        mark(w, idx);
        c.answered = true;
        app::error(out_of(c), 400, "bad request");
        c.close_after = true;
        off = len;
        break;
      }
    }
    if (off < len) {
      if (!joined) {
        c.part = new std::vector<uint8_t>(buf + off, buf + len);
      } else {
        part.erase(part.begin(), part.begin() + static_cast<ptrdiff_t>(off));
        c.part = new std::vector<uint8_t>(std::move(part));
      }
    }
  }

  size_t drain(Worker& w, uint32_t wi, size_t budget) {
    size_t n = 0;
    unsigned head = 0;
    struct io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&w.ring().raw(), head, cqe) {
      if (n == budget) break;
      n++;
      const uint64_t u = cqe->user_data;
      const int res = cqe->res;
      const uint32_t flags = cqe->flags;
      const uint64_t op = u >> 56;
      const uint16_t gen = static_cast<uint16_t>((u >> 32) & 0xFFFF);
      const uint32_t idx = static_cast<uint32_t>(u);
      switch (op) {
        case kEvAccept: {
          w.accepts()--;
          if (res >= 0) {
            net::Connection& c = w.table().at(static_cast<size_t>(res));
            const uint16_t g = static_cast<uint16_t>(c.gen + 1);
            c = net::Connection{};
            c.gen = g;
            c.open = true;
            c.last_active = now_sec_;
            arm_recv(w, wi, static_cast<uint32_t>(res), g);
            w.open()++;
            if (static_cast<size_t>(w.open()) + kAcceptDepth >= w.table().size())
              w.full() = true;
          } else {
            static int accept_errs = 0;
            if (accept_errs < 20)
              std::fprintf(stderr, "accept fail res=%d errno=%d open=%u accepts=%u\n", res, -res,
                           w.open(), w.accepts());
            accept_errs++;
            if (res == -ENFILE) w.full() = true;
          }
          break;
        }
        case kEvRecv: {
          const bool has = (flags & IORING_CQE_F_BUFFER) != 0;
          const uint16_t bid = static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
          net::Connection& c = w.table().at(idx);
          if (c.gen != gen || !c.open || c.closing) {
            if (has) w.bufs().give(bid);
            break;
          }
          if (res > 0) {
            if (has) {
              c.last_active = now_sec_;
            serve(w, wi, idx, w.bufs().data(bid), static_cast<size_t>(res));
              w.bufs().give(bid);
            }
            if (!(flags & IORING_CQE_F_MORE)) {
              c.read = net::ReadState::Queued;
              w.pending().push_back(idx);
            }
          } else if (res == -ENOBUFS) {
            c.read = net::ReadState::Queued;
            w.pending().push_back(idx);
          } else {
            if (has) w.bufs().give(bid);
            c.close_after = true;
            mark(w, idx);
          }
          break;
        }
        case kEvRead: {
          w.reads()--;
          const bool has = (flags & IORING_CQE_F_BUFFER) != 0;
          const uint16_t bid = static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
          net::Connection& c = w.table().at(idx);
          if (c.gen != gen || !c.open || c.closing) {
            if (has) w.bufs().give(bid);
            break;
          }
          if (res > 0) {
            if (!has) {
              c.read = net::ReadState::Queued;
              w.pending().push_back(idx);
              break;
            }
            c.last_active = now_sec_;
            serve(w, wi, idx, w.bufs().data(bid), static_cast<size_t>(res));
            w.bufs().give(bid);
            if (static_cast<size_t>(res) == kBufSize) {
              c.read = net::ReadState::Queued;
              w.pending().push_back(idx);
            } else {
              arm_recv(w, wi, idx, gen);
            }
          } else if (res == -EAGAIN) {
            arm_recv(w, wi, idx, gen);
          } else if (res == -ENOBUFS) {
            c.read = net::ReadState::Queued;
            w.pending().push_back(idx);
          } else {
            if (has) w.bufs().give(bid);
            c.close_after = true;
            mark(w, idx);
          }
          break;
        }
        case kEvSend: {
          net::Connection& c = w.table().at(idx);
          if (c.gen != gen || c.write != net::WriteState::Sending) break;
          if (res < 0) {
            c.write = net::WriteState::Idle;
            app::give_buf(pool_, c.wire);
            app::give_buf(pool_, c.out);
            c.close_after = true;
            mark(w, idx);
          } else {
            c.sent += static_cast<uint32_t>(res);
            if (c.sent < c.wire->len) {
              arm_send(w, wi, idx, c);
            } else {
              c.write = net::WriteState::Idle;
              app::give_buf(pool_, c.wire);
              if ((c.out && c.out->len) || c.close_after) {
                mark(w, idx);
              } else {
                app::give_buf(pool_, c.out);
              }
            }
          }
          break;
        }
        case kEvEvfd: {
        const uint32_t wi = static_cast<uint32_t>((u >> 48) & 0xFF);
        if (!(flags & IORING_CQE_F_MORE) && wi < workers_.size()) {
          io_uring_sqe* sqe = workers_[0]->ring().sqe(1);
          if (sqe) {
            io_uring_prep_poll_multishot(sqe, workers_[wi]->efd(), POLLIN);
            sqe->user_data = ev(kEvEvfd, wi, 0, 0);
          }
        }
        break;
      }
      case kEvClose: {
          net::Connection& c = w.table().at(idx);
          if (c.gen == gen && c.open) {
            const uint16_t g = c.gen;
            app::give_buf(pool_, c.out);
            app::give_buf(pool_, c.wire);
            delete c.part;
            c = net::Connection{};
            c.gen = g;
            w.open()--;
          }
          break;
        }
        default:
          break;
      }
    }
    io_uring_cq_advance(&w.ring().raw(), static_cast<unsigned>(n));
    w.bufs().publish();
    if (w.accepting() && w.listener() >= 0) {
      if (w.full()) {
        w.close_listener();
      } else {
        arm_accept(w, wi);
      }
    }
    return n;
  }

  void run_reads(Worker& w, uint32_t wi) {
    while (!w.pending().empty() && w.reads() < kBufCount) {
      const uint32_t idx = w.pending().front();
      w.pending().pop_front();
      net::Connection& c = w.table().at(idx);
      if (!c.open || c.closing || c.read != net::ReadState::Queued) continue;
      w.reads()++;
      arm_read(w, wi, idx, c.gen);
    }
  }

  void commit_and_flush() {
    bool ok = app_->db.commit();
    app_->wrote = false;
    for (auto& wp : workers_) {
      Worker& w = *wp;
      for (uint32_t idx : w.dirty()) {
        net::Connection& c = w.table().at(idx);
        if (!ok && c.answered) {
          app::ByteBuf& out = out_of(c);
          out.len = 0;
          app::error(out, 500, "internal server error");
          c.close_after = true;
        }
        c.in_batch = false;
        c.answered = false;
        responded_++;
        if (!c.open || c.closing || c.write == net::WriteState::Sending) continue;
        if (c.out && c.out->len) {
          c.wire = c.out;
          c.out = nullptr;
          c.sent = 0;
          arm_send(w, w.index(), idx, c);
        } else if (c.close_after) {
          arm_close(w, w.index(), idx, c);
        }
      }
      w.dirty().clear();
    }
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  uint32_t now_sec_ = 0;
  std::vector<int> listeners_;
  app::App* app_;
  app::Pool pool_;
  uint32_t slots_;
  unsigned cq_;
  const char* host_ = nullptr;
  uint16_t port_ = 0;
  uint64_t responded_ = 0;
  uint64_t stat_pass = 0;
  bool writes_ok_ = true;
  uint32_t sweep_ring_ = 0;
  size_t sweep_off_ = 0;
};

/* Direct-descriptor slots per ring, and therefore the connection table per ring. Sized for the
 * target connection count (6 x 131,072 = 786,432, against a scored peak of ~320,000), not by
 * RLIMIT_NOFILE: the old 1M-per-ring tables were 288 MB of preallocated, pre-faulted memory that
 * the box cannot spare at 300k connections. */
constexpr uint32_t kSlotsPerRing = 262144;

void raise_nofile() {
  struct rlimit rl {};
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
}

const char* env_or(const char* k, const char* d) {
  const char* v = std::getenv(k);
  return v ? v : d;
}

}  // namespace

static void on_term(int) { std::exit(0); }

int main() {
  std::signal(SIGTERM, on_term);
  std::signal(SIGINT, on_term);
  {
    std::FILE* mf = std::fopen("/proc/self/maps", "r");
    if (mf) {
      char line[512];
      while (std::fgets(line, sizeof line, mf)) {
        unsigned long a = 0, b = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &a, &b, perms) == 3 && perms[2] == 'x')
          ::mlock(reinterpret_cast<void*>(a), static_cast<size_t>(b - a));
      }
      std::fclose(mf);
    }
  }
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
  auto app = std::make_unique<app::App>(std::move(db),
                                        reinterpret_cast<const uint8_t*>(secret),
                                        std::strlen(secret));
  constexpr unsigned cq = kCqEntries;
  raise_nofile();
  Server server(app.get(), kSlotsPerRing, cq);
  if (!server.init(host, static_cast<uint16_t>(std::atoi(port_s)))) {
    std::fprintf(stderr, "server init failed\n");
    return 1;
  }
  std::fprintf(stderr, "listening on %s:%s; up to %u rings x %u slots, cq=%u\n", host, port_s,
               kMaxRings, kSlotsPerRing, cq);
  server.run();
}
