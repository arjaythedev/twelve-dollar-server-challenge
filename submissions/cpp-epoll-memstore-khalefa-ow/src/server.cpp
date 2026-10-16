// The $12 server challenge API with an app-specific in-memory store. THIS SERVER DOES NOT USE
// SQLITE: it never opens $SQLITE_PATH and doesn't link SQLite. bin/import (src/import.cpp) converts
// the seed feed.db into the store's snapshot once, before this server starts.
//
// Not a valid entry: it breaks rule 2 (the database is SQLite) and rule 5 (no in-memory copies of
// tables). It measures how much of the per-request cost is SQLite.
//
// Same HTTP server as cpp-epoll v2: one epoll event loop on one thread. The data lives in memory in
// the layout the endpoints need (see store.hpp); every write is appended to a WAL, one write() per
// batch of events, before any of the batch's responses is sent. A forked child writes snapshots,
// which compact the WAL.
//
// Durability matches SQLite's WAL mode with synchronous=NORMAL: a commit survives a process crash
// at once and reaches the disk with the OS's writeback; the WAL is synced before a snapshot takes it
// over (SQLite: before a checkpoint), so a power loss can drop the last commits but never corrupts
// the store. WAL_SYNC=full makes every acknowledged write durable, like synchronous=FULL, with group
// commit on a sync thread: a write's response waits for its fdatasync, but the loop doesn't.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "store.hpp"

namespace {

using ms::append_int;
using ms::append_json_str;
using ms::die;

constexpr size_t kReadChunk = 64 * 1024;
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr int kIdleTimeoutS = 120;  // SPEC: keep idle connections at least 65 s
constexpr int64_t kMaxId = INT64_MAX;

int64_t monotonic_s() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return ts.tv_sec;
}

// A positive integer in plain decimal digits. Returns 1 if valid, 0 if not a positive integer,
// and 2 if it is valid but beyond 64-bit range (so no post can have it).
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

// Callers keep their JField arrays static, so `str` keeps its capacity across requests and the
// parser fills it without allocating.
struct JField {
  JField(std::string_view k, size_t reserve = 64) : key(k) { str.reserve(reserve); }
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
    for (size_t i = 0; i < nfields; i++) {
      fields[i].type = J_NONE;
      fields[i].str.clear();
    }
    ws();
    if (!value(top, nullptr, nullptr, fields, nfields)) return false;
    ws();
    return p_ == e_;
  }

 private:
  const char* p_;
  const char* e_;
  int depth_ = 0;
  // Shared by every parser (one thread): a key is only compared, before the value is parsed.
  inline static std::string key_ = [] { std::string s; s.reserve(256); return s; }();

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

  static std::string decoded = [] { std::string s; s.reserve(1024); return s; }();
  JType top;
  static JField alg[1] = {{"alg"}};
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
  static JField f[4] = {{"sub"}, {"username"}, {"exp"}, {"nbf"}};
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
  user->username.assign(f[kUsername].str);  // a copy, so both buffers keep their capacity
  return AuthResult::kOk;
}

// ---------------------------------------------------------------------------------------------
// the store and its WAL

ms::Store g_store;
std::string g_dir;           // the store directory
ms::SrcId g_src;             // the feed.db it was imported from
std::string g_walbuf;        // WAL records of the current batch, written at its end
int g_wal_fd = -1;
uint64_t g_wal_seq = 0;      // sequence number of the open WAL file
uint64_t g_wal_bytes = 0;    // bytes in the WAL files no snapshot covers yet
uint64_t g_snapshot_at = 0;  // a snapshot starts when g_wal_bytes reaches this
uint64_t g_snapshot_every = 0;
bool g_sync_full = false;    // WAL_SYNC=full: fdatasync each batch that wrote
pid_t g_child = 0;           // the snapshot writer, while it runs
uint64_t g_child_next_wal = 0, g_child_bytes = 0;

std::string wal_path(uint64_t seq) {
  char name[32];
  std::snprintf(name, sizeof name, "/wal.%016llu", static_cast<unsigned long long>(seq));
  return g_dir + name;
}

std::vector<uint64_t> wal_seqs() {
  std::vector<uint64_t> seqs;
  DIR* d = opendir(g_dir.c_str());
  if (!d) die("cannot open", g_dir.c_str());
  while (dirent* e = readdir(d)) {
    std::string_view name = e->d_name;
    if (name.size() != 20 || name.substr(0, 4) != "wal.") continue;
    uint64_t seq;
    auto r = std::from_chars(name.data() + 4, name.data() + 20, seq);
    if (r.ec == std::errc() && r.ptr == name.data() + 20) seqs.push_back(seq);
  }
  closedir(d);
  std::sort(seqs.begin(), seqs.end());
  return seqs;
}

