// The $12 server challenge API in C++: one epoll event loop on one thread, SQLite in-process.
//
// The box has one vCPU, so a single thread that never blocks on the network is the cheapest way
// to serve it: every query is an index lookup that takes a few microseconds, and running them
// inline avoids all locking and thread hand-offs. The database is opened in exclusive locking
// mode (no file locks per transaction), and WAL checkpoints run on the loop between batches of
// events, after the responses of the batch have been sent.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite3.h"

namespace {

constexpr size_t kReadChunk = 64 * 1024;
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr int kIdleTimeoutS = 120;  // SPEC: keep idle connections at least 65 s
constexpr int64_t kMaxId = INT64_MAX;

// ---------------------------------------------------------------------------------------------
// small helpers

[[noreturn]] void die(const char* what, const char* detail = nullptr) {
  std::fprintf(stderr, "fatal: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
  std::exit(1);
}

int64_t monotonic_s() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return ts.tv_sec;
}

void append_int(std::string& out, int64_t v) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr - buf);
}

// Index of the first byte in s[i, n) that JSON needs escaped (< 0x20, '"' or '\\'), or n.
// Post bodies almost never contain one, so this checks 16 bytes (SSE2) or 8 bytes at a time.
size_t next_escape(const char* s, size_t i, size_t n) {
#ifdef __SSE2__
  const __m128i quote = _mm_set1_epi8('"'), bslash = _mm_set1_epi8('\\'), ctl = _mm_set1_epi8(0x1F);
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
    // c <= 0x1F as unsigned: min(c, 0x1F) == c
    __m128i hit = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, bslash)),
                               _mm_cmpeq_epi8(_mm_min_epu8(v, ctl), v));
    if (int mask = _mm_movemask_epi8(hit)) return i + __builtin_ctz(mask);
  }
#else
  constexpr uint64_t kOnes = 0x0101010101010101ULL, kHigh = 0x8080808080808080ULL;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, s + i, 8);
    auto has_zero = [&](uint64_t x) { return (x - kOnes) & ~x & kHigh; };
    uint64_t hit = has_zero(v ^ (kOnes * '"')) | has_zero(v ^ (kOnes * '\\')) | ((v - kOnes * 0x20) & ~v & kHigh);
    if (hit) break;  // finish this word byte by byte (the borrow trick can flag a later byte)
  }
#endif
  for (; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == '"' || c == '\\') return i;
  }
  return n;
}

// Appends s as a JSON string literal (quotes included). Non-ASCII UTF-8 passes through as is.
void append_json_str(std::string& out, const char* s, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  out.push_back('"');
  size_t run = 0;
  for (size_t i = next_escape(s, 0, n); i < n; i = next_escape(s, i + 1, n)) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    out.append(s + run, i - run);
    run = i + 1;
    switch (c) {
      case '"': out.append("\\\"", 2); break;
      case '\\': out.append("\\\\", 2); break;
      case '\n': out.append("\\n", 2); break;
      case '\r': out.append("\\r", 2); break;
      case '\t': out.append("\\t", 2); break;
      case '\b': out.append("\\b", 2); break;
      case '\f': out.append("\\f", 2); break;
      default: {
        char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
        out.append(esc, 6);
      }
    }
  }
  out.append(s + run, n - run);
  out.push_back('"');
}
void append_json_str(std::string& out, std::string_view s) { append_json_str(out, s.data(), s.size()); }

// A positive integer in plain decimal digits. Returns 1 if valid, 0 if not a positive integer,
// and 2 if it is valid but beyond SQLite's INTEGER range (so no row can have it).
int parse_id(std::string_view s, int64_t* out) {
  if (s.empty()) return 0;
  int64_t v = 0;
  bool overflow = false;
  for (char ch : s) {
    if (ch < '0' || ch > '9') return 0;
    int d = ch - '0';
    if (!overflow && v > (kMaxId - d) / 10) overflow = true;
    if (!overflow) v = v * 10 + d;
  }
  if (overflow) return 2;
  if (v <= 0) return 0;
  *out = v;
  return 1;
}

// ---------------------------------------------------------------------------------------------
// strict JSON (RFC 8259) parser that extracts selected top-level fields of an object

enum JType : uint8_t { J_NONE, J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct JField {
  JField(std::string_view k) : key(k) {}
  std::string_view key;
  JType type = J_NONE;  // J_NONE: key absent
  double num = 0;
  std::string str;
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view s) : p_(s.data()), e_(s.data() + s.size()) {}

  // Parses the whole document. If it is an object, `fields` with matching keys are filled in
  // (the last duplicate wins). Returns false if the document is not valid JSON.
  bool parse(JType* top, JField* fields, size_t nfields) {
    ws();
    if (!value(top, nullptr, nullptr, fields, nfields)) return false;
    ws();
    return p_ == e_;
  }

 private:
  const char* p_;
  const char* e_;
  int depth_ = 0;
  std::string key_;

