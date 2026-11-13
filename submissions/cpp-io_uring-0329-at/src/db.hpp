#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "buf.hpp"

namespace app {

struct DbError {};

enum class PostLookup { Found, Missing };

class Db {
 public:
  bool open(const char* path, std::string& err);

  bool commit();

  void tick();

  bool writes_held() const;

  bool health(std::string& err);

  PostLookup post(int64_t id, ByteBuf& out);
  bool post_exists(int64_t id);

  void feed(ByteBuf& out);

  int64_t insert_post(int64_t user_id, const uint8_t* body, size_t n, char created[64], int& created_len);

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
  int64_t count_likes_slow(int64_t id);
  bool feed_rows();
  void feed_json(ByteBuf& out);
  void feed_sql(ByteBuf& out);

  bool in_tx_ = false;
  void* cursors_[4];
  std::vector<Row> rows_;
  std::vector<uint8_t> text_;
  std::vector<uint32_t> order_;
};

}
