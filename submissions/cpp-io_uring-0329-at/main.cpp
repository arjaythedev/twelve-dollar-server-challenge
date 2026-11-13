#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <liburing.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <simdjson.h>
#include <sqlite3.h>

#include "picohttpparser.h"
#include "src/sqlite_shim.h"
#include "src/util.hpp"

using app::append_int;
using app::b64url;
using app::ieq;
using app::json_escape;
using app::parse_id;
using app::status_line;
using app::trim_ws;
using app::utf8_len;

static constexpr int kListeners = 8;
static constexpr int kAcceptDepth = 512;
static constexpr size_t kBufSize = 3072;
static constexpr unsigned kBufCount = 8192;
static constexpr unsigned kSqEntries = 4096;
static constexpr unsigned kCqEntries = 65536;
static constexpr int kMaxConns = 400000;
static constexpr size_t kHdrRoom = 160;
static constexpr size_t kArenaInit = 16u << 20;
static constexpr size_t kArenaFlush = 8u << 20;

enum : int { kOpAccept = 0, kOpRecv = 1 };

static constexpr uint64_t ud(int op, int slot, uint32_t gen) {
  return (static_cast<uint64_t>(gen) << 32) | (static_cast<uint64_t>(op & 0xF) << 24) |
         static_cast<uint64_t>(slot & 0xFFFFFF);
}

static constexpr size_t header_into(char* dst, int status, long long body_len) {
  size_t n = 0;
  for (const char* s = status_line(status); *s; s++) dst[n++] = *s;
  static constexpr char kType[] = "Content-Type: application/json\r\n";
  for (size_t i = 0; i < sizeof kType - 1; i++) dst[n++] = kType[i];
  static constexpr char kLen[] = "Content-Length: ";
  for (size_t i = 0; i < sizeof kLen - 1; i++) dst[n++] = kLen[i];
  char tmp[24];
  int k = 0;
  unsigned long long u = static_cast<unsigned long long>(body_len);
  do {
    tmp[k++] = static_cast<char>('0' + u % 10);
    u /= 10;
  } while (u);
  while (k) dst[n++] = tmp[--k];
  dst[n++] = '\r';
  dst[n++] = '\n';
  dst[n++] = '\r';
  dst[n++] = '\n';
  return n;
}

static struct io_uring g_ring;
static io_uring_buf_ring* g_bufring = nullptr;
static char* g_bufbase = nullptr;
static int g_listen[kListeners];
static int g_nlisten = 0, g_lrr = 0, g_accept_inflight = 0;

struct Conn {
  int fd = -1;
  bool active = false;
  bool pending = false;
  uint32_t gen = 0;
  int status = 0;
  uint32_t off = 0;
  uint32_t len = 0;
};
static std::vector<Conn> g_conns;
static std::vector<int> g_free_slots;
static std::vector<int> g_batch;

static std::string g_arena;
static size_t g_body_off = 0;

static sqlite3* g_db = nullptr;
static sqlite3_stmt *g_feed, *g_post, *g_create, *g_like, *g_exists, *g_count;
static bool g_txn = false;
static std::string g_secret = "twelve-dollar-challenge";
static SHA256_CTX g_hi, g_ho;
static bool g_cursors = false;
static std::chrono::steady_clock::time_point g_start;

struct Req {
  std::string_view method, path, body;
  const phr_header* headers = nullptr;
  size_t nheaders = 0;
};

static long long uptime_s() {
  long long s = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - g_start)
                    .count();
  return s < 1 ? 1 : s;
}

static void note_fallback() {
  static bool once = false;
  if (once) return;
  once = true;
  std::fprintf(stderr, "shim: cursor path unavailable, using SQL\n");
}

static constexpr int http_parse(const char* buf, size_t len, phr_header* headers, size_t* nh,
                                Req* req, size_t* consumed) {
  const char *m = nullptr, *p = nullptr;
  size_t ml = 0, pl = 0;
  int minor = 0;
  *nh = 32;
  int r = phr_parse_request(buf, len, &m, &ml, &p, &pl, &minor, headers, nh, 0);
  if (r == -2) return -2;
  if (r < 0) return -1;
  req->method = {m, ml};
  req->path = {p, pl};
  req->headers = headers;
  req->nheaders = *nh;
  *consumed = static_cast<size_t>(r);
  return 0;
}

