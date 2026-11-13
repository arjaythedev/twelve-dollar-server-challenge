#include "db.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "json.hpp"
#include "shim.h"

namespace app {

namespace {

void checkpoint_loop(dbx* h, CkState* st) {
  std::unique_lock<std::mutex> lk(st->mu);
  while (!st->stop.load()) {
    st->cv.wait_for(lk, std::chrono::milliseconds(200), [st] { return st->stop.load(); });
    if (st->stop.load()) break;
    lk.unlock();
    if (dbx_wal_frames(h) >= 4096) dbx_checkpoint(h);
    lk.lock();
  }
}


constexpr size_t kFeedLen = 20;

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
  const size_t n = serial_len(c.t);
  uint64_t raw = 0;
  std::memcpy(&raw, c.b, n);
  raw = std::byteswap(raw) >> (64 - 8 * n);
  return static_cast<int64_t>(raw << (64 - 8 * n)) >> (64 - 8 * n);
}

const uint8_t* text_of(const Col& c, uint32_t& n) {
  if (c.t >= 13 && (c.t & 1)) {
    n = static_cast<uint32_t>(c.n);
    return c.b;
  }
  throw DbError{};
}

const uint8_t* payload(dbx* h, int cur, uint32_t& n) {
  unsigned len = 0;
  const unsigned char* p = dbx_record(h, cur, &len);
  if (!p) throw DbError{};
  n = len;
  return p;
}

}  // namespace

bool Db::open(const char* path, std::string& err) {
  char buf[256] = {};
  h_ = dbx_open(path, buf, sizeof buf);
  if (!h_) {
    err = buf;
    return false;
  }
  rows_.reserve(kFeedLen);

  order_.reserve(kFeedLen);
  ck_ = std::make_unique<CkState>();
  dbx* h = h_;
  ck_->th = std::thread([h, st = ck_.get()] { checkpoint_loop(h, st); });
  return true;
}

Db::Db(Db&& other) noexcept
    : h_(other.h_),
      rows_(std::move(other.rows_)),
      text_buf_(other.text_buf_),
      text_len_(other.text_len_),
      order_(std::move(other.order_)),
      ck_(std::move(other.ck_)) {
  other.h_ = nullptr;
}

Db::~Db() {
  if (ck_) {
    ck_->stop.store(true);
    ck_->cv.notify_all();
    if (ck_->th.joinable()) ck_->th.join();
  }
  if (h_) dbx_close(h_);
}

void Db::text_put(const uint8_t* p, size_t n) {
  if (text_len_ + n > text_buf_.size()) throw DbError{};
  std::memcpy(text_buf_.data() + text_len_, p, n);
  text_len_ += n;
}

void Db::begin() {
  if (dbx_begin(h_) != 0) throw DbError{};
}

bool Db::commit() { return dbx_commit(h_) == 0; }

void Db::tick() {}

bool Db::writes_held() const { return false; }

bool Db::health(std::string& err) {
  try {
    begin();
  } catch (const DbError&) {
    err = "cannot begin transaction";
    return false;
  }
  char buf[256] = {};
  if (dbx_health(h_, buf, sizeof buf) != 0) {
    err = buf;
    return false;
  }
  return true;
}

int64_t Db::count_likes(int64_t id) {
  long long n = dbx_likes(h_, id);
  if (n < 0) throw DbError{};
  return n;
}

bool Db::author(int64_t user_id, ByteBuf& out) {
  int r = dbx_seek(h_, DBX_USERS, user_id);
  if (r == 0) return false;
  if (r != 1) throw DbError{};
  uint32_t plen = 0;
  const uint8_t* p = payload(h_, DBX_USERS, plen);
  Col c[2];
  columns(p, plen, c);
  uint32_t n = 0;
  const uint8_t* name = text_of(c[1], n);
  json::escape_into(out, name, n);
  return true;
}

PostLookup Db::post(int64_t id, ByteBuf& out) {
  begin();
  int r = dbx_seek(h_, DBX_POSTS, id);
  if (r == 0) return PostLookup::Missing;
  if (r != 1) throw DbError{};
  uint32_t plen = 0;
  const uint8_t* p = payload(h_, DBX_POSTS, plen);
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
  buf_append(out, created, n);
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
  int r = dbx_seek(h_, DBX_POSTS, id);
  if (r == 1) return true;
  if (r == 0) return false;
  throw DbError{};
}

void Db::feed(ByteBuf& out) {
  begin();
  feed_rows();
  feed_json(out);
}

