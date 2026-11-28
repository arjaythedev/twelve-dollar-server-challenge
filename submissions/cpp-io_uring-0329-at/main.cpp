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

namespace app { extern unsigned long long g_commits; }  /* STATS 行的 commits/s */

namespace {

constexpr uint32_t kTargetConns = 400000;  /* connection ceiling the ring count must cover */
constexpr uint32_t kMaxRingsLimit = 16;    /* hard cap on rings (rust-iouring-kensac #264 uses 16) */
/* accept 在途数：**每个 ring 固定 512**，不随 ring 数稀释。原来是「总 2048 除以 ring 数」，
 * 而 ring 数正是随连接数增长的（300k 时约 10 个），于是每 listener 的在途 accept 从 51 掉到
 * 32，ring 再多就会撞上 per<16 的下限而完全停投。c-iouring-kensac #264 与
 * fsharp-uring-delneg #437 都是每 ring 512（每 listener 64），与 ring 数无关。
 * 顺带把 full() 的阈值也放宽：现在要到 open + 512 才判定「满」，ring 顶端的 1,536 个槽位
 * 不再被提前判掉——310k/320k 正卡在这个边界上。 */
constexpr uint32_t kAcceptDepth = 512;

// 同一端口上相互独立的 SO_REUSEPORT listener，由所有 ring 共享。每个 listener 有自己的
// accept 队列（受 net.core.somaxconn 限制），所以 8 个 listener 在 ramp 期就是 8 条待建连接
// 队列，而不是每个 ring 一条——这正是 rust-iouring-kensac #264 的做法（LISTENERS = 8），也是
// 230k 的 ramp 不会积压成 SYN 重传（1 秒/3 秒/7 秒）的原因；那种积压表现为「零错误但 p99
// 高达数秒」。
constexpr uint32_t kListeners = 8;
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

/* 一个 pass 内「发单发读 → 收回来」最多走几轮。读和答在同一个 pass 里完成，连接就不用
 * 多等一个完整的 pass（wait + drain + COMMIT + send）；pavlyhalim #245 量过两者的差别：
 * 「在批末尾发出去的读要多等一批加最多 2 ms：p95 4.7 对 2.7 ms（20,000 用户）」，kensac
 * #264 的 read_rounds 是同一形状，他们把它记成 270k → 310k 的那一步。 */
constexpr int kMaxReadRounds = 4;
/* 每 256 个完成就把 CQ 头和 buffer ring 的尾指针交还内核（见 drain）。 */
constexpr size_t kPublishEvery = 256;

// 单个 pass 最多答多少个请求。读不受此限：超过上限的接收数据留在连接上，由后续 pass 按
// 最老优先回答，所以一个繁忙的 pass 不会把它后面排队的所有请求一起拖慢
// （rust-iouring-kensac #264 同样限制 2,048，并用就绪队列保序）。
/* 每 pass 答多少请求。2,048 是错的：一个请求 27 us，2,048 个就是 55 ms，pass 本身就长，
 * 排队一叠加 p99 就失控（280k 探针 5.6 s）。fsharp-uring-delneg #437 的 ANSWERS 默认是 64，
 * 一个 pass 约 1.7 ms，backlog 被摊平成很多短 pass。 */
/* 每 pass 答多少请求。2,048 太长（一个 pass 55 ms，ramp 期 p99 失控）；64 让 pass 只有 1.7 ms，
 * 但把 COMMIT 推到 391 次/秒（约 1.2% CPU）；256 是折中：pass 6.9 ms、COMMIT 98 次/秒，本地
 * 20k 连接的 p99 从 64 时的 227 ms 降到 87.6 ms。 */
constexpr uint32_t kAnswersPerPass = 256;
/* 组提交窗口。我们每个 pass 都提交（285k 时约 2,600 次/秒），而每次只带 0.2 个写请求，
 * 于是 85% 的提交是空的；每写一条 like 要动 3 个页（likes 表叶子、user_id 自动索引、
 * likes_post_id_idx），提交越碎，同一批页就越被反复重写，WAL 帧率越高。合并成 20 ms 一次
 * 之后本地实测帧率 −34%（2,141 → 1,415 帧/秒）、COMMIT −88%（348 → 42 次/秒）。 */
constexpr uint64_t kCommitWindowNs = 20000000ULL;
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
  std::deque<std::pair<uint32_t, uint16_t>>& held() { return held_; }
  const int* listeners() const { return listeners_; }
  uint32_t* accepts() { return accepts_; }
  int& efd() { return efd_; }
  uint32_t& open() { return open_; }
  uint32_t& reads() { return reads_; }
  bool& accepting() { return accepting_; }
  bool& full() { return full_; }

