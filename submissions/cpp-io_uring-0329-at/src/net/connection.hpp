#pragma once

#include <cstdint>
#include <sys/mman.h>
#include <memory>
#include <vector>

#include "buf.hpp"

namespace net {

enum class ReadState : uint8_t {
  Idle,
  Queued,
  Reading,
};

enum class WriteState : uint8_t {
  Idle,
  Sending,
  /* 数据已经全部交给内核（零拷贝发送返回了完整长度），但内核可能还在读这块缓冲：
   * 必须等 IORING_CQE_F_NOTIF 到达才能把它还回池里。 */
  Flushed,
};

struct Connection {
  int32_t fd = -1;
  uint16_t gen = 0;
  ReadState read = ReadState::Idle;
  WriteState write = WriteState::Idle;
  bool open = false;
  bool closing = false;
  bool close_after = false;
  bool in_batch = false;
  bool answered = false;
  bool held = false;
  bool ready_queued = false;
  bool zbuf_done = false; /* 零拷贝发送的 NOTIF 已到 */
  bool zc_used = false;   /* 这次发送用的是 SEND_ZC（决定要不要等 NOTIF） */
  /* 这条连接攒着的响应里包含写（like/发帖），必须在 COMMIT 返回之后才能交给内核。
   * 组提交把 20 ms 内的写攒到一起，未提交前这些响应就留在 c.out 里不发。 */
  bool wrote_db = false;
  /* 已经在 pend_commit_ 里等着了（推迟可能跨多个 pass，别重复入队） */
  bool commit_pending = false;
  uint32_t sent = 0;
  uint32_t last_active = 0;
  app::ByteBuf* out = nullptr;
  app::ByteBuf* wire = nullptr;
  /* 不完整请求的暂存；唯一所有者。刻意不用 pmr 池：池会持有不归还的空闲块，
   * 270k 连接时实测比 std::vector + malloc_trim 还高 117 MB。 */
  std::unique_ptr<std::vector<uint8_t>> part;

  bool live() const { return open && !closing; }
};

class SlotTable {
 public:
  void resize(size_t n) {
    slots_.resize(n);
    if (!n) return;
    void* p = slots_.data();
    size_t bytes = n * sizeof(Connection);
    madvise(p, bytes, MADV_HUGEPAGE);
    madvise(p, bytes, MADV_POPULATE_READ);
  }
  Connection& at(size_t i) { return slots_[i]; }
  const Connection& at(size_t i) const { return slots_[i]; }
  size_t size() const { return slots_.size(); }

 private:
  std::vector<Connection> slots_;
};

}  // namespace net
