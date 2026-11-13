#include "db.hpp"

#include <algorithm>
#include <cstring>

#include "json.hpp"
#include "shim.h"

namespace app {
namespace {

constexpr int kPosts = 0;
constexpr int kUsers = 1;
constexpr int kFeedIdx = 2;
constexpr int kLikesIdx = 3;
constexpr size_t kFeedLen = 20;

constexpr int64_t kRangeSpan = 512;

uint64_t varint(const uint8_t* p, size_t len, size_t& i) {

  if (i < len && p[i] < 0x80) return p[i++];
  uint64_t v = 0;
  for (int k = 0; k < 9; k++) {
    if (i >= len) throw DbError{};
    uint8_t b = p[i++];
    if (k == 8) return (v << 8) | static_cast<uint64_t>(b);
    v = (v << 7) | static_cast<uint64_t>(b & 0x7f);
    if (!(b & 0x80)) return v;
  }
  return v;
}

size_t serial_len(uint64_t t) {
  static constexpr uint8_t kLen[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  if (t < 12) return kLen[t];
  return static_cast<size_t>((t - 12) / 2);
}

struct Col {
  uint64_t t;
  const uint8_t* b;
  size_t n;
};

template <int N>
void columns(const uint8_t* rec, uint32_t len, Col (&out)[N]) {
  size_t i = 0;
  uint64_t hsize = varint(rec, len, i);
  if (hsize > len) throw DbError{};
  size_t body = static_cast<size_t>(hsize);
  for (int c = 0; c < N; c++) {
    if (i >= hsize) throw DbError{};
    uint64_t t = varint(rec, hsize, i);
    size_t n = serial_len(t);
    if (body + n > len) throw DbError{};
    out[c] = Col{t, rec + body, n};
    body += n;
  }
}

int64_t int_of(const Col& c) {
  switch (c.t) {
    case 8:
      return 0;
    case 9:
      return 1;
    default:
      break;
  }
  if (c.t < 1 || c.t > 6) throw DbError{};
  size_t n = serial_len(c.t);
  int64_t v = static_cast<int8_t>(c.b[0]);
  for (size_t k = 1; k < n; k++) v = (v << 8) | c.b[k];
  return v;
}

const uint8_t* text_of(const Col& c, uint32_t& n) {
  if (c.t >= 13 && (c.t & 1)) {
    n = static_cast<uint32_t>(c.n);
    return c.b;
  }
  throw DbError{};
}

const uint8_t* payload(void* cur, uint32_t& n) {
  unsigned int len = 0;
  const unsigned char* p = td_payload(cur, &len);
  if (!p) throw DbError{};
  n = len;
  return p;
}

}

bool Db::open(const char* path, std::string& err) {
  char buf[256] = {};
  if (td_open(path, buf, sizeof buf) != 0) {
    err = buf;
    return false;
  }
  for (int i = 0; i < 4; i++) cursors_[i] = td_cursor(i);
  rows_.reserve(kFeedLen);
  text_.reserve(16384);
  order_.reserve(kFeedLen);
  return true;
}

void Db::begin() {
  in_tx_ = true;
  if (td_begin() != 0) throw DbError{};
}

bool Db::commit() {
  if (!in_tx_) return true;

  in_tx_ = false;
  return td_commit() == 0;
}

void Db::tick() { td_tick(); }

bool Db::writes_held() const { return td_writes_held() != 0; }

bool Db::health(std::string& err) {
  try {
    begin();
  } catch (const DbError&) {
    err = "cannot begin transaction";
    return false;
  }
  char buf[256] = {};
  if (td_health(buf, sizeof buf) != 0) {
    err = buf;
    return false;
  }
  return true;
}

int64_t Db::count_likes(int64_t id) {
  unsigned int n = 0;
  int r = td_count_likes(cursors_[kLikesIdx], id, id, &n);
  if (r == 0) return n;
  if (r == 1) return count_likes_slow(id);
  throw DbError{};
}

int64_t Db::count_likes_slow(int64_t id) {
  int r = td_seek_ge(cursors_[kLikesIdx], id);
  int64_t n = 0;
  while (r == 1) {
    uint32_t plen = 0;
    const uint8_t* p = payload(cursors_[kLikesIdx], plen);
    Col c[1];
    columns(p, plen, c);
    if (int_of(c[0]) != id) break;
    n++;
    r = td_next(cursors_[kLikesIdx]);
  }
  if (r < 0) throw DbError{};
  return n;
}

bool Db::author(int64_t user_id, ByteBuf& out) {
  int r = td_seek(cursors_[kUsers], user_id);
  if (r == 0) return false;
  if (r != 1) throw DbError{};
  uint32_t plen = 0;
  const uint8_t* p = payload(cursors_[kUsers], plen);
  Col c[2];
  columns(p, plen, c);
  uint32_t n = 0;
  const uint8_t* name = text_of(c[1], n);
  json::escape_into(out, name, n);
  return true;
}

PostLookup Db::post(int64_t id, ByteBuf& out) {
  begin();
  int r = td_seek(cursors_[kPosts], id);
  if (r == 0) return PostLookup::Missing;
  if (r != 1) throw DbError{};
  uint32_t plen = 0;
  const uint8_t* p = payload(cursors_[kPosts], plen);
  Col c[4];
  columns(p, plen, c);
  int64_t user_id = int_of(c[1]);
  const size_t mark = out.len;
  buf_append(out, "{\"post\":{\"id\":");
  json::push_int(out, id);
  buf_append(out, ",\"body\":\"");
  uint32_t n = 0;
  const uint8_t* body = text_of(c[2], n);
  json::escape_into(out, body, n);
  buf_append(out, "\",\"created_at\":\"");
  const uint8_t* created = text_of(c[3], n);
  json::escape_into(out, created, n);
  buf_append(out, "\",\"author\":\"");

  if (!author(user_id, out)) {
    out.len = static_cast<uint32_t>(mark);
    return PostLookup::Missing;
  }
  buf_append(out, "\",\"like_count\":");
  json::push_int(out, count_likes(id));
  buf_append(out, "}}");
  return PostLookup::Found;
}

bool Db::post_exists(int64_t id) {
  begin();
  int r = td_seek(cursors_[kPosts], id);
  if (r == 1) return true;
  if (r == 0) return false;
  throw DbError{};
}

void Db::feed(ByteBuf& out) {
  begin();
  if (!feed_rows()) {
    feed_sql(out);
    return;
  }
  feed_json(out);
}

bool Db::feed_rows() {
  void** cur = cursors_;
  rows_.clear();
  text_.clear();

  int r = td_first(cur[kFeedIdx]);
  while (r == 1 && rows_.size() < kFeedLen) {
    uint32_t plen = 0;
    const uint8_t* p = payload(cur[kFeedIdx], plen);
    Col c[3];
    columns(p, plen, c);
    uint32_t n = 0;
    const uint8_t* created = text_of(c[0], n);
    Row row{};
    row.id = int_of(c[1]);
    row.created_off = static_cast<uint32_t>(text_.size());
    row.created_len = n;
    text_.insert(text_.end(), created, created + n);
    rows_.push_back(row);
    r = td_next(cur[kFeedIdx]);
  }
  if (r < 0) throw DbError{};

  order_.clear();

  for (size_t i = rows_.size(); i-- > 0;) order_.push_back(static_cast<uint32_t>(i));
  bool ascending = true;
  for (size_t i = 0; i + 1 < order_.size(); i++) {
    if (!(rows_[order_[i]].id < rows_[order_[i + 1]].id)) {
      ascending = false;
      break;
    }
  }
  if (!ascending) {
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
      return rows_[a].id < rows_[b].id;
    });
  }
  for (uint32_t i : order_) {
    if (td_seek(cur[kPosts], rows_[i].id) != 1) return false;
    uint32_t plen = 0;
    const uint8_t* p = payload(cur[kPosts], plen);
    Col c[3];
    columns(p, plen, c);
    rows_[i].user_id = int_of(c[1]);
    uint32_t n = 0;
    const uint8_t* body = text_of(c[2], n);
    rows_[i].body_off = static_cast<uint32_t>(text_.size());
    rows_[i].body_len = n;
    text_.insert(text_.end(), body, body + n);
  }

