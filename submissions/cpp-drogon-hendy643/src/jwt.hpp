#pragma once
#include <glaze/glaze.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace jwt {
    enum class Status { Ok, MissingBearer, InvalidToken, InvalidPayload };

    struct Identity {
        int64_t user_id = 0;
        std::string username;
    };

    inline constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    inline constexpr std::array<int8_t, 256> kDecode = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();

    inline bool b64url_decode(const std::string_view in, std::string &out) {
        out.clear();
        if (in.size() % 4 == 1) return false;
        uint32_t acc = 0;
        int bits = 0;
        for (const unsigned char c: in) {
            const int v = kDecode[c];
            if (v < 0) return false;
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((acc >> bits) & 0xFF));
            }
        }
        return true;
    }

    inline std::string b64url_encode(const std::string_view in) {
        std::string out;
        out.reserve((in.size() * 4 + 2) / 3);
        uint32_t acc = 0;
        int bits = 0;
        for (const unsigned char c: in) {
            acc = (acc << 8) | c;
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                out.push_back(kAlphabet[(acc >> bits) & 63]);
            }
        }
        if (bits > 0) out.push_back(kAlphabet[(acc << (6 - bits)) & 63]);
        return out;
    }

    inline std::string hmac_sha256_b64url(const std::string_view secret, const std::string_view data) {
        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char *>(data.data()), data.size(), mac, &len);
        return b64url_encode({reinterpret_cast<const char *>(mac), len});
    }

    struct Header {
        std::string_view alg;
    };

    struct Claims {
        glz::raw_json_view sub, username, exp;
    };

    inline constexpr glz::opts kLoose{.error_on_unknown_keys = false};

    inline Status verify(const std::string_view authorization, const std::string_view secret, const int64_t now,
                         Identity &id) {
        constexpr std::string_view kBearer = "Bearer ";
        if (!authorization.starts_with(kBearer)) return Status::MissingBearer;
        const std::string_view token = authorization.substr(kBearer.size());

        const size_t d1 = token.find('.');
        if (d1 == std::string_view::npos) return Status::InvalidToken;
        const size_t d2 = token.find('.', d1 + 1);
        if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos)
            return
                    Status::InvalidToken;

        thread_local std::string buf;
        if (Header header; !b64url_decode(token.substr(0, d1), buf) || glz::read<kLoose>(header, buf) || header.alg !=
                           "HS256")
            return Status::InvalidToken;

        const std::string expected = hmac_sha256_b64url(secret, token.substr(0, d2));
        const std::string_view sig = token.substr(d2 + 1);
        if (sig.size() != expected.size() || CRYPTO_memcmp(sig.data(), expected.data(), sig.size()) != 0)
            return Status::InvalidToken;

        Claims claims;
        if (!b64url_decode(token.substr(d1 + 1, d2 - d1 - 1), buf) || glz::read<kLoose>(claims, buf))
            return Status::InvalidPayload;

        if (!claims.exp.str.empty()) {
            double exp = 0;
            const auto &s = claims.exp.str;
            if (std::from_chars(s.data(), s.data() + s.size(), exp).ec != std::errc{}) return Status::InvalidPayload;
            if (exp <= static_cast<double>(now)) return Status::InvalidToken;
        }

        const std::string_view sub = claims.sub.str;
        if (sub.size() < 3 || sub.front() != '"' || sub.back() != '"') return Status::InvalidPayload;
        const std::string_view digits = sub.substr(1, sub.size() - 2);
        int64_t user_id = 0;
        if (const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), user_id);
            ec != std::errc{} || ptr != digits.data() + digits.size() || user_id <= 0)
            return
                    Status::InvalidPayload;

        if (const std::string_view name = claims.username.str;
            name.empty() || name.front() != '"' || glz::read_json(id.username, name)) return Status::InvalidPayload;
        id.user_id = user_id;
        return Status::Ok;
    }
}
