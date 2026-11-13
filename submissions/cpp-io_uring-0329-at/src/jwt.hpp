#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "buf.hpp"

namespace app::jwt {

struct Sha256 {
  uint32_t h[8];
  uint64_t bytes;
  uint8_t block[64];
  uint32_t fill;

  static Sha256 create();
  void update(const void* p, size_t n);

  void final(uint8_t out[32]);
};

enum class AuthError { Ok, Missing, Invalid, Payload };

struct Claims {
  int64_t user_id = 0;

  std::vector<uint8_t> username;
};

class Verifier {
 public:
  Verifier(const uint8_t* secret, size_t n);

  AuthError verify(bool has_header, const uint8_t* header, uint32_t header_len, double now,
                   Claims& out);

 private:
  Sha256 inner_;
  Sha256 outer_;
  ByteBuf scratch_;
};

bool parse_positive(const uint8_t* s, size_t n, int64_t& out);

}
