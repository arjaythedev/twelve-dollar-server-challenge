#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace app::json {
namespace {

constexpr uint32_t kMaxDepth = 64;

constexpr bool is_digit(uint8_t c) { return c >= '0' && c <= '9'; }
constexpr bool is_hex(uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

struct P {
  const uint8_t* s;
  size_t n;
  size_t i;

  inline void ws() {
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
  }
  inline bool peek(uint8_t& c) const {
    if (i >= n) return false;
    c = s[i];
    return true;
  }
  bool lit(const char* w, size_t wl) {
    if (i + wl > n || std::memcmp(s + i, w, wl) != 0) return false;
    i += wl;
    return true;
  }

  bool string(const uint8_t*& raw, uint32_t& raw_len, bool& esc) {
    const size_t start = i;
    esc = false;
    for (;;) {
      if (i >= n) return false;
      uint8_t c = s[i];
      if (c == '"') {
        raw = s + start;
        raw_len = static_cast<uint32_t>(i - start);
        i++;
        return true;
      }
      if (c == '\\') {
        esc = true;
        if (i + 1 >= n) return false;
        uint8_t e = s[i + 1];
        switch (e) {
          case '"':
          case '\\':
          case '/':
          case 'b':
          case 'f':
          case 'n':
          case 'r':
          case 't':
            i += 2;
            break;
          case 'u':
            if (i + 6 > n) return false;
            for (size_t k = 2; k < 6; k++)
              if (!is_hex(s[i + k])) return false;
            i += 6;
            break;
          default:
            return false;
        }
        continue;
      }
      if (c <= 0x1f) return false;
      i++;
    }
  }
  bool number(double& out) {
    const size_t start = i;
    uint8_t c;
    if (peek(c) && c == '-') i++;
    if (!peek(c)) return false;
    if (c == '0') {
      i++;
    } else if (c >= '1' && c <= '9') {
      while (peek(c) && is_digit(c)) i++;
    } else {
      return false;
    }
    if (peek(c) && c == '.') {
      i++;
      if (!peek(c) || !is_digit(c)) return false;
      while (peek(c) && is_digit(c)) i++;
    }
    if (peek(c) && (c == 'e' || c == 'E')) {
      i++;
      if (peek(c) && (c == '+' || c == '-')) i++;
      if (!peek(c) || !is_digit(c)) return false;
      while (peek(c) && is_digit(c)) i++;
    }
    size_t len = i - start;
    char tmp[64];
    char* buf = tmp;
    std::string big;
    if (len >= sizeof tmp) {
      big.assign(reinterpret_cast<const char*>(s + start), len);
      buf = big.data();
    } else {
      std::memcpy(tmp, s + start, len);
      tmp[len] = 0;
    }
    char* endp = nullptr;
    out = std::strtod(buf, &endp);
    return endp == buf + len;
  }
  bool value(uint32_t depth, Val& out);
  bool object(uint32_t depth, MemberFn f, void* ctx);
};

bool P::value(uint32_t depth, Val& out) {
  if (depth > kMaxDepth) return false;
  ws();
  uint8_t c;
  if (!peek(c)) return false;
  switch (c) {
    case '"': {
      i++;
      out.kind = Kind::Str;
      if (!string(out.s, out.n, out.esc)) return false;
      return true;
    }
    case '{': {
      if (!object(depth, nullptr, nullptr)) return false;
      out.kind = Kind::Other;
      return true;
    }
    case '[': {
      i++;
      ws();
      if (peek(c) && c == ']') {
        i++;
        out.kind = Kind::Other;
        return true;
      }
      for (;;) {
        Val inner;
        if (!value(depth + 1, inner)) return false;
        ws();
        if (!peek(c)) return false;
        if (c == ',') {
          i++;
          continue;
        }
        if (c == ']') {
          i++;
          out.kind = Kind::Other;
          return true;
        }
        return false;
      }
    }
    case 't':
      if (!lit("true", 4)) return false;
      out.kind = Kind::Other;
      return true;
    case 'f':
      if (!lit("false", 5)) return false;
      out.kind = Kind::Other;
      return true;
    case 'n':
      if (!lit("null", 4)) return false;
      out.kind = Kind::Other;
      return true;
    default:
      out.kind = Kind::Num;
      return number(out.num);
  }
}

bool P::object(uint32_t depth, MemberFn f, void* ctx) {
  i++;
  ws();
  uint8_t c;
  if (peek(c) && c == '}') {
    i++;
    return true;
  }
  for (;;) {
    ws();
    if (!peek(c) || c != '"') return false;
    i++;
    const uint8_t* k = nullptr;
    uint32_t klen = 0;
    bool kesc = false;
    if (!string(k, klen, kesc)) return false;
    ws();
    if (!peek(c) || c != ':') return false;
    i++;
    Val v;
    if (!value(depth + 1, v)) return false;
    if (f) {
      if (kesc) {
        std::vector<uint8_t> name;
        unescape(k, klen, name);
        f(ctx, name.data(), static_cast<uint32_t>(name.size()), v);
      } else {
        f(ctx, k, klen, v);
      }
    }
    ws();
    if (!peek(c)) return false;
    if (c == ',') {
      i++;
      continue;
    }
    if (c == '}') {
      i++;
      return true;
    }
    return false;
  }
}

}

bool valid_utf8(const uint8_t* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = s[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    size_t need;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + need >= n) return false;
    for (size_t k = 1; k <= need; k++) {
      if (i + k >= n) return false;
      uint8_t cc = s[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }

    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need + 1;
  }
  return true;
}

int parse_object(const uint8_t* s, size_t n, MemberFn f, void* ctx) {
  if (!valid_utf8(s, n)) return kParseBad;
  P p{s, n, 0};
  p.ws();
  const bool obj = p.i < n && p.s[p.i] == '{';
  if (obj) {
    if (!p.object(0, f, ctx)) return kParseBad;
  } else {
    Val v;
    if (!p.value(0, v)) return kParseBad;
  }
  p.ws();
  if (p.i != n) return kParseBad;
  return obj ? kParseObject : kParseOther;
}

namespace {

uint32_t hex4(const uint8_t* h) {
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    uint8_t c = h[k];
    uint32_t d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else d = c - 'A' + 10;
    v = (v << 4) | d;
  }
  return v;
}

void put_utf8(std::vector<uint8_t>& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<uint8_t>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<uint8_t>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<uint8_t>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<uint8_t>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
  }
}

}

