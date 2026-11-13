#pragma once

#include <atomic>
#include <condition_variable>
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
  std::thread th;
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
  bool author(int64_t user_id, ByteBuf& out);
  int64_t count_likes(int64_t id);
  bool feed_rows();
  void feed_json(ByteBuf& out);

  dbx* h_ = nullptr;
  std::unique_ptr<CkState> ck_;
  std::vector<Row> rows_;
  std::vector<uint8_t> text_;
  std::vector<uint32_t> order_;
};

}  // namespace app
