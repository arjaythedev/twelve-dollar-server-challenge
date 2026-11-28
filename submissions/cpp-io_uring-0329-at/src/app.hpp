#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "buf.hpp"
#include "db.hpp"
#include "http.hpp"
#include "jwt.hpp"

namespace app {

void error(ByteBuf& out, int status, const char* msg);

class App {
 public:
  App(Db&& db, const uint8_t* secret, size_t secret_len);

  // 当请求是写、且此刻不能执行时返回 true（WAL 闸门关闭）：没有向 `out` 写任何东西，
  // 调用方把这个请求留到后续 pass。
  bool handle(const http::Request& req, ByteBuf& out, bool writes_ok);

  Db db;

  bool wrote = false;
  /* 上一次 handle() 有没有真的写库（组提交据此决定哪些响应要等提交） */
  bool wrote_last = false;

 private:
  void health(ByteBuf& out);

  bool auth(const http::Request& req, ByteBuf& out);
  void create(const http::Request& req, ByteBuf& out);
  void like(const http::Request& req, const uint8_t* id, uint32_t id_len, ByteBuf& out);

  jwt::Verifier jwt_;
  jwt::Claims claims_;
  std::chrono::steady_clock::time_point started_;
  std::vector<uint8_t> body_;
};

}