  void ws() {
    while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool lit(const char* word, size_t n) {
    if (static_cast<size_t>(e_ - p_) < n || std::memcmp(p_, word, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool value(JType* t, double* num, std::string* str, JField* fields, size_t nfields) {
    if (p_ >= e_) return false;
    JType type;
    switch (*p_) {
      case '{':
        type = J_OBJ;
        if (!object(fields, nfields)) return false;
        break;
      case '[':
        type = J_ARR;
        if (!array()) return false;
        break;
      case '"':
        type = J_STR;
        if (!string(str)) return false;
        break;
      case 't':
        type = J_BOOL;
        if (!lit("true", 4)) return false;
        break;
      case 'f':
        type = J_BOOL;
        if (!lit("false", 5)) return false;
        break;
      case 'n':
        type = J_NULL;
        if (!lit("null", 4)) return false;
        break;
      default:
        type = J_NUM;
        if (!number(num)) return false;
    }
    if (t) *t = type;
    return true;
  }

  bool object(JField* fields, size_t nfields) {
    if (++depth_ > 256) return false;
    ++p_;
    ws();
    if (p_ < e_ && *p_ == '}') {
      ++p_;
      --depth_;
      return true;
    }
    for (;;) {
      ws();
      if (p_ >= e_ || *p_ != '"') return false;
      key_.clear();
      if (!string(&key_)) return false;
      JField* f = nullptr;
      for (size_t i = 0; i < nfields; i++)
        if (fields[i].key == key_) f = &fields[i];
      ws();
      if (p_ >= e_ || *p_ != ':') return false;
      ++p_;
      ws();
      if (f) {
        f->str.clear();
        if (!value(&f->type, &f->num, &f->str, nullptr, 0)) return false;
      } else if (!value(nullptr, nullptr, nullptr, nullptr, 0)) {
        return false;
      }
      ws();
      if (p_ >= e_) return false;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ != '}') return false;
      ++p_;
      --depth_;
      return true;
    }
  }

  bool array() {
    if (++depth_ > 256) return false;
    ++p_;
    ws();
    if (p_ < e_ && *p_ == ']') {
      ++p_;
      --depth_;
      return true;
    }
    for (;;) {
      ws();
      if (!value(nullptr, nullptr, nullptr, nullptr, 0)) return false;
      ws();
      if (p_ >= e_) return false;
      if (*p_ == ',') {
        ++p_;
        continue;
      }
      if (*p_ != ']') return false;
      ++p_;
      --depth_;
      return true;
    }
  }

  bool number(double* out) {
    const char* start = p_;
    if (p_ < e_ && *p_ == '-') ++p_;
    if (p_ >= e_) return false;
    if (*p_ == '0') {
      ++p_;
    } else if (*p_ >= '1' && *p_ <= '9') {
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    } else {
      return false;
    }
    if (p_ < e_ && *p_ == '.') {
      ++p_;
      if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < e_ && (*p_ == '+' || *p_ == '-')) ++p_;
      if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (out) {
      auto r = std::from_chars(start, p_, *out);
      if (r.ec == std::errc::result_out_of_range) *out = (*start == '-') ? -HUGE_VAL : HUGE_VAL;
    }
    return true;
  }

  static int hex4(const char* s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
      char c = s[i];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return -1;
      v = v * 16 + d;
    }
    return v;
  }

  static void put_utf8(std::string* out, uint32_t cp) {
    if (!out) return;
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Length of the valid UTF-8 sequence starting at p (lead byte >= 0x80), or 0 if invalid.
  static size_t utf8_seq(const unsigned char* p, const unsigned char* e) {
    unsigned char c = p[0];
    size_t n;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c == 0xE0) n = 3, lo = 0xA0;
    else if (c >= 0xE1 && c <= 0xEC) n = 3;
    else if (c == 0xED) n = 3, hi = 0x9F;
    else if (c >= 0xEE && c <= 0xEF) n = 3;
    else if (c == 0xF0) n = 4, lo = 0x90;
    else if (c >= 0xF1 && c <= 0xF3) n = 4;
    else if (c == 0xF4) n = 4, hi = 0x8F;
    else return 0;
    if (static_cast<size_t>(e - p) < n) return 0;
    if (p[1] < lo || p[1] > hi) return 0;
    for (size_t i = 2; i < n; i++)
      if ((p[i] & 0xC0) != 0x80) return 0;
    return n;
  }

  bool string(std::string* out) {
    ++p_;  // opening quote
    const char* run = p_;
    auto flush = [&] {
      if (out) out->append(run, p_ - run);
    };
    while (p_ < e_) {
      unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        flush();
        ++p_;
        return true;
      }
      if (c < 0x20) return false;
      if (c < 0x80 && c != '\\') {
        ++p_;
        continue;
      }
      if (c >= 0x80) {
        size_t n = utf8_seq(reinterpret_cast<const unsigned char*>(p_),
                            reinterpret_cast<const unsigned char*>(e_));
        if (!n) return false;
        p_ += n;
        continue;
      }
      // backslash escape
      flush();
      if (e_ - p_ < 2) return false;
      char esc = p_[1];
      p_ += 2;
      switch (esc) {
        case '"': if (out) out->push_back('"'); break;
        case '\\': if (out) out->push_back('\\'); break;
        case '/': if (out) out->push_back('/'); break;
        case 'b': if (out) out->push_back('\b'); break;
        case 'f': if (out) out->push_back('\f'); break;
        case 'n': if (out) out->push_back('\n'); break;
        case 'r': if (out) out->push_back('\r'); break;
        case 't': if (out) out->push_back('\t'); break;
        case 'u': {
          if (e_ - p_ < 4) return false;
          int cp = hex4(p_);
          if (cp < 0) return false;
          p_ += 4;
          if (cp >= 0xDC00 && cp <= 0xDFFF) return false;  // lone low surrogate
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
            int lo = hex4(p_ + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            p_ += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(out, static_cast<uint32_t>(cp));
          break;
        }
        default:
          return false;
      }
      run = p_;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------------------------
// body trimming (JavaScript String.prototype.trim()'s whitespace set) and length

bool is_js_space(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

// Decodes the (already validated) UTF-8 code point at s[i], returning its byte length.
size_t decode_utf8(const unsigned char* s, uint32_t* cp) {
  unsigned char c = s[0];
  if (c < 0x80) return *cp = c, 1;
  if (c < 0xE0) return *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F), 2;
  if (c < 0xF0) return *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F), 3;
  *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  return 4;
}

std::string_view trim_js(std::string_view s) {
  auto* b = reinterpret_cast<const unsigned char*>(s.data());
  size_t lo = 0, hi = s.size();
  while (lo < hi) {
    uint32_t cp;
    size_t n = decode_utf8(b + lo, &cp);
    if (!is_js_space(cp)) break;
    lo += n;
  }
  while (hi > lo) {
    size_t start = hi - 1;
    while (start > lo && (b[start] & 0xC0) == 0x80) --start;
    uint32_t cp;
    decode_utf8(b + start, &cp);
    if (!is_js_space(cp)) break;
    hi = start;
  }
  return s.substr(lo, hi - lo);
}

size_t utf8_length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

// ---------------------------------------------------------------------------------------------
// JWT (HS256 only). The HMAC key pads are hashed once at startup; every token is still fully
// verified on every request (rule 5).

SHA256_CTX g_hmac_inner, g_hmac_outer;

void hmac_init(std::string_view key) {
  unsigned char k[SHA256_CBLOCK] = {0};
  if (key.size() > SHA256_CBLOCK) {
    SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), k);
  } else {
    std::memcpy(k, key.data(), key.size());
  }
  unsigned char ipad[SHA256_CBLOCK], opad[SHA256_CBLOCK];
  for (int i = 0; i < SHA256_CBLOCK; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  SHA256_Init(&g_hmac_inner);
  SHA256_Update(&g_hmac_inner, ipad, sizeof ipad);
  SHA256_Init(&g_hmac_outer);
  SHA256_Update(&g_hmac_outer, opad, sizeof opad);
}

void hmac_sha256(const char* msg, size_t n, unsigned char out[32]) {
  unsigned char inner[32];
  SHA256_CTX c = g_hmac_inner;
  SHA256_Update(&c, msg, n);
  SHA256_Final(inner, &c);
  c = g_hmac_outer;
  SHA256_Update(&c, inner, sizeof inner);
  SHA256_Final(out, &c);
}

const char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int8_t b64url_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

bool b64url_decode(std::string_view in, std::string& out) {
  out.clear();
  if (in.size() % 4 == 1) return false;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : in) {
    int v = b64url_value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

size_t b64url_encode(const unsigned char* in, size_t n, char* out) {
  size_t o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out[o++] = kB64Url[v >> 18];
    out[o++] = kB64Url[(v >> 12) & 63];
    out[o++] = kB64Url[(v >> 6) & 63];
    out[o++] = kB64Url[v & 63];
  }
  if (n - i == 1) {
    uint32_t v = in[i] << 16;
    out[o++] = kB64Url[v >> 18];
    out[o++] = kB64Url[(v >> 12) & 63];
  } else if (n - i == 2) {
    uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
    out[o++] = kB64Url[v >> 18];
    out[o++] = kB64Url[(v >> 12) & 63];
    out[o++] = kB64Url[(v >> 6) & 63];
  }
  return o;
}

enum class AuthResult { kOk, kMissing, kInvalid, kBadPayload };

struct AuthUser {
  int64_t id = 0;
  std::string username;
};

AuthResult authenticate(std::string_view header, bool present, AuthUser* user) {
  if (!present || header.size() < 7 || header.substr(0, 7) != "Bearer ") return AuthResult::kMissing;
  std::string_view token = header.substr(7);
  size_t d1 = token.find('.');
  if (d1 == std::string_view::npos) return AuthResult::kInvalid;
  size_t d2 = token.find('.', d1 + 1);
  if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos)
    return AuthResult::kInvalid;
  std::string_view head_b64 = token.substr(0, d1);
  std::string_view payload_b64 = token.substr(d1 + 1, d2 - d1 - 1);
  std::string_view sig_b64 = token.substr(d2 + 1);

  static std::string decoded;
  JType top;
  JField alg[1] = {{"alg"}};
  if (!b64url_decode(head_b64, decoded) || !JsonParser(decoded).parse(&top, alg, 1) || top != J_OBJ ||
      alg[0].type != J_STR || alg[0].str != "HS256")
    return AuthResult::kInvalid;

  unsigned char mac[32];
  hmac_sha256(token.data(), d2, mac);
  char expected[44];
  size_t elen = b64url_encode(mac, sizeof mac, expected);
  if (sig_b64.size() != elen) return AuthResult::kInvalid;
  unsigned char diff = 0;
  for (size_t i = 0; i < elen; i++) diff |= static_cast<unsigned char>(expected[i] ^ sig_b64[i]);
  if (diff) return AuthResult::kInvalid;

  enum { kSub, kUsername, kExp, kNbf };
  JField f[4] = {{"sub"}, {"username"}, {"exp"}, {"nbf"}};
  if (!b64url_decode(payload_b64, decoded) || !JsonParser(decoded).parse(&top, f, 4) || top != J_OBJ)
    return AuthResult::kInvalid;
  double now = static_cast<double>(time(nullptr));
  JType exp_t = f[kExp].type, nbf_t = f[kNbf].type;
  if (exp_t != J_NONE && exp_t != J_NULL && (exp_t != J_NUM || now >= f[kExp].num))
    return AuthResult::kInvalid;
  if (nbf_t != J_NONE && nbf_t != J_NULL && (nbf_t != J_NUM || now < f[kNbf].num))
    return AuthResult::kInvalid;

  if (f[kSub].type != J_STR || parse_id(f[kSub].str, &user->id) != 1 || f[kUsername].type != J_STR)
    return AuthResult::kBadPayload;
  user->username = std::move(f[kUsername].str);
  return AuthResult::kOk;
}

// ---------------------------------------------------------------------------------------------
// database

sqlite3* g_db;
sqlite3_stmt* g_st_ping;
sqlite3_stmt* g_st_feed;
sqlite3_stmt* g_st_post;
sqlite3_stmt* g_st_insert_post;
sqlite3_stmt* g_st_insert_like;
sqlite3_stmt* g_st_post_exists;

#define POST_SELECT                                                       \
  "SELECT p.id, p.body, p.created_at, u.username,"                        \
  " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"                \
  " FROM posts p JOIN users u ON u.id = p.user_id"

void exec_or_die(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) die(sql, err);
}

constexpr int kCheckpointFrames = 1000;  // SQLite's default auto-checkpoint threshold
int g_wal_frames = 0;                    // WAL size after the last commit

// Replaces SQLite's auto-checkpoint (which is itself a WAL hook). Auto-checkpoint would run inside
// the commit that crosses the threshold, so that request's response would wait for the page
// copying and two fsyncs. Instead the event loop checkpoints after a batch's responses are sent.
int on_wal_commit(void*, sqlite3*, const char*, int frames) {
  g_wal_frames = frames;
  return SQLITE_OK;
}

void checkpoint_if_due() {
  if (g_wal_frames < kCheckpointFrames) return;
  g_wal_frames = 0;
  // The only connection, so a PASSIVE checkpoint always copies the whole WAL, and the next write
  // starts it over from the beginning.
  sqlite3_wal_checkpoint_v2(g_db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
}

sqlite3_stmt* prepare(const char* sql) {
  sqlite3_stmt* st;
  if (sqlite3_prepare_v3(g_db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, nullptr) != SQLITE_OK)
    die("prepare failed", sqlite3_errmsg(g_db));
  return st;
}

// Reads every page of every table and index once, through SQLite's mmap. That loads the file into
// the OS page cache and maps it into this process, so early requests don't stall on page faults.
// The pages stay owned by the OS page cache (rule 5): nothing is copied or kept by the server.
void prewarm() {
  static const char* const kScans[] = {
      "SELECT count(*), sum(length(username)), sum(length(created_at)) FROM users NOT INDEXED",
      "SELECT count(*), sum(user_id), sum(length(body)), sum(length(created_at)) FROM posts NOT INDEXED",
      "SELECT count(*), sum(length(created_at)) FROM likes NOT INDEXED",
      "SELECT count(*) FROM users INDEXED BY sqlite_autoindex_users_1 WHERE username >= ''",
      "SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx WHERE created_at >= ''",
      "SELECT count(*) FROM posts INDEXED BY posts_user_id_idx WHERE user_id >= 0",
      "SELECT count(*) FROM likes INDEXED BY likes_post_id_idx WHERE post_id >= 0",
  };
  for (const char* sql : kScans) {
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) continue;  // best effort
    while (sqlite3_step(st) == SQLITE_ROW) {
    }
    sqlite3_finalize(st);
  }
}

void init_db(const char* path) {
  if (sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
    die("cannot open database", sqlite3_errmsg(g_db));
  // One connection, so no other process or connection needs the database. Exclusive mode takes
  // the file locks once and keeps them, instead of fcntl lock/unlock calls on every transaction,
  // and keeps the WAL index in heap memory instead of a shared -shm mapping. It must be set
  // before WAL mode is entered.
  exec_or_die(g_db, "PRAGMA locking_mode = EXCLUSIVE");
  exec_or_die(g_db, "PRAGMA journal_mode = WAL");
  exec_or_die(g_db, "PRAGMA synchronous = NORMAL");    // rule 6: WAL + NORMAL
  exec_or_die(g_db, "PRAGMA mmap_size = 1073741824");  // reads go straight to the OS page cache
  // Reads come through mmap, so the page cache only holds pages read from the WAL and dirty
  // pages. A small one keeps commit cheap: on each commit SQLite scans the cache's whole hash
  // table, which with 32 MiB was ~13% of user CPU.
  exec_or_die(g_db, "PRAGMA cache_size = 500");
  exec_or_die(g_db, "PRAGMA temp_store = MEMORY");
  exec_or_die(g_db, "PRAGMA journal_size_limit = 67108864");  // truncate a WAL that grew past 64 MiB
  sqlite3_wal_hook(g_db, on_wal_commit, nullptr);  // replaces wal_autocheckpoint
  // Take the exclusive locks now (the first read does), so a startup failure is reported here.
  exec_or_die(g_db, "SELECT count(*) FROM sqlite_schema");
  prewarm();
  g_st_ping = prepare("SELECT 1");
  g_st_feed = prepare(POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
  g_st_post = prepare(POST_SELECT " WHERE p.id = ?1");
  g_st_insert_post = prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
  // Inserts only if the post exists; 0 changes means "already liked" or "no such post".
  g_st_insert_like = prepare(
      "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
      " ON CONFLICT (user_id, post_id) DO NOTHING");
  g_st_post_exists = prepare("SELECT 1 FROM posts WHERE id = ?1");
}

// ---------------------------------------------------------------------------------------------
// handlers: each writes a JSON body into `body` and returns the HTTP status

int64_t g_start_s;

int error(std::string& body, int status, const char* msg) {
  body.append("{\"error\":\"");
  body.append(msg);
  body.append("\"}");
  return status;
}

void append_post(std::string& b, sqlite3_stmt* st) {
  b.append("{\"id\":");
  append_int(b, sqlite3_column_int64(st, 0));
  b.append(",\"body\":");
  append_json_str(b, reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), sqlite3_column_bytes(st, 1));
  b.append(",\"created_at\":");
  append_json_str(b, reinterpret_cast<const char*>(sqlite3_column_text(st, 2)), sqlite3_column_bytes(st, 2));
  b.append(",\"author\":");
  append_json_str(b, reinterpret_cast<const char*>(sqlite3_column_text(st, 3)), sqlite3_column_bytes(st, 3));
  b.append(",\"like_count\":");
  append_int(b, sqlite3_column_int64(st, 4));
  b.push_back('}');
}

int handle_health(std::string& b) {
  int rc = sqlite3_step(g_st_ping);
  sqlite3_reset(g_st_ping);
  if (rc != SQLITE_ROW) {
    b.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
    append_json_str(b, sqlite3_errstr(rc));
    b.push_back('}');
    return 503;
  }
  b.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  append_int(b, monotonic_s() - g_start_s);
  b.push_back('}');
  return 200;
}

int handle_feed(std::string& b) {
  b.append("{\"posts\":[");
  int rc;
  bool first = true;
  while ((rc = sqlite3_step(g_st_feed)) == SQLITE_ROW) {
    if (!first) b.push_back(',');
    first = false;
    append_post(b, g_st_feed);
  }
  sqlite3_reset(g_st_feed);
  if (rc != SQLITE_DONE) {
    b.clear();
    return error(b, 500, "internal server error");
  }
  b.append("]}");
  return 200;
}

int handle_get_post(std::string& b, std::string_view id_str) {
  int64_t id;
  int v = parse_id(id_str, &id);
  if (v == 0) return error(b, 400, "invalid post id");
  if (v == 2) return error(b, 404, "post not found");
  sqlite3_bind_int64(g_st_post, 1, id);
  int rc = sqlite3_step(g_st_post);
  int status;
  if (rc == SQLITE_ROW) {
    b.append("{\"post\":");
    append_post(b, g_st_post);
    b.push_back('}');
    status = 200;
  } else if (rc == SQLITE_DONE) {
    status = error(b, 404, "post not found");
  } else {
    status = error(b, 500, "internal server error");
  }
  sqlite3_reset(g_st_post);
  return status;
}

int auth_error(std::string& b, AuthResult r) {
  switch (r) {
    case AuthResult::kMissing: return error(b, 401, "missing bearer token");
    case AuthResult::kBadPayload: return error(b, 401, "invalid token payload");
    default: return error(b, 401, "invalid or expired token");
  }
}

int handle_create_post(std::string& b, std::string_view auth, bool has_auth, std::string_view req_body) {
  static AuthUser user;
  AuthResult ar = authenticate(auth, has_auth, &user);
  if (ar != AuthResult::kOk) return auth_error(b, ar);

  JType top;
  JField field[1] = {{"body"}};
  if (!JsonParser(req_body).parse(&top, field, 1)) return error(b, 400, "malformed JSON body");
  if (top != J_OBJ || field[0].type != J_STR) return error(b, 400, "body is required");
  std::string_view text = trim_js(field[0].str);
  if (text.empty()) return error(b, 400, "body is required");
  if (utf8_length(text) > 500) return error(b, 400, "body must be at most 500 characters");

  sqlite3_stmt* st = g_st_insert_post;
  sqlite3_bind_int64(st, 1, user.id);
  sqlite3_bind_text(st, 2, text.data(), static_cast<int>(text.size()), SQLITE_STATIC);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(st);
    return error(b, 500, "internal server error");
  }
  int64_t id = sqlite3_column_int64(st, 0);
  b.append("{\"post\":{\"id\":");
  append_int(b, id);
  b.append(",\"body\":");
  append_json_str(b, text);
  b.append(",\"created_at\":");
  append_json_str(b, reinterpret_cast<const char*>(sqlite3_column_text(st, 1)), sqlite3_column_bytes(st, 1));
  b.append(",\"author\":");
  append_json_str(b, user.username);
  b.append(",\"like_count\":0}}");
  // Run the statement to completion: that commits the autocommit transaction before we respond.
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
  }
  int reset_rc = sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  if (rc != SQLITE_DONE || reset_rc != SQLITE_OK) {
    b.clear();
    return error(b, 500, "internal server error");
  }
  return 201;
}