void unescape(const uint8_t* raw, size_t n, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = raw[i];
    if (c != '\\') {
      size_t j = i;
      while (j < n && raw[j] != '\\') j++;
      out.insert(out.end(), raw + i, raw + j);
      i = j;
      continue;
    }
    uint8_t e = raw[i + 1];
    i += 2;
    uint32_t cp;
    switch (e) {
      case 'b': cp = 8; break;
      case 'f': cp = 12; break;
      case 'n': cp = '\n'; break;
      case 'r': cp = '\r'; break;
      case 't': cp = '\t'; break;
      case 'u': {
        cp = hex4(raw + i);
        i += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= n && raw[i] == '\\' && raw[i + 1] == 'u') {
          uint32_t lo = hex4(raw + i + 2);
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        break;
      }
      default: cp = e; break;
    }
    if (!((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)) {
      put_utf8(out, cp);
    } else {
      put_utf8(out, 0xFFFD);
    }
  }
}

namespace {

constexpr bool esc_table_at(size_t i) {
  return i < 0x20 || i == '"' || i == '\\';
}
struct EscTable {
  uint8_t t[256];
  constexpr EscTable() : t() {
    for (size_t i = 0; i < 256; i++) t[i] = esc_table_at(i) ? 1 : 0;
  }
};
constexpr EscTable kEsc{};

constexpr uint64_t kOnes = 0x0101010101010101ULL;
constexpr uint64_t kHigh = 0x8080808080808080ULL;

inline uint64_t word_needs_escape(uint64_t w) {
  uint64_t ctl = (w - kOnes * 0x20) & ~w;
  uint64_t q = w ^ (kOnes * static_cast<uint64_t>('"'));
  q = (q - kOnes) & ~q;
  uint64_t b = w ^ (kOnes * static_cast<uint64_t>('\\'));
  b = (b - kOnes) & ~b;
  return (ctl | q | b) & kHigh;
}

#if defined(__SSE2__)

inline uint32_t esc_mask16(const uint8_t* src) {
  __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
  __m128i ctl = _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1f)), v);
  __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
  __m128i b = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
  return static_cast<uint32_t>(_mm_movemask_epi8(_mm_or_si128(ctl, _mm_or_si128(q, b))));
}
#endif

