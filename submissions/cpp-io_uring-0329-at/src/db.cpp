#include "db.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sys/resource.h>
#include <ctime>
#include <thread>
#include <unistd.h>

#include "json.hpp"
#include "shim.h"

namespace app {

namespace {

// Roles are fixed for each worker's lifetime: no unprivileged nice restoration.
// Dispatch samples WAL once per complete dbx_ck_run stage, not during a pass.
enum { kCkBulk = 0, kCkUrgent = 1, kCkTail = 2 };
constexpr long long kCkBehindFrames = 32768;

void checkpoint_loop(dbx* h, CkState* st, int role) {
  if (std::getenv("NO_CKPT")) return;
  const int requested = role == kCkBulk ? 19 : role == kCkUrgent ? 10 : 0;
  // Tail inherits the event-loop priority. Do not try to restore it to zero.
  if (role != kCkTail && setpriority(PRIO_PROCESS, 0, requested) != 0) {
    const int e = errno;
    std::fprintf(stderr, "checkpoint role=%d setpriority(%d) failed: %s (%d)\n",
                 role, requested, std::strerror(e), e);
  }
  errno = 0;
  const int actual = getpriority(PRIO_PROCESS, 0);
  const int e = errno;
  if (e != 0) {
    std::fprintf(stderr, "checkpoint role=%d getpriority failed: %s (%d)\n",
                 role, std::strerror(e), e);
  } else {
    std::fprintf(stderr, "checkpoint role=%d requested_nice=%d actual_nice=%d\n",
                 role, requested, actual);
  }
  std::unique_lock<std::mutex> lk(st->mu);
  while (!st->stop.load()) {
    st->cv.wait(lk, [&] { return st->stop.load() || st->pending == role; });
    if (st->stop.load()) break;
    st->pending = -1;
    st->running = true;
    lk.unlock();
    dbx_ck_run(h, role == kCkTail);
    lk.lock();
    st->running = false;
  }
}


constexpr size_t kFeedLen = 20;
// Dense counters are useful only while their ID span fits a bounded budget.
constexpr size_t kLikeCap = (16u * 1024u * 1024u) / sizeof(unsigned);

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
  ck_->th_bulk = std::thread([h, st = ck_.get()] { checkpoint_loop(h, st, kCkBulk); });
  ck_->th_urgent = std::thread([h, st = ck_.get()] { checkpoint_loop(h, st, kCkUrgent); });
  ck_->th_tail = std::thread([h, st = ck_.get()] { checkpoint_loop(h, st, kCkTail); });
  return true;
}

Db::Db(Db&& other) noexcept
    : h_(other.h_),
      ck_(std::move(other.ck_)),
      rows_(std::move(other.rows_)),
      like_counts_(std::move(other.like_counts_)),
      like_base_(other.like_base_),
      like_counts_ok_(other.like_counts_ok_),
      like_counts_off_(other.like_counts_off_),
      text_buf_(other.text_buf_),
      text_len_(other.text_len_),
      order_(std::move(other.order_)) {
  other.h_ = nullptr;
  other.like_counts_ok_ = false;
  author_off_ = std::move(other.author_off_);
  author_len_ = std::move(other.author_len_);
  author_names_ = std::move(other.author_names_);
  authors_ok_ = other.authors_ok_;
  authors_off_ = other.authors_off_;
  other.authors_ok_ = false;
  other.feed_table_ok_ = false;
  other.feed_table_checked_ = true;
}

Db::~Db() {
  if (ck_) {
    {
      std::lock_guard<std::mutex> lk(ck_->mu);
      ck_->stop.store(true);
    }
    ck_->cv.notify_all();
    if (ck_->th_bulk.joinable()) ck_->th_bulk.join();
    if (ck_->th_urgent.joinable()) ck_->th_urgent.join();
    if (ck_->th_tail.joinable()) ck_->th_tail.join();
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
  if (!like_counts_ok_ && !like_counts_off_ && !rebuild_like_counts()) {
    /* 范围超出上限（或 cell 格式不认识）：永久退回逐次扫描，不再重试。 */
    like_counts_off_ = true;
  }
  if (!feed_table_checked_) {
    feed_table_checked_ = true;
    feed_table_ok_ = check_feed_order();
  }
  if (!authors_ok_ && !authors_off_) {
    /* NO_AUTHORS=1 关掉缓存，用来在本机做 A/B（缓存本身只是把 users 表读进内存）。 */
    if (std::getenv("NO_AUTHORS"))
      authors_off_ = true;
    else if (!rebuild_authors())
      authors_off_ = true;
  }
}

unsigned long long g_commits = 0;
bool Db::commit() {
  g_commits++;
  if (dbx_commit(h_) == 0) return true;
  /* 事务没提交上：内存里的 like 计数已经跟着插入走了，可能和表不一致。标记失效，
   * 下一次 begin() 重建；这一批的响应会整体变成 500，调用方不会读到旧计数。 */
  like_counts_ok_ = false;
  return false;
}

long long Db::wal_frames_total() const { return dbx_wal_frames_total(h_); }


void Db::ck_kick() {
  std::lock_guard<std::mutex> lk(ck_->mu);
  // Keep the shared SQLITE_OPEN_NOMUTEX checkpoint connection single-owner,
  // including the gap between publishing *_DONE and returning from ck_run.
  if (ck_->running || ck_->pending != -1) return;
  if (!dbx_ck_kick(h_)) return;
  if (dbx_ck_wants(h_, true)) {
    ck_->pending = kCkTail;
  } else if (dbx_ck_wants(h_, false)) {
    ck_->pending = dbx_wal_frames_now(h_) >= kCkBehindFrames ? kCkUrgent : kCkBulk;
  }
  if (ck_->pending != -1) ck_->cv.notify_all();
}

bool Db::writes_held() const { return dbx_writes_held(h_) != 0; }

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
  if (like_counts_ok_) return static_cast<int64_t>(likes_of(id));
  long long n = dbx_likes(h_, id);
  if (n < 0) throw DbError{};
  return n;
}

bool Db::author(int64_t user_id, ByteBuf& out) {
  if (authors_ok_) {
    const uint8_t* name = nullptr;
    uint32_t n = 0;
    if (!author_name(user_id, name, n)) return false;
    buf_append(out, name, n);  // cache contains JSON string contents, without quotes
    return true;
  }
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
  /* feed_rows() 只有在数据不符合 B-tree 计划的假设时才返回 false（例如 posts 指向的 user
   * 行不存在：参考 SQL 的 JOIN 会把这一行丢掉）。这里绝不能拿半填的 rows_ 去拼 JSON——
   * 那会输出 body/author 为空、like_count 为 0 的坏数据；按 SPEC「其它情况 500」处理。 */
  if (!feed_rows()) throw DbError{};
  feed_json(out);
}

bool Db::rebuild_authors() {
  author_off_.clear();
  author_len_.clear();
  author_names_.clear();
  author_names_.reserve(1u << 21);
  std::vector<uint8_t> escaped;
  int r = dbx_first(h_, DBX_USERS);
  if (r < 0) return false;
  while (r == 1) {
    const long long id = dbx_key(h_, DBX_USERS);
    if (id < 0 || id > (1 << 22)) return false;
    uint32_t plen = 0;
    const uint8_t* p = payload(h_, DBX_USERS, plen);
    Col c[2];
    columns(p, plen, c);
    uint32_t n = 0;
    const uint8_t* name = text_of(c[1], n);
    const size_t idx = static_cast<size_t>(id);
    if (author_off_.size() <= idx) {
      author_off_.resize(idx + 1, 0);
      author_len_.resize(idx + 1, 0);
    }
    escaped.resize(json::esc_bound(n) + 1);
    json::Cur cur{escaped.data(), escaped.data() + escaped.size()};
    cur.esc(name, n);
    const size_t len = static_cast<size_t>(cur.p - escaped.data());
    author_off_[idx] = static_cast<uint32_t>(author_names_.size());
    author_len_[idx] = static_cast<uint32_t>(len);
    author_names_.insert(author_names_.end(), escaped.data(), escaped.data() + len);
    r = dbx_next(h_, DBX_USERS);
  }
  if (r < 0) return false;
  authors_ok_ = true;
  return true;
}

bool Db::author_name(int64_t id, const uint8_t*& p, uint32_t& n) const {
  if (id < 0 || static_cast<size_t>(id) >= author_off_.size()) return false;
  n = author_len_[static_cast<size_t>(id)];
  if (n == 0) return false;
  p = author_names_.data() + author_off_[static_cast<size_t>(id)];
  return true;
}

bool Db::check_feed_order() {
  /* NO_TABLE=1：强制走「索引取 id + 回表」的旧路径，用来在本机做 A/B。 */
  if (std::getenv("NO_TABLE")) return false;
  long long ids[kFeedLen] = {};
  size_t n = 0;
  int r = dbx_first(h_, DBX_FEED_IDX);
  while (r == 1 && n < kFeedLen) {
    uint32_t plen = 0;
    const uint8_t* p = payload(h_, DBX_FEED_IDX, plen);
    Col c[3];
    columns(p, plen, c);
    ids[n] = int_of(c[1]);
    if (n == 0) {
      uint32_t clen = 0;
      const uint8_t* created = text_of(c[0], clen);
      if (clen >= sizeof feed_max_created_) return false;
      std::memcpy(feed_max_created_, created, clen);
      feed_max_len_ = clen;
    }
    n++;
    r = dbx_next(h_, DBX_FEED_IDX);
  }
  if (r < 0 || n == 0) return false;
  int rt = dbx_last(h_, DBX_POSTS);
  size_t k = 0;
  while (rt == 1 && k < n) {
    if (dbx_key(h_, DBX_POSTS) != ids[k]) return false;
    k++;
    rt = dbx_prev(h_, DBX_POSTS);
  }
  return k == n;
}

bool Db::feed_rows() {
  rows_.clear();
  text_len_ = 0;

  if (feed_table_ok_) {
    int rt = dbx_last(h_, DBX_POSTS);
    while (rt == 1 && rows_.size() < kFeedLen) {
      uint32_t plen = 0;
      const uint8_t* p = payload(h_, DBX_POSTS, plen);
      Col c[4];
      columns(p, plen, c);
      Row row{};
      row.id = dbx_key(h_, DBX_POSTS);
      row.user_id = int_of(c[1]);
      uint32_t n = 0;
      const uint8_t* body = text_of(c[2], n);
      row.body_off = static_cast<uint32_t>(text_len_);
      row.body_len = n;
      text_put(body, n);
      const uint8_t* created = text_of(c[3], n);
      row.created_off = static_cast<uint32_t>(text_len_);
      row.created_len = n;
      text_put(created, n);
      rows_.push_back(row);
      rt = dbx_prev(h_, DBX_POSTS);
    }
    if (rt < 0) throw DbError{};
  } else {
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
  }

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

  if (!feed_table_ok_ && !rows_.empty()) {
    const long long lo = rows_[order_[0]].id;
    const long long hi = rows_[order_.back()].id;
    const long long span = hi - lo;
    /* 20 个 post 的 id 在持续写入下跨度很容易超过 64；超过就退回 20 次独立 seek，每次都是一次
     * 从根到叶的下降（~285 ns）。rust-iouring-kensac #264 的区间阈值是 512，这里对齐：一次
     * seek 加顺序走 span 个 cell 仍然比 20 次下降便宜。 */
    if (span >= 0 && span < 512) {
      std::array<int8_t, 512> slot{};
      slot.fill(-1);
      for (size_t i = 0; i < order_.size(); i++)
        slot[rows_[order_[i]].id - lo] = static_cast<int8_t>(i);
      int rc = dbx_seek_ge_table(h_, DBX_POSTS, lo);
      size_t found = 0;
      while (rc == 1) {
        long long id = dbx_key(h_, DBX_POSTS);
        if (id > hi) break;
        int8_t idx = slot[id - lo];
        if (idx >= 0) {
          uint32_t i = order_[static_cast<size_t>(idx)];
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
          if (++found == order_.size()) break;
        }
        rc = dbx_next(h_, DBX_POSTS);
      }
      if (rc < 0 || found != order_.size()) return false;
    } else {
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
    }
  }

  if (!rows_.empty()) {
    const size_t n = rows_.size();
    if (authors_ok_) {
      for (size_t k = 0; k < n; k++) {
        Row& row = rows_[order_[k]];
        const uint8_t* name = nullptr;
        uint32_t alen = 0;
        if (!author_name(row.user_id, name, alen)) return false;
        // Cache storage is immutable while serializing the response: no staging copy.
        row.author_off = author_off_[static_cast<size_t>(row.user_id)];
        row.author_len = alen;
      }
    } else {
      std::array<long long, kFeedLen> uids{};
      std::array<unsigned, kFeedLen> ulens{};
      std::array<unsigned char, kFeedLen * 256> ubuf{};
      std::array<uint32_t, kFeedLen> uorder{};
      for (size_t k = 0; k < n; k++) uorder[k] = static_cast<uint32_t>(k);
      std::sort(uorder.begin(), uorder.begin() + static_cast<ptrdiff_t>(n),
                [&](uint32_t a, uint32_t b) {
                  return rows_[order_[a]].user_id < rows_[order_[b]].user_id;
                });
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
  }

  if (!rows_.empty() && like_counts_ok_) {
    for (size_t k = 0; k < rows_.size(); k++)
      rows_[order_[k]].likes = static_cast<int64_t>(likes_of(rows_[order_[k]].id));
  } else if (!rows_.empty()) {
    const size_t n = rows_.size();
    std::array<long long, kFeedLen> ids{};
    std::array<unsigned, kFeedLen> counts{};
    for (size_t k = 0; k < n; k++) ids[k] = rows_[order_[k]].id;
    int lr = dbx_likes_many(h_, ids.data(), static_cast<int>(n), counts.data());
    if (lr < 0) throw DbError{};
    if (lr > 0) {
      // 页级扫描解不了的 cell（记录过大或格式特殊）：退回逐个 post 一次 seek。
      for (size_t k = 0; k < n; k++) {
        int c = count_likes_one(ids[k]);
        if (c < 0) throw DbError{};
        counts[k] = static_cast<unsigned>(c);
      }
    }
    for (size_t k = 0; k < n; k++) rows_[order_[k]].likes = counts[k];
  }
  return true;
}

/* 启动时扫一次全表。行数不超过 kLikeCap 组时建立缓存，否则放弃（feed 退回逐次扫）。
 * 之后每次成功的 like 插入让对应计数 +1，所以只有在 commit 失败回滚或数据被外部
 * 改动时才需要重建。 */
bool Db::rebuild_like_counts() {
  long long lo = 0, hi = 0;
  int r = dbx_likes_range(h_, &lo, &hi);
  if (r < 0) return false;
  if (r == 0) {
    like_counts_.clear();
    like_base_ = 0;
    like_counts_ok_ = true;
    return true;
  }
  // Unsigned subtraction avoids signed overflow even for extreme rowids.
  if (hi < lo || static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) >= kLikeCap) {
    std::vector<unsigned>().swap(like_counts_);
    return false;
  }
  like_base_ = lo;
  like_counts_.assign(static_cast<size_t>(static_cast<uint64_t>(hi) -
                                         static_cast<uint64_t>(lo)) + 1, 0);
  if (dbx_likes_fill(h_, lo, like_counts_.data(), like_counts_.size()) != 0) {
    like_counts_.clear();
    return false;
  }
  like_counts_ok_ = true;
  return true;
}

unsigned Db::likes_of(int64_t id) const {
  if (!like_counts_ok_ || id < like_base_) return 0;
  const uint64_t idx = static_cast<uint64_t>(id) - static_cast<uint64_t>(like_base_);
  return idx < like_counts_.size() ? like_counts_[static_cast<size_t>(idx)] : 0;
}

int Db::count_likes_one(long long id) {
  int r = dbx_seek_ge(h_, DBX_LIKES_IDX, id);
  if (r < 0) return -1;
  int cnt = 0;
  while (r == 1) {
    unsigned len = 0;
    const uint8_t* p = dbx_record(h_, DBX_LIKES_IDX, &len);
    if (!p) return -1;
    Col cols[2];
    columns(p, len, cols);
    if (int_of(cols[0]) != id) break;
    cnt++;
    r = dbx_next(h_, DBX_LIKES_IDX);
  }
  return r < 0 ? -1 : cnt;
}

void Db::feed_json(ByteBuf& out) {
  constexpr size_t kRowLiteral =
      sizeof(",{\"id\":,\"body\":\"\",\"created_at\":\"\",\"author\":\"\",\"like_count\":}") - 1;
  constexpr size_t kRow = kRowLiteral + 2 * json::INT_BOUND;
  size_t author_bytes = 0;
  if (authors_ok_)
    for (const Row& row : rows_) author_bytes += row.author_len;
  const size_t bound = (sizeof("{\"posts\":[]}") - 1) + rows_.size() * kRow +
                       json::esc_bound(text_len_) + author_bytes;
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
      if (authors_ok_)
        c.raw(author_names_.data() + row.author_off, row.author_len);
      else
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
  if (feed_table_ok_) {
    const std::string_view now(created, static_cast<size_t>(cn));
    if (now < std::string_view(feed_max_created_, feed_max_len_)) {
      /* 时钟倒退：新帖的 created_at 比现有最大的还小，索引顺序不再等于 id 顺序。 */
      feed_table_ok_ = false;
    } else {
      std::memcpy(feed_max_created_, created, static_cast<size_t>(cn));
      feed_max_len_ = static_cast<uint32_t>(cn);
    }
  }
  return static_cast<int64_t>(id);
}

bool Db::insert_like(int64_t user_id, int64_t post_id) {
  begin();
  int r = dbx_insert_like(h_, user_id, post_id);
  if (r == 1) {
    // A sparse initial likes table may start above an existing post id.
    // Rebuild on next begin() instead of silently losing that new like.
    if (like_counts_ok_ && post_id < like_base_) like_counts_ok_ = false;
    if (like_counts_ok_ && post_id >= like_base_) {
      const uint64_t distance = static_cast<uint64_t>(post_id) - static_cast<uint64_t>(like_base_);
      if (distance >= kLikeCap) {
        like_counts_ok_ = false;
        like_counts_off_ = true;
        std::vector<unsigned>().swap(like_counts_);
      } else {
        const size_t idx = static_cast<size_t>(distance);
        if (idx >= like_counts_.size()) {
          const size_t size = std::min(idx + 64, kLikeCap);
          // Do not let vector::resize's geometric growth exceed the budget.
          if (size > like_counts_.capacity()) like_counts_.reserve(size);
          like_counts_.resize(size, 0);
        }
        like_counts_[idx]++;
      }
    }
    return true;
  }
  if (r == 0) return false;
  throw DbError{};
}

}  // namespace app
