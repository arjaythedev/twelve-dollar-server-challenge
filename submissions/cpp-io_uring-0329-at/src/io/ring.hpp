#pragma once

#include <liburing.h>
#include <cerrno>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

namespace io {
inline void prefault(void* p, size_t n) {
  if (!p || !n) return;
  madvise(p, n, MADV_HUGEPAGE);
  madvise(p, n, MADV_POPULATE_READ);
}
}
#include <sys/syscall.h>
#include <unistd.h>

namespace io {

class BufRing {
 public:
  BufRing() = default;
  ~BufRing() {
    if (ring_) munmap(ring_, static_cast<size_t>(count_) * 16);
    std::free(mem_);
  }
  BufRing(const BufRing&) = delete;
  BufRing& operator=(const BufRing&) = delete;

  bool init(int ring_fd, uint16_t count, size_t size) {
    count_ = count;
    size_ = size;
    void* ring = mmap(nullptr, static_cast<size_t>(count) * 16, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ring == MAP_FAILED) return false;
    ring_ = static_cast<uint8_t*>(ring);
    mem_ = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(count) * size));
    if (!mem_) return false;
    prefault(mem_, static_cast<size_t>(count) * size);
    if (!register_ring(ring_fd)) return false;
    for (uint16_t i = 0; i < count; i++) give(i);
    publish();
    return true;
  }

  void give(uint16_t bid) {
    auto* e = reinterpret_cast<struct io_uring_buf*>(ring_ + (tail_ & (count_ - 1)) * 16);
    e->addr = reinterpret_cast<uint64_t>(mem_ + static_cast<size_t>(bid) * size_);
    e->len = static_cast<uint32_t>(size_);
    e->bid = bid;
    tail_++;
  }

  void publish() {
    __atomic_store_n(reinterpret_cast<uint16_t*>(ring_ + 14), tail_, __ATOMIC_RELEASE);
  }

  const uint8_t* data(uint16_t bid) const { return mem_ + static_cast<size_t>(bid) * size_; }
  uint16_t count() const { return count_; }
  size_t buf_size() const { return size_; }

 private:
  bool register_ring(int fd) {
    struct io_uring_buf_reg reg {};
    reg.ring_addr = reinterpret_cast<uint64_t>(ring_);
    reg.ring_entries = count_;
    reg.bgid = 0;
    int rc = 0;
    for (int i = 0; i < 2; i++) {
      rc = static_cast<int>(syscall(SYS_io_uring_register, fd, IORING_REGISTER_PBUF_RING, &reg, 1));
      if (rc == 0 || errno != EINVAL) break;
      reg.resv[0] = 1;
    }
    return rc == 0;
  }

  uint8_t* ring_ = nullptr;
  uint8_t* mem_ = nullptr;
  uint16_t tail_ = 0;
  uint16_t count_ = 0;
  size_t size_ = 0;
};

class Ring {
 public:
  Ring() = default;
  ~Ring() {
    if (ready_) io_uring_queue_exit(&ring_);
  }
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  // `defer` 只设在循环阻塞等待的那个 ring 上：DEFER_TASKRUN 把内核的 task work 攒到
  // 那一次 enter 后面，正是让等待能低成本停靠线程的原因。其他 ring 不能 defer，它们的
  // 完成由内核即时投递，循环可以直接从共享内存读它们的队列
  // （c-iouring-pavlyhalim #245 的做法；每个 pass 都 enter 每个 ring，在全部 ring 繁忙时
  // 是每批 4.7 次系统调用）。
  bool init(unsigned sq, unsigned cq, unsigned slots, bool defer, unsigned* err) {
    struct io_uring_params p {};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    if (defer) p.flags |= IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    p.cq_entries = cq;
    int rc = io_uring_queue_init_params(sq, &ring_, &p);
    if (rc < 0) {
      *err = static_cast<unsigned>(-rc);
      std::memset(&ring_, 0, sizeof ring_);
      return false;
    }
    ready_ = true;
    rc = io_uring_register_files_sparse(&ring_, slots);
    if (rc < 0) {
      *err = static_cast<unsigned>(-rc);
      return false;
    }
    return true;
  }

  // 内核入口的来源，便于把系统调用密集的循环归因到具体位置而不是靠猜：0 = 阻塞等待，
  // 1 = 提交队列满迫使 sqe() 内部冲刷，2 = 其他 ring 的每 pass enter。STATS=1 会打印
  // 这三个计数器。
  static unsigned long& enters(int kind) {
    static unsigned long c[3] = {0, 0, 0};
    return c[kind];
  }

  io_uring_sqe* sqe(unsigned room) {
    for (unsigned tries = 0;; tries++) {
      if (io_uring_sq_space_left(&ring_) >= room) {
        io_uring_sqe* s = io_uring_get_sqe(&ring_);
        if (s) {
          std::memset(s, 0, sizeof *s);
          return s;
        }
      }
      enters(1)++;
      io_uring_submit(&ring_);
      io_uring_get_events(&ring_);
      if (tries == 1000) return nullptr;
    }
  }

  // 如果这个 ring 的 SQE 有积压，就发布出去。不调用 get_events：一个不 defer task work
  // 的 ring 不需要为了完成而进入内核，而每个 pass 都 enter 它正是让循环无法随 ring 数
  // 扩展的那笔系统调用账。
  void submit_if_ready() {
    if (io_uring_sq_ready(&ring_) > 0) {
      enters(2)++;
      io_uring_submit(&ring_);
    }
  }

  // 内核已经投递的 CQE，直接从共享内存读取，不发系统调用。
  unsigned ready_cqes() { return io_uring_cq_ready(&ring_); }

  int wait(unsigned min_complete, unsigned ns) {
    enters(0)++;
    struct __kernel_timespec ts {};
    ts.tv_sec = 0;
    ts.tv_nsec = ns;
    struct io_uring_cqe* cqe = nullptr;
    return io_uring_submit_and_wait_timeout(&ring_, &cqe, min_complete, &ts, nullptr);
  }

  io_uring& raw() { return ring_; }
  int fd() const { return ring_.ring_fd; }
  int register_eventfd(int efd) { return io_uring_register_eventfd(&ring_, efd); }

 private:
  io_uring ring_{};
  bool ready_ = false;
};

}  // namespace io