static constexpr std::string_view hdr(const Req& r, const char* name, size_t nlen) {
  const phr_header* end = r.headers + r.nheaders;
  const phr_header* it = std::find_if(r.headers, end,
                                      [&](const phr_header& h) { return ieq(h.name, h.name_len, name, nlen); });
  if (it == end) return {};
  const char* v = it->value;
  size_t n = it->value_len;
  auto b = std::find_if_not(v, v + n, [](char c) { return c == ' ' || c == '\t'; });
  return {b, static_cast<size_t>(v + n - b)};
}

static constexpr void set_body(std::string& a, std::string_view s) {
  a.resize(g_body_off);
  a.append(s.data(), s.size());
}

static constexpr void finish_response(int slot, int status) {
  Conn& c = g_conns[slot];
  size_t body_len = g_arena.size() - g_body_off;
  char hb[256];
  size_t hl = header_into(hb, status, static_cast<long long>(body_len));
  size_t start = g_body_off - hl;
  memcpy(g_arena.data() + start, hb, hl);
  c.status = status;
  c.off = static_cast<uint32_t>(start);
  c.len = static_cast<uint32_t>(hl + body_len);
  c.pending = true;
}

static constexpr void hmac(const void* msg, size_t n, unsigned char out[32]) {
  unsigned char inner[32];
  SHA256_CTX c = g_hi;
  SHA256_Update(&c, msg, n);
  SHA256_Final(inner, &c);
  c = g_ho;
  SHA256_Update(&c, inner, 32);
  SHA256_Final(out, &c);
}

static constexpr void jwt_init(std::string_view secret) {
  unsigned char k[SHA256_CBLOCK] = {0};
  size_t n = secret.size() > SHA256_CBLOCK ? SHA256_CBLOCK : secret.size();
  memcpy(k, secret.data(), n);
  unsigned char ip[SHA256_CBLOCK], op[SHA256_CBLOCK];
  for (int i = 0; i < SHA256_CBLOCK; i++) {
    ip[i] = k[i] ^ 0x36;
    op[i] = k[i] ^ 0x5c;
  }
  SHA256_Init(&g_hi);
  SHA256_Update(&g_hi, ip, sizeof ip);
  SHA256_Init(&g_ho);
  SHA256_Update(&g_ho, op, sizeof op);
}

static constexpr int authenticate(const Req& r, long long* uid, std::string* name, std::string& out) {
  std::string_view a = hdr(r, "authorization", 13);
  if (a.size() < 7 || a.substr(0, 7) != "Bearer ") {
    set_body(out, "{\"error\":\"missing bearer token\"}");
    return 401;
  }
  std::string_view t = a.substr(7);
  size_t d1 = t.find('.');
  size_t d2 = d1 == std::string_view::npos ? d1 : t.find('.', d1 + 1);
  if (d1 == std::string_view::npos || d2 == std::string_view::npos) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }

  unsigned char mac[32];
  hmac(t.data(), d2, mac);
  char sig[64];
  size_t sn = b64url(t.substr(d2 + 1), sig, sizeof sig);
  if (sn != 32 || CRYPTO_memcmp(sig, mac, 32) != 0) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }

  char hd[512], py[1024];
  size_t hn = b64url(t.substr(0, d1), hd, sizeof hd);
  size_t pn = b64url(t.substr(d1 + 1, d2 - d1 - 1), py, sizeof py);
  if (!hn || !pn) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }

  static simdjson::dom::parser parser;
  simdjson::dom::element h, p;
  if (parser.parse_unpadded(std::string_view(hd, hn)).get(h) != simdjson::SUCCESS) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }
  std::string_view alg;
  if (h["alg"].get_string().get(alg) != simdjson::SUCCESS || alg != "HS256") {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }
  if (parser.parse_unpadded(std::string_view(py, pn)).get(p) != simdjson::SUCCESS) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }
  std::string_view sub, nm;
  if (p["sub"].get_string().get(sub) != simdjson::SUCCESS ||
      p["username"].get_string().get(nm) != simdjson::SUCCESS) {
    set_body(out, "{\"error\":\"invalid token payload\"}");
    return 401;
  }
  long long id = 0;
  if (parse_id(sub, &id) != 1) {
    set_body(out, "{\"error\":\"invalid token payload\"}");
    return 401;
  }
  int64_t exp = 0;
  if (p["exp"].get_int64().get(exp) != simdjson::SUCCESS) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }
  if (static_cast<int64_t>(time(nullptr)) >= exp) {
    set_body(out, "{\"error\":\"invalid or expired token\"}");
    return 401;
  }
  *uid = id;
  name->assign(nm.data(), nm.size());
  return 0;
}

