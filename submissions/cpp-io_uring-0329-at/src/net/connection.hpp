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
  uint32_t sent = 0;
  uint32_t last_active = 0;
  app::ByteBuf* out = nullptr;
  app::ByteBuf* wire = nullptr;
  std::unique_ptr<std::vector<uint8_t>> part; /* 不完整请求的暂存；唯一所有者 */

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
