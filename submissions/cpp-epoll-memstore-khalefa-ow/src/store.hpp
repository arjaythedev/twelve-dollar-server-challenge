// The app-specific store, shared by the server and the importer: the in-memory data, the snapshot
// file and the WAL records.
//
// Memory layout, built for the five endpoints:
//   - posts[id] holds the post's JSON already rendered up to `"like_count":`, its like count, and
//     its created_at. GET /posts/:id is one array index and two appends.
//   - order holds every post id sorted by (created_at, id); the feed is its last 20 entries.
//   - likes is an open-addressing hash set of (user, post) keys; it only answers "already liked?".
//
// Disk:
//   - <dir>/snapshot: the whole store at some point, written by a forked child and renamed into place.
//     The server maps it, and the rendered JSON of every snapshot post is served straight from it.
//   - <dir>/wal.<seq>: every write since, as CRC-checked records, appended and fdatasync'ed once per
//     event-loop batch before any response of that batch is sent.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ms {

[[noreturn]] inline void die(const char* what, const char* detail = nullptr) {
  std::fprintf(stderr, "fatal: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
  std::exit(1);
}

inline void append_int(std::string& out, int64_t v) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr - buf);
}

// Index of the first byte in s[i, n) that JSON needs escaped (< 0x20, '"' or '\\'), or n.
inline size_t next_escape(const char* s, size_t i, size_t n) {
#ifdef __SSE2__
  const __m128i quote = _mm_set1_epi8('"'), bslash = _mm_set1_epi8('\\'), ctl = _mm_set1_epi8(0x1F);
  for (; i + 16 <= n; i += 16) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
    __m128i hit = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, bslash)),
                               _mm_cmpeq_epi8(_mm_min_epu8(v, ctl), v));
    if (int mask = _mm_movemask_epi8(hit)) return i + __builtin_ctz(mask);
  }
#endif
  for (; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == '"' || c == '\\') return i;
  }
  return n;
}

// Appends s as a JSON string literal (quotes included). Non-ASCII UTF-8 passes through as is.
inline void append_json_str(std::string& out, const char* s, size_t n) {
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
inline void append_json_str(std::string& out, std::string_view s) { append_json_str(out, s.data(), s.size()); }

// CRC-32C, table-driven: WAL records are a few hundred bytes.
inline uint32_t crc32c(const void* data, size_t n) {
  static const auto kTable = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = ~0u;
  auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; i++) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

inline void fsync_dir(const std::string& dir) {
  int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) {
    fsync(fd);
    close(fd);
  }
}

// ---------------------------------------------------------------------------------------------
// memory

// Append-only storage for rendered posts created after the snapshot. Blocks never move.
class Arena {
 public:
  const char* copy(const char* p, size_t n) {
    if (n > cap_ - used_) {
      size_t size = std::max(n, kBlock);
      blocks_.emplace_back(new char[size]);
      cur_ = blocks_.back().get();
      cap_ = size;
      used_ = 0;
    }
    char* d = cur_ + used_;
    std::memcpy(d, p, n);
    used_ += n;
    return d;
  }

 private:
  static constexpr size_t kBlock = 4 << 20;
  std::vector<std::unique_ptr<char[]>> blocks_;
  char* cur_ = nullptr;
  size_t cap_ = 0, used_ = 0;
};

// The set of (user, post) likes. A key is user << 32 | post in one 8-byte slot (0 = empty), linear
// probing, at most 70% full. User ids that don't fit in 32 bits (none in the seed) go to a std::set.
class LikeSet {
 public:
  void reserve(size_t n) {
    size_t cap = 1024;
    while (cap * 7 / 10 < n) cap *= 2;
    if (cap > slots_.size()) rehash(cap);
  }