int handle_like(std::string& b, std::string_view auth, bool has_auth, std::string_view id_str) {
  static AuthUser user;
  AuthResult ar = authenticate(auth, has_auth, &user);  // auth before the id, per the spec
  if (ar != AuthResult::kOk) return auth_error(b, ar);
  int64_t id;
  int v = parse_id(id_str, &id);
  if (v == 0) return error(b, 400, "invalid post id");
  if (v == 2) return error(b, 404, "post not found");

  sqlite3_bind_int64(g_st_insert_like, 1, user.id);
  sqlite3_bind_int64(g_st_insert_like, 2, id);
  int rc = sqlite3_step(g_st_insert_like);
  sqlite3_reset(g_st_insert_like);
  if (rc != SQLITE_DONE) return error(b, 500, "internal server error");
  bool inserted = sqlite3_changes(g_db) == 1;
  if (!inserted) {
    sqlite3_bind_int64(g_st_post_exists, 1, id);
    rc = sqlite3_step(g_st_post_exists);
    sqlite3_reset(g_st_post_exists);
    if (rc == SQLITE_DONE) return error(b, 404, "post not found");
    if (rc != SQLITE_ROW) return error(b, 500, "internal server error");
  }
  b.append(inserted ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                    : "{\"liked\":true,\"already_liked\":true,\"post_id\":");
  append_int(b, id);
  b.push_back('}');
  return inserted ? 201 : 200;
}