static constexpr void post_json(std::string& b, sqlite3_stmt* st) {
  b += "{\"id\":";
  append_int(b, sqlite3_column_int64(st, 0));
  b += ",\"body\":";
  const unsigned char* x = sqlite3_column_text(st, 1);
  json_escape(b, x ? std::string_view((const char*)x, sqlite3_column_bytes(st, 1)) : std::string_view());
  b += ",\"created_at\":";
  x = sqlite3_column_text(st, 2);
  json_escape(b, x ? std::string_view((const char*)x, sqlite3_column_bytes(st, 2)) : std::string_view());
  b += ",\"author\":";
  x = sqlite3_column_text(st, 3);
  json_escape(b, x ? std::string_view((const char*)x, sqlite3_column_bytes(st, 3)) : std::string_view());
  b += ",\"like_count\":";
  append_int(b, sqlite3_column_int64(st, 4));
  b.push_back('}');
}

static constexpr long long count_likes(long long post_id) {
  sqlite3_reset(g_count);
  sqlite3_bind_int64(g_count, 1, post_id);
  long long n = 0;
  if (sqlite3_step(g_count) == SQLITE_ROW) n = sqlite3_column_int64(g_count, 0);
  sqlite3_reset(g_count);
  return n;
}

static constexpr int handle_feed_sql(std::string& out) {
  out.resize(g_body_off);
  out += "{\"posts\":[";
  int rc;
  bool first = true;
  sqlite3_reset(g_feed);
  while ((rc = sqlite3_step(g_feed)) == SQLITE_ROW) {
    if (!first) out.push_back(',');
    first = false;
    post_json(out, g_feed);
  }
  sqlite3_reset(g_feed);
  if (rc != SQLITE_DONE) {
    set_body(out, "{\"error\":\"internal server error\"}");
    return 500;
  }
  out += "]}";
  return 200;
}

static constexpr int handle_feed(std::string& out) {
  if (g_cursors) {
    shim_feed_entry e[20];
    int n = shim_feed(g_db, e, 20);
    if (n == 20) {
      out.resize(g_body_off);
      out += "{\"posts\":[";
      for (int i = 0; i < n; i++) {
        if (i) out.push_back(',');
        out += "{\"id\":";
        append_int(out, e[i].id);
        out += ",\"body\":";
        json_escape(out, std::string_view(e[i].body, (size_t)e[i].body_len));
        out += ",\"created_at\":";
        json_escape(out, std::string_view(e[i].created_at, (size_t)e[i].created_len));
        out += ",\"author\":";
        json_escape(out, std::string_view(e[i].author, (size_t)e[i].author_len));
        out += ",\"like_count\":";
        append_int(out, e[i].like_count);
        out.push_back('}');
      }
      out += "]}";
      return 200;
    }
    note_fallback();
  }
  return handle_feed_sql(out);
}

static constexpr int handle_post_sql(long long id, std::string& out) {
  sqlite3_reset(g_post);
  sqlite3_bind_int64(g_post, 1, id);
  int rc = sqlite3_step(g_post);
  int status;
  if (rc == SQLITE_ROW) {
    out.resize(g_body_off);
    out += "{\"post\":";
    post_json(out, g_post);
    out.push_back('}');
    status = 200;
  } else if (rc == SQLITE_DONE) {
    set_body(out, "{\"error\":\"post not found\"}");
    status = 404;
  } else {
    set_body(out, "{\"error\":\"internal server error\"}");
    status = 500;
  }
  sqlite3_reset(g_post);
  return status;
}

