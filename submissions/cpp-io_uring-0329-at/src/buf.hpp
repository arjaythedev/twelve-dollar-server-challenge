#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace app {

inline constexpr uint32_t kOutCap = 8192;

struct ByteBuf {
  uint8_t* p = nullptr;
  uint32_t len = 0;
  uint32_t cap = 0;
};

[[noreturn]] inline void buf_oom() {
  std::fprintf(stderr, "out of memory\n");
  std::abort();
}

inline void buf_reserve(ByteBuf& b, uint32_t need) {
  if (need <= b.cap) return;
  uint32_t cap = b.cap ? b.cap : 256;
  while (cap < need) {
    uint32_t next = cap + (cap >> 1) + 64;
    if (next < cap) buf_oom();
    cap = next;
  }
  auto* np = static_cast<uint8_t*>(std::realloc(b.p, cap));
  if (!np) buf_oom();
  b.p = np;
  b.cap = cap;
}

inline void buf_set_len(ByteBuf& b, uint32_t n) {
  if (n > b.cap) buf_reserve(b, n);
  b.len = n;
}

inline void buf_append(ByteBuf& b, const void* s, size_t n) {
  if (n == 0) return;
  buf_reserve(b, b.len + static_cast<uint32_t>(n));
  std::memcpy(b.p + b.len, s, n);
  b.len += static_cast<uint32_t>(n);
}

template <size_t N>
inline void buf_append(ByteBuf& b, const char (&s)[N]) {
  buf_append(b, s, N - 1);
}

inline void buf_put(ByteBuf& b, uint8_t c) {
  buf_reserve(b, b.len + 1);
  b.p[b.len++] = c;
}

inline void buf_clear(ByteBuf& b) { b.len = 0; }

inline void buf_release(ByteBuf& b) {
  std::free(b.p);
  b.p = nullptr;
  b.len = 0;
  b.cap = 0;
}

using Pool = std::vector<ByteBuf*>;

inline ByteBuf* take_buf(Pool& pool) {
  if (!pool.empty()) {
    ByteBuf* b = pool.back();
    pool.pop_back();
    b->len = 0;
    return b;
  }
  auto* b = new ByteBuf;
  buf_reserve(*b, kOutCap);
  return b;
}

inline void give_buf(Pool& pool, ByteBuf*& b) {
  if (!b) return;
  if (pool.size() < 2048 && b->cap <= 4 * kOutCap) {
    b->len = 0;
    pool.push_back(b);
  } else {
    buf_release(*b);
    delete b;
  }
  b = nullptr;
}

}
