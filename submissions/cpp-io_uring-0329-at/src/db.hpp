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

#include "shim.h"

namespace app {

struct CkState {
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> stop{false};
  // Protected by mu; exactly one worker may own the checkpoint connection.
  int pending = -1;
  bool running = false;
  std::thread th_bulk; /* 大块拷贝，nice 19 */
  std::thread th_urgent; /* 新一轮积压较高时使用，固定 nice 10 */
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
  [[nodiscard]] dbx_checkpoint_stats checkpoint_stats() const;
  /* like 计数缓存是否生效（STATS 行）。false 表示退回逐次扫描，功能正确但更慢。 */
  [[nodiscard]] bool like_cache_on() const { return like_counts_ok_; }
  /* 作者名缓存是否生效（STATS 行）。 */
  [[nodiscard]] bool author_cache_on() const { return authors_ok_; }
  /* feed 是否走 posts 表尾（STATS 行）。false 表示退回索引扫描 + 按 id 回表。 */
  [[nodiscard]] bool feed_from_table() const { return feed_table_ok_; }
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
  /* 作者名同理：feed 的每一行都要一条 users 记录，而用户名永远不会变（没有接口改它），
   * 所以启动时把整张 users 表读进内存，之后每个 slot 只是一次数组查表，不再寻址加解记录。 */
  bool rebuild_authors();
  [[nodiscard]] bool author_name(int64_t id, const uint8_t*& p, uint32_t& n) const;
  void text_put(const uint8_t* p, size_t n);
  bool author(int64_t user_id, ByteBuf& out);
  int64_t count_likes(int64_t id);
  int count_likes_one(long long id);
  /* 索引顺序（created_at DESC, id DESC）与 rowid 倒序一致的校验：成立时 feed 直接从 posts
   * 表尾倒着走 20 行，省掉「先扫索引、再按 id 回表」那两趟记录解码。 */
  bool check_feed_order();
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
  /* users.id 连续，author_names_ 存已 JSON 转义、不带引号的名字；偏移和长度直接用于写出。 */
  std::vector<uint32_t> author_off_;
  std::vector<uint32_t> author_len_;
  std::vector<uint8_t> author_names_;
  bool authors_ok_ = false;
  bool authors_off_ = false;
  bool feed_table_ok_ = false;
  bool feed_table_checked_ = false;
  /* 表尾那篇 post 的 created_at：新帖的时间戳只要比它小，索引顺序就不再等于 id 顺序。 */
  char feed_max_created_[40] = {};
  uint32_t feed_max_len_ = 0;
  // 20 posts × (500 code points × 4 UTF-8 bytes + 24-byte timestamp +
  // up to 256 bytes of uncached author record) fit below 64 KiB.
  std::array<uint8_t, 65536> text_buf_{};
  size_t text_len_ = 0;
  std::vector<uint32_t> order_;
};

}  // namespace app