void open_wal(uint64_t seq) {
  g_wal_fd = open(wal_path(seq).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (g_wal_fd < 0) die("cannot create WAL", strerror(errno));
  ms::fsync_dir(g_dir);
  g_wal_seq = seq;
}

// Maps the snapshot, replays the WAL files after it, and starts a new WAL file.
void load_store() {
  ms::SnapHeader h;
  ms::load_snapshot(g_store, g_dir, &h);
  if (!ms::same_src(h.src, g_src)) die("the store was imported from another feed.db; run bin/import");
  unlink((g_dir + "/snapshot.tmp").c_str());  // from a snapshot writer that didn't finish
  uint64_t last = h.next_wal - 1;
  std::string data;
  for (uint64_t seq : wal_seqs()) {
    std::string path = wal_path(seq);
    if (seq < h.next_wal) {  // covered by the snapshot; a crash came before its deletion
      unlink(path.c_str());
      continue;
    }
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) die("cannot open", path.c_str());
    data.clear();
    char buf[1 << 16];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) != 0;) {
      if (n < 0) die("cannot read", path.c_str());
      data.append(buf, n);
    }
    size_t valid = ms::wal_replay(g_store, data.data(), data.size());
    if (valid == 0) {  // nothing committed in it (each start opens a new file)
      close(fd);
      unlink(path.c_str());
      continue;
    }
    if (valid < data.size()) {
      std::fprintf(stderr, "%s: dropped %zu bytes of torn tail\n", path.c_str(), data.size() - valid);
      if (ftruncate(fd, valid) != 0) die("cannot truncate", path.c_str());
    }
    // Writes after this start go to a new file. Sync this one first, so a power loss can't keep
    // those while losing the end of this one: what survives is always a prefix of the commits.
    if (fdatasync(fd) != 0) die("cannot sync", path.c_str());
    close(fd);
    g_wal_bytes += valid;
    last = seq;
  }
  open_wal(last + 1);
}

// WAL_SYNC=full uses group commit on a sync thread, so the loop never waits for the disk. The loop
// write()s each batch's records and moves on; the thread fdatasyncs everything written so far, so
// the commits that arrive during one sync all share the next one. Only the responses of connections
// that wrote wait for their sync (Conn::held); reads are sent at once.
std::atomic<uint64_t> g_sync_target{0};  // WAL bytes written, counted across files: the thread's goal
std::atomic<uint64_t> g_synced{0};       // WAL bytes known to be on disk
uint64_t g_written = 0;                  // WAL bytes written (loop only)
std::mutex g_wal_fd_mu;                  // a sync in progress vs. the loop switching WAL files
int g_wake_efd = -1;                     // loop -> thread: there is more to sync
int g_done_efd = -1;                     // thread -> loop (in epoll): g_synced moved

void sync_thread() {
  for (;;) {
    uint64_t v;
    if (read(g_wake_efd, &v, sizeof v) < 0 && errno != EINTR) die("eventfd read failed", strerror(errno));
    for (;;) {
      // All bytes up to t were written before t was published, so this fdatasync covers them.
      uint64_t t = g_sync_target.load(std::memory_order_acquire);
      if (t <= g_synced.load(std::memory_order_relaxed)) break;
      {
        std::lock_guard<std::mutex> lock(g_wal_fd_mu);
        if (fdatasync(g_wal_fd) != 0) die("WAL fdatasync failed", strerror(errno));
      }
      g_synced.store(t, std::memory_order_release);
      uint64_t one = 1;
      if (write(g_done_efd, &one, sizeof one) < 0) die("eventfd write failed", strerror(errno));
    }
  }
}

// Writes the batch's records. The responses of the batch are sent only after this returns (and,
// with WAL_SYNC=full, those of connections that wrote only after their sync).
void wal_commit() {
  if (g_walbuf.empty()) return;
  const char* p = g_walbuf.data();
  size_t n = g_walbuf.size();
  while (n > 0) {
    ssize_t w = write(g_wal_fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      die("WAL write failed", strerror(errno));  // nothing of this batch was acknowledged
    }
    p += w;
    n -= w;
  }
  g_wal_bytes += g_walbuf.size();
  g_written += g_walbuf.size();
  g_walbuf.clear();
  if (g_sync_full) {
    g_sync_target.store(g_written, std::memory_order_release);
    uint64_t one = 1;
    if (write(g_wake_efd, &one, sizeof one) < 0) die("eventfd write failed", strerror(errno));
  }
}

