#pragma once

#include <atomic>
#include <condition_variable>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "buf.hpp"

struct dbx;

namespace app {

struct CkState {
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> stop{false};
  std::thread th_bulk; /* 大块拷贝，nice 19 */
  std::thread th_tail; /* 尾段拷贝，普通优先级 */
};

struct DbError {};

enum class PostLookup { Found, Missing };

class Db {
 public:
  Db() = default;
  ~Db();
  Db(Db&& other) noexcept;
  Db& operator=(Db&&) = delete;
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  bool open(const char* path, std::string& err);

  bool commit();
  [[nodiscard]] long long wal_frames_total() const;
  /* like 计数缓存是否生效（STATS 行）。false 表示退回逐次扫描，功能正确但更慢。 */
  [[nodiscard]] bool like_cache_on() const { return like_counts_ok_; }
  /* 每个 pass 调一次：推进检查点状态机，需要线程干活时唤醒它。 */
  void ck_kick();
  bool writes_held() const;
  bool health(std::string& err);

  PostLookup post(int64_t id, ByteBuf& out);
  bool post_exists(int64_t id);
  void feed(ByteBuf& out);

  int64_t insert_post(int64_t user_id, const uint8_t* body, size_t n, char created[64],
                      int& created_len);
  bool insert_like(int64_t user_id, int64_t post_id);

 private:
  struct Row {
    int64_t id;
    int64_t user_id;
    uint32_t body_off, body_len;
    uint32_t created_off, created_len;
    uint32_t author_off, author_len;
    int64_t likes;
  };

  void begin();
  /* like 计数在进程内维护：启动时（或 commit 失败后）全表算一次直方图，之后
   * 每成功插入一行 like 就 +1。feed 因此不再逐次扫索引——like 表越大，逐次扫
   * 越慢（20 个 post 的 id 跨度里所有的 like 行都要解码一遍）。 */
  bool rebuild_like_counts();
  [[nodiscard]] unsigned likes_of(int64_t id) const;
  void text_put(const uint8_t* p, size_t n);
  bool author(int64_t user_id, ByteBuf& out);
  int64_t count_likes(int64_t id);
  int count_likes_one(long long id);
  bool feed_rows();
  void feed_json(ByteBuf& out);

  dbx* h_ = nullptr;
  std::unique_ptr<CkState> ck_;
  std::vector<Row> rows_;
  /* post_id 从 like_base_ 开始连续，所以计数可以直接按下标存。 */
  std::vector<unsigned> like_counts_;
  int64_t like_base_ = 0;
  bool like_counts_ok_ = false;
  bool like_counts_off_ = false; /* 直方图超上限：永久退回逐次扫描 */
  std::array<uint8_t, 16384> text_buf_{};
  size_t text_len_ = 0;
  std::vector<uint32_t> order_;
};

}  // namespace app
