#pragma once

#include "sha256.hpp"
#include <string_view>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <charconv>

namespace sec {

// Base64URL decode table (-1 for invalid)
inline constexpr int8_t B64_TABLE[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1, // '-' is 62
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1, // '0'-'9'
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 'A'-'O'
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63, // 'P'-'Z', '_' is 63
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 'a'-'o'
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, // 'p'-'z'
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

inline bool base64url_decode(std::string_view in, uint8_t* out, size_t max_out, size_t& out_len) {
    out_len = 0;
    uint32_t val = 0;
    int bits = 0;

    for (unsigned char c : in) {
        if (c == '=') break;
        int8_t d = B64_TABLE[c];
        if (d < 0) return false;
        val = (val << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= max_out) return false;
            out[out_len++] = static_cast<uint8_t>((val >> bits) & 0xff);
        }
    }
    return true;
}

enum class AuthError {
    None,
    MissingBearer,
    InvalidOrExpired,
    InvalidPayload
};

struct AuthResult {
    AuthError error{AuthError::None};
    int64_t user_id{0};
    char username_buf[64]{};
    size_t username_len{0};

    std::string_view username() const {
        return std::string_view(username_buf, username_len);
    }
};

class JwtVerifier {
public:
    void init(std::string_view secret) {
        uint8_t k_pad[64];
        std::memset(k_pad, 0, 64);
        if (secret.size() <= 64) {
            std::memcpy(k_pad, secret.data(), secret.size());
        } else {
            Sha256Ctx ctx;
            sha256_init(&ctx);
            sha256_update(&ctx, reinterpret_cast<const uint8_t*>(secret.data()), secret.size());
            sha256_final(&ctx, k_pad);
        }

        uint8_t ipad[64];
        uint8_t opad[64];
        for (int i = 0; i < 64; ++i) {
            ipad[i] = k_pad[i] ^ 0x36;
            opad[i] = k_pad[i] ^ 0x5c;
        }

        Sha256Ctx ctx_in;
        sha256_init(&ctx_in);
        sha256_transform(ctx_in.state, ipad);
        std::memcpy(inner_state_, ctx_in.state, sizeof(inner_state_));

        Sha256Ctx ctx_out;
        sha256_init(&ctx_out);
        sha256_transform(ctx_out.state, opad);
        std::memcpy(outer_state_, ctx_out.state, sizeof(outer_state_));
    }

