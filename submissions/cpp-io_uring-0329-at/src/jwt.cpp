#include "jwt.hpp"

#include <cstring>

#include "json.hpp"

namespace app::jwt {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t h[8], const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = static_cast<uint32_t>(block[4 * i]) << 24 |
           static_cast<uint32_t>(block[4 * i + 1]) << 16 |
           static_cast<uint32_t>(block[4 * i + 2]) << 8 | static_cast<uint32_t>(block[4 * i + 3]);
  for (int i = 16; i < 64; i++) {
    uint32_t x = w[i - 15], y = w[i - 2];
    uint32_t s0 = rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
    uint32_t s1 = rotr(y, 17) ^ rotr(y, 19) ^ (y >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
    uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

struct B64Table {
  uint8_t t[256];
  constexpr B64Table() : t() {
    for (int i = 0; i < 256; i++) t[i] = 0xff;
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; i++) t[static_cast<uint8_t>(a[i])] = static_cast<uint8_t>(i);
  }
};
constexpr B64Table kB64{};

bool b64url(const uint8_t* s, size_t n, ByteBuf& out) {
  if (n % 4 == 1) return false;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    uint8_t a = kB64.t[s[i]], b = kB64.t[s[i + 1]], d = kB64.t[s[i + 2]], e = kB64.t[s[i + 3]];
    if ((a | b | d | e) & 0xc0) return false;
    uint32_t v = static_cast<uint32_t>(a) << 18 | static_cast<uint32_t>(b) << 12 |
                 static_cast<uint32_t>(d) << 6 | e;
    const uint8_t bytes[3] = {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                              static_cast<uint8_t>(v)};
    buf_append(out, bytes, 3);
  }
  size_t r = n - i;
  if (r == 0) return true;
  uint32_t v = 0;
  for (size_t k = 0; k < r; k++) {
    uint8_t x = kB64.t[s[i + k]];
    if (x & 0xc0) return false;
    v |= static_cast<uint32_t>(x) << (18 - 6 * k);
  }

  uint32_t unused = r == 2 ? v & 0xffff : v & 0xff;
  if (r < 2 || unused != 0) return false;
  uint8_t b0 = static_cast<uint8_t>(v >> 16);
  buf_append(out, &b0, 1);
  if (r == 3) {
    uint8_t b1 = static_cast<uint8_t>(v >> 8);
    buf_append(out, &b1, 1);
  }
  return true;
}

struct AlgCtx {
  bool alg_ok;
};

void alg_member(void* ctx, const uint8_t* k, uint32_t klen, const json::Val& v) {
  auto* c = static_cast<AlgCtx*>(ctx);
  if (klen != 3 || std::memcmp(k, "alg", 3) != 0) return;
  if (v.kind != json::Kind::Str) {
    c->alg_ok = false;
    return;
  }
  if (!v.esc) {
    c->alg_ok = v.n == 5 && std::memcmp(v.s, "HS256", 5) == 0;
  } else {
    std::vector<uint8_t> t;
    json::unescape(v.s, v.n, t);
    c->alg_ok = t.size() == 5 && std::memcmp(t.data(), "HS256", 5) == 0;
  }
}

struct PayloadCtx {
  bool has_exp;
  double exp;
  bool has_sub;
  const uint8_t* sub;
  uint32_t sub_len;
  bool sub_esc;
  bool has_name;
  const uint8_t* name;
  uint32_t name_len;
  bool name_esc;
};

void payload_member(void* ctx, const uint8_t* k, uint32_t klen, const json::Val& v) {
  auto* c = static_cast<PayloadCtx*>(ctx);
  if (klen == 3 && std::memcmp(k, "exp", 3) == 0) {
    if (v.kind == json::Kind::Num) {
      c->has_exp = true;
      c->exp = v.num;
    } else {
      c->has_exp = false;
    }
  } else if (klen == 3 && std::memcmp(k, "sub", 3) == 0) {
    if (v.kind == json::Kind::Str) {
      c->has_sub = true;
      c->sub = v.s;
      c->sub_len = v.n;
      c->sub_esc = v.esc;
    } else {
      c->has_sub = false;
    }
  } else if (klen == 8 && std::memcmp(k, "username", 8) == 0) {
    if (v.kind == json::Kind::Str) {
      c->has_name = true;
      c->name = v.s;
      c->name_len = v.n;
      c->name_esc = v.esc;
    } else {
      c->has_name = false;
    }
  }
}

inline bool starts_with(const uint8_t* s, size_t n, const char* w) {
  size_t m = std::strlen(w);
  return n >= m && std::memcmp(s, w, m) == 0;
}

}

Sha256 Sha256::create() {
  Sha256 s{};
  s.h[0] = 0x6a09e667;
  s.h[1] = 0xbb67ae85;
  s.h[2] = 0x3c6ef372;
  s.h[3] = 0xa54ff53a;
  s.h[4] = 0x510e527f;
  s.h[5] = 0x9b05688c;
  s.h[6] = 0x1f83d9ab;
  s.h[7] = 0x5be0cd19;
  s.bytes = 0;
  s.fill = 0;
  return s;
}

