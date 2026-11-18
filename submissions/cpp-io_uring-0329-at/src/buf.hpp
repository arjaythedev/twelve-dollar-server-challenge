#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace app {

inline constexpr uint32_t kOutCap = 8192; /* a feed response is ~5.5 KB; posts ~300 B */

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


inline void buf_release(ByteBuf& b) {
  std::free(b.p);
  b.p = nullptr;
  b.len = 0;
  b.cap = 0;
}

using Pool = std::vector<ByteBuf*>;

// 响应缓冲按连接持有，只活到该响应的 send 完成，所以池决定了在途内存的上限：
// 4,096 个可复用缓冲，每个不超过 8 x kOutCap 字节。池被抽干时（在途数超过上限，
// 例如检查点停顿期间）会新分配一个并计数，于是 miss 率会出现在 STATS 行里，
// 而不是悄悄变成每个响应一次分配。
inline unsigned long& pool_misses() {
  static unsigned long misses = 0;
  return misses;
}

constexpr size_t kPoolCap = 4096;
// 启动时预填：响应缓冲在收割完成时取走、在 send 完成时归还，所以繁忙的 pass 里
// 每次 take 都发生在对应的 give 之前。因此一个空池会在每个 pass 的每个请求上 miss
// （实测：每个响应一次分配，10 秒内 37,738 次 brk）。事先把池填满，就变成纯粹的
// 栈式复用。
constexpr size_t kPoolPreload = 256;

inline void preload_pool(Pool& pool) {
  pool.reserve(kPoolPreload);
  for (size_t i = 0; i < kPoolPreload; i++) {
    auto* b = new ByteBuf;
    buf_reserve(*b, kOutCap);
    pool.push_back(b);
  }
}

inline ByteBuf* take_buf(Pool& pool) {
  if (!pool.empty()) {
    ByteBuf* b = pool.back();
    pool.pop_back();
    b->len = 0;
    return b;
  }
  pool_misses()++;
  auto* b = new ByteBuf;
  buf_reserve(*b, kOutCap);
  return b;
}

inline void give_buf(Pool& pool, ByteBuf*& b) {
  if (!b) return;
  if (pool.size() < kPoolCap && b->cap <= 8 * kOutCap) {
    b->len = 0;
    pool.push_back(b);
  } else {
    buf_release(*b);
    delete b;
  }
  b = nullptr;
}

}