    AuthResult verify(std::string_view auth_header, int64_t now_s) const {
        AuthResult res;
        if (!auth_header.starts_with("Bearer ")) {
            res.error = AuthError::MissingBearer;
            return res;
        }

        std::string_view token = auth_header.substr(7);
        // Find 2 dots: header.payload.signature
        size_t dot1 = token.find('.');
        if (dot1 == std::string_view::npos) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }
        size_t dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string_view::npos) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }
        // Ensure no 4th part
        if (token.find('.', dot2 + 1) != std::string_view::npos) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }

        std::string_view b64_header = token.substr(0, dot1);
        std::string_view b64_payload = token.substr(dot1 + 1, dot2 - dot1 - 1);
        std::string_view b64_sig = token.substr(dot2 + 1);

        if (b64_sig.empty()) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }

        // Decode signature (must be exactly 32 bytes for SHA256)
        uint8_t expected_sig[48];
        size_t expected_sig_len = 0;
        if (!base64url_decode(b64_sig, expected_sig, sizeof(expected_sig), expected_sig_len) ||
            expected_sig_len != 32) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }

        // Compute HMAC on token.substr(0, dot2)
        std::string_view msg = token.substr(0, dot2);
        Sha256Ctx ctx_in;
        std::memcpy(ctx_in.state, inner_state_, sizeof(inner_state_));
        ctx_in.count = 64; // already processed 64-byte ipad

        sha256_update(&ctx_in, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        uint8_t inner_hash[32];
        sha256_final(&ctx_in, inner_hash);

        Sha256Ctx ctx_out;
        std::memcpy(ctx_out.state, outer_state_, sizeof(outer_state_));
        ctx_out.count = 64; // already processed 64-byte opad

        sha256_update(&ctx_out, inner_hash, 32);
        uint8_t computed_sig[32];
        sha256_final(&ctx_out, computed_sig);

        // Constant time compare
        uint8_t diff = 0;
        for (int i = 0; i < 32; ++i) {
            diff |= (computed_sig[i] ^ expected_sig[i]);
        }
        if (diff != 0) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }

        // Decode Header: check alg == HS256
        uint8_t header_buf[256];
        size_t header_len = 0;
        if (!base64url_decode(b64_header, header_buf, sizeof(header_buf), header_len)) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }
        std::string_view hdr(reinterpret_cast<char*>(header_buf), header_len);
        if (!hdr.contains("\"alg\":\"HS256\"") && !hdr.contains("\"alg\": \"HS256\"")) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }

        // Decode Payload
        uint8_t payload_buf[512];
        size_t payload_len = 0;
        if (!base64url_decode(b64_payload, payload_buf, sizeof(payload_buf), payload_len)) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }
        std::string_view pld(reinterpret_cast<char*>(payload_buf), payload_len);

        // Parse "exp": <int>
        size_t exp_pos = pld.find("\"exp\":");
        if (exp_pos == std::string_view::npos) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }
        size_t exp_val_start = pld.find_first_not_of(" \t", exp_pos + 6);
        if (exp_val_start == std::string_view::npos) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }
        int64_t exp_val = 0;
        auto [ptr1, ec1] = std::from_chars(pld.data() + exp_val_start, pld.data() + pld.size(), exp_val);
        if (ec1 != std::errc{} || exp_val <= now_s) {
            res.error = AuthError::InvalidOrExpired;
            return res;
        }

        // Parse "sub":"<user id>" -> MUST be a quoted positive integer string
        size_t sub_pos = pld.find("\"sub\":");
        if (sub_pos == std::string_view::npos) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        size_t sub_val_start = pld.find_first_not_of(" \t", sub_pos + 6);
        if (sub_val_start == std::string_view::npos || pld[sub_val_start] != '"') {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        size_t sub_val_end = pld.find('"', sub_val_start + 1);
        if (sub_val_end == std::string_view::npos) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        std::string_view sub_str = pld.substr(sub_val_start + 1, sub_val_end - sub_val_start - 1);
        if (sub_str.empty()) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        int64_t user_id = 0;
        auto [ptr2, ec2] = std::from_chars(sub_str.data(), sub_str.data() + sub_str.size(), user_id);
        if (ec2 != std::errc{} || ptr2 != sub_str.data() + sub_str.size() || user_id <= 0) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        res.user_id = user_id;

        // Parse "username":"<name>" -> MUST be a quoted string
        size_t uname_pos = pld.find("\"username\":");
        if (uname_pos == std::string_view::npos) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        size_t uname_val_start = pld.find_first_not_of(" \t", uname_pos + 11);
        if (uname_val_start == std::string_view::npos || pld[uname_val_start] != '"') {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        size_t uname_val_end = pld.find('"', uname_val_start + 1);
        if (uname_val_end == std::string_view::npos) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        std::string_view uname_str = pld.substr(uname_val_start + 1, uname_val_end - uname_val_start - 1);
        if (uname_str.size() >= sizeof(res.username_buf)) {
            res.error = AuthError::InvalidPayload;
            return res;
        }
        std::memcpy(res.username_buf, uname_str.data(), uname_str.size());
        res.username_len = uname_str.size();
        res.username_buf[res.username_len] = '\0';

        res.error = AuthError::None;
        return res;
    }

private:
    uint32_t inner_state_[8];
    uint32_t outer_state_[8];
};

} // namespace sec