void Sha256::update(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  bytes += n;
  if (fill != 0) {
    size_t take = 64 - fill;
    if (take > n) take = n;
    std::memcpy(block + fill, p, take);
    fill += static_cast<uint32_t>(take);
    p += take;
    n -= take;
    if (fill == 64) {
      compress(h, block);
      fill = 0;
    }
  }
  while (n >= 64) {
    compress(h, p);
    p += 64;
    n -= 64;
  }
  if (n != 0) {
    std::memcpy(block, p, n);
    fill = static_cast<uint32_t>(n);
  }
}

void Sha256::final(uint8_t out[32]) {
  uint64_t bits = bytes * 8;
  const uint8_t pad = 0x80;
  update(&pad, 1);
  const uint8_t zero = 0;
  while (fill != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = static_cast<uint8_t>(h[i] >> 24);
    out[4 * i + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[4 * i + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[4 * i + 3] = static_cast<uint8_t>(h[i]);
  }
}

Verifier::Verifier(const uint8_t* secret, size_t n) {
  uint8_t key[64] = {};
  if (n > 64) {
    Sha256 d = Sha256::create();
    d.update(secret, n);
    uint8_t dig[32];
    d.final(dig);
    std::memcpy(key, dig, 32);
  } else {
    std::memcpy(key, secret, n);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = key[i] ^ 0x36;
    opad[i] = key[i] ^ 0x5c;
  }
  inner_ = Sha256::create();
  inner_.update(ipad, 64);
  outer_ = Sha256::create();
  outer_.update(opad, 64);
  buf_reserve(scratch_, 512);
}

AuthError Verifier::verify(bool has_header, const uint8_t* header, uint32_t header_len, double now,
                           Claims& out) {
  if (!has_header || !starts_with(header, header_len, "Bearer ")) return AuthError::Missing;
  const uint8_t* token = header + 7;
  uint32_t n = header_len - 7;
  const uint8_t* d1 = static_cast<const uint8_t*>(std::memchr(token, '.', n));
  if (!d1) return AuthError::Invalid;
  size_t h_len = static_cast<size_t>(d1 - token);
  const uint8_t* rest = d1 + 1;
  size_t rest_n = n - h_len - 1;
  const uint8_t* d2 = static_cast<const uint8_t*>(std::memchr(rest, '.', rest_n));
  if (!d2) return AuthError::Invalid;
  size_t p_len = static_cast<size_t>(d2 - rest);
  const uint8_t* sig = d2 + 1;
  size_t s_len = rest_n - p_len - 1;
  if (std::memchr(sig, '.', s_len)) return AuthError::Invalid;

  scratch_.len = 0;
  if (!b64url(sig, s_len, scratch_)) return AuthError::Invalid;
  if (scratch_.len != 32) return AuthError::Invalid;
  const uint8_t* signed_data = token;
  size_t signed_len = h_len + 1 + p_len;
  uint8_t want[32];
  {
    Sha256 ih = inner_;
    ih.update(signed_data, signed_len);
    uint8_t inner_digest[32];
    ih.final(inner_digest);
    Sha256 oh = outer_;
    oh.update(inner_digest, 32);
    oh.final(want);
  }
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= want[i] ^ scratch_.p[i];
  if (diff != 0) return AuthError::Invalid;

  scratch_.len = 0;
  if (!b64url(token, h_len, scratch_)) return AuthError::Invalid;
  AlgCtx ac{false};
  int hr = json::parse_object(scratch_.p, scratch_.len, alg_member, &ac);
  if (hr != json::kParseObject || !ac.alg_ok) return AuthError::Invalid;

  scratch_.len = 0;
  if (!b64url(rest, p_len, scratch_)) return AuthError::Invalid;
  PayloadCtx pc{};
  int pr = json::parse_object(scratch_.p, scratch_.len, payload_member, &pc);
  if (pr != json::kParseObject) return AuthError::Invalid;
  if (!pc.has_exp || !(now < pc.exp)) return AuthError::Invalid;
  int64_t user_id;
  if (!pc.has_sub) return AuthError::Payload;
  if (!pc.sub_esc) {
    if (!parse_positive(pc.sub, pc.sub_len, user_id)) return AuthError::Payload;
  } else {
    std::vector<uint8_t> t;
    json::unescape(pc.sub, pc.sub_len, t);
    if (!parse_positive(t.data(), t.size(), user_id)) return AuthError::Payload;
  }
  if (!pc.has_name) return AuthError::Payload;
  out.username.clear();
  if (!pc.name_esc) {
    out.username.assign(pc.name, pc.name + pc.name_len);
  } else {
    json::unescape(pc.name, pc.name_len, out.username);
  }
  out.user_id = user_id;
  return AuthError::Ok;
}

bool parse_positive(const uint8_t* s, size_t n, int64_t& out) {
  if (n == 0 || n > 18 || s[0] == '0') return false;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<int64_t>(c - '0');
  }
  out = v;
  return true;
}

}
