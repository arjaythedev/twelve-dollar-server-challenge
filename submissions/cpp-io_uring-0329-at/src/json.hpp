#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "buf.hpp"

namespace app::json {

enum class Kind : uint8_t { Str, Num, Other };

struct Val {
  Kind kind = Kind::Other;

  const uint8_t* s = nullptr;
  uint32_t n = 0;

  bool esc = false;
  double num = 0.0;
};

using MemberFn = void (*)(void* ctx, const uint8_t* k, uint32_t klen, const Val& v);

enum : int { kParseBad = -1, kParseOther = 0, kParseObject = 1 };

int parse_object(const uint8_t* s, size_t n, MemberFn f, void* ctx);

bool valid_utf8(const uint8_t* s, size_t n);

void unescape(const uint8_t* raw, size_t n, std::vector<uint8_t>& out);

constexpr size_t esc_bound(size_t n) { return 6 * n; }

inline constexpr size_t INT_BOUND = 20;

void escape_into(ByteBuf& out, const uint8_t* s, size_t n);
inline void escape_into(ByteBuf& out, const std::vector<uint8_t>& s) { escape_into(out, s.data(), s.size()); }

void push_int(ByteBuf& out, int64_t v);

struct Cur {
  uint8_t* p;
  uint8_t* end;

  inline void lit(const void* s, size_t n) {
    std::memcpy(p, s, n);
    p += n;
  }
  template <size_t N>
  inline void lit(const char (&s)[N]) {
    std::memcpy(p, s, N - 1);
    p += N - 1;
  }
  inline void byte(uint8_t b) { *p++ = b; }

  inline void raw(const uint8_t* s, size_t n) {
    std::memcpy(p, s, n);
    p += n;
  }

  void int_(int64_t v);

  void esc(const uint8_t* s, size_t n);
};

template <typename F>
inline void fill(ByteBuf& out, size_t bound, F&& f) {
  const uint32_t base = out.len;
  buf_reserve(out, static_cast<uint32_t>(base + bound));
  Cur c{out.p + base, out.p + base + bound};
  f(c);
  out.len = static_cast<uint32_t>(c.p - out.p);
}

}