#if defined(__AVX2__)

inline uint32_t esc_mask32(const uint8_t* src) {
  __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
  __m256i ctl = _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8(0x1f)), v);
  __m256i q = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'));
  __m256i b = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'));
  return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(ctl, _mm256_or_si256(q, b))));
}
#endif

void escape_slow(ByteBuf& out, const uint8_t* s, size_t n) {  size_t start = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (!kEsc.t[c]) continue;
    buf_append(out, s + start, i - start);
    switch (c) {
      case '"': buf_append(out, "\\\"", 2); break;
      case '\\': buf_append(out, "\\\\", 2); break;
      case '\n': buf_append(out, "\\n", 2); break;
      case '\r': buf_append(out, "\\r", 2); break;
      case '\t': buf_append(out, "\\t", 2); break;
      case 8: buf_append(out, "\\b", 2); break;
      case 12: buf_append(out, "\\f", 2); break;
      default: {
        static constexpr char kHex[] = "0123456789abcdef";
        char e[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
        buf_append(out, e, 6);
      }
    }
    start = i + 1;
  }
  buf_append(out, s + start, n - start);
}

}

void escape_into(ByteBuf& out, const uint8_t* s, size_t n) {

  size_t i = 0;
  bool clean = true;
#if defined(__AVX2__)
  while (i + 32 <= n) {
    if (esc_mask32(s + i) != 0) {
      clean = false;
      break;
    }
    i += 32;
  }
#endif
#if defined(__SSE2__)
  if (clean) {
    while (i + 16 <= n) {
      if (esc_mask16(s + i) != 0) {
        clean = false;
        break;
      }
      i += 16;
    }
  }
#endif
  if (clean) {
    while (i + 8 <= n) {
      uint64_t w;
      std::memcpy(&w, s + i, 8);
      if (word_needs_escape(w) != 0) {
        clean = false;
        break;
      }
      i += 8;
    }
  }
  if (clean) {
    for (size_t j = i; j < n; j++) {
      if (kEsc.t[s[j]]) {
        clean = false;
        break;
      }
    }
  }
  if (clean) {
    buf_append(out, s, n);
    return;
  }
  escape_slow(out, s, n);
}

namespace {

constexpr char kDigits2[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

}

void push_int(ByteBuf& out, int64_t v) {
  char buf[20];
  int i = 20;
  uint64_t n = v < 0 ? ~static_cast<uint64_t>(v) + 1ULL : static_cast<uint64_t>(v);
  while (n >= 100) {
    size_t r = static_cast<size_t>(n % 100) * 2;
    n /= 100;
    i -= 2;
    buf[i] = kDigits2[r];
    buf[i + 1] = kDigits2[r + 1];
  }
  if (n >= 10) {
    size_t r = static_cast<size_t>(n) * 2;
    i -= 2;
    buf[i] = kDigits2[r];
    buf[i + 1] = kDigits2[r + 1];
  } else {
    i -= 1;
    buf[i] = static_cast<char>('0' + n);
  }
  if (v < 0) buf_put(out, '-');
  buf_append(out, buf + i, static_cast<size_t>(20 - i));
}

void Cur::int_(int64_t v) {
  if (v >= 0 && v < 100) {
    if (v < 10) {
      *p++ = static_cast<uint8_t>('0' + v);
    } else {
      const size_t r = static_cast<size_t>(v) * 2;
      *p++ = static_cast<uint8_t>(kDigits2[r]);
      *p++ = static_cast<uint8_t>(kDigits2[r + 1]);
    }
    return;
  }
  if (v >= 100000 && v < 1000000) {
    const uint32_t u = static_cast<uint32_t>(v);
    const size_t r0 = (u / 10000) * 2;
    const size_t r1 = ((u / 100) % 100) * 2;
    const size_t r2 = (u % 100) * 2;
    *p++ = static_cast<uint8_t>(kDigits2[r0]);
    *p++ = static_cast<uint8_t>(kDigits2[r0 + 1]);
    *p++ = static_cast<uint8_t>(kDigits2[r1]);
    *p++ = static_cast<uint8_t>(kDigits2[r1 + 1]);
    *p++ = static_cast<uint8_t>(kDigits2[r2]);
    *p++ = static_cast<uint8_t>(kDigits2[r2 + 1]);
    return;
  }
  uint8_t buf[20];
  int i = 20;
  uint64_t n = v < 0 ? ~static_cast<uint64_t>(v) + 1ULL : static_cast<uint64_t>(v);
  while (n >= 100) {
    size_t r = static_cast<size_t>(n % 100) * 2;
    n /= 100;
    i -= 2;
    buf[i] = static_cast<uint8_t>(kDigits2[r]);
    buf[i + 1] = static_cast<uint8_t>(kDigits2[r + 1]);
  }
  if (n >= 10) {
    size_t r = static_cast<size_t>(n) * 2;
    i -= 2;
    buf[i] = static_cast<uint8_t>(kDigits2[r]);
    buf[i + 1] = static_cast<uint8_t>(kDigits2[r + 1]);
  } else {
    i -= 1;
    buf[i] = static_cast<uint8_t>('0' + n);
  }
  if (v < 0) {
    i -= 1;
    buf[i] = '-';
  }
  const size_t len = static_cast<size_t>(20 - i);
  std::memcpy(p, buf + i, len);
  p += len;
}

#if defined(__SSE2__)

inline uint32_t store_check16(const uint8_t* src, uint8_t* dst) {
  __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
  __m128i ctl = _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(0x1f)), v);
  __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
  __m128i b = _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'));
  return static_cast<uint32_t>(_mm_movemask_epi8(_mm_or_si128(ctl, _mm_or_si128(q, b))));
}
#endif