  for (Row& row : rows_) {
    int rr = td_seek(cur[kUsers], row.user_id);
    if (rr == 0) return false;
    if (rr != 1) throw DbError{};
    uint32_t plen = 0;
    const uint8_t* p = payload(cur[kUsers], plen);
    Col c[2];
    columns(p, plen, c);
    uint32_t n = 0;
    const uint8_t* name = text_of(c[1], n);
    row.author_off = static_cast<uint32_t>(text_.size());
    row.author_len = n;
    text_.insert(text_.end(), name, name + n);
  }

  if (!order_.empty()) {
    const int64_t lo = rows_[order_.front()].id;
    const int64_t hi = rows_[order_.back()].id;
    if (hi - lo <= kRangeSpan) {
      uint32_t n[kRangeSpan + 1];
      std::memset(n, 0, sizeof(uint32_t) * static_cast<size_t>(hi - lo + 1));
      int rr = td_count_likes(cur[kLikesIdx], lo, hi, n);
      if (rr == 0) {
        for (Row& row : rows_) row.likes = n[row.id - lo];
      } else if (rr == 1) {
        for (Row& row : rows_) row.likes = count_likes_slow(row.id);
      } else {
        throw DbError{};
      }
    } else {
      for (Row& row : rows_) row.likes = count_likes(row.id);
    }
  }
  return true;
}

