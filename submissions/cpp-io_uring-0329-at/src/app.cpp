#include "app.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

#include "json.hpp"

namespace app {
namespace {

constexpr size_t kMaxBodyChars = 500;

constexpr std::string_view status_line(int s) {
  switch (s) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 201: return "HTTP/1.1 201 Created\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 401: return "HTTP/1.1 401 Unauthorized\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 503: return "HTTP/1.1 503 Service Unavailable\r\n";
    default: return "HTTP/1.1 500 Internal Server Error\r\n";
  }
}

size_t start(ByteBuf& out, int status) {
  const std::string_view s = status_line(status);
  buf_append(out, s.data(), s.size());
  static constexpr char kHead[] = "Content-Type: application/json\r\nContent-Length: 0000\r\n\r\n";
  buf_append(out, kHead, sizeof kHead - 1);
  return out.len;
}

void finish(ByteBuf& out, size_t body_start) {
  const size_t n = out.len - body_start;
  const size_t slot = body_start - 8;
  char digits[20];
  int d = 20;
  size_t v = n;
  for (;;) {
    digits[--d] = static_cast<char>('0' + v % 10);
    v /= 10;
    if (v == 0) break;
  }
  const size_t dl = static_cast<size_t>(20 - d);
  if (dl != 4) {

    const ptrdiff_t delta = static_cast<ptrdiff_t>(dl) - 4;
    const size_t new_start = static_cast<size_t>(static_cast<ptrdiff_t>(body_start) + delta);
    buf_set_len(out, static_cast<uint32_t>(n + new_start));
    std::memmove(out.p + new_start, out.p + body_start, n);
    out.len = static_cast<uint32_t>(new_start + n);
    std::memcpy(out.p + slot, digits + d, dl);
    std::memcpy(out.p + slot + dl, "\r\n\r\n", 4);
  } else {
    std::memcpy(out.p + slot, digits + d, 4);
  }
}

bool post_id(const uint8_t* s, uint32_t n, int64_t& out) { return jwt::parse_positive(s, n, out); }

double now_secs() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

bool is_js_space(uint32_t c) {
  switch (c) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
      return true;
    default:
      return c >= 0x2000 && c <= 0x200A;
  }
}

