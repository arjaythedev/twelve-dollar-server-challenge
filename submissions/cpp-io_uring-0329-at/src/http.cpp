#include "http.hpp"

#include <cstring>

namespace app::http {
namespace {

constexpr size_t kMaxHead = 16 * 1024;
constexpr size_t kMaxBody = 1024 * 1024;

inline const uint8_t* trim(const uint8_t* s, size_t& n) {
  while (n > 0 && (s[0] == ' ' || s[0] == '\t')) {
    s++;
    n--;
  }
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  return s;
}

template <size_t N>
inline bool eq_ignore_case(const uint8_t* a, size_t n, const char (&lower)[N]) {
  if (n != N - 1) return false;
  for (size_t i = 0; i < N - 1; i++) {
    uint8_t c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c + 32);
    if (c != static_cast<uint8_t>(lower[i])) return false;
  }
  return true;
}

}

Parsed parse(const uint8_t* buf, size_t len, Request& req, size_t& used) {
  const auto partial = [&]() { return len > kMaxHead ? Parsed::Bad : Parsed::Partial; };

  const uint8_t* nl = static_cast<const uint8_t*>(std::memchr(buf, '\n', len));
  if (!nl) return partial();
  size_t nl_pos = static_cast<size_t>(nl - buf);
  size_t line_len = nl_pos;
  if (line_len > 0 && buf[line_len - 1] == '\r') line_len--;
  const uint8_t* line = buf;
  const uint8_t* sp1 = static_cast<const uint8_t*>(std::memchr(line, ' ', line_len));
  if (!sp1) return Parsed::Bad;
  size_t sp1_pos = static_cast<size_t>(sp1 - line);
  const uint8_t* method = line;
  size_t method_len = sp1_pos;
  const uint8_t* rest = line + sp1_pos + 1;
  size_t rest_len = line_len - sp1_pos - 1;
  const uint8_t* sp2 = static_cast<const uint8_t*>(std::memchr(rest, ' ', rest_len));
  if (!sp2) return Parsed::Bad;
  size_t sp2_pos = static_cast<size_t>(sp2 - rest);
  const uint8_t* target = rest;
  size_t target_len = sp2_pos;
  const uint8_t* version = rest + sp2_pos + 1;
  size_t version_len = rest_len - sp2_pos - 1;
  if (version_len < 7 || std::memcmp(version, "HTTP/1.", 7) != 0 || method_len == 0 || target_len == 0)
    return Parsed::Bad;
  const uint8_t* q = static_cast<const uint8_t*>(std::memchr(target, '?', target_len));
  size_t path_len = q ? static_cast<size_t>(q - target) : target_len;

  const uint8_t* auth = nullptr;
  size_t auth_len = 0;
  bool has_auth = false;
  size_t body_len = 0;
  size_t pos = nl_pos + 1;
  for (;;) {
    const uint8_t* n2 =
        static_cast<const uint8_t*>(std::memchr(buf + pos, '\n', len - pos));
    if (!n2) return partial();
    size_t n = static_cast<size_t>(n2 - (buf + pos));
    const uint8_t* l = buf + pos;
    pos += n + 1;
    size_t l_len = n;
    if (l_len > 0 && l[l_len - 1] == '\r') l_len--;
    if (l_len == 0) break;
    const uint8_t* colon = static_cast<const uint8_t*>(std::memchr(l, ':', l_len));
    if (!colon) continue;
    size_t name_len = static_cast<size_t>(colon - l);
    const uint8_t* value = l + name_len + 1;
    size_t value_len = l_len - name_len - 1;
    if (name_len == 13 && eq_ignore_case(l, name_len, "authorization")) {
      auth = trim(value, value_len);
      auth_len = value_len;
      has_auth = true;
    } else if (name_len == 14 && eq_ignore_case(l, name_len, "content-length")) {
      value = trim(value, value_len);
      if (value_len == 0 || value_len > 9) return Parsed::Bad;
      size_t v = 0;
      for (size_t i = 0; i < value_len; i++) {
        uint8_t c = value[i];
        if (c < '0' || c > '9') return Parsed::Bad;
        v = v * 10 + static_cast<size_t>(c - '0');
      }
      if (v > kMaxBody) return Parsed::Bad;
      body_len = v;
    } else if (name_len == 17 && eq_ignore_case(l, name_len, "transfer-encoding")) {
      return Parsed::Bad;
    }
  }
  if (len < pos + body_len) return Parsed::Partial;
  req.method = method;
  req.method_len = static_cast<uint32_t>(method_len);
  req.path = target;
  req.path_len = static_cast<uint32_t>(path_len);
  req.auth = auth;
  req.auth_len = static_cast<uint32_t>(auth_len);
  req.has_auth = has_auth;
  req.body = buf + pos;
  req.body_len = static_cast<uint32_t>(body_len);
  used = pos + body_len;
  return Parsed::Done;
}

}