// Starts a snapshot once the WAL is big enough, and deletes the WAL files it covers once it is done.
// fork() gives the child a frozen copy-on-write image of the store, so the loop keeps serving.
void snapshot_step() {
  if (g_child) {
    int status;
    pid_t r = waitpid(g_child, &status, WNOHANG);
    if (r == 0) return;
    g_child = 0;
    if (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      for (uint64_t seq : wal_seqs())
        if (seq < g_child_next_wal) unlink(wal_path(seq).c_str());
      g_wal_bytes -= g_child_bytes;
      g_snapshot_at = g_snapshot_every;
    } else {
      std::fprintf(stderr, "snapshot failed; keeping the WAL\n");
      g_snapshot_at = g_wal_bytes + g_snapshot_every;
    }
    return;
  }
  if (g_wal_bytes < g_snapshot_at) return;
  // Everything committed so far is in WAL files < the new one: exactly what the child will write.
  // Sync the old file first (SQLite syncs the WAL before a checkpoint), so a power loss can't keep
  // commits of the new file while losing earlier ones.
  {
    std::lock_guard<std::mutex> lock(g_wal_fd_mu);  // waits for a sync of the old file to finish
    if (fdatasync(g_wal_fd) != 0) die("WAL fdatasync failed", strerror(errno));
    close(g_wal_fd);
    open_wal(g_wal_seq + 1);
  }
  pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "fork failed: %s\n", strerror(errno));
    g_snapshot_at = g_wal_bytes + g_snapshot_every;
    return;
  }
  if (pid == 0) {
    // Drop the inherited sockets: otherwise a connection the parent closes stays open in the child
    // for the length of the snapshot.
    close_range(3, ~0u, 0);
    _exit(ms::write_snapshot(g_store, g_dir, g_wal_seq, g_src) ? 0 : 1);
  }
  g_child = pid;
  g_child_next_wal = g_wal_seq;
  g_child_bytes = g_wal_bytes;
}

// created_at for a new row, as SQLite's strftime('%Y-%m-%dT%H:%M:%fZ', 'now') writes it.
void now_ts(char out[ms::kTsLen]) {
  static time_t cached = -1;
  static char prefix[20];  // 2025-12-31T23:59:19
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  if (ts.tv_sec != cached) {
    cached = ts.tv_sec;
    tm t;
    gmtime_r(&ts.tv_sec, &t);
    std::strftime(prefix, sizeof prefix, "%Y-%m-%dT%H:%M:%S", &t);
  }
  std::memcpy(out, prefix, 19);
  int ms = static_cast<int>(ts.tv_nsec / 1000000);
  out[19] = '.';
  out[20] = static_cast<char>('0' + ms / 100);
  out[21] = static_cast<char>('0' + ms / 10 % 10);
  out[22] = static_cast<char>('0' + ms % 10);
  out[23] = 'Z';
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

void append_post(std::string& b, const ms::Post& p) {
  b.append(p.json, p.json_len);
  append_int(b, p.likes);
  b.push_back('}');
}

int handle_health(std::string& b) {
  b.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  append_int(b, monotonic_s() - g_start_s);
  b.push_back('}');
  return 200;
}

int handle_feed(std::string& b) {
  b.append("{\"posts\":[");
  int n = 0;
  for (auto it = g_store.order.rbegin(); it != g_store.order.rend() && n < 20; ++it) {
    const ms::Post& p = g_store.posts[*it];
    if (!p.visible) continue;
    if (n++) b.push_back(',');
    append_post(b, p);
  }
  b.append("]}");
  return 200;
}

int handle_get_post(std::string& b, std::string_view id_str) {
  int64_t id;
  int v = parse_id(id_str, &id);
  if (v == 0) return error(b, 400, "invalid post id");
  if (v == 2 || !g_store.has_post(id) || !g_store.posts[id].visible) return error(b, 404, "post not found");
  b.append("{\"post\":");
  append_post(b, g_store.posts[id]);
  b.push_back('}');
  return 200;
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
  static JField field[1] = {{"body", 4096}};
  if (!JsonParser(req_body).parse(&top, field, 1)) return error(b, 400, "malformed JSON body");
  if (top != J_OBJ || field[0].type != J_STR) return error(b, 400, "body is required");
  std::string_view text = trim_js(field[0].str);
  if (text.empty()) return error(b, 400, "body is required");
  if (utf8_length(text) > 500) return error(b, 400, "body must be at most 500 characters");

  int64_t id = g_store.max_post_id + 1;  // as SQLite assigns a rowid: one past the largest
  char ts[ms::kTsLen];
  now_ts(ts);
  g_store.add_post(id, user.id, ts, text);
  ms::wal_post(g_walbuf, id, user.id, ts, text);

  b.append("{\"post\":{\"id\":");
  append_int(b, id);
  b.append(",\"body\":");
  append_json_str(b, text);
  b.append(",\"created_at\":\"");
  b.append(ts, ms::kTsLen);
  b.append("\",\"author\":");
  append_json_str(b, user.username);
  b.append(",\"like_count\":0}}");
  return 201;
}

int handle_like(std::string& b, std::string_view auth, bool has_auth, std::string_view id_str) {
  static AuthUser user;
  AuthResult ar = authenticate(auth, has_auth, &user);  // auth before the id, per the spec
  if (ar != AuthResult::kOk) return auth_error(b, ar);
  int64_t id;
  int v = parse_id(id_str, &id);
  if (v == 0) return error(b, 400, "invalid post id");
  if (v == 2 || !g_store.has_post(id)) return error(b, 404, "post not found");

  bool inserted = g_store.like(user.id, id);
  if (inserted) ms::wal_like(g_walbuf, user.id, id);
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
  // WAL_SYNC=full: responses waiting until the WAL is synced up to held_until. Once a connection
  // has some, all its later responses queue behind them, so its responses stay in order.
  std::string held;
  uint64_t held_until = 0;
};

// The responses of one batch wait in g_out until the batch's WAL records are written, so no client
// sees a write (its own or anyone's) that a process crash could still lose.
struct Pending {
  Conn* c;
  size_t off, len;
  bool wrote;  // the connection's requests in this batch wrote WAL records
};

int g_epoll;
std::vector<Conn*> g_conns;  // indexed by fd
char g_rbuf[kReadChunk];
std::string g_out;              // every response of the current batch
std::vector<Pending> g_pending;  // one entry per connection that has responses in g_out
std::vector<Conn*> g_held;      // connections with held responses
std::string g_release;          // a connection's held responses being sent
std::string g_body;             // the response body being built
std::string g_chunked;          // decoded chunked request body
int64_t g_now;

void close_conn(Conn* c) {
  if (!c->held.empty()) g_held.erase(std::find(g_held.begin(), g_held.end(), c));
  epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, nullptr);
  close(c->fd);
  g_conns[c->fd] = nullptr;
  delete c;
}