static constexpr int handle_get_post(std::string_view idstr, std::string& out) {
  long long id = 0;
  int v = parse_id(idstr, &id);
  if (v == 0) {
    set_body(out, "{\"error\":\"invalid post id\"}");
    return 400;
  }
  if (v == 2) {
    set_body(out, "{\"error\":\"post not found\"}");
    return 404;
  }

  if (g_cursors) {
    shim_post p;
    int rc = shim_get_post(g_db, id, &p);
    if (rc == 0) {
      long long lc = count_likes(id);
      out.resize(g_body_off);
      out += "{\"post\":{\"id\":";
      append_int(out, id);
      out += ",\"body\":";
      json_escape(out, std::string_view(p.body, static_cast<size_t>(p.body_len)));
      out += ",\"created_at\":";
      json_escape(out, std::string_view(p.created_at, static_cast<size_t>(p.created_len)));
      out += ",\"author\":";
      json_escape(out, std::string_view(p.author, static_cast<size_t>(p.author_len)));
      out += ",\"like_count\":";
      append_int(out, lc);
      out += "}}";
      return 200;
    }
    if (rc == 1) {
      set_body(out, "{\"error\":\"post not found\"}");
      return 404;
    }
    note_fallback();
  }
  return handle_post_sql(id, out);
}

static constexpr int handle_like(const Req& r, std::string_view idstr, std::string& out) {
  long long uid = 0;
  std::string name;
  int st = authenticate(r, &uid, &name, out);
  if (st) return st;
  long long id = 0;
  int v = parse_id(idstr, &id);
  if (v == 0) {
    set_body(out, "{\"error\":\"invalid post id\"}");
    return 400;
  }
  if (v == 2) {
    set_body(out, "{\"error\":\"post not found\"}");
    return 404;
  }

  sqlite3_reset(g_like);
  sqlite3_bind_int64(g_like, 1, uid);
  sqlite3_bind_int64(g_like, 2, id);
  int rc = sqlite3_step(g_like);
  sqlite3_reset(g_like);
  if (rc != SQLITE_DONE) {
    set_body(out, "{\"error\":\"internal server error\"}");
    return 500;
  }

  bool inserted = sqlite3_changes(g_db) == 1;
  if (!inserted) {
    sqlite3_reset(g_exists);
    sqlite3_bind_int64(g_exists, 1, id);
    rc = sqlite3_step(g_exists);
    sqlite3_reset(g_exists);
    if (rc == SQLITE_DONE) {
      set_body(out, "{\"error\":\"post not found\"}");
      return 404;
    }
    if (rc != SQLITE_ROW) {
      set_body(out, "{\"error\":\"internal server error\"}");
      return 500;
    }
  }
  out.resize(g_body_off);
  out += inserted ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                  : "{\"liked\":true,\"already_liked\":true,\"post_id\":";
  append_int(out, id);
  out.push_back('}');
  return inserted ? 201 : 200;
}

static constexpr int handle_create(const Req& r, std::string& out) {
  long long uid = 0;
  std::string name;
  int st = authenticate(r, &uid, &name, out);
  if (st) return st;

  static simdjson::dom::parser parser;
  simdjson::dom::element doc;
  if (parser.parse_unpadded(r.body).get(doc) != simdjson::SUCCESS) {
    set_body(out, "{\"error\":\"malformed JSON body\"}");
    return 400;
  }
  std::string_view f;
  if (doc["body"].get_string().get(f) != simdjson::SUCCESS) {
    set_body(out, "{\"error\":\"body is required\"}");
    return 400;
  }
  std::string_view text = trim_ws(f);
  if (text.empty()) {
    set_body(out, "{\"error\":\"body is required\"}");
    return 400;
  }
  if (utf8_len(text) > 500) {
    set_body(out, "{\"error\":\"body must be at most 500 characters\"}");
    return 400;
  }

  sqlite3_reset(g_create);
  sqlite3_bind_int64(g_create, 1, uid);
  sqlite3_bind_text(g_create, 2, text.data(), (int)text.size(), SQLITE_STATIC);
  int rc = sqlite3_step(g_create);
  if (rc != SQLITE_ROW) {
    sqlite3_reset(g_create);
    set_body(out, "{\"error\":\"internal server error\"}");
    return 500;
  }
  long long nid = sqlite3_column_int64(g_create, 0);
  const unsigned char* cp = sqlite3_column_text(g_create, 1);
  std::string created((const char*)cp, sqlite3_column_bytes(g_create, 1));
  while ((rc = sqlite3_step(g_create)) == SQLITE_ROW) {
  }
  sqlite3_reset(g_create);
  sqlite3_clear_bindings(g_create);
  if (rc != SQLITE_DONE) {
    set_body(out, "{\"error\":\"internal server error\"}");
    return 500;
  }

  out.resize(g_body_off);
  out += "{\"post\":{\"id\":";
  append_int(out, nid);
  out += ",\"body\":";
  json_escape(out, text);
  out += ",\"created_at\":";
  json_escape(out, created);
  out += ",\"author\":";
  json_escape(out, name);
  out += ",\"like_count\":0}}";
  return 201;
}