bool Db::feed_rows() {
  rows_.clear();
  text_len_ = 0;

  int r = dbx_first(h_, DBX_FEED_IDX);
  while (r == 1 && rows_.size() < kFeedLen) {
    uint32_t plen = 0;
    const uint8_t* p = payload(h_, DBX_FEED_IDX, plen);
    Col c[3];
    columns(p, plen, c);
    uint32_t n = 0;
    const uint8_t* created = text_of(c[0], n);
    Row row{};
    row.id = int_of(c[1]);
    row.created_off = static_cast<uint32_t>(text_len_);
    row.created_len = n;
    text_put(created, n);
    rows_.push_back(row);
    r = dbx_next(h_, DBX_FEED_IDX);
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
    if (dbx_seek(h_, DBX_POSTS, rows_[i].id) != 1) return false;
    uint32_t plen = 0;
    const uint8_t* p = payload(h_, DBX_POSTS, plen);
    Col c[3];
    columns(p, plen, c);
    rows_[i].user_id = int_of(c[1]);
    uint32_t n = 0;
    const uint8_t* body = text_of(c[2], n);
    rows_[i].body_off = static_cast<uint32_t>(text_len_);
    rows_[i].body_len = n;
    text_put(body, n);
  }

  if (!rows_.empty()) {
    const size_t n = rows_.size();
    std::array<long long, kFeedLen> uids{};
    std::array<unsigned, kFeedLen> ulens{};
    std::array<unsigned char, kFeedLen * 256> ubuf{};
    std::array<uint32_t, kFeedLen> uorder{};
    for (size_t k = 0; k < n; k++) uorder[k] = static_cast<uint32_t>(k);
    std::sort(uorder.begin(), uorder.begin() + static_cast<ptrdiff_t>(n),
              [&](uint32_t a, uint32_t b) { return rows_[order_[a]].user_id < rows_[order_[b]].user_id; });
    for (size_t k = 0; k < n; k++) uids[k] = rows_[order_[uorder[k]]].user_id;
    if (dbx_users_seek_many(h_, uids.data(), static_cast<int>(n), ubuf.data(), ulens.data()) != 0)
      throw DbError{};
    for (size_t k = 0; k < n; k++) {
      Row& row = rows_[order_[uorder[k]]];
      if (ulens[k] == 0) return false;
      Col c[2];
      columns(ubuf.data() + k * 256, ulens[k], c);
      uint32_t alen = 0;
      const uint8_t* name = text_of(c[1], alen);
      row.author_off = static_cast<uint32_t>(text_len_);
      row.author_len = alen;
      text_put(name, alen);
    }
  }

  if (!rows_.empty()) {
    const size_t n = rows_.size();
    std::array<long long, kFeedLen> ids{};
    std::array<unsigned, kFeedLen> counts{};
    for (size_t k = 0; k < n; k++) ids[k] = rows_[order_[k]].id;
    if (dbx_likes_many(h_, ids.data(), static_cast<int>(n), counts.data()) != 0) throw DbError{};
    for (size_t k = 0; k < n; k++) rows_[order_[k]].likes = counts[k];
  }
  return true;
}

void Db::feed_json(ByteBuf& out) {
  constexpr size_t kRowLiteral =
      sizeof(",{\"id\":,\"body\":\"\",\"created_at\":\"\",\"author\":\"\",\"like_count\":}") - 1;
  constexpr size_t kRow = kRowLiteral + 2 * json::INT_BOUND;
  const size_t bound = (sizeof("{\"posts\":[]}") - 1) + rows_.size() * kRow +
                       json::esc_bound(text_len_);
  const uint8_t* text = text_buf_.data();
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
      c.raw(text + row.created_off, row.created_len);
      c.lit("\",\"author\":\"");
      c.esc(text + row.author_off, row.author_len);
      c.lit("\",\"like_count\":");
      c.int_(row.likes);
      c.byte('}');
    }
    c.lit("]}");
  });
}

int64_t Db::insert_post(int64_t user_id, const uint8_t* body, size_t n, char created[64],
                        int& created_len) {
  begin();
  long long id = 0;
  int cn = 0;
  if (dbx_insert_post(h_, user_id, reinterpret_cast<const char*>(body), static_cast<int>(n), &id,
                      created, &cn) != 0)
    throw DbError{};
  created_len = cn;
  return static_cast<int64_t>(id);
}

bool Db::insert_like(int64_t user_id, int64_t post_id) {
  begin();
  int r = dbx_insert_like(h_, user_id, post_id);
  if (r == 1) return true;
  if (r == 0) return false;
  throw DbError{};
}

}  // namespace app