void set_events(Conn* c, bool want_write) {
  if (c->want_write == want_write) return;
  c->want_write = want_write;
  epoll_event ev{};
  ev.events = want_write ? EPOLLOUT : EPOLLIN | EPOLLRDHUP;
  ev.data.fd = c->fd;
  // ENOENT: on_readable took a half-closed connection out of epoll while it had held responses.
  if (epoll_ctl(g_epoll, EPOLL_CTL_MOD, c->fd, &ev) != 0 && errno == ENOENT)
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, c->fd, &ev);
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
    if (data >= c->out.data() && data <= c->out.data() + c->out.size())
      c->out.erase(0, data - c->out.data());  // resending c->out itself: keep its buffer
    else
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

// Serves every complete request in [data, data+len) into g_out. Returns the bytes consumed.
size_t serve(Conn* c, const char* data, size_t len) {
  size_t start = g_out.size(), wal_start = g_walbuf.size(), off = 0;
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
      append_response(g_out, status, g_body, true);
      off = len;
      break;
    }
    int status = route(req, g_body);
    if (!req.keep_alive) c->close_after = true;
    append_response(g_out, status, g_body, c->close_after);
    off += used;
  }
  if (g_out.size() > start) g_pending.push_back({c, start, g_out.size() - start, g_walbuf.size() > wal_start});
  return off;
}

bool has_pending(Conn* c) { return !g_pending.empty() && g_pending.back().c == c; }

void on_readable(Conn* c) {
  ssize_t n = recv(c->fd, g_rbuf, sizeof g_rbuf, 0);
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (n == 0 && !c->held.empty()) {
      // The client finished sending but still gets its held responses: stop reading, and close
      // once they are sent.
      c->close_after = true;
      epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, nullptr);
      return;
    }
    close_conn(c);
    return;
  }
  c->last_active = g_now;
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
  if (c->close_after && !has_pending(c)) close_conn(c);
}

void on_writable(Conn* c) {
  c->last_active = g_now;
  if (!send_or_queue(c, c->out.data(), c->out.size())) return;
  // Requests that arrived while we were blocked on output are still in c->in.
  if (!c->want_write && !c->in.empty()) {
    size_t used = serve(c, c->in.data(), c->in.size());
    c->in.erase(0, used);
    if (c->close_after && !has_pending(c)) close_conn(c);
  }
}