static constexpr int route(const Req& r, std::string& out) {
  std::string_view path = r.path;
  size_t q = path.find('?');
  if (q != std::string_view::npos) path = path.substr(0, q);
  bool get = r.method == "GET", post = r.method == "POST";

  if (get && path == "/health") {
    out.resize(g_body_off);
    out += "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":";
    append_int(out, uptime_s());
    out.push_back('}');
    return 200;
  }
  if (get && path == "/feed") return handle_feed(out);
  if (path.size() > 7 && path.substr(0, 7) == "/posts/") {
    std::string_view rest = path.substr(7);
    size_t sl = rest.find('/');
    if (get && sl == std::string_view::npos) return handle_get_post(rest, out);
    if (post && sl != std::string_view::npos && rest.substr(sl) == "/like")
      return handle_like(r, rest.substr(0, sl), out);
  }
  if (post && path == "/posts") return handle_create(r, out);
  set_body(out, "{\"error\":\"not found\"}");
  return 404;
}

static constexpr void arm_accept() {
  while (g_accept_inflight < kAcceptDepth) {
    io_uring_sqe* sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) return;
    io_uring_prep_multishot_accept(sqe, g_listen[g_lrr++ % g_nlisten], nullptr, nullptr, 0);
    sqe->user_data = ud(kOpAccept, 0, 0);
    g_accept_inflight++;
  }
}

static constexpr void close_conn(int slot) {
  Conn& c = g_conns[slot];
  if (!c.active) return;
  if (c.fd >= 0) ::close(c.fd);
  c.fd = -1;
  c.active = false;
  c.pending = false;
  c.off = 0;
  c.len = 0;
  c.gen++;
  g_free_slots.push_back(slot);
}