// ---------------------------------------------------------------------------------------------
// HTTP

struct Request {
  std::string_view method;
  std::string_view path;
  std::string_view auth;
  bool has_auth = false;
  std::string_view body;
  bool keep_alive = true;
};

int route(const Request& r, std::string& b) {
  std::string_view path = r.path.substr(0, r.path.find('?'));
  bool get = r.method == "GET", post = r.method == "POST";
  if (get && path == "/feed") return handle_feed(b);
  if (path.substr(0, 7) == "/posts/" && path.size() > 7) {
    std::string_view rest = path.substr(7);
    size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
      if (get) return handle_get_post(b, rest);
    } else if (post && rest.substr(slash) == "/like" && slash > 0) {
      return handle_like(b, r.auth, r.has_auth, rest.substr(0, slash));
    }
  } else if (post && path == "/posts") {
    return handle_create_post(b, r.auth, r.has_auth, r.body);
  } else if (get && path == "/health") {
    return handle_health(b);
  }
  return error(b, 404, "not found");
}

const char* status_line(int status) {
  switch (status) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 413: return "HTTP/1.1 413 Payload Too Large\r\n";
    case 431: return "HTTP/1.1 431 Request Header Fields Too Large\r\n";
    case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
    default: return "HTTP/1.1 500 Internal Server Error\r\n";
  }
}

