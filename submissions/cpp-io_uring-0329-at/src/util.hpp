#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace app {

constexpr void append_int(std::string& s, long long v) {
  char tmp[24];
  int n = 0;
  bool neg = v < 0;
  unsigned long long u = neg ? ~static_cast<unsigned long long>(v) + 1ULL
                             : static_cast<unsigned long long>(v);
  do {
    tmp[n++] = static_cast<char>('0' + u % 10);
    u /= 10;
  } while (u);
  if (neg) tmp[n++] = '-';
  while (n) s.push_back(tmp[--n]);
}

constexpr const char* status_line(int s) {
  switch (s) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 413: return "HTTP/1.1 413 Payload Too Large\r\n";
    default: return "HTTP/1.1 500 Internal Server Error\r\n";
  }
}

constexpr int parse_id(std::string_view s, long long* out) {
  if (s.empty()) return 0;
  if (!std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
  long long v = 0;
  for (char c : s) {
    if (v > (INT64_MAX - (c - '0')) / 10) return 2;
    v = v * 10 + (c - '0');
  }
  if (v <= 0) return 0;
  *out = v;
  return 1;
}

constexpr size_t utf8_len(std::string_view s) {
  return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](unsigned char c) {
    return (c & 0xC0) != 0x80;
  }));
}

constexpr bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr std::string_view trim_ws(std::string_view s) {
  auto b = std::find_if_not(s.begin(), s.end(), is_space);
  if (b == s.end()) return {};
  auto e = std::find_if_not(s.rbegin(), s.rend(), is_space).base();
  return {b, static_cast<size_t>(e - b)};
}

constexpr bool ieq(const char* a, size_t alen, const char* b, size_t blen) {
  if (alen != blen) return false;
  for (size_t i = 0; i < alen; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
  }
  return true;
}

constexpr size_t b64url(std::string_view in, char* out, size_t cap) {
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else if (c == '=') break;
    else return 0;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return 0;
      out[o++] = static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return o;
}

constexpr bool same(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

constexpr bool needs_escape(unsigned char c) { return c == '"' || c == '\\' || c < 0x20; }

constexpr void json_escape(std::string& out, std::string_view s) {
  constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  size_t i = 0;
  while (i < s.size()) {
    auto it = std::find_if(s.begin() + static_cast<ptrdiff_t>(i), s.end(),
                           [](char c) { return needs_escape(static_cast<unsigned char>(c)); });
    size_t j = static_cast<size_t>(it - s.begin());
    out.append(s.data() + i, j - i);
    if (j == s.size()) break;
    unsigned char c = static_cast<unsigned char>(s[j]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        out += "\\u00";
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 15]);
    }
    i = j + 1;
  }
  out.push_back('"');
}

constexpr void build_response(std::string& out, int status, std::string_view body) {
  out += status_line(status);
  out += "Content-Type: application/json\r\nContent-Length: ";
  append_int(out, static_cast<long long>(body.size()));
  out += "\r\n\r\n";
  out.append(body.data(), body.size());
}

consteval bool util_selftest() {
  long long v = 0;
  if (parse_id("123", &v) != 1 || v != 123) return false;
  if (parse_id("0", &v) != 0) return false;
  if (parse_id("", &v) != 0) return false;
  if (parse_id("abc", &v) != 0) return false;
  if (parse_id("-1", &v) != 0) return false;
  if (parse_id("1.5", &v) != 0) return false;

  if (utf8_len("") != 0) return false;
  if (utf8_len("abc") != 3) return false;
  if (utf8_len("h\xC3\xA9") != 2) return false;
  if (utf8_len("\xF0\x9F\x90\x8B") != 1) return false;

  if (trim_ws("  x  ") != "x") return false;
  if (trim_ws("\t\r\n x \n") != "x") return false;
  if (trim_ws("   ") != "") return false;
  if (trim_ws("x") != "x") return false;

  if (std::string_view(status_line(200)) != "HTTP/1.1 200 OK\r\n") return false;
  if (std::string_view(status_line(404)) != "HTTP/1.1 404 Not Found\r\n") return false;

  if (!ieq("Authorization", 13, "authorization", 13)) return false;
  if (ieq("Authorization", 13, "authorizatio", 12)) return false;

  {
    char buf[64];
    size_t n = b64url("eyJhbGciOiJIUzI1NiJ9", buf, sizeof buf);
    if (n != 15 || !same({buf, n}, "{\"alg\":\"HS256\"}")) return false;
    n = b64url("eyJzdWIiOiIxIn0", buf, sizeof buf);
    if (n != 11 || !same({buf, n}, "{\"sub\":\"1\"}")) return false;
    n = b64url("PDw8Pj4-", buf, sizeof buf);
    if (n != 6 || !same({buf, n}, "<<<>>>")) return false;
    n = b64url("Pz8_", buf, sizeof buf);
    if (n != 3 || !same({buf, n}, "???")) return false;
  }

  {
    std::string j;
    json_escape(j, "a\"b\\c\nd\x01");
    if (!same(j, "\"a\\\"b\\\\c\\nd\\u0001\"")) return false;
    j.clear();
    json_escape(j, "");
    if (!same(j, "\"\"")) return false;
    j.clear();
    json_escape(j, "h\xC3\xA9llo");
    if (!same(j, "\"h\xC3\xA9llo\"")) return false;
    j.clear();
    json_escape(j, "plain text 123");
    if (!same(j, "\"plain text 123\"")) return false;
  }

  {
    std::string r;
    build_response(r, 200, "{}");
    if (!same(r, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"))
      return false;
    r.clear();
    build_response(r, 201, "");
    if (!same(r, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n"))
      return false;
  }

  {
    std::string n;
    append_int(n, 0);
    if (!same(n, "0")) return false;
    n.clear();
    append_int(n, -1);
    if (!same(n, "-1")) return false;
    n.clear();
    append_int(n, 500000);
    if (!same(n, "500000")) return false;
    n.clear();
    append_int(n, INT64_MIN);
    if (!same(n, "-9223372036854775808")) return false;
    n.clear();
    append_int(n, INT64_MAX);
    if (!same(n, "9223372036854775807")) return false;
  }
  return true;
}

static_assert(util_selftest(), "util selftest");

}  // namespace app