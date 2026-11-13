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

  bool init(unsigned sq, unsigned cq, unsigned slots) {
    struct io_uring_params p {};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL |
              IORING_SETUP_CQSIZE | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    p.cq_entries = cq;
    if (io_uring_queue_init_params(sq, &ring_, &p) < 0) {
      std::memset(&ring_, 0, sizeof ring_);
      return false;
    }
    ready_ = true;
    if (io_uring_register_files_sparse(&ring_, slots) < 0) return false;
    return true;
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
      io_uring_submit(&ring_);
      io_uring_get_events(&ring_);
      if (tries == 1000) return nullptr;
    }
  }

  void enter() {
    io_uring_submit(&ring_);
    io_uring_get_events(&ring_);
  }

  int wait(unsigned min_complete, unsigned ns) {
    struct __kernel_timespec ts {};
    ts.tv_sec = 0;
    ts.tv_nsec = ns;
    struct io_uring_cqe* cqe = nullptr;
    return io_uring_submit_and_wait_timeout(&ring_, &cqe, min_complete, &ts, nullptr);
  }

  io_uring& raw() { return ring_; }
  int fd() const { return ring_.ring_fd; }

 private:
  io_uring ring_{};
  bool ready_ = false;
};

}  // namespace io
