#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

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

constexpr uint32_t kRingCount = 8;
constexpr uint32_t kAcceptDepth = 32;
constexpr unsigned kSqEntries = 2048;
constexpr unsigned kCqEntries = 32768;
constexpr size_t kBufSize = 4096;
constexpr uint16_t kBufCount = 512;
constexpr uint32_t kWaitNs = 1000000;
constexpr size_t kDrainBudget = 512;

constexpr uint64_t kEvAccept = 1;
constexpr uint64_t kEvRecv = 2;
constexpr uint64_t kEvSend = 3;
constexpr uint64_t kEvShutdown = 4;
constexpr uint64_t kEvClose = 5;
constexpr uint64_t kEvRead = 6;

inline uint64_t ev(uint64_t op, uint32_t wi, uint16_t gen, uint32_t idx) {
  return op << 56 | static_cast<uint64_t>(wi) << 48 | static_cast<uint64_t>(gen) << 32 | idx;
}

class Worker {
 public:
  bool init(uint32_t index, uint32_t slots, int listener) {
    index_ = index;
    listener_ = listener;
    accepting_ = true;
    if (!ring_.init(kSqEntries, kCqEntries, slots)) return false;
    if (!bufs_.init(ring_.fd(), kBufCount, kBufSize)) return false;
    table_.resize(slots);
    return true;
  }

  uint32_t index() const { return index_; }
  io::Ring& ring() { return ring_; }
  io::BufRing& bufs() { return bufs_; }
  net::SlotTable& table() { return table_; }
  std::vector<uint32_t>& dirty() { return dirty_; }
  std::deque<uint32_t>& pending() { return pending_; }
  int listener() const { return listener_; }
  uint32_t& accepts() { return accepts_; }
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
  int listener_ = -1;
  uint32_t accepts_ = 0;
  uint32_t open_ = 0;
  uint32_t reads_ = 0;
  bool accepting_ = false;
  bool full_ = false;
};

class Server {
 public:
  Server(app::App* app, uint32_t slots) : app_(app), slots_(slots) {}

  bool init(const char* host, uint16_t port) {
    if (!make_listeners(host, port)) return false;
    for (uint32_t i = 0; i < kRingCount; i++) {
      auto w = std::make_unique<Worker>();
      if (!w->init(i, slots_, listeners_[i])) return false;
      workers_.push_back(std::move(w));
      arm_accept(*workers_.back(), i);
    }
    return true;
  }

  [[noreturn]] void run() {
    for (;;) {
      for (size_t k = workers_.size(); k-- > 0;) {
        Worker& w = *workers_[k];
        if (k == 0) {
          w.ring().wait(512, kWaitNs);
        } else {
          w.ring().enter();
        }
      }
      for (size_t i = 0; i < workers_.size(); i++) drain(*workers_[i], static_cast<uint32_t>(i), kDrainBudget);
      for (size_t i = 0; i < workers_.size(); i++) run_reads(*workers_[i], static_cast<uint32_t>(i));
      commit_and_flush();
    }
  }

 private:
  bool make_listeners(const char* host, uint16_t port) {
    struct in_addr addr {};
    if (inet_pton(AF_INET, host, &addr) != 1) {
      std::fprintf(stderr, "HOST must be an IPv4 address\n");
      return false;
    }
    for (uint32_t i = 0; i < kRingCount; i++) {
      int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) return false;
      const int one = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) return false;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      struct sockaddr_in a {};
      a.sin_family = AF_INET;
      a.sin_port = htons(port);
      a.sin_addr = addr;
      if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0) return false;
      if (listen(fd, 65535) < 0) return false;
      listeners_.push_back(fd);
    }
    return true;
  }

  app::ByteBuf& out_of(net::Connection& c) {
    if (!c.out) c.out = app::take_buf(pool_);
    return *c.out;
  }

  void mark(Worker& w, uint32_t idx) {
    net::Connection& c = w.table().at(idx);
    if (!c.in_batch) {
      c.in_batch = true;
      w.dirty().push_back(idx);
    }
  }

  void arm_accept(Worker& w, uint32_t wi) {
    while (w.accepts() < kAcceptDepth && w.listener() >= 0) {
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
        mark(w, idx);
        c.answered = true;
        app_->handle(req, out_of(c));
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

  void drain(Worker& w, uint32_t wi, size_t budget) {
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
            arm_recv(w, wi, static_cast<uint32_t>(res), g);
            w.open()++;
            if (static_cast<size_t>(w.open()) + kAcceptDepth >= w.table().size())
              w.full() = true;
          } else if (res == -ENFILE) {
            w.full() = true;
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
  std::vector<int> listeners_;
  app::App* app_;
  app::Pool pool_;
  uint32_t slots_;
};

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

}  // namespace

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
  auto app = std::make_unique<app::App>(std::move(db),
                                        reinterpret_cast<const uint8_t*>(secret),
                                        std::strlen(secret));
  Server server(app.get(), descriptor_slots());
  if (!server.init(host, static_cast<uint16_t>(std::atoi(port_s)))) {
    std::fprintf(stderr, "server init failed\n");
    return 1;
  }
  std::fprintf(stderr, "listening on %s:%s; %u rings x %u slots\n", host, port_s, kRingCount,
               descriptor_slots());
  server.run();
}