void append_response(std::string& out, int status, const std::string& body, bool close) {
  out.append(status_line(status));
  out.append("Content-Type: application/json\r\nContent-Length: ");
  append_int(out, static_cast<int64_t>(body.size()));
  out.append(close ? "\r\nConnection: close\r\n\r\n" : "\r\n\r\n");
  out.append(body);
}

bool ieq(std::string_view a, std::string_view lower) {
  if (a.size() != lower.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c != lower[i]) return false;
  }
  return true;
}

bool icontains(std::string_view hay, std::string_view lower) {
  if (hay.size() < lower.size()) return false;
  for (size_t i = 0; i + lower.size() <= hay.size(); i++)
    if (ieq(hay.substr(i, lower.size()), lower)) return true;
  return false;
}

std::string_view trim_ows(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

enum class Parse { kOk, kIncomplete, kBad, kHeadersTooLarge, kBodyTooLarge };

// Decodes a chunked body starting at p. Returns bytes consumed, 0 if incomplete, -1 if malformed.
ssize_t decode_chunked(const char* p, const char* e, std::string& out) {
  const char* start = p;
  out.clear();
  for (;;) {
    const char* eol = static_cast<const char*>(memmem(p, e - p, "\r\n", 2));
    if (!eol) return 0;
    size_t size = 0;
    const char* q = p;
    for (; q < eol && *q != ';'; q++) {
      char c = *q;
      int v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else return -1;
      size = size * 16 + v;
      if (size > kMaxBodyBytes) return -1;
    }
    if (q == p) return -1;
    p = eol + 2;
    if (size == 0) {
      // optional trailers, then an empty line
      for (;;) {
        const char* end = static_cast<const char*>(memmem(p, e - p, "\r\n", 2));
        if (!end) return 0;
        bool empty = end == p;
        p = end + 2;
        if (empty) return p - start;
      }
    }
    if (static_cast<size_t>(e - p) < size + 2) return 0;
    if (p[size] != '\r' || p[size + 1] != '\n') return -1;
    out.append(p, size);
    if (out.size() > kMaxBodyBytes) return -1;
    p += size + 2;
  }
}

// Parses one request from [data, data+len). On kOk, *consumed is its total length.
Parse parse_request(const char* data, size_t len, Request* r, size_t* consumed, std::string& chunked) {
  const char* end = static_cast<const char*>(memmem(data, len, "\r\n\r\n", 4));
  if (!end) return len > kMaxHeaderBytes ? Parse::kHeadersTooLarge : Parse::kIncomplete;
  size_t head_len = end - data + 4;
  std::string_view head(data, end - data + 2);  // includes the last header line's CRLF

  size_t eol = head.find("\r\n");
  std::string_view line = head.substr(0, eol);
  size_t sp1 = line.find(' ');
  size_t sp2 = line.rfind(' ');
  if (sp1 == std::string_view::npos || sp2 == sp1) return Parse::kBad;
  r->method = line.substr(0, sp1);
  r->path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  std::string_view version = line.substr(sp2 + 1);
  if (version.substr(0, 5) != "HTTP/") return Parse::kBad;
  bool http10 = version == "HTTP/1.0";
  r->keep_alive = !http10;
  r->has_auth = false;
  r->auth = {};
  r->body = {};

  size_t content_length = 0;
  bool chunked_te = false;
  size_t pos = eol + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    std::string_view h = head.substr(pos, next - pos);
    pos = next + 2;
    size_t colon = h.find(':');
    if (colon == std::string_view::npos) return Parse::kBad;
    std::string_view name = h.substr(0, colon);
    std::string_view value = trim_ows(h.substr(colon + 1));
    switch (name.size()) {
      case 13:
        if (ieq(name, "authorization")) {
          r->auth = value;
          r->has_auth = true;
        }
        break;
      case 14:
        if (ieq(name, "content-length")) {
          if (value.empty()) return Parse::kBad;
          size_t v = 0;
          for (char c : value) {
            if (c < '0' || c > '9') return Parse::kBad;
            v = v * 10 + (c - '0');
            if (v > kMaxBodyBytes) return Parse::kBodyTooLarge;
          }
          content_length = v;
        }
        break;
      case 10:
        if (ieq(name, "connection")) {
          if (icontains(value, "close")) r->keep_alive = false;
          else if (icontains(value, "keep-alive")) r->keep_alive = true;
        }
        break;
      case 17:
        if (ieq(name, "transfer-encoding") && icontains(value, "chunked")) chunked_te = true;
        break;
    }
  }

  if (chunked_te) {
    ssize_t n = decode_chunked(data + head_len, data + len, chunked);
    if (n < 0) return Parse::kBad;
    if (n == 0) return len - head_len > kMaxBodyBytes + 64 * 1024 ? Parse::kBodyTooLarge : Parse::kIncomplete;
    r->body = chunked;
    *consumed = head_len + n;
    return Parse::kOk;
  }
  if (len - head_len < content_length) return Parse::kIncomplete;
  r->body = std::string_view(data + head_len, content_length);
  *consumed = head_len + content_length;
  return Parse::kOk;
}