  // Returns false if the set already holds (user, post).
  bool insert(int64_t user, uint32_t post) {
    uint64_t key = (static_cast<uint64_t>(user) << 32) | post;
    if (static_cast<uint64_t>(user) > UINT32_MAX || key == 0) return big_.emplace(user, post).second;
    if ((n_ + 1) * 10 > slots_.size() * 7) rehash(std::max<size_t>(1024, slots_.size() * 2));
    size_t i = slot(key);
    while (slots_[i]) {
      if (slots_[i] == key) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = key;
    n_++;
    return true;
  }

  template <class F>
  void for_each(F f) const {
    for (uint64_t k : slots_)
      if (k) f(static_cast<int64_t>(k >> 32), static_cast<uint32_t>(k));
    for (const auto& [u, p] : big_) f(u, p);
  }

  size_t size() const { return n_ + big_.size(); }

 private:
  std::vector<uint64_t> slots_;
  size_t n_ = 0, mask_ = 0;
  int shift_ = 64;
  std::set<std::pair<int64_t, uint32_t>> big_;

  size_t slot(uint64_t key) const { return (key * 0x9E3779B97F4A7C15ull) >> shift_; }

  void rehash(size_t cap) {
    std::vector<uint64_t> old;
    old.swap(slots_);
    slots_.assign(cap, 0);
    mask_ = cap - 1;
    shift_ = 64 - __builtin_ctzll(cap);
    for (uint64_t k : old) {
      if (!k) continue;
      size_t i = slot(k);
      while (slots_[i]) i = (i + 1) & mask_;
      slots_[i] = k;
    }
  }
};

constexpr size_t kTsLen = 24;  // 2025-12-31T23:59:19.409Z

struct Post {
  const char* json = nullptr;  // `{"id":…,"body":…,"created_at":…,"author":…,"like_count":`; null: no such post
  uint32_t json_len = 0;
  uint32_t likes = 0;
  int64_t user_id = 0;
  char created_at[kTsLen];
  bool visible = false;  // the author is in users: the API joins posts to users, so others are never served
};

class Store {
 public:
  std::vector<std::string> usernames;  // by user id
  std::vector<uint8_t> user_exists;
  std::vector<Post> posts;      // by post id; posts[0] is unused
  std::vector<uint32_t> order;  // post ids by (created_at, id), oldest first: the feed reads the back
  LikeSet likes;
  int64_t max_post_id = 0;

  void add_user(int64_t id, std::string_view name) {
    if (id <= 0 || id > INT32_MAX) die("user id out of range");
    if (static_cast<size_t>(id) >= usernames.size()) {
      usernames.resize(id + 1);
      user_exists.resize(id + 1, 0);
    }
    usernames[id].assign(name);
    user_exists[id] = 1;
  }

  bool has_user(int64_t id) const {
    return id > 0 && static_cast<uint64_t>(id) < user_exists.size() && user_exists[id];
  }

  bool has_post(int64_t id) const { return id > 0 && id <= max_post_id && posts[id].json; }

  // Stores a post whose JSON prefix is already rendered (json must outlive the store). The caller
  // places it in `order`.
  Post& put_post(int64_t id, int64_t user_id, const char* ts, bool visible, const char* json, uint32_t json_len) {
    if (id <= 0 || id > INT32_MAX) die("post id out of range");
    if (static_cast<size_t>(id) >= posts.size()) posts.resize(std::max<size_t>(id + 1, posts.size() * 3 / 2));
    Post& p = posts[id];
    p.json = json;
    p.json_len = json_len;
    p.user_id = user_id;
    std::memcpy(p.created_at, ts, kTsLen);
    p.visible = visible;
    max_post_id = std::max(max_post_id, id);
    return p;
  }

  // Renders and stores a new post. With in_order, also places it in `order` (the importer instead
  // sorts once at the end).
  void add_post(int64_t id, int64_t user_id, const char* ts, std::string_view body, bool in_order = true) {
    bool visible = has_user(user_id);
    tmp_.clear();
    tmp_.append("{\"id\":");
    append_int(tmp_, id);
    tmp_.append(",\"body\":");
    append_json_str(tmp_, body);
    tmp_.append(",\"created_at\":\"");
    tmp_.append(ts, kTsLen);
    tmp_.append("\",\"author\":");
    append_json_str(tmp_, visible ? std::string_view(usernames[user_id]) : std::string_view());
    tmp_.append(",\"like_count\":");
    put_post(id, user_id, ts, visible, arena_.copy(tmp_.data(), tmp_.size()), static_cast<uint32_t>(tmp_.size()));
    if (in_order) insert_order(static_cast<uint32_t>(id));
  }

  bool before(uint32_t a, uint32_t b) const {
    int c = std::memcmp(posts[a].created_at, posts[b].created_at, kTsLen);
    return c < 0 || (c == 0 && a < b);
  }

  // New posts are almost always the newest, so this is a push_back; a clock step back inserts.
  void insert_order(uint32_t id) {
    if (order.empty() || before(order.back(), id)) {
      order.push_back(id);
      return;
    }
    auto it = std::upper_bound(order.begin(), order.end(), id, [&](uint32_t a, uint32_t b) { return before(a, b); });
    order.insert(it, id);
  }

  void sort_order() {
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return before(a, b); });
  }

  // Records a like on an existing post. Returns false if this user already liked it.
  bool like(int64_t user_id, int64_t post_id) {
    if (!likes.insert(user_id, static_cast<uint32_t>(post_id))) return false;
    posts[post_id].likes++;
    return true;
  }

 private:
  Arena arena_;
  std::string tmp_;
};