uint32_t utf8_decode(const uint8_t* p, size_t n, size_t& len) {
  uint8_t c = p[0];
  if (c < 0x80) {
    len = 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && n >= 2) {
    len = 2;
    return (static_cast<uint32_t>(c & 0x1F) << 6) | (p[1] & 0x3F);
  }
  if ((c & 0xF0) == 0xE0 && n >= 3) {
    len = 3;
    return (static_cast<uint32_t>(c & 0x0F) << 12) | (static_cast<uint32_t>(p[1] & 0x3F) << 6) |
           (p[2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0 && n >= 4) {
    len = 4;
    return (static_cast<uint32_t>(c & 0x07) << 18) | (static_cast<uint32_t>(p[1] & 0x3F) << 12) |
           (static_cast<uint32_t>(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
  }
  len = 1;
  return 0xFFFD;
}

void trim_js(const uint8_t* s, size_t n, size_t& a, size_t& b) {
  a = 0;
  b = n;
  while (a < b) {
    size_t l = 0;
    uint32_t cp = utf8_decode(s + a, b - a, l);
    if (!is_js_space(cp)) break;
    a += l;
  }
  while (b > a) {
    size_t st = b - 1;
    while (st > a && (s[st] & 0xC0) == 0x80) st--;
    size_t l = 0;
    uint32_t cp = utf8_decode(s + st, b - st, l);
    if (!is_js_space(cp)) break;
    b = st;
  }
}

size_t char_count(const uint8_t* s, size_t n) {
  size_t c = 0;
  for (size_t i = 0; i < n; i++)
    if ((s[i] & 0xC0) != 0x80) c++;
  return c;
}

inline bool method_is(const http::Request& r, std::string_view m) {
  return r.method_len == m.size() && std::memcmp(r.method, m.data(), m.size()) == 0;
}

inline bool path_is(const http::Request& r, std::string_view p) {
  return r.path_len == p.size() && std::memcmp(r.path, p.data(), p.size()) == 0;
}

}

void error(ByteBuf& out, int status, const char* msg) {
  const size_t b = start(out, status);
  buf_append(out, "{\"error\":\"");
  buf_append(out, msg, std::strlen(msg));
  buf_append(out, "\"}");
  finish(out, b);
}

App::App(Db&& db, const uint8_t* secret, size_t secret_len)
    : db(std::move(db)), jwt_(secret, secret_len), started_(std::chrono::steady_clock::now()) {
  body_.reserve(2048);
}

void App::handle(const http::Request& req, ByteBuf& out) {
  const uint32_t mark = out.len;
  const uint8_t* p = req.path;
  const uint32_t pl = req.path_len;
  try {
    if (method_is(req, "GET")) {
      if (path_is(req, "/feed")) {
        const size_t b = start(out, 200);
        db.feed(out);
        finish(out, b);
      } else if (pl >= 7 && std::memcmp(p, "/posts/", 7) == 0) {
        const uint8_t* rest = p + 7;
        const uint32_t rl = pl - 7;
        if (std::memchr(rest, '/', rl)) {
          error(out, 404, "not found");
          return;
        }
        int64_t id = 0;
        if (!post_id(rest, rl, id)) {
          error(out, 400, "invalid post id");
          return;
        }
        const uint32_t m = out.len;
        const size_t b = start(out, 200);
        if (db.post(id, out) == PostLookup::Found) {
          finish(out, b);
        } else {
          out.len = m;
          error(out, 404, "post not found");
        }
      } else if (path_is(req, "/health")) {
        health(out);
      } else {
        error(out, 404, "not found");
      }
    } else if (method_is(req, "POST")) {
      if (path_is(req, "/posts")) {
        create(req, out);
      } else if (pl >= 7 && std::memcmp(p, "/posts/", 7) == 0) {
        const uint8_t* rest = p + 7;
        const uint32_t rl = pl - 7;
        const bool like_suffix = rl >= 5 && std::memcmp(rest + rl - 5, "/like", 5) == 0;
        if (like_suffix) {
          const uint32_t idl = rl - 5;
          if (std::memchr(rest, '/', idl)) {
            error(out, 404, "not found");
            return;
          }
          like(req, rest, idl, out);
        } else {
          error(out, 404, "not found");
        }
      } else {
        error(out, 404, "not found");
      }
    } else {
      error(out, 404, "not found");
    }
  } catch (const DbError&) {
    out.len = mark;
    error(out, 500, "internal server error");
  }
}

void App::health(ByteBuf& out) {
  std::string err;
  if (db.health(err)) {
    const size_t b = start(out, 200);
    buf_append(out, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - started_)
                          .count();
    json::push_int(out, static_cast<int64_t>(secs));
    buf_put(out, '}');
    finish(out, b);
  } else {
    const size_t b = start(out, 503);
    buf_append(out, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    json::escape_into(out, reinterpret_cast<const uint8_t*>(err.data()), err.size());
    buf_put(out, '}');
    finish(out, b);
  }
}

bool App::auth(const http::Request& req, ByteBuf& out) {
  switch (jwt_.verify(req.has_auth, req.auth, req.auth_len, now_secs(), claims_)) {
    case jwt::AuthError::Ok:
      return true;
    case jwt::AuthError::Missing:
      error(out, 401, "missing bearer token");
      return false;
    case jwt::AuthError::Invalid:
      error(out, 401, "invalid or expired token");
      return false;
    case jwt::AuthError::Payload:
      error(out, 401, "invalid token payload");
      return false;
  }
  return false;
}

namespace {

struct BodyCtx {
  bool found;
  const uint8_t* s;
  uint32_t n;
  bool esc;
};

void body_member(void* ctx, const uint8_t* k, uint32_t klen, const json::Val& v) {
  auto* c = static_cast<BodyCtx*>(ctx);
  if (klen != 4 || std::memcmp(k, "body", 4) != 0) return;
  if (v.kind == json::Kind::Str) {
    c->found = true;
    c->s = v.s;
    c->n = v.n;
    c->esc = v.esc;
  } else {
    c->found = false;
  }
}

}

void App::create(const http::Request& req, ByteBuf& out) {
  if (!auth(req, out)) return;
  const int64_t uid = claims_.user_id;
  const std::vector<uint8_t>& username = claims_.username;

  BodyCtx ctx{false, nullptr, 0, false};
  const int parsed = json::parse_object(req.body, req.body_len, body_member, &ctx);
  if (parsed == json::kParseBad) {
    error(out, 400, "malformed JSON body");
    return;
  }
  if (parsed == json::kParseOther || !ctx.found) {
    error(out, 400, "body is required");
    return;
  }
  const uint8_t* text;
  uint32_t tlen;
  if (ctx.esc) {
    body_.clear();
    json::unescape(ctx.s, ctx.n, body_);
    text = body_.data();
    tlen = static_cast<uint32_t>(body_.size());
  } else {
    text = ctx.s;
    tlen = ctx.n;
  }
  size_t a = 0, b = 0;
  trim_js(text, tlen, a, b);
  const size_t t = b - a;
  if (t == 0) {
    error(out, 400, "body is required");
    return;
  }
  if (t > kMaxBodyChars && char_count(text + a, t) > kMaxBodyChars) {
    error(out, 400, "body must be at most 500 characters");
    return;
  }
  char created[64];
  int created_len = 0;
  const int64_t id = db.insert_post(uid, text + a, t, created, created_len);
  wrote = true;
  const size_t bb = start(out, 201);
  buf_append(out, "{\"post\":{\"id\":");
  json::push_int(out, id);
  buf_append(out, ",\"body\":\"");
  json::escape_into(out, text + a, t);
  buf_append(out, "\",\"created_at\":\"");
  json::escape_into(out, reinterpret_cast<const uint8_t*>(created), static_cast<size_t>(created_len));
  buf_append(out, "\",\"author\":\"");
  json::escape_into(out, username);
  buf_append(out, "\",\"like_count\":0}}");
  finish(out, bb);
}

void App::like(const http::Request& req, const uint8_t* idstr, uint32_t id_len, ByteBuf& out) {
  if (!auth(req, out)) return;
  const int64_t uid = claims_.user_id;
  int64_t id = 0;
  if (!post_id(idstr, id_len, id)) {
    error(out, 400, "invalid post id");
    return;
  }
  if (!db.post_exists(id)) {
    error(out, 404, "post not found");
    return;
  }
  const bool is_new = db.insert_like(uid, id);
  wrote |= is_new;
  const size_t b = start(out, is_new ? 201 : 200);
  if (is_new) {
    buf_append(out, "{\"liked\":true,\"already_liked\":false,\"post_id\":");
  } else {
    buf_append(out, "{\"liked\":true,\"already_liked\":true,\"post_id\":");
  }
  json::push_int(out, id);
  buf_put(out, '}');
  finish(out, b);
}

}