  // ring 满了就停止继续投递 accept；listener 本身是共享的，为其他 ring 保持打开。
  void stop_accepting() { accepting_ = false; }
  /* 恢复接客：ring 满之后 open 会随着关闭回落，必须能重新开始，否则一次短暂的表满就永久
   * 拒绝新连接（270k/280k 的 connection refused 就是这么来的）。kensac #264 的恢复条件是
   * open + 2*margin < slots，这里用 kAcceptDepth 作 margin。 */
  void resume_accepting() { accepting_ = true; }

 private:
  uint32_t index_ = 0;
  io::Ring ring_;
  io::BufRing bufs_;
  net::SlotTable table_;
  std::vector<uint32_t> dirty_;
  std::deque<uint32_t> pending_;
  std::deque<std::pair<uint32_t, uint16_t>> held_;
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
    /* ANSWERS 覆盖每 pass 的答案上限：310,000 那次保持用的是 2,048（kensac #264 的数），
     * 本地 20k 连接的 A/B 又支持 256，留一个旋钮在评测机上复测，默认保持 256。 */
    if (const char* a = std::getenv("ANSWERS")) {
      long v = std::strtol(a, nullptr, 10);
      if (v > 0 && v <= 65536) answers_cap_ = static_cast<uint32_t>(v);
    }
    /* COMMIT_WINDOW_MS：组提交窗口。0 = 关闭（退回每 pass 一次提交），负值同关。 */
    if (const char* cw = std::getenv("COMMIT_WINDOW_MS")) {
      const long v = std::strtol(cw, nullptr, 10);
      commit_window_ns_ = v > 0 ? static_cast<uint64_t>(v) * 1000000ULL : 0;
    }
  }

  bool init(const char* host, uint16_t port) {
    host_ = host;
    port_ = port;
    app::preload_pool(pool_);
    if (!add_ring()) return false;
    memgov_init(); /* listener 已经建好，这里设接收缓冲上限 */
    return true;
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
        /* 关掉 TCP_QUICKACK：连接建立后的第一个请求由响应顺带确认，而不是内核先补一个
         * 单独的 bare ACK。ramp 期每个连接都能少一个包（fsharp-uring-delneg #437 的做法，
         * 他们因此把 p95 从 154 ms 降到 16 ms 的尾部问题里也提到过；QUICKACK=1 恢复默认）。 */
        int quickack = 0;
        setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof quickack);
        /* 接收缓冲上限设在 listener 上，accept 出来的连接会继承（c-iouring-jowinjestine #486
         * 就是这么做的：8 个 listener 一律 16 KiB，PSI 高压时再降到 8 KiB）。默认的自动调优
         * 会一路涨到上百 KB，2 GB 的机器在 300k 连接时正是被这一项顶到内存墙上的。请求只有
         * 一两百字节，16 KiB 足够放下几十个，且上限一旦设定，内核就不会再往上调。 */
        int rcvbuf = 16384;
        if (const char* v = std::getenv("RCVBUF")) rcvbuf = std::atoi(v);
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
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

  // 按本轮剩余答案额度恢复挂写请求。额度耗尽后留在原队列，下轮继续。
  void retry_held() {
    for (size_t i = 0; i < workers_.size() && answers_left_; i++) {
      const uint32_t wi = static_cast<uint32_t>((held_ring_ + i) % workers_.size());
      Worker& w = *workers_[wi];
      while (!w.held().empty() && answers_left_) {
        const auto [idx, gen] = w.held().front();
        w.held().pop_front();
        net::Connection& c = w.table().at(idx);
        if (c.gen != gen || !c.held) continue;
        c.held = false;
        if (!c.open || c.closing) continue;
        serve(w, wi, idx, nullptr, 0);
      }
      if (!answers_left_) {
        held_ring_ = (wi + 1) % workers_.size();
        break;
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
    uint64_t commit_max = 0, commit_slow = 0, enobufs_prev = 0;
    unsigned long long commits_prev = 0, wframes_prev = 0;
    uint64_t last_sweep = now_ns();
    now_sec_ = now_sec();
    for (;;) {
      const uint64_t p0 = now_ns();
      writes_ok_ = !app_->db.writes_held();
      if (!writes_ok_) held_gate++;
      answers_left_ = answers_cap_;
      if (writes_ok_) retry_held();
      // 只在确实没事可做时才阻塞：每个 pass 无条件等 1 ms，正是把循环钉在 ~70% CPU 却永远清不掉
      // 积压的原因（kensac/mojo 在有工作时都用 min_complete 0 进入）。「有工作」指等待读的 FIFO 和
      // 等待回答的就绪队列——不包括 dirty()（本 pass 产生、还没提交的响应）。把 dirty() 算进去，会让
      // 任何答过请求的 pass 都跳过等待、在完成队列为空时立刻返回：20k 连接时循环每秒跑 4,000 个 pass
      // 却只处理 2,000 个请求，一半是空转，而空转纯属系统调用和缓存开销。
      // rust-iouring-kensac #264 的 busy 标志就是 rq/ready。
      bool busy = answers_left_ != answers_cap_ || last_cqes >= 8 || !ready_.empty();
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
      last_cqes = 0;
      // 旧 ready 先获得本轮额度；否则新 CQE 可每轮耗尽额度，让老化策略永远无机会运行。
      run_ready();
      // 即使答复额度已耗尽也收割 CQ，归还接收缓冲；新请求留在连接上排队。
      for (size_t i = 0; i < workers_.size(); i++)
        last_cqes += drain(*workers_[i], static_cast<uint32_t>(i), kDrainBudget);
      // 先处理更早的 pass 不得不延后的请求，再处理本 pass 读到的：这样请求才不会排在它之后到达的
      // 所有请求后面。
      run_ready();
      const uint64_t p2 = now_ns();
      /* 单发读在本 pass 内「发出去 → 收回来 → 答掉」，最多 kMaxReadRounds 轮。原来只在
       * pass 末尾发出去，完成要等下一个 pass 的 drain，于是每个走单发读的连接都要多等一个
       * 完整的 pass（低负载时是 1 ms 的等待，突发时是整个 backlog 的长度）。只发本 pass
       * 还答得掉的个数：多读进来的请求只能 defer，而 defer 会为它分配一块 part 缓冲。 */
      reads_budget_ = answers_left_;
      for (int round = 0; round < kMaxReadRounds && reads_budget_ != 0; round++) {
        size_t issued = 0;
        for (size_t i = 0; i < workers_.size(); i++)
          issued += run_reads(*workers_[i], static_cast<uint32_t>(i));
        if (!issued) break;
        for (auto& wp : workers_) wp->ring().submit_nowait();
        size_t got = 0;
        for (size_t i = 0; i < workers_.size(); i++)
          got += drain(*workers_[i], static_cast<uint32_t>(i), kDrainBudget);
        last_cqes += got;
        if (!got) break;
        reads_budget_ = answers_left_; /* 这一轮答案用掉多少，下一轮就只能读多少 */
      }
      maybe_grow();
      const uint64_t p3 = now_ns();
      commit_and_flush();
      const uint64_t p4 = now_ns();
      /* 检查点状态机：WAL 过闸门帧数先做不挂写的大块拷贝，只剩尾段时才挂写并由普通优先级
       * 线程拷完（见 shim.c 的 CK_* 状态机）。 */
      app_->db.ck_kick();
      if (!writes_ok_) writes_ok_ = !app_->db.writes_held();
      // 按时间而不是按 pass 驱动：负载下 pass 频率没有下界，按 pass 节奏清扫会在每秒把整张槽位表
      // 走很多遍。
      /* 每 10 秒一次（且只在没有 backlog 时）把空闲堆还给内核。glibc 释放的内存默认留在进程的
       * arena 里，RSS 只增不减：backlog 期间为了 part/响应缓冲分配的那些块，在 backlog 清空后
       * 仍然算在 RSS 上。c-iouring-jowinjestine #404 记的实验是「backlog 碰过的每个连接都永久
       * 留着一块」，我们在 210k 连接时看到的 674 MB 里绝大部分就是这个。没有 backlog 才 trim，
       * 免得在正忙的时候去遍历堆。 */
      if (p4 - last_trim >= 10000000000ULL) {
        bool backlog = !ready_.empty();
        if (!backlog) {
          for (auto& wp : workers_) {
            if (!wp->pending().empty()) {
              backlog = true;
              break;
            }
          }
        }
        /* backlog 清空时每 10 秒收一次；backlog 持续不断时也每 30 秒收一次——原来只要还有
         * 积压就永远不 trim，而 270k 的保持期恰恰一直是那种状态，堆只涨不还。
         * malloc_trim 走 mprotect/brk，几十微秒量级，30 秒一次不影响 p99。 */
        if (!backlog) {
          last_trim = p4;
          malloc_trim(0);
        } else if (p4 - last_trim >= 30000000000ULL) {
          last_trim = p4;
          malloc_trim(0);
        }
      }
      if (p4 - last_sweep >= 50000000ULL) {
        last_sweep = p4;
        now_sec_ = now_sec();
        sweep_slice();
      }
      const uint64_t p5 = now_ns();
      memgov_tick(p5);
      t_wait += p1 - p0;
      t_drain += p2 - p1;
      t_read += p3 - p2;
      t_commit += p4 - p3;
      t_sweep += p5 - p4;
      if (p4 - p3 > commit_max) commit_max = p4 - p3;
      if (p4 - p3 > 5000000ULL) commit_slow++;
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
          errlog("stats: pass={}/s cqes={} req={} enobufs={} gated={}/{} wait={:.1f}% drain={:.1f}% "
                 "read={:.1f}% commit={:.1f}% (max {:.1f}ms, >5ms {}) sweep={:.1f}% pool_miss={} "
                 "pool={} enter[wait={} sqe={} ring={}] commits={}/s walframes={}/s likecache={} authcache={} feedtbl={}",
                 pass - stat_pass, cqes_sum, requests_sum, enobufs_ - enobufs_prev, held_gate,
                 pass - stat_pass, 100.0 * t_wait / (p5 - stat_at), 100.0 * t_drain / (p5 - stat_at),
                 100.0 * t_read / (p5 - stat_at), 100.0 * t_commit / (p5 - stat_at),
                 commit_max / 1e6, commit_slow, 100.0 * t_sweep / (p5 - stat_at),
                 app::pool_misses(), pool_.size(), io::Ring::enters(0) - enter_base_[0],
                 io::Ring::enters(1) - enter_base_[1], io::Ring::enters(2) - enter_base_[2],
                 app::g_commits - commits_prev, app_->db.wal_frames_total() - wframes_prev,
                 static_cast<int>(app_->db.like_cache_on()),
                 static_cast<int>(app_->db.author_cache_on()),
                 static_cast<int>(app_->db.feed_from_table()));
          enter_base_[0] = io::Ring::enters(0);
          enter_base_[1] = io::Ring::enters(1);
          enter_base_[2] = io::Ring::enters(2);
          stat_at = p5;
          stat_pass = pass;
          t_wait = t_drain = t_read = t_commit = t_sweep = 0;
          cqes_sum = requests_sum = held_gate = 0;
          commit_max = commit_slow = 0;
          enobufs_prev = enobufs_;
          commits_prev = app::g_commits;
          wframes_prev = app_->db.wal_frames_total();
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
    const uint32_t per = kAcceptDepth / kListeners;
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
      /* MSG_WAITALL：让 io_uring 自己把短写补完，完成就是全部发完，省掉一次「再挂一个
       * send」的 SQE 和完成（c-iouring-jowinjestine #486 与 c-iouring-pavlyhalim #245
       * 的发送都是 MSG_NOSIGNAL|MSG_WAITALL）。续传也走这里。 */
      io_uring_prep_send(sqe, static_cast<int>(idx), c.wire->p + c.sent, c.wire->len - c.sent,
                         MSG_NOSIGNAL | MSG_WAITALL);
    }
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->user_data = ev(kEvSend, wi, c.gen, idx);
    c.write = net::WriteState::Sending;
  }

  /* 一次发送彻底完成后的收尾：把 wire 还给池，处理排队中的响应或关闭。 */
  /* ---- TCP 接收内存治理（c-iouring-jowinjestine #486） --------------------------------
   * 只改**未来 accept** 的连接继承到的缓冲上限，绝不动已有连接、也不节流或拒绝任何连接。
   * cgroup v2 的 PSI 与 sock 统计每 2 秒采一次，避免在热路径上做文件 I/O。 */
  struct MemGov {
    bool enabled = false;
    int normal = 16384;
    int pressure = 8192;
    int current = 16384;
    uint32_t high_streak = 0;
    uint32_t low_streak = 0;
    uint64_t last_ns = 0;
    double psi_some = 0.0;
    double psi_full = 0.0;
    uint64_t cg_cur = 0;
    uint64_t cg_max = 0;
    uint64_t cg_sock = 0;
    uint64_t changes = 0;
    uint64_t samples = 0;
  } mg_;

  static bool slurp(const char* path, char* buf, size_t cap) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = ::read(fd, buf, cap - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
  }

  static uint64_t read_u64(const char* path) {
    char buf[64];
    if (!slurp(path, buf, sizeof buf) || buf[0] < '0' || buf[0] > '9') return 0; /* "max" → 0 */
    return std::strtoull(buf, nullptr, 10);
  }

  bool apply_listener_rcvbuf(int v) {
    for (uint32_t li = 0; li < kListeners; li++) {
      if (listeners_[li] < 0) continue;
      if (::setsockopt(listeners_[li], SOL_SOCKET, SO_RCVBUF, &v, sizeof v) < 0) return false;
    }
    mg_.current = v;
    mg_.changes++;
    return true;
  }

  void memgov_init() {
    if (const char* e = std::getenv("TCP_MEM_GOVERNOR"); e && e[0] == '0') return;
    if (const char* e = std::getenv("TCP_RCVBUF_NORMAL")) mg_.normal = std::atoi(e);
    if (const char* e = std::getenv("TCP_RCVBUF_PRESSURE")) mg_.pressure = std::atoi(e);
    if (mg_.pressure < 4096 || mg_.normal > (1 << 20) || mg_.pressure > mg_.normal) return;
    if (apply_listener_rcvbuf(mg_.normal)) mg_.enabled = true;
  }

  void memgov_tick(uint64_t now) {
    if (!mg_.enabled || (mg_.last_ns && now - mg_.last_ns < 2000000000ULL)) return;
    mg_.last_ns = now;
    mg_.samples++;
    mg_.cg_cur = read_u64("/sys/fs/cgroup/memory.current");
    mg_.cg_max = read_u64("/sys/fs/cgroup/memory.max");
    mg_.cg_sock = 0;
    mg_.psi_some = 0.0;
    mg_.psi_full = 0.0;
    char buf[8192];
    bool psi_ok = false;
    if (slurp("/sys/fs/cgroup/memory.stat", buf, sizeof buf)) {
      if (const char* p = std::strstr(buf, "sock ")) mg_.cg_sock = std::strtoull(p + 5, nullptr, 10);
    }
    if (slurp("/sys/fs/cgroup/memory.pressure", buf, sizeof buf)) {
      const char* p = std::strstr(buf, "some avg10=");
      const char* q = std::strstr(buf, "full avg10=");
      if (p && q && std::sscanf(p, "some avg10=%lf", &mg_.psi_some) == 1 &&
          std::sscanf(q, "full avg10=%lf", &mg_.psi_full) == 1)
        psi_ok = true;
    }
    /* 高压：PSI 明显停顿，或者 cgroup 已经用掉 85% 以上。 */
    const bool high = (psi_ok && (mg_.psi_full >= 2.0 || mg_.psi_some >= 10.0)) ||
                      (mg_.cg_max && mg_.cg_cur > mg_.cg_max * 85 / 100);
    if (high) {
      mg_.high_streak++;
      mg_.low_streak = 0;
    } else {
      mg_.low_streak++;
      mg_.high_streak = 0;
    }
    if (mg_.high_streak >= 2 && mg_.current != mg_.pressure) {
      if (apply_listener_rcvbuf(mg_.pressure)) mg_.high_streak = 0;
    } else if (mg_.low_streak >= 4 && mg_.current != mg_.normal) {
      if (apply_listener_rcvbuf(mg_.normal)) mg_.low_streak = 0;
    }
  }

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
   * 所以 backlog 里最新优先（LIFO），只给等太久的请求留出口：等过 8 s 的请求拿走一半的答案
   * 额度、等过 16 s 的全拿走（见 run_ready），既不让多数人久等，也不让任何人被饿死。连接内部
   * 的请求顺序不变（同一连接的数据在它自己的 part 缓冲里，一次 serve 按序处理）。 */
  static constexpr uint64_t kAgedAfterNs = 8000000000ULL;

  // 把收到的字节留在连接上，本 pass 不回答。
  void defer(uint32_t wi, uint32_t idx, net::Connection& c, const uint8_t* data, size_t n) {
    if (!c.part) c.part = std::make_unique<std::vector<uint8_t>>();
    c.part->insert(c.part->end(), data, data + n);
    if (!c.held && !c.ready_queued) {
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
      const uint64_t waited = now - ready_.back().ts;
      const bool aged = waited > kAgedAfterNs;
      /* 8 秒拿一半，16 秒全给老的（objc-uring-delneg #326 的第二级）——再久也不让任何请求
       * 无限期被新请求插队。 */
      const bool very_old = waited > 2 * kAgedAfterNs;
      const bool take_old = very_old || (aged && ((turn++ & 1u) == 0));
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
    if (c.held) {
      if (n) defer(wi, idx, c, data, n);
      return;
    }
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
            w.held().emplace_back(idx, c.gen);
          }
          break;
        }
        answers_left_--;
        mark(w, idx);
        c.answered = true;
        app_->handle(req, out_of(c), writes_ok_);
        if (app_->wrote_last) c.wrote_db = true;
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
    io_uring& r = w.ring().raw();
    size_t n = 0;
    size_t unposted = 0; /* 已经处理、但还没交还内核的完成数 */
    unsigned head = *r.cq.khead;
    const unsigned tail = io_uring_smp_load_acquire(r.cq.ktail);
    while (head != tail && n < budget) {
      const struct io_uring_cqe* cqe = &r.cq.cqes[head & *r.cq.kring_mask];
      head++;
      n++;
      unposted++;
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
                defer(wi, idx, c, w.bufs().data(bid), static_cast<size_t>(res));
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
            enobufs_++;
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
              defer(wi, idx, c, w.bufs().data(bid), static_cast<size_t>(res));
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
            enobufs_++;
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
            } else if (c.zbuf_done || !c.zc_used) {
              /* 普通 send 不会有 NOTIF，完成即可收尾；只有真正走过零拷贝的才等 NOTIF。
               * 早先这里少判了 !c.zc_used，于是 post/like（响应 < 4 KB，走普通 send）完成后也
               * 被置成 Flushed，永远等一个不会来的通知——连接从此不再发响应，表现就是 CPU
               * 空闲、请求 60 秒超时，而 42 项测试照过（响应其实已经发出去了）。 */
              finish_send(w, idx, c);
            } else {
              c.write = net::WriteState::Flushed;
            }
          }
          break;
        }
        case kEvEvfd: {
          const uint32_t sw = static_cast<uint32_t>((u >> 48) & 0xFF);
          if (!(flags & IORING_CQE_F_MORE) && sw < workers_.size()) {
            io_uring_sqe* sqe = workers_[0]->ring().sqe(1);
            if (sqe) {
              io_uring_prep_poll_multishot(sqe, workers_[sw]->efd(), POLLIN);
              sqe->user_data = ev(kEvEvfd, sw, 0, 0);
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
      /* 每 256 个完成就把 CQ 头还给内核、把 buffer ring 的尾指针发布出去。一个 drain 最多
       * 处理 kDrainBudget = 2,048 个完成，而 provided-buffer ring 正好 2,048 项：整批结束时才
       * 发布的话，内核在 drain 中途就会把接收缓冲用光，剩下的 multishot receive 全部以
       * -ENOBUFS 结束、被塞进 pending 队列、等下一个 pass 的单发读——ramp 与突发之后每个连接
       * 都立刻撞上这件事（c-iouring-jowinjestine #486 的中途 publish，他们量到 2.15M 次
       * ENOBUFS，publish 之后降到 119k，最长的一次循环轮次 1.9 s → 0.65 s）。 */
      if ((unposted & (kPublishEvery - 1)) == 0) {
        io_uring_cq_advance(&r, static_cast<unsigned>(unposted));
        unposted = 0;
        w.bufs().publish();
      }
    }
    if (unposted) io_uring_cq_advance(&r, static_cast<unsigned>(unposted));
    w.bufs().publish();
    if (w.accepting()) {
      if (w.full()) {
        w.stop_accepting();
      } else {
        arm_accept(w, wi);
      }
    } else if (w.open() + 2 * kAcceptDepth < w.table().size()) {
      /* 从「满」里退出来了：让出余量（两个 margin）再恢复，避免在边界上来回抖。 */
      w.full() = false;
      w.resume_accepting();
      arm_accept(w, wi);
    }
    return n;
  }

  /* 给排队等读的连接各发一个单发读，返回发出的个数。`reads_budget_` 是本 pass 一共还能
   * 答多少个请求：读进来的数据一定答得掉，不会被 defer 到就绪队列（那要为每个连接分配
   * 一块 part 缓冲，正是突发时最不该做的分配）。 */
  size_t run_reads(Worker& w, uint32_t wi) {
    size_t n = 0;
    while (reads_budget_ && !w.pending().empty() && w.reads() < kBufCount) {
      const uint32_t idx = w.pending().front();
      w.pending().pop_front();
      net::Connection& c = w.table().at(idx);
      if (!c.open || c.closing || c.read != net::ReadState::Queued) continue;
      w.reads()++;
      reads_budget_--;
      n++;
      arm_read(w, wi, idx, c.gen);
    }
    return n;
  }

  /* 把一条已答完的连接收尾：要么把响应交给内核，要么关连接。*/
  void flush_response(Worker& w, uint32_t idx, bool commit_ok) {
    net::Connection& c = w.table().at(idx);
    if (!commit_ok && c.answered) {
      app::ByteBuf& out = out_of(c);
      out.len = 0;
      app::error(out, 500, "internal server error");
      c.close_after = true;
    }
    c.wrote_db = false;
    c.in_batch = false;
    c.answered = false;
    responded_++;
    if (!c.open || c.closing || c.write == net::WriteState::Sending ||
        c.write == net::WriteState::Flushed)
      return;
    if (c.out && c.out->len) {
      c.wire = c.out;
      c.out = nullptr;
      c.sent = 0;
      arm_send(w, w.index(), idx, c);
    } else if (c.close_after) {
      arm_close(w, w.index(), idx, c);
    }
  }

  void commit_and_flush() {
    const uint64_t now = now_ns();
    const bool have_pending = !pend_commit_.empty();
    const bool need = app_->wrote || have_pending;
    /* One SQLite connection shares the transaction across all clients. Once it has
     * writes, any read can observe them: retain every dirty connection until COMMIT,
     * not just the writer. Preserve whole-connection response ordering as well. */
    if (need && commit_window_ns_ != 0 && last_commit_ns_ != 0 &&
        now - last_commit_ns_ < commit_window_ns_) {
      for (auto& wp : workers_) {
        Worker& w = *wp;
        for (uint32_t idx : w.dirty()) {
          net::Connection& c = w.table().at(idx);
          c.in_batch = false;
          /* Even another client's read may depend on this uncommitted transaction.
           * Keep answered set so a failed COMMIT replaces its response with a 500.
           * commit_pending deduplicates clients touched again in later passes. */
          if (c.open && !c.closing && !c.commit_pending) {
            c.commit_pending = true;
            pend_commit_.push_back(ReadyRef{w.index(), idx, c.gen, 0});
          }
        }
        w.dirty().clear();
      }
      return;
    }
    bool ok = true;
    if (need) {
      ok = app_->db.commit();
      last_commit_ns_ = now;
    }
    app_->wrote = false;
    /* 先补发上一轮攒下的写响应，再发本 pass 的。*/
    if (!pend_commit_.empty()) {
      for (const ReadyRef& pr : pend_commit_) {
        if (pr.wi >= workers_.size()) continue;
        Worker& w = *workers_[pr.wi];
        net::Connection& c = w.table().at(pr.idx);
        /* 槽位已被回收复用：旧响应跟着旧连接一起没了，别发给新连接。 */
        if (c.gen != pr.gen) continue;
        c.commit_pending = false;
        flush_response(w, pr.idx, ok);
      }
      pend_commit_.clear();
    }
    for (auto& wp : workers_) {
      Worker& w = *wp;
      for (uint32_t idx : w.dirty()) flush_response(w, idx, ok);
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
  unsigned cq_;
  uint32_t max_rings_ = 1;  /* kTargetConns / slots, capped */
  size_t held_ring_ = 0;
  uint32_t answers_left_ = 0;
  uint32_t answers_cap_ = kAnswersPerPass;
  uint32_t reads_budget_ = 0;
  uint64_t enobufs_ = 0;
  uint64_t last_trim = 0;
  uint64_t last_commit_ns_ = 0;
    uint64_t commit_window_ns_ = kCommitWindowNs;
  /* 等提交的写响应：(worker, slot)。提交后统一发出去。 */
    /* 等提交的写响应。带 gen：推迟期间连接可能被关闭、槽位被新连接复用，
     * 只按 (worker, slot) 找回来就会把旧响应发给别人。 */
    std::vector<ReadyRef> pend_commit_;
  bool zc_ok_ = true; /* 内核支持零拷贝发送；-EINVAL 后永久关闭 */
  std::deque<ReadyRef> ready_;
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
  /* 只留一个 malloc arena：事件循环和 checkpoint 线程轮流跑，各自一个 arena 意味着两份
   * 高水位，而 glibc 在 free 之后不会把 arena 缩回去（c-iouring-raoz 的做法）。 */
  mallopt(M_ARENA_MAX, 1);
  std::signal(SIGTERM, on_term);
  std::signal(SIGINT, on_term);
  /* 把可执行文件的**所有**映射（text / rodata / data）都锁进内存。文件页正是内存紧张时被
   * 回收的那一类，而 consteval 拼出来的响应头、JSON 转义表、base64url 表都在 rodata 里，
   * 每个请求都要碰：只锁 r-x 段的话，内存墙上一旦发生回收，主循环就要为这些页等一次
   * major fault（c-iouring-jowinjestine #486 与 c-iouring-pavlyhalim #245 都锁整个映像）。 */
  {
    char exe[512] = {};
    const ssize_t el = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::FILE* mf = std::fopen("/proc/self/maps", "r");
    if (mf) {
      char line[1024];
      while (std::fgets(line, sizeof line, mf)) {
        unsigned long a = 0, b = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        const bool is_exe = el > 0 && std::strstr(line, exe) != nullptr;
        if ((is_exe || perms[2] == 'x') && b > a)
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
