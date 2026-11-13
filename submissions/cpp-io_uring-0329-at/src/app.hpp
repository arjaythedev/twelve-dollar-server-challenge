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

  void handle(const http::Request& req, ByteBuf& out);

  Db db;

  bool wrote = false;

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
