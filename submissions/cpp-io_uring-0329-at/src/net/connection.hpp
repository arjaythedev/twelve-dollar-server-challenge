#pragma once

#include <cstdint>
#include <sys/mman.h>
#include <memory>
#include <memory_resource>
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
  uint32_t sent = 0;
  uint32_t last_active = 0;
  app::ByteBuf* out = nullptr;
  app::ByteBuf* wire = nullptr;
  /* backlog 时每个被延后的连接都要在自己身上留一份未处理字节。用 pmr 的池资源接管：
   * 固定块复用、没有每次 malloc 的元数据与 1.5 倍扩容浪费（270k 连接时这一项约 400 MB）。
   * 池在 backlog 清空、malloc_trim 的时候一起 release()。 */
  std::unique_ptr<std::pmr::vector<uint8_t>> part;

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
