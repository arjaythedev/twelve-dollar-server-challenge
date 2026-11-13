#pragma once

#include <cstdint>
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
  uint32_t sent = 0;
  app::ByteBuf* out = nullptr;
  app::ByteBuf* wire = nullptr;
  std::vector<uint8_t>* part = nullptr;

  bool live() const { return open && !closing; }
};

class SlotTable {
 public:
  void resize(size_t n) { slots_.resize(n); }
  Connection& at(size_t i) { return slots_[i]; }
  const Connection& at(size_t i) const { return slots_[i]; }
  size_t size() const { return slots_.size(); }

 private:
  std::vector<Connection> slots_;
};

}  // namespace net