static constexpr void arm_recv(int slot) {
  Conn& c = g_conns[slot];
  io_uring_sqe* sqe = io_uring_get_sqe(&g_ring);
  if (!sqe) {
    close_conn(slot);
    return;
  }
  io_uring_prep_recv_multishot(sqe, c.fd, nullptr, 0, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = 0;
  sqe->user_data = ud(kOpRecv, slot, c.gen);
}

static constexpr void commit_pass() {
  if (!g_txn) return;
  if (g_cursors) shim_txn_end();
  if (sqlite3_exec(g_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
    g_arena.clear();
    for (int s : g_batch) {
      Conn& c = g_conns[s];
      if (!c.active) continue;
      g_body_off = g_arena.size() + kHdrRoom;
      g_arena.resize(g_body_off);
      g_arena += "{\"error\":\"internal server error\"}";
      finish_response(s, 500);
      c.pending = true;
    }
    g_txn = false;
    return;
  }
  g_txn = false;
}

static void send_batch() {
  for (int s : g_batch) {
    Conn& c = g_conns[s];
    if (!c.active || !c.pending) continue;
    const char* p = g_arena.data() + c.off;
    size_t total = c.len, sent = 0;
    bool ok = true;
    while (sent < total) {
      ssize_t n = ::send(c.fd, p + sent, total - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      ok = false;
      break;
    }
    c.pending = false;
    if (!ok) close_conn(s);
  }
  g_batch.clear();
  g_arena.clear();
  g_body_off = 0;
}

int main() {
  g_start = std::chrono::steady_clock::now();

  if (const char* s = getenv("JWT_SECRET")) g_secret = s;
  const char* host = getenv("HOST");
  int port = getenv("PORT") ? atoi(getenv("PORT")) : 3000;
  const char* dbpath = getenv("SQLITE_PATH");
  const char* shim_env = getenv("SHIM_CURSORS");
  g_cursors = shim_env && shim_env[0] == '1';

  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(1048576);
  setrlimit(RLIMIT_NOFILE, &rl);
  getrlimit(RLIMIT_NOFILE, &rl);
  int max_conns = static_cast<int>(rl.rlim_cur > (rlim_t)kMaxConns ? (rlim_t)kMaxConns : rl.rlim_cur - 128);

  int smc = 0;
  {
    int fd = open("/proc/sys/net/core/somaxconn", O_RDONLY | O_CLOEXEC);
    char b[32] = {0};
    if (fd >= 0) {
      ssize_t n = read(fd, b, sizeof b - 1);
      close(fd);
      if (n > 0) smc = atoi(b);
    }
  }
  std::fprintf(stderr, "nofile=%ld max_conns=%d somaxconn=%d\n", (long)rl.rlim_cur, max_conns, smc);

  {
    char p[64];
    snprintf(p, sizeof p, "/tmp/feedapi-%d.lock", port);
    int fd = open(p, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
      std::fprintf(stderr, "already running\n");
      return 1;
    }
  }

  const char* db_file = dbpath ? dbpath : "seed/feed.db";

  int open_rc = sqlite3_open_v2(db_file, &g_db,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                "unix-excl");
  if (open_rc != SQLITE_OK) {
    sqlite3_close(g_db);
    g_db = nullptr;
    open_rc = sqlite3_open_v2(db_file, &g_db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                              nullptr);
  }
  if (open_rc != SQLITE_OK) {
    std::fprintf(stderr, "db open failed\n");
    return 1;
  }

  const char* pragmas[] = {
      "PRAGMA journal_mode=WAL;",        "PRAGMA synchronous=NORMAL;",  "PRAGMA mmap_size=0;",
      "PRAGMA busy_timeout=5000;",       "PRAGMA temp_store=MEMORY;",   "PRAGMA cache_size=-65536;",
      "PRAGMA wal_autocheckpoint=4096;", "PRAGMA journal_size_limit=67108864;"};
  for (auto p : pragmas) sqlite3_exec(g_db, p, nullptr, nullptr, nullptr);

  auto prep = [](const char* sql) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(g_db, sql, -1, &s, nullptr);
    return s;
  };
  g_feed = prep(
      "SELECT p.id,p.body,p.created_at,u.username,"
      "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u "
      "ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
  g_post = prep(
      "SELECT p.id,p.body,p.created_at,u.username,"
      "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u "
      "ON u.id=p.user_id WHERE p.id=?1");
  g_create = prep("INSERT INTO posts (user_id,body) VALUES (?1,?2) RETURNING id,created_at");
  g_like = prep(
      "INSERT INTO likes (user_id,post_id) SELECT ?1,?2 WHERE EXISTS "
      "(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT (user_id,post_id) DO NOTHING");
  g_exists = prep("SELECT 1 FROM posts WHERE id=?1");
  g_count = prep("SELECT count(*) FROM likes WHERE post_id=?1");

  if (!shim_init(g_db)) {
    std::fprintf(stderr, "shim: disabled\n");
    g_cursors = false;
  }
  jwt_init(g_secret);

  g_arena.reserve(kArenaInit);

  for (int i = 0; i < kListeners; i++) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      perror("socket");
      return 1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    if (host && *host) inet_pton(AF_INET, host, &a.sin_addr);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0) {
      perror("bind");
      return 1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
      perror("listen");
      return 1;
    }
    g_listen[g_nlisten++] = fd;
  }

  int ring_rc = -1;
  {
    struct io_uring_params ip;
    memset(&ip, 0, sizeof ip);
    ip.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    ip.cq_entries = kCqEntries;
    ring_rc = io_uring_queue_init_params(kSqEntries, &g_ring, &ip);
    if (ring_rc < 0) {
      memset(&ip, 0, sizeof ip);
      ip.cq_entries = kCqEntries;
      ring_rc = io_uring_queue_init_params(kSqEntries, &g_ring, &ip);
    }
    if (ring_rc < 0) {
      ring_rc = io_uring_queue_init(kSqEntries, &g_ring, 0);
    }
  }
  if (ring_rc < 0) {
    std::fprintf(stderr, "io_uring init failed: %s\n", std::strerror(-ring_rc));
    return 1;
  }

  int ret = 0;
  g_bufring = io_uring_setup_buf_ring(&g_ring, kBufCount, 0, 0, &ret);
  if (!g_bufring) {
    std::fprintf(stderr, "buf ring failed: %d\n", ret);
    return 1;
  }
  size_t bytes = static_cast<size_t>(kBufCount) * kBufSize;
  g_bufbase = static_cast<char*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (g_bufbase == MAP_FAILED) {
    std::fprintf(stderr, "mmap failed\n");
    return 1;
  }
  for (unsigned i = 0; i < kBufCount; i++)
    io_uring_buf_ring_add(g_bufring, g_bufbase + static_cast<size_t>(i) * kBufSize, kBufSize,
                          static_cast<unsigned short>(i), io_uring_buf_ring_mask(kBufCount), static_cast<int>(i));
  io_uring_buf_ring_advance(g_bufring, static_cast<int>(kBufCount));

  g_conns.resize(static_cast<size_t>(max_conns));
  g_free_slots.reserve(static_cast<size_t>(max_conns));
  g_batch.reserve(65536);
  for (int i = max_conns - 1; i >= 0; i--) g_free_slots.push_back(i);

  arm_accept();
  io_uring_submit(&g_ring);
  std::fprintf(stderr, "listening on %s:%d (%d listeners, sq=%u cq=%u, max %d conns, cursors=%d)\n",
               host ? host : "0.0.0.0", port, g_nlisten, kSqEntries, kCqEntries, max_conns,
               static_cast<int>(g_cursors));

  for (;;) {
    int r = io_uring_submit_and_wait(&g_ring, 1);
    if (r < 0) {
      if (r == -EINTR) continue;
      break;
    }

    struct io_uring_cqe* cqe = nullptr;
    if (io_uring_peek_cqe(&g_ring, &cqe) != 0) continue;

    do {
      int res = cqe->res;
      uint32_t flags = cqe->flags;
      uint64_t u = cqe->user_data;
      int op = static_cast<int>((u >> 24) & 0xF);
      int slot = static_cast<int>(u & 0xFFFFFF);
      uint32_t gen = static_cast<uint32_t>(u >> 32);
      io_uring_cqe_seen(&g_ring, cqe);

      if (op == kOpAccept) {
        if (!(flags & IORING_CQE_F_MORE)) {
          g_accept_inflight--;
          arm_accept();
        }
        if (res >= 0) {
          int fd = res, one = 1;
          setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
          if (g_free_slots.empty()) {
            ::close(fd);
          } else {
            int s2 = g_free_slots.back();
            g_free_slots.pop_back();
            Conn& c = g_conns[s2];
            c.fd = fd;
            c.active = true;
            c.pending = false;
            c.gen++;
            c.off = 0;
            c.len = 0;
            arm_recv(s2);
          }
        }
        continue;
      }

      if (slot < 0 || slot >= static_cast<int>(g_conns.size())) continue;
      Conn& c = g_conns[slot];
      if (!c.active || c.gen != gen) continue;

      if (op == kOpRecv) {
        if (res == -105 || res == -11 || res == -125) {
          arm_recv(slot);
          continue;
        }
        if (res <= 0) {
          close_conn(slot);
          continue;
        }
        if (!(flags & IORING_CQE_F_MORE)) arm_recv(slot);

        unsigned short bid = static_cast<unsigned short>((flags & 0xFFFF0000u) >> IORING_CQE_BUFFER_SHIFT);
        const char* buf = g_bufbase + static_cast<size_t>(bid) * kBufSize;

        phr_header hs[32];
        size_t nh = 0;
        Req req;
        size_t used = 0;
        if (http_parse(buf, static_cast<size_t>(res), hs, &nh, &req, &used) != 0) {
          close_conn(slot);
          continue;
        }
        size_t clen = 0;
        std::string_view cl = hdr(req, "content-length", 14);
        if (!cl.empty()) {
          for (char ch : cl) {
            if (ch < '0' || ch > '9') {
              clen = 0;
              break;
            }
            clen = clen * 10 + static_cast<size_t>(ch - '0');
          }
        }
        if (static_cast<size_t>(res) < used + clen) {
          close_conn(slot);
          continue;
        }
        req.body = std::string_view(buf + used, clen);

        if (!g_txn) {
          g_txn = sqlite3_exec(g_db, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK;
          if (g_txn && g_cursors) shim_txn_begin(g_db);
        }

        g_body_off = g_arena.size() + kHdrRoom;
        g_arena.resize(g_body_off);
        int status = route(req, g_arena);
        finish_response(slot, status);
        g_batch.push_back(slot);

        io_uring_buf_ring_add(g_bufring, (void*)buf, kBufSize, bid, io_uring_buf_ring_mask(kBufCount), 0);
        io_uring_buf_ring_advance(g_bufring, 1);

        if (g_arena.size() >= kArenaFlush) {
          commit_pass();
          send_batch();
        }
        continue;
      }
    } while (io_uring_peek_cqe(&g_ring, &cqe) == 0);

    commit_pass();
    send_batch();
  }
  return 0;
}