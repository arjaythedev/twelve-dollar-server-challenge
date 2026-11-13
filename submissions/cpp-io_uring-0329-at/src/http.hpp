#pragma once

#include <cstddef>
#include <cstdint>

namespace app::http {

struct Request {
  const uint8_t* method;
  uint32_t method_len;
  const uint8_t* path;
  uint32_t path_len;
  const uint8_t* auth;
  uint32_t auth_len;
  bool has_auth;
  const uint8_t* body;
  uint32_t body_len;
};

enum class Parsed { Done, Partial, Bad };

Parsed parse(const uint8_t* buf, size_t len, Request& req, size_t& used);

}