void Db::feed_json(ByteBuf& out) {
  constexpr size_t kRowLiteral =
      sizeof(",{\"id\":,\"body\":\"\",\"created_at\":\"\",\"author\":\"\",\"like_count\":}") - 1;
  constexpr size_t kRow = kRowLiteral + 2 * json::INT_BOUND;
  const size_t bound = (sizeof("{\"posts\":[]}") - 1) + rows_.size() * kRow +
                       json::esc_bound(text_.size());
  const uint8_t* text = text_.data();
  json::fill(out, bound, [&](json::Cur& c) {
    c.lit("{\"posts\":[");
    for (size_t k = 0; k < rows_.size(); k++) {
      const Row& row = rows_[k];
      if (k > 0) c.byte(',');
      c.lit("{\"id\":");
      c.int_(row.id);
      c.lit(",\"body\":\"");
      c.esc(text + row.body_off, row.body_len);
      c.lit("\",\"created_at\":\"");
      c.esc(text + row.created_off, row.created_len);
      c.lit("\",\"author\":\"");
      c.esc(text + row.author_off, row.author_len);
      c.lit("\",\"like_count\":");
      c.int_(row.likes);
      c.byte('}');
    }
    c.lit("]}");
  });
}

void Db::feed_sql(ByteBuf& out) {
  buf_append(out, "{\"posts\":[");
  bool first = true;
  for (;;) {
    long long id = 0, likes = 0;
    const unsigned char *b = nullptr, *c = nullptr, *a = nullptr;
    int bl = 0, cl = 0, al = 0;
    int r = td_feed_sql_step(&id, &b, &bl, &c, &cl, &a, &al, &likes);
    if (r == 0) break;
    if (r < 0) throw DbError{};
    if (!first) buf_put(out, ',');
    first = false;
    buf_append(out, "{\"id\":");
    json::push_int(out, id);
    buf_append(out, ",\"body\":\"");
    json::escape_into(out, b, static_cast<size_t>(bl));
    buf_append(out, "\",\"created_at\":\"");
    json::escape_into(out, c, static_cast<size_t>(cl));
    buf_append(out, "\",\"author\":\"");
    json::escape_into(out, a, static_cast<size_t>(al));
    buf_append(out, "\",\"like_count\":");
    json::push_int(out, likes);
    buf_put(out, '}');
  }
  buf_append(out, "]}");
}

int64_t Db::insert_post(int64_t user_id, const uint8_t* body, size_t n, char created[64],
                        int& created_len) {
  begin();
  long long id = 0;
  int cn = 0;
  if (td_insert_post(user_id, reinterpret_cast<const char*>(body), static_cast<int>(n), &id,
                     created, &cn) != 0)
    throw DbError{};
  created_len = cn;
  return static_cast<int64_t>(id);
}

bool Db::insert_like(int64_t user_id, int64_t post_id) {
  begin();
  int r = td_insert_like(user_id, post_id);
  if (r == 1) return true;
  if (r == 0) return false;
  throw DbError{};
}

}