// ---------------------------------------------------------------------------------------------
// snapshot

// Identifies the feed.db the store was imported from, so a fresh copy at the same path re-imports.
struct SrcId {
  uint64_t dev, ino, size;
  int64_t mtime_ns;
};

inline SrcId src_id(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0) die("cannot stat", path);
  return {static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino), static_cast<uint64_t>(st.st_size),
          static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec};
}

inline bool same_src(const SrcId& a, const SrcId& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

constexpr char kSnapMagic[8] = {'M', 'E', 'M', 'S', 'N', 'A', 'P', '1'};
constexpr char kSnapEnd[8] = {'S', 'N', 'A', 'P', '-', 'E', 'N', 'D'};

struct SnapHeader {
  char magic[8];
  uint64_t next_wal;  // WAL files from this sequence number on are newer than the snapshot
  SrcId src;
  uint64_t n_users, n_posts, n_likes;
};

// Layout after the header (native endianness, unaligned):
//   users: n_users × { i64 id, u32 len, name }
//   posts: n_posts × { i64 id, i64 user_id, char created_at[24], u8 visible, u32 len, rendered JSON }, in feed order
//   likes: n_likes × { i64 user_id, i64 post_id }
//   kSnapEnd
inline bool write_snapshot(const Store& s, const std::string& dir, uint64_t next_wal, const SrcId& src) {
  std::string tmp = dir + "/snapshot.tmp", final_path = dir + "/snapshot";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  std::vector<char> buf(1 << 20);
  std::setvbuf(f, buf.data(), _IOFBF, buf.size());
  auto put = [&](const void* p, size_t n) { std::fwrite(p, 1, n, f); };
  auto put64 = [&](int64_t v) { put(&v, 8); };
  auto put32 = [&](uint32_t v) { put(&v, 4); };

  SnapHeader h{};
  std::memcpy(h.magic, kSnapMagic, 8);
  h.next_wal = next_wal;
  h.src = src;
  for (uint8_t e : s.user_exists) h.n_users += e;
  h.n_posts = s.order.size();
  h.n_likes = s.likes.size();
  put(&h, sizeof h);
  for (size_t id = 0; id < s.usernames.size(); id++) {
    if (!s.user_exists[id]) continue;
    put64(static_cast<int64_t>(id));
    put32(static_cast<uint32_t>(s.usernames[id].size()));
    put(s.usernames[id].data(), s.usernames[id].size());
  }
  for (uint32_t id : s.order) {
    const Post& p = s.posts[id];
    put64(id);
    put64(p.user_id);
    put(p.created_at, kTsLen);
    uint8_t vis = p.visible;
    put(&vis, 1);
    put32(p.json_len);
    put(p.json, p.json_len);
  }
  s.likes.for_each([&](int64_t u, uint32_t p) {
    put64(u);
    put64(p);
  });
  put(kSnapEnd, 8);

  bool ok = !std::ferror(f) && std::fflush(f) == 0 && fsync(fileno(f)) == 0;
  ok = (std::fclose(f) == 0) && ok;
  if (!ok || std::rename(tmp.c_str(), final_path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  fsync_dir(dir);
  return true;
}

// Reads <dir>/snapshot's header only. False if there is no valid snapshot.
inline bool read_snapshot_header(const std::string& dir, SnapHeader* h) {
  FILE* f = std::fopen((dir + "/snapshot").c_str(), "rb");
  if (!f) return false;
  bool ok = std::fread(h, sizeof *h, 1, f) == 1 && std::memcmp(h->magic, kSnapMagic, 8) == 0;
  std::fclose(f);
  return ok;
}

// Loads <dir>/snapshot into an empty store. The file stays mapped for the life of the process:
// the rendered JSON of its posts is served from the mapping (a later snapshot is renamed over it,
// which leaves this inode alive).
inline void load_snapshot(Store& s, const std::string& dir, SnapHeader* h) {
  std::string path = dir + "/snapshot";
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) die("cannot open", path.c_str());
  struct stat st;
  fstat(fd, &st);
  size_t n = st.st_size;
  if (n < sizeof(SnapHeader) + 8) die("snapshot is truncated", path.c_str());
  auto* base = static_cast<const char*>(mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0));
  close(fd);
  if (base == MAP_FAILED) die("cannot map", path.c_str());
  madvise(const_cast<char*>(base), n, MADV_WILLNEED);

  std::memcpy(h, base, sizeof *h);
  if (std::memcmp(h->magic, kSnapMagic, 8) != 0 || std::memcmp(base + n - 8, kSnapEnd, 8) != 0)
    die("snapshot is corrupt", path.c_str());
  const char* p = base + sizeof *h;
  const char* end = base + n - 8;
  auto need = [&](size_t k) {
    if (static_cast<size_t>(end - p) < k) die("snapshot is corrupt", path.c_str());
  };
  auto get64 = [&] { need(8); int64_t v; std::memcpy(&v, p, 8); p += 8; return v; };
  auto get32 = [&] { need(4); uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; };

  for (uint64_t i = 0; i < h->n_users; i++) {
    int64_t id = get64();
    uint32_t len = get32();
    need(len);
    s.add_user(id, std::string_view(p, len));
    p += len;
  }
  s.order.reserve(h->n_posts + h->n_posts / 2);
  for (uint64_t i = 0; i < h->n_posts; i++) {
    int64_t id = get64(), user = get64();
    need(kTsLen + 1);
    const char* ts = p;
    bool visible = p[kTsLen];
    p += kTsLen + 1;
    uint32_t len = get32();
    need(len);
    s.put_post(id, user, ts, visible, p, len);
    s.order.push_back(static_cast<uint32_t>(id));  // written in feed order
    p += len;
  }
  s.likes.reserve(h->n_likes * 3 / 2);  // room to grow without a rehash while serving
  for (uint64_t i = 0; i < h->n_likes; i++) {
    int64_t user = get64(), post = get64();
    if (!s.has_post(post)) die("snapshot is corrupt: like of a missing post");
    s.like(user, post);
  }
  if (p != end) die("snapshot is corrupt", path.c_str());
}

// ---------------------------------------------------------------------------------------------
// WAL records: { u32 payload length, u32 crc32c(payload), payload }. A record that is cut short or
// fails its CRC ends the file (a torn write from a crash: it was never acknowledged).

enum : uint8_t { kRecPost = 'P', kRecLike = 'L' };

template <class T>
inline void wal_put(std::string& w, T v) {
  w.append(reinterpret_cast<const char*>(&v), sizeof v);
}

inline size_t wal_begin(std::string& w) {
  size_t at = w.size();
  w.append(8, '\0');
  return at;
}

inline void wal_end(std::string& w, size_t at) {
  uint32_t len = static_cast<uint32_t>(w.size() - at - 8);
  uint32_t crc = crc32c(w.data() + at + 8, len);
  std::memcpy(&w[at], &len, 4);
  std::memcpy(&w[at + 4], &crc, 4);
}

// P: i64 id, i64 user_id, created_at[24], u32 body length, body
inline void wal_post(std::string& w, int64_t id, int64_t user_id, const char* ts, std::string_view body) {
  size_t at = wal_begin(w);
  w.push_back(static_cast<char>(kRecPost));
  wal_put(w, id);
  wal_put(w, user_id);
  w.append(ts, kTsLen);
  wal_put(w, static_cast<uint32_t>(body.size()));
  w.append(body);
  wal_end(w, at);
}

// L: i64 user_id, i64 post_id
inline void wal_like(std::string& w, int64_t user_id, int64_t post_id) {
  size_t at = wal_begin(w);
  w.push_back(static_cast<char>(kRecLike));
  wal_put(w, user_id);
  wal_put(w, post_id);
  wal_end(w, at);
}

// Applies the records of one WAL file. Returns the length of its valid prefix.
inline size_t wal_replay(Store& s, const char* data, size_t n) {
  size_t off = 0;
  while (n - off >= 8) {
    uint32_t len, crc;
    std::memcpy(&len, data + off, 4);
    std::memcpy(&crc, data + off + 4, 4);
    if (len == 0 || len > n - off - 8) break;
    const char* p = data + off + 8;
    if (crc32c(p, len) != crc) break;
    if (p[0] == kRecPost && len >= 1 + 8 + 8 + kTsLen + 4) {
      int64_t id, user;
      uint32_t blen;
      std::memcpy(&id, p + 1, 8);
      std::memcpy(&user, p + 9, 8);
      std::memcpy(&blen, p + 17 + kTsLen, 4);
      if (len != 1 + 8 + 8 + kTsLen + 4 + blen) break;
      s.add_post(id, user, p + 17, std::string_view(p + 21 + kTsLen, blen));
    } else if (p[0] == kRecLike && len == 17) {
      int64_t user, post;
      std::memcpy(&user, p + 1, 8);
      std::memcpy(&post, p + 9, 8);
      if (s.has_post(post)) s.like(user, post);
    } else {
      break;
    }
    off += 8 + len;
  }
  return off;
}

}  // namespace ms