// ---------------------------------------------------------------------------------------------
// connections and the event loop

struct Conn {
  int fd = -1;
  bool close_after = false;
  bool want_write = false;
  int64_t last_active = 0;
  std::string in;   // a partial request; empty almost always
  std::string out;  // unsent response bytes; empty almost always
};

int g_epoll;
std::vector<Conn*> g_conns;  // indexed by fd
char g_rbuf[kReadChunk];
std::string g_wbuf;      // responses for the connection being served
std::string g_body;      // the response body being built
std::string g_chunked;   // decoded chunked request body
int64_t g_now;
int64_t g_requests, g_accepts, g_closes;  // counters for STALL_LOG_MS

void close_conn(Conn* c) {
  epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, nullptr);
  close(c->fd);
  g_conns[c->fd] = nullptr;
  delete c;
  g_closes++;
}

void set_events(Conn* c, bool want_write) {
  if (c->want_write == want_write) return;
  c->want_write = want_write;
  epoll_event ev{};
  ev.events = want_write ? EPOLLOUT : EPOLLIN | EPOLLRDHUP;
  ev.data.fd = c->fd;
  epoll_ctl(g_epoll, EPOLL_CTL_MOD, c->fd, &ev);
}

// Sends `data`; keeps whatever the socket won't take in c->out and waits for EPOLLOUT.
// Returns false if the connection was closed.
bool send_or_queue(Conn* c, const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = send(c->fd, data, len, MSG_NOSIGNAL);
    if (n > 0) {
      data += n;
      len -= n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    close_conn(c);
    return false;
  }
  if (len > 0) {
    c->out.assign(data, len);
    set_events(c, true);
    return true;
  }
  c->out.clear();
  if (c->close_after) {
    close_conn(c);
    return false;
  }
  set_events(c, false);
  return true;
}

// Serves every complete request in [data, data+len) into g_wbuf. Returns the bytes consumed.
size_t serve(Conn* c, const char* data, size_t len) {
  size_t off = 0;
  Request req;
  while (off < len && !c->close_after) {
    size_t used = 0;
    Parse p = parse_request(data + off, len - off, &req, &used, g_chunked);
    if (p == Parse::kIncomplete) break;
    g_body.clear();
    if (p != Parse::kOk) {
      c->close_after = true;
      int status = p == Parse::kHeadersTooLarge ? 431 : p == Parse::kBodyTooLarge ? 413 : 400;
      error(g_body, status, status == 400 ? "bad request" : status == 413 ? "payload too large"
                                                                          : "headers too large");
      append_response(g_wbuf, status, g_body, true);
      off = len;
      break;
    }
    int status = route(req, g_body);
    g_requests++;
    if (!req.keep_alive) c->close_after = true;
    append_response(g_wbuf, status, g_body, c->close_after);
    off += used;
  }
  return off;
}

void on_readable(Conn* c) {
  ssize_t n = recv(c->fd, g_rbuf, sizeof g_rbuf, 0);
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    close_conn(c);
    return;
  }
  c->last_active = g_now;
  g_wbuf.clear();
  if (c->in.empty()) {
    // Fast path: parse straight out of the shared read buffer, keep only a partial tail.
    size_t used = serve(c, g_rbuf, n);
    if (used < static_cast<size_t>(n)) c->in.assign(g_rbuf + used, n - used);
  } else {
    c->in.append(g_rbuf, n);
    size_t used = serve(c, c->in.data(), c->in.size());
    c->in.erase(0, used);
    if (c->in.capacity() > 4 * kReadChunk && c->in.size() < kReadChunk) c->in.shrink_to_fit();
  }
  if (!g_wbuf.empty()) send_or_queue(c, g_wbuf.data(), g_wbuf.size());
  else if (c->close_after) close_conn(c);
}

void on_writable(Conn* c) {
  c->last_active = g_now;
  std::string pending;
  pending.swap(c->out);
  if (!send_or_queue(c, pending.data(), pending.size())) return;
  // Requests that arrived while we were blocked on output are still in c->in.
  if (!c->want_write && !c->in.empty()) {
    g_wbuf.clear();
    size_t used = serve(c, c->in.data(), c->in.size());
    c->in.erase(0, used);
    if (!g_wbuf.empty()) send_or_queue(c, g_wbuf.data(), g_wbuf.size());
  }
}

// Accepts up to 64 pending connections. During a ramp-up thousands can be queued, and accepting
// them all at once held up every request behind them for tens of ms. The listening socket is
// level-triggered, so the rest come in the next loop iterations, between requests.
void accept_some(int lfd) {
  for (int i = 0; i < 64; i++) {
    int fd = accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      return;  // EAGAIN, or out of descriptors until some close
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (static_cast<size_t>(fd) >= g_conns.size()) g_conns.resize(fd * 2 + 1, nullptr);
    Conn* c = new Conn;
    c->fd = fd;
    c->last_active = g_now;
    g_conns[fd] = c;
    g_accepts++;
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);
  }
}

// STALL_LOG_MS diagnostics: one line per slow batch, splitting its time into serving requests and
// the checkpoint, with the page faults and the context switches it took.
struct Stall {
  timespec t0, t1;
  rusage r0;
  int64_t req0, acc0, close0;

  static double ms(const timespec& a, const timespec& b) {
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
  }
  static double cpu_ms(const rusage& r) {
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e3 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e3;
  }
  void begin() {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    getrusage(RUSAGE_SELF, &r0);
    req0 = g_requests;
    acc0 = g_accepts;
    close0 = g_closes;
  }
  void mark() { clock_gettime(CLOCK_MONOTONIC, &t1); }
  void end(int events, double threshold_ms) {
    timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (ms(t0, t2) < threshold_ms) return;
    rusage r1;
    getrusage(RUSAGE_SELF, &r1);
    std::fprintf(stderr,
                 "t=%llds stall %.2f ms (cpu %.2f ms): %d events, %lld requests, %lld accepts, %lld closes in %.2f ms, "
                 "checkpoint %.2f ms, "
                 "faults %ld minor %ld major, ctxsw %ld vol %ld invol\n",
                 static_cast<long long>(g_now - g_start_s), ms(t0, t2), cpu_ms(r1) - cpu_ms(r0), events, static_cast<long long>(g_requests - req0),
                 static_cast<long long>(g_accepts - acc0), static_cast<long long>(g_closes - close0),
                 ms(t0, t1), ms(t1, t2),
                 r1.ru_minflt - r0.ru_minflt, r1.ru_majflt - r0.ru_majflt, r1.ru_nvcsw - r0.ru_nvcsw,
                 r1.ru_nivcsw - r0.ru_nivcsw);
  }
};

int listen_on(const char* host, const char* port) {
  addrinfo hints{}, *res;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) die("getaddrinfo", gai_strerror(rc));
  int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) die("socket", strerror(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) die("bind", strerror(errno));
  if (listen(fd, 65535) < 0) die("listen", strerror(errno));
  freeaddrinfo(res);
  return fd;
}

}  // namespace

int main() {
  const char* path = std::getenv("SQLITE_PATH");
  const char* secret = std::getenv("JWT_SECRET");
  const char* host = std::getenv("HOST");
  const char* port = std::getenv("PORT");
  if (!path || !*path) die("SQLITE_PATH is not set");
  if (!secret) die("JWT_SECRET is not set");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3000";

  signal(SIGPIPE, SIG_IGN);
  rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  // Diagnostics, off by default: log every event-loop batch that takes longer than this.
  const char* stall_env = std::getenv("STALL_LOG_MS");
  const double stall_ms = stall_env ? std::atof(stall_env) : 0;

  g_start_s = monotonic_s();
  hmac_init(secret);
  init_db(path);

  int lfd = listen_on(host, port);
  g_epoll = epoll_create1(EPOLL_CLOEXEC);
  epoll_event lev{};
  lev.events = EPOLLIN;
  lev.data.fd = lfd;
  epoll_ctl(g_epoll, EPOLL_CTL_ADD, lfd, &lev);
  g_conns.resize(4096, nullptr);
  g_wbuf.reserve(64 * 1024);
  g_body.reserve(16 * 1024);
  std::fprintf(stderr, "listening on %s:%s\n", host, port);

  std::vector<epoll_event> events(1024);
  int64_t last_sweep = monotonic_s();
  for (;;) {
    int n = epoll_wait(g_epoll, events.data(), static_cast<int>(events.size()), 1000);
    g_now = monotonic_s();
    Stall stall;
    if (stall_ms > 0) stall.begin();
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == lfd) {
        accept_some(lfd);
        continue;
      }
      Conn* c = g_conns[fd];
      if (!c) continue;
      uint32_t ev = events[i].events;
      if (ev & EPOLLOUT) on_writable(c);
      else if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) on_readable(c);
    }
    // Every response of this batch has been sent (or queued in the socket), so a checkpoint here
    // delays only requests that are already waiting, not the write that made it due.
    if (stall_ms > 0) stall.mark();
    checkpoint_if_due();
    if (stall_ms > 0) stall.end(n, stall_ms);
    if (g_now - last_sweep >= 10) {  // drop connections idle well past the keep-alive window
      last_sweep = g_now;
      for (Conn* c : g_conns)
        if (c && g_now - c->last_active > kIdleTimeoutS) close_conn(c);
    }
  }
}
