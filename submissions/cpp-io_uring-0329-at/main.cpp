#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <malloc.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "src/shim.h"

#include "src/app.hpp"
#include "src/io/ring.hpp"
#include "src/net/connection.hpp"

namespace {

constexpr uint32_t kTargetConns = 400000;  /* connection ceiling the ring count must cover */
constexpr uint32_t kMaxRingsLimit = 16;    /* hard cap on rings (rust-iouring-kensac #264 uses 16) */
constexpr uint32_t kAcceptDepth = 2048;   /* total, divided by the live ring count */
constexpr uint32_t kAcceptsPerRing = 256; /* floor, so 16 rings still keep 256 each */

// 同一端口上相互独立的 SO_REUSEPORT listener，由所有 ring 共享。每个 listener 有自己的
// accept 队列（受 net.core.somaxconn 限制），所以 8 个 listener 在 ramp 期就是 8 条待建连接
// 队列，而不是每个 ring 一条——这正是 rust-iouring-kensac #264 的做法（LISTENERS = 8），也是
// 230k 的 ramp 不会积压成 SYN 重传（1 秒/3 秒/7 秒）的原因；那种积压表现为「零错误但 p99
// 高达数秒」。
constexpr uint32_t kListeners = 8;
constexpr size_t kHeldPerPass = 1024;
constexpr unsigned kSqEntries = 8192;
constexpr unsigned kCqEntries = 65536;
constexpr size_t kBufSize = 2048;
constexpr uint16_t kBufCount = 2048;
constexpr uint32_t kWaitNs = 1000000;
/* 每个 ring 每个 pass 最多处理多少个完成。读远多于答是有意的：读一个请求的成本是它那么大
 * 的一次拷贝，而把它留在 socket 里要占 3–4 KB 的内核缓冲，所以 socket 要尽早抽干，超出的
 * 回答丢进就绪队列由后续 pass 处理（rust-iouring-kensac #264 与 c-iouring-jowinjestine #404
 * 都用 16,384 的读配额配 2,048 的答案配额；我们原来是 512/ring，ramp 期要几十个 pass 才能
 * 消化完积压的完成，backlog 就是这么堆起来的）。 */
constexpr size_t kDrainBudget = 2048;

// 单个 pass 最多答多少个请求。读不受此限：超过上限的接收数据留在连接上，由后续 pass 按
// 最老优先回答，所以一个繁忙的 pass 不会把它后面排队的所有请求一起拖慢
// （rust-iouring-kensac #264 同样限制 2,048，并用就绪队列保序）。
constexpr uint32_t kAnswersPerPass = 2048;
constexpr uint32_t kIdleSeconds = 90;

constexpr uint64_t kEvAccept = 1;
constexpr uint64_t kEvRecv = 2;
constexpr uint64_t kEvSend = 3;
constexpr uint64_t kEvShutdown = 4;
constexpr uint64_t kEvClose = 5;
constexpr uint64_t kEvRead = 6;
// 所有诊断输出都走 std::format：格式串在编译期与实参逐一检查，所以像 CI 抓到过的
// 「%u 打印指针」这类错误，现在这里是编译错误而不是未定义行为。
// （GCC 13 自带 std::format，因此不需要任何第三方格式化库。）
/* shim.c 的诊断（C 文件，格式串仍是 printf 风格）从这里出来，与其他日志共用一个出口。 */
static void shim_log_sink(const char* s, size_t n) {
  std::fwrite(s, 1, n, stderr);
  std::fputc('\n', stderr);
}

template <typename... Args>
void errlog(std::format_string<Args...> fmt, Args&&... args) {
  const std::string line = std::format(fmt, std::forward<Args>(args)...);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fputc('\n', stderr);
}

constexpr uint64_t kEvEvfd = 7;

inline uint64_t ev(uint64_t op, uint32_t wi, uint16_t gen, uint32_t idx) {
  return op << 56 | static_cast<uint64_t>(wi) << 48 | static_cast<uint64_t>(gen) << 32 | idx;
}

class Worker {
 public:
  enum class Init { Ok, RingFail, BufFail };