#if defined(__AVX2__)

inline uint32_t store_check32(const uint8_t* src, uint8_t* dst) {
  __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), v);
  __m256i ctl = _mm256_cmpeq_epi8(_mm256_min_epu8(v, _mm256_set1_epi8(0x1f)), v);
  __m256i q = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'));
  __m256i b = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'));
  return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(ctl, _mm256_or_si256(q, b))));
}
#endif

namespace {

void esc_byte(Cur& c, uint8_t ch) {
  if (!kEsc.t[ch]) {
    c.byte(ch);
    return;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  switch (ch) {
    case '"': c.lit("\\\"", 2); break;
    case '\\': c.lit("\\\\", 2); break;
    case '\n': c.lit("\\n", 2); break;
    case '\r': c.lit("\\r", 2); break;
    case '\t': c.lit("\\t", 2); break;
    case 8: c.lit("\\b", 2); break;
    case 12: c.lit("\\f", 2); break;
    default: {
      const char e[6] = {'\\', 'u', '0', '0', kHex[ch >> 4], kHex[ch & 15]};
      c.lit(e, 6);
    }
  }
}

inline void chunk_tail(Cur& c, const uint8_t* s, size_t i, size_t n, uint32_t m) {
  if (m == 0) {
    c.p += n;
    return;
  }
  unsigned k = static_cast<unsigned>(__builtin_ctz(m));
  if (k > n) k = static_cast<unsigned>(n);
  c.p += k;
  for (size_t j = i + k; j < i + n; j++) esc_byte(c, s[j]);
}

}

void Cur::esc(const uint8_t* s, size_t n) {
  size_t i = 0;
#if defined(__AVX2__)
  while (i + 32 <= n) {
    uint32_t m = store_check32(s + i, p);
    chunk_tail(*this, s, i, 32, m);
    i += 32;
  }
#endif
#if defined(__SSE2__)
  while (i + 16 <= n) {
    uint32_t m = store_check16(s + i, p);
    chunk_tail(*this, s, i, 16, m);
    i += 16;
  }
#endif
  while (i + 8 <= n) {
    uint64_t w;
    std::memcpy(&w, s + i, 8);
    if (word_needs_escape(w) != 0) {
      for (size_t j = i; j < i + 8; j++) esc_byte(*this, s[j]);
    } else {
      std::memcpy(p, &w, 8);
      p += 8;
    }
    i += 8;
  }
  for (; i < n; i++) esc_byte(*this, s[i]);
}

}
