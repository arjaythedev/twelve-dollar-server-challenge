#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <bit>

namespace sec {

inline constexpr uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

inline void sha256_init(Sha256Ctx* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

inline void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + K256[i] + w[i];
        uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

inline void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    size_t buffer_idx = static_cast<size_t>(ctx->count & 63);
    ctx->count += len;

    if (buffer_idx > 0) {
        size_t left = 64 - buffer_idx;
        if (len < left) {
            std::memcpy(ctx->buffer + buffer_idx, data, len);
            return;
        }
        std::memcpy(ctx->buffer + buffer_idx, data, left);
        sha256_transform(ctx->state, ctx->buffer);
        data += left;
        len -= left;
    }

    while (len >= 64) {
        sha256_transform(ctx->state, data);
        data += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(ctx->buffer, data, len);
    }
}

inline void sha256_final(Sha256Ctx* ctx, uint8_t hash[32]) {
    uint8_t pad = 0x80;
    size_t buffer_idx = static_cast<size_t>(ctx->count & 63);
    uint64_t total_bits = ctx->count * 8;

    sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    while ((ctx->count & 63) != 56) {
        sha256_update(ctx, &zero, 1);
    }

    uint8_t bits[8];
    for (int i = 0; i < 8; ++i) {
        bits[i] = static_cast<uint8_t>((total_bits >> (56 - i * 8)) & 0xff);
    }
    sha256_update(ctx, bits, 8);

    for (int i = 0; i < 8; ++i) {
        hash[i * 4] = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xff);
        hash[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xff);
        hash[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 8) & 0xff);
        hash[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i] & 0xff);
    }
}

} // namespace sec