  Init init(uint32_t index, uint32_t slots, const int* listeners, unsigned cq, unsigned* err) {
    index_ = index;
    for (uint32_t li = 0; li < kListeners; li++) listeners_[li] = listeners[li];
    accepting_ = true;
    if (!ring_.init(kSqEntries, cq, slots, index == 0, err)) {
      errlog("ring {} setup failed (slots={}, cq={}): {}", index, slots, cq,
             std::strerror(static_cast<int>(*err)));
      return Init::RingFail;
    }
    if (!bufs_.init(ring_.fd(), kBufCount, kBufSize)) {
      errlog("ring {} buffer ring failed (slots={}, cq={})", index, slots, cq);
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
  const int* listeners() const { return listeners_; }
  uint32_t* accepts() { return accepts_; }
  int& efd() { return efd_; }
  uint32_t& open() { return open_; }
  uint32_t& reads() { return reads_; }
  bool& accepting() { return accepting_; }
  bool& full() { return full_; }

  // ring 满了就停止继续投递 accept；listener 本身是共享的，为其他 ring 保持打开。
  void stop_accepting() { accepting_ = false; }

 private:
  uint32_t index_ = 0;
  io::Ring ring_;
  io::BufRing bufs_;
  net::SlotTable table_;
  std::vector<uint32_t> dirty_;
  std::deque<uint32_t> pending_;
  std::deque<uint32_t> held_;
  int listeners_[kListeners] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int efd_ = -1;
  uint32_t accepts_[kListeners] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t open_ = 0;
  uint32_t reads_ = 0;
  bool accepting_ = false;
  bool full_ = false;
};

class Server {
 public:
  Server(app::App* app, uint32_t slots, unsigned cq, uint32_t max_rings)
      : app_(app), slots_(slots), cq_(cq), max_rings_(max_rings) {
    /* ZC=0 关掉零拷贝发送，用来在评测机上做 A/B。 */
    const char* zc = std::getenv("ZC");
    if (zc && zc[0] == '0') zc_ok_ = false;
  }

  bool init(const char* host, uint16_t port) {
    host_ = host;
    port_ = port;
    app::preload_pool(pool_);
    return add_ring();
  }

  // ring 按需创建：一个就够，直到它的描述符表接近满；在单 vCPU 上每多一个 ring，每个 pass 就
  // 多一次 io_uring_enter（实测 6 个 ring 是 15.9k req/s，2 个是 22.5k，其余完全相同）。按需
  // 增长让低连接数路径保持单 ring 的成本，同时给 300k 连接路径提供第二个 CQ、buffer ring 和
  // listen 队列。
  bool add_ring() {
    if (workers_.size() >= max_rings_) return false;
    struct in_addr addr {};
    if (inet_pton(AF_INET, host_, &addr) != 1) {
      errlog("HOST must be an IPv4 address");
      return false;
    }
    const uint32_t i = static_cast<uint32_t>(workers_.size());
    if (listeners_[0] < 0) {
      struct sockaddr_in a {};
      a.sin_family = AF_INET;
      a.sin_port = htons(port_);
      a.sin_addr = addr;
      const int one = 1;
      for (uint32_t li = 0; li < kListeners; li++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) {
          ::close(fd);
          return false;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0 ||
            listen(fd, 65535) < 0) {
          ::close(fd);
          return false;
        }
        listeners_[li] = fd;
      }
    }
    auto w = std::make_unique<Worker>();
    unsigned cq = cq_;
    unsigned slots = slots_;
    for (;;) {
      unsigned err = 0;
      Worker::Init r = w->init(i, slots, listeners_, cq, &err);
      if (r == Worker::Init::Ok) {
        slot_cap_ = slots;
        break;
      }
      if (r == Worker::Init::BufFail) return false;
      // 先降完成队列，再降描述符表：RLIMIT_MEMLOCK 很小或 io_uring 受限的 runner 会拒绝 262,144 项
      // 的稀疏表（CI 就发生过），把它减半就足以用更小的连接上限起来，而不是让构建失败。
      if (cq > 8192) {
        cq /= 2;
        continue;
      }
      if (slots > 8192) {
        slots /= 2;
        errlog("ring {}: descriptor table reduced to {} slots", i, slots);
        continue;
      }
      return false;
    }
    workers_.push_back(std::move(w));
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
    errlog("ring {} up: {} rings x {} slots", i,
                 static_cast<unsigned>(workers_.size()), slots_);
    return true;
  }

  void maybe_grow() {
    if (workers_.size() >= max_rings_) return;
    Worker& w = *workers_.back();
    // 半满就增长，而不是四分之三，并且一次补齐：ramp 每秒增加几千个连接，一个 pass 只加一个 ring
    // 跟不上，表现为 accept 找不到空槽、连接被丢弃。rust-iouring-kensac #264 在 ring 的 1/8 处就
    // 开始迁移 accept；用一半是同一思路的保守版本。
    while (slot_cap_ && w.open() * 2 >= slot_cap_ && workers_.size() < max_rings_) {
      if (!add_ring()) break;
    }
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

  // 空闲连接清扫按片进行：一次走完所有 ring 的槽位表要 25–70 ms（实测），在单 vCPU 上就是 p99
  // 悬崖。每个 pass 扫一个 ring 的 1/32。
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

  // 回答被 WAL 闸门挂起的请求。一次性放行：被挂起的请求在这里无条件执行，所以它的延迟最多多一个
  // pass，而不是再等一个新的 400 ms 窗口。kHeldPerPass 限制单个 pass 的工作量。
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
      // 闸门可能在两个 pass 之间打开：它挂起的请求必须在这里重放，否则会一直等一个再也不会到来的关闭。
      if (writes_ok_) retry_held();
      // 只在确实没事可做时才阻塞：每个 pass 无条件等 1 ms，正是把循环钉在 ~70% CPU 却永远清不掉
      // 积压的原因（kensac/mojo 在有工作时都用 min_complete 0 进入）。「有工作」指等待读的 FIFO 和
      // 等待回答的就绪队列——不包括 dirty()（本 pass 产生、还没提交的响应）。把 dirty() 算进去，会让
      // 任何答过请求的 pass 都跳过等待、在完成队列为空时立刻返回：20k 连接时循环每秒跑 4,000 个 pass
      // 却只处理 2,000 个请求，一半是空转，而空转纯属系统调用和缓存开销。
      // rust-iouring-kensac #264 的 busy 标志就是 rq/ready。
      bool busy = last_cqes >= 8 || !ready_.empty();
      if (!busy) {
        for (auto& wp : workers_) {
          if (!wp->pending().empty()) {
            busy = true;
            break;
          }
        }
      }
      for (size_t k = workers_.size(); k-- > 0;) {
        Worker& w = *workers_[k];
        if (k == 0) {
          if (busy && (last_cqes != 0 || last_reqs != 0)) {
            w.ring().wait(1, 0);
          } else {
            // 等一批，而不是等下一个完成。min_complete 以前只在上一 pass 看到 8 个以上完成时才取 64，于是
            // 中等负载下（20k 连接、约 2,100 req/s）它只等一个，每秒醒 4,000 次来回答 2,100 个请求——一半的
            // pass 什么也没做。改成要 256 个、由 1 ms 超时截断，延迟不变而每个 pass 现在携带多个完成
            // （rust-iouring-kensac #264 的 WAIT_MIN）。
            w.ring().wait(busy ? 1 : 256, busy ? 50000 : kWaitNs);
          }
        } else {
          w.ring().submit_if_ready();
        }
      }
      const uint64_t p1 = now_ns();
      answers_left_ = kAnswersPerPass;
      last_cqes = 0;
      for (size_t i = 0; i < workers_.size(); i++)
        last_cqes += drain(*workers_[i], static_cast<uint32_t>(i), kDrainBudget);
      // 先处理更早的 pass 不得不延后的请求，再处理本 pass 读到的：这样请求才不会排在它之后到达的
      // 所有请求后面。
      run_ready();
      const uint64_t p2 = now_ns();
      for (size_t i = 0; i < workers_.size(); i++) run_reads(*workers_[i], static_cast<uint32_t>(i));
      maybe_grow();
      const uint64_t p3 = now_ns();
      commit_and_flush();
      const uint64_t p4 = now_ns();
      if (!writes_ok_) writes_ok_ = !app_->db.writes_held();
      // 按时间而不是按 pass 驱动：负载下 pass 频率没有下界，按 pass 节奏清扫会在每秒把整张槽位表
      // 走很多遍。
      /* 每 10 秒一次（且只在没有 backlog 时）把空闲堆还给内核。glibc 释放的内存默认留在进程的
       * arena 里，RSS 只增不减：backlog 期间为了 part/响应缓冲分配的那些块，在 backlog 清空后
       * 仍然算在 RSS 上。c-iouring-jowinjestine #404 记的实验是「backlog 碰过的每个连接都永久
       * 留着一块」，我们在 210k 连接时看到的 674 MB 里绝大部分就是这个。没有 backlog 才 trim，
       * 免得在正忙的时候去遍历堆。 */
      if (p4 - last_trim >= 10000000000ULL) {
        last_trim = p4;
        bool backlog = !ready_.empty();
        if (!backlog) {
          for (auto& wp : workers_) {
            if (!wp->pending().empty()) {
              backlog = true;
              break;
            }
          }
        }
        if (!backlog) malloc_trim(0);
      }
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
          errlog("slow pass {:.1f}ms cqes={}: wait {:.1f} drain {:.1f} reads {:.1f} commit {:.1f} "
                 "sweep {:.1f}",
                 span / 1e6, last_cqes, (p1 - p0) / 1e6, (p2 - p1) / 1e6, (p3 - p2) / 1e6,
                 (p4 - p3) / 1e6, (p5 - p4) / 1e6);
        }
        if (p5 - stat_at >= 1000000000ULL) {
          errlog("stats: pass={}/s cqes={} req={} gated={}/{} wait={:.1f}% drain={:.1f}% "
                 "read={:.1f}% commit={:.1f}% sweep={:.1f}% pool_miss={} pool={} "
                 "enter[wait={} sqe={} ring={}]",
                 pass - stat_pass, cqes_sum, requests_sum, held_gate, pass - stat_pass,
                 100.0 * t_wait / (p5 - stat_at), 100.0 * t_drain / (p5 - stat_at),
                 100.0 * t_read / (p5 - stat_at), 100.0 * t_commit / (p5 - stat_at),
                 100.0 * t_sweep / (p5 - stat_at), app::pool_misses(), pool_.size(),
                 io::Ring::enters(0) - enter_base_[0], io::Ring::enters(1) - enter_base_[1],
                 io::Ring::enters(2) - enter_base_[2]);
          enter_base_[0] = io::Ring::enters(0);
          enter_base_[1] = io::Ring::enters(1);
          enter_base_[2] = io::Ring::enters(2);
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
    uint32_t depth = static_cast<uint32_t>(kAcceptDepth / workers_.size());
    if (depth < kAcceptsPerRing) depth = kAcceptsPerRing;
    const uint32_t per = depth / kListeners;
    if (per < 16) return;
    for (uint32_t li = 0; li < kListeners; li++) {
      const int fd = w.listeners()[li];
      if (fd < 0) continue;
      uint32_t& n = w.accepts()[li];
      while (n < per) {
        io_uring_sqe* sqe = w.ring().sqe(1);
        if (!sqe) return;
        io_uring_prep_accept_direct(sqe, fd, nullptr, nullptr, 0, IORING_FILE_INDEX_ALLOC);
        sqe->user_data = ev(kEvAccept, wi, 0, li);
        n++;
      }
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

  /* 一次发送：首次用零拷贝（内核直接读用户缓冲，省掉 5.5 KB 的响应拷进内核），续传用普通
   * send——续传时第一块缓冲还被内核占着，再发一次零拷贝会让 NOTIF 的归属变得复杂。
   * 内核不支持时（-EINVAL/-ENOSYS）回退并永久关闭 ZC。 */
  void arm_send(Worker& w, uint32_t wi, uint32_t idx, net::Connection& c) {
    io_uring_sqe* sqe = w.ring().sqe(1);
    if (!sqe) return;
    /* 只对大的响应用零拷贝：feed 的 JSON 约 5.5 KB，省下的那次拷贝值一个 NOTIF 完成；
     * post/like 的响应只有几百字节，多一个 NOTIF 反而不划算（本地 20k 连接上就量到
     * sys 53 → 62 µs）。关闭中的响应也不用，避免关闭路径上还挂着 NOTIF。 */
    const bool zc = c.sent == 0 && !c.zbuf_done && zc_ok_ && !c.close_after && c.wire->len >= 4096;
    c.zc_used = zc;
    if (zc) {
      io_uring_prep_send_zc(sqe, static_cast<int>(idx), c.wire->p, c.wire->len, MSG_NOSIGNAL, 0);
    } else {
      io_uring_prep_send(sqe, static_cast<int>(idx), c.wire->p + c.sent, c.wire->len - c.sent,
                         MSG_NOSIGNAL);
    }
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->user_data = ev(kEvSend, wi, c.gen, idx);
    c.write = net::WriteState::Sending;
  }

  /* 一次发送彻底完成后的收尾：把 wire 还给池，处理排队中的响应或关闭。 */
  void finish_send(Worker& w, uint32_t idx, net::Connection& c) {
    app::give_buf(pool_, c.wire);
    c.wire = nullptr;
    c.write = net::WriteState::Idle;
    c.zbuf_done = false;
    c.zc_used = false;
    if ((c.out && c.out->len) || c.close_after) {
      mark(w, idx);
    } else {
      app::give_buf(pool_, c.out);
    }
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

  struct ReadyRef {
    uint32_t wi;
    uint32_t idx;
    uint16_t gen;
    uint64_t ts; /* 进入就绪队列的时刻，用于老化 */
  };

  /* backlog 存在时谁先被回答。
   *
   * 最老优先（我们原来的做法，照 #264 学的）会让每个新到的请求也排在一长串旧请求后面：230k 的
   * ramp 期所有新连接同时发第一个请求，队列一长，新请求的延迟就跟最老的请求一样高——benchmark
   * 上就是「p99 3.54 s 但 0 错误」。objc-uring-delneg #326 论证得更细：回复晚于请求 40 ms 就要
   * 多一个 bare ACK，约每请求 +20% CPU，而多出来的 CPU 又让 backlog 更难清掉。
   *
   * 所以 backlog 里最新优先（LIFO），只给等太久的请求留出口：等超过 8 s 的请求开始按最老优先
   * 回答（每 pass 取一个最老的），这样既不让多数人久等，也不让任何人被饿死。连接内部的请求
   * 顺序不变（同一连接的数据在它自己的 part 缓冲里，一次 serve 按序处理）。 */
  static constexpr uint64_t kAgedAfterNs = 8000000000ULL;

  // 把收到的字节留在连接上，本 pass 不回答。
  void defer(Worker& w, uint32_t wi, uint32_t idx, net::Connection& c, const uint8_t* data,
             size_t n) {
    if (!c.part) c.part = std::make_unique<std::vector<uint8_t>>();
    c.part->insert(c.part->end(), data, data + n);
    if (!c.ready_queued) {
      c.ready_queued = true;
      ready_.push_front(ReadyRef{wi, idx, c.gen, now_ns()});
    }
  }

  // 在 pass 还有答案额度时，按最老优先回答被延后的连接。
  void run_ready() {
    uint32_t turn = 0;
    while (!ready_.empty() && answers_left_ > 0) {
      const uint64_t now = now_ns();
      /* 队尾是最老的。它等过 8 s 之后**拿一半的额度**（objc-uring-delneg #326：the oldest get
       * half of each pass），而不是一旦老化就整批取队尾——backlog 持续时队尾会一直老化，全取
       * 队尾就退化成最老优先，新请求又被拖回原来的问题里（基准机 230k 探针 p99 3.08 s 就是
       * 这个形状）。连接内部顺序不变，`serve()` 按自己的 part 缓冲顺序处理。 */
      const bool aged = (now - ready_.back().ts) > kAgedAfterNs;
      const bool take_old = aged && ((turn++ & 1u) == 0);
      const ReadyRef r = take_old ? ready_.back() : ready_.front();
      if (take_old) {
        ready_.pop_back();
      } else {
        ready_.pop_front();
      }
      if (r.wi >= workers_.size()) continue;
      Worker& w = *workers_[r.wi];
      if (r.idx >= w.table().size()) continue;
      net::Connection& c = w.table().at(r.idx);
      if (c.gen != r.gen || !c.open || c.closing) continue;
      c.ready_queued = false;
      serve(w, r.wi, r.idx, nullptr, 0);
    }
  }

  void serve(Worker& w, uint32_t wi, uint32_t idx, const uint8_t* data, size_t n) {
    net::Connection& c = w.table().at(idx);
    if (c.close_after) return;
    std::vector<uint8_t> part;
    bool joined = false;
    if (c.part) {
      part = std::move(*c.part);
      c.part.reset();
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
      if (pr == app::http::Parsed::Done) [[likely]] {
        if (answers_left_ == 0) {
          // 本 pass 的答案额度已用完：把这个请求（以及它后面的内容）留在连接上，放到就绪队列尾部，
          // 队列按最老优先处理。
          if (!c.ready_queued) {
            c.ready_queued = true;
            ready_.push_front(ReadyRef{wi, idx, c.gen, now_ns()});
          }
          break;
        }
        if (!writes_ok_ && is_write(req)) {
          // WAL 闸门关闭：把这个请求（以及它后面的内容）留在连接上，稍后的 pass 再回答。什么都没写，
          // 所以也不发响应。
          if (!c.held) {
            c.held = true;
            w.held().push_back(idx);
          }
          break;
        }
        answers_left_--;
        mark(w, idx);
        c.answered = true;
        app_->handle(req, out_of(c), writes_ok_);
        off += used;
      } else if (pr == app::http::Parsed::Partial) [[likely]] {
        break;
      } else [[unlikely]] {
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
        c.part = std::make_unique<std::vector<uint8_t>>(buf + off, buf + len);
      } else {
        part.erase(part.begin(), part.begin() + static_cast<ptrdiff_t>(off));
        c.part = std::make_unique<std::vector<uint8_t>>(std::move(part));
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
          if (idx < kListeners && w.accepts()[idx] > 0) w.accepts()[idx]--;
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
            if (accept_errs < 20) {
              uint32_t inflight = 0;
              for (uint32_t li = 0; li < kListeners; li++) inflight += w.accepts()[li];
              errlog("accept fail res={} errno={} open={} accepts={}", res, -res, w.open(), inflight);
            }
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
          if (res > 0) [[likely]] {
            if (has) {
              c.last_active = now_sec_;
              if (answers_left_ == 0) [[unlikely]] {
                defer(w, wi, idx, c, w.bufs().data(bid), static_cast<size_t>(res));
              } else {
                serve(w, wi, idx, w.bufs().data(bid), static_cast<size_t>(res));
              }
              w.bufs().give(bid);
            }
            if (!(flags & IORING_CQE_F_MORE)) {
              c.read = net::ReadState::Queued;
              w.pending().push_back(idx);
            }
          } else if (res == -ENOBUFS) [[unlikely]] {
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
            if (answers_left_ == 0) {
              defer(w, wi, idx, c, w.bufs().data(bid), static_cast<size_t>(res));
            } else {
              serve(w, wi, idx, w.bufs().data(bid), static_cast<size_t>(res));
            }
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
          /* 零拷贝的 NOTIF：内核已经不再引用这块缓冲。数据是否已全部发出由普通完成决定，
           * 所以这里只标记并把可能已结束的发送收尾。 */
          if (flags & IORING_CQE_F_NOTIF) {
            if (c.gen != gen) break;
            c.zbuf_done = true;
            if (c.write == net::WriteState::Flushed) finish_send(w, idx, c);
            break;
          }
          if (c.gen != gen || c.write != net::WriteState::Sending) break;
          if (res < 0) {
            if (res == -EINVAL || res == -ENOSYS) {
              /* 内核/驱动不支持零拷贝发送：关掉它，用普通 send 重发这一块。 */
              zc_ok_ = false;
              c.zbuf_done = false;
              arm_send(w, wi, idx, c);
              break;
            }
            c.write = net::WriteState::Idle;
            app::give_buf(pool_, c.wire);
            c.wire = nullptr;
            c.zbuf_done = false;
            c.zc_used = false;
            app::give_buf(pool_, c.out);
            c.close_after = true;
            mark(w, idx);
          } else {
            /* IORING_CQE_F_MORE 缺席 = 内核不会发 NOTIF（例如完成被合并，或本次没有走到
             * 零拷贝路径）。c-iouring-public 的 loop.c 也是这么处理的：否则连接会永远停在
             * Flushed 等一个不会到来的完成。 */
            if (c.zc_used && !(flags & IORING_CQE_F_MORE)) c.zbuf_done = true;
            c.sent += static_cast<uint32_t>(res);
            if (c.sent < c.wire->len) {
              arm_send(w, wi, idx, c);
            } else if (c.zbuf_done) {
              finish_send(w, idx, c);
            } else {
              /* 数据已全部交给内核，等 NOTIF 再归还缓冲。 */
              c.write = net::WriteState::Flushed;
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
            c.part.reset();
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
    if (w.accepting()) {
      if (w.full()) {
        w.stop_accepting();
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
        if (!c.open || c.closing || c.write == net::WriteState::Sending ||
            c.write == net::WriteState::Flushed)
          continue;
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
  int listeners_[kListeners] = {-1, -1, -1, -1, -1, -1, -1, -1};
  app::App* app_;
  app::Pool pool_;
  uint32_t slots_;
  uint32_t slot_cap_ = 0;   /* slots the ring actually registered; may be below slots_ */
  uint32_t max_rings_ = 1;  /* kTargetConns / slots, capped */
  uint32_t answers_left_ = 0;
  uint64_t last_trim = 0;
  bool zc_ok_ = true; /* 内核支持零拷贝发送；-EINVAL 后永久关闭 */
  std::deque<ReadyRef> ready_;
  unsigned cq_;
  const char* host_ = nullptr;
  uint16_t port_ = 0;
  uint64_t responded_ = 0;
  uint64_t stat_pass = 0;
  unsigned long enter_base_[3] = {0, 0, 0};
  bool writes_ok_ = true;
  uint32_t sweep_ring_ = 0;
  size_t sweep_off_ = 0;
};

// 每个 ring 的直接描述符槽位上限，同样决定每个 ring 的连接表大小。它按目标连接数的上限定，
// 而不是按 RLIMIT_NOFILE：早先每 ring 1M 的表是 288 MB 预分配并预触的内存，2 GB 的机器在
// 300k 连接时腾不出来。
constexpr uint32_t kSlotsPerRing = 262144;

// 描述符表跟随环境的硬限制，上限是本服务想要的大小：rust-iouring-kensac #264 就是这么做的
// （min(硬 NOFILE, 1M)），这也是他们的构建能在 NOFILE 很小的 CI runner 上跑起来的原因——固定
// 262,144 项的稀疏表在那里会被 io_uring_register_files_sparse 拒绝，服务根本起不来。
uint32_t descriptor_slots() {
  struct rlimit rl {};
  getrlimit(RLIMIT_NOFILE, &rl);
  rlim_t want = rl.rlim_max;
  // 服务通常带着 LimitNOFILE=65535 启动，这把单个 ring 的文件表上限卡在 65,535 个连接。以 root
  // 运行时可以把硬限制提到 /proc/sys/fs/nr_open；提不上去（非特权）时，下面的 ring 数量会补上
  // 差额。
  if (std::FILE* f = std::fopen("/proc/sys/fs/nr_open", "r")) {
    unsigned long long nr_open = 0;
    if (std::fscanf(f, "%llu", &nr_open) == 1 && nr_open > want) want = static_cast<rlim_t>(nr_open);
    std::fclose(f);
  }
  if (want > (1u << 20)) want = 1u << 20;
  rl.rlim_cur = want;
  rl.rlim_max = want;
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  rlim_t n = rl.rlim_cur;
  if (n > kSlotsPerRing) n = kSlotsPerRing;
  if (n < 1024) n = 1024;
  return static_cast<uint32_t>(n);
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
    errlog("database: {}", err);
    return 1;
  }
  auto app = std::make_unique<app::App>(std::move(db),
                                        reinterpret_cast<const uint8_t*>(secret),
                                        std::strlen(secret));
  dbx_set_logger(shim_log_sink);
  constexpr unsigned cq = kCqEntries;
  const uint32_t slots = descriptor_slots();
  uint32_t rings_needed = (kTargetConns + slots - 1) / slots;
  if (rings_needed < 1) rings_needed = 1;
  if (rings_needed > kMaxRingsLimit) rings_needed = kMaxRingsLimit;
  Server server(app.get(), slots, cq, rings_needed);
  if (!server.init(host, static_cast<uint16_t>(std::atoi(port_s)))) {
    errlog("server init failed");
    return 1;
  }
  errlog("listening on {}:{}; up to {} rings x {} slots (={} connections), cq={}", host, port_s,
         rings_needed, slots, rings_needed * slots, cq);
  server.run();
}
