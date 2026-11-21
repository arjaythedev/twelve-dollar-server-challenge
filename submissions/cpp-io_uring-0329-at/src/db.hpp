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
  void tick();
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
  void text_put(const uint8_t* p, size_t n);
  bool author(int64_t user_id, ByteBuf& out);
  int64_t count_likes(int64_t id);
  int count_likes_one(long long id);
  bool feed_rows();
  void feed_json(ByteBuf& out);

  dbx* h_ = nullptr;
  std::unique_ptr<CkState> ck_;
  std::vector<Row> rows_;
  std::array<uint8_t, 16384> text_buf_{};
  size_t text_len_ = 0;
  std::vector<uint32_t> order_;
};

}  // namespace app