// End of a batch: one WAL write for all its writes, then all its responses. With WAL_SYNC=full, the
// responses of connections that wrote are held until the sync thread has synced their records.
void commit_batch() {
  wal_commit();
  for (const Pending& p : g_pending) {
    Conn* c = p.c;
    if (g_sync_full && (p.wrote || !c->held.empty())) {
      if (c->held.empty()) g_held.push_back(c);
      c->held.append(g_out.data() + p.off, p.len);
      c->held_until = g_written;
    } else {
      send_or_queue(c, g_out.data() + p.off, p.len);
    }
  }
  g_pending.clear();
  g_out.clear();
}

// The sync thread moved g_synced: send the held responses it covers.
void release_synced() {
  uint64_t v;
  if (read(g_done_efd, &v, sizeof v) < 0) return;  // EAGAIN: already handled
  uint64_t synced = g_synced.load(std::memory_order_acquire);
  size_t keep = 0;
  for (size_t i = 0; i < g_held.size(); i++) {
    Conn* c = g_held[i];
    if (c->held_until > synced) {
      g_held[keep++] = c;
      continue;
    }
    g_release.clear();
    g_release.swap(c->held);  // c->held is now empty, so close_conn won't touch g_held
    if (!c->out.empty()) c->out.append(g_release);  // already waiting for EPOLLOUT
    else send_or_queue(c, g_release.data(), g_release.size());
  }
  g_held.resize(keep);
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
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);
  }
}

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
  const char* dir = std::getenv("STORE_DIR");
  const char* sync = std::getenv("WAL_SYNC");
  const char* snap_mb = std::getenv("SNAPSHOT_WAL_MB");
  if (!path || !*path) die("SQLITE_PATH is not set");
  if (!secret) die("JWT_SECRET is not set");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3000";
  g_dir = dir && *dir ? dir : std::string(path) + ".memstore";
  g_sync_full = sync && std::strcmp(sync, "full") == 0;
  g_snapshot_every = static_cast<uint64_t>(snap_mb && *snap_mb ? std::atof(snap_mb) : 64) << 20;
  g_snapshot_at = g_snapshot_every;

  signal(SIGPIPE, SIG_IGN);
  rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  g_start_s = monotonic_s();
  hmac_init(secret);
  g_src = ms::src_id(path);
  timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  load_store();
  clock_gettime(CLOCK_MONOTONIC, &t1);
  std::fprintf(stderr, "loaded %zu posts, %zu likes in %.0f ms (WAL_SYNC=%s)\n", g_store.order.size(),
               g_store.likes.size(), (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
               g_sync_full ? "full" : "normal");

  int lfd = listen_on(host, port);
  g_epoll = epoll_create1(EPOLL_CLOEXEC);
  epoll_event lev{};
  lev.events = EPOLLIN;
  lev.data.fd = lfd;
  epoll_ctl(g_epoll, EPOLL_CTL_ADD, lfd, &lev);
  if (g_sync_full) {
    g_wake_efd = eventfd(0, EFD_CLOEXEC);
    g_done_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_wake_efd < 0 || g_done_efd < 0) die("eventfd", strerror(errno));
    epoll_event dev{};
    dev.events = EPOLLIN;
    dev.data.fd = g_done_efd;
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, g_done_efd, &dev);
    std::thread(sync_thread).detach();
  }
  g_conns.resize(4096, nullptr);
  g_out.reserve(256 * 1024);
  g_body.reserve(16 * 1024);
  g_walbuf.reserve(64 * 1024);
  std::fprintf(stderr, "listening on %s:%s\n", host, port);

  std::vector<epoll_event> events(1024);
  int64_t last_sweep = monotonic_s();
  for (;;) {
    int n = epoll_wait(g_epoll, events.data(), static_cast<int>(events.size()), 1000);
    g_now = monotonic_s();
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == lfd) {
        accept_some(lfd);
        continue;
      }
      if (fd == g_done_efd) {
        release_synced();
        continue;
      }
      Conn* c = g_conns[fd];
      if (!c) continue;
      uint32_t ev = events[i].events;
      if (ev & EPOLLOUT) on_writable(c);
      else if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) on_readable(c);
    }
    commit_batch();
    snapshot_step();
    if (g_now - last_sweep >= 10) {  // drop connections idle well past the keep-alive window
      last_sweep = g_now;
      for (Conn* c : g_conns)
        if (c && g_now - c->last_active > kIdleTimeoutS) close_conn(c);
    }
  }
}
