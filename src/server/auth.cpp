#include "auth.hpp"
#include "json.hpp"
#include "parsing.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace tds
{
namespace
{

constexpr ApiError invalid_token{Status::unauthorized, "invalid or expired token"};
constexpr ApiError invalid_payload{Status::unauthorized, "invalid token payload"};

bool valid_expiration(yyjson_val *value)
{
    double expires = 0;
    if (yyjson_is_num(value))
    {
        expires = yyjson_get_num(value);
    }
    else if (yyjson_is_raw(value))
    {
        const std::string_view raw(yyjson_get_raw(value), yyjson_get_len(value));
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), expires);
        if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size())
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    const double now = std::chrono::duration<double>(elapsed).count();

    return std::isfinite(expires) && expires > now;
}

bool valid_signature(std::string_view content, std::string_view signature, std::string_view secret)
{
    std::array<unsigned char, 32> expected{};
    std::size_t written = 0;
    const auto *bytes = reinterpret_cast<const unsigned char *>(content.data());
    const auto *result =
        EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(), bytes,
                  content.size(), expected.data(), expected.size(), &written);
    if (!result)
    {
        throw std::runtime_error("HMAC failed");
    }

    return written == expected.size() && signature.size() == expected.size() &&
           CRYPTO_memcmp(expected.data(), signature.data(), expected.size()) == 0;
}

} // namespace

std::expected<Identity, ApiError> authenticate(std::string_view header, std::string_view secret)
{
    if (!header.starts_with("Bearer "))
    {
        return std::unexpected(ApiError{Status::unauthorized, "missing bearer token"});
    }

    const auto token = header.substr(7);
    const auto first_dot = token.find('.');
    if (first_dot == std::string_view::npos)
    {
        return std::unexpected(invalid_token);
    }

    const auto second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos ||
        token.find('.', second_dot + 1) != std::string_view::npos)
    {
        return std::unexpected(invalid_token);
    }

    const auto metadata = parsing::base64url(token.substr(0, first_dot));
    const auto payload =
        parsing::base64url(token.substr(first_dot + 1, second_dot - first_dot - 1));
    const auto signature = parsing::base64url(token.substr(second_dot + 1));
    if (!metadata || !payload || !signature || signature->size() != 32)
    {
        return std::unexpected(invalid_token);
    }

    auto header_document = read_json(*metadata);
    auto payload_document = read_json(*payload);
    if (!header_document || !payload_document)
    {
        return std::unexpected(invalid_token);
    }

    auto *metadata_root = yyjson_doc_get_root(header_document->get());
    auto *claims = yyjson_doc_get_root(payload_document->get());
    if (text_value(member(metadata_root, "alg")) != "HS256" || !yyjson_is_obj(claims))
    {
        return std::unexpected(invalid_token);
    }
    if (!valid_signature(token.substr(0, second_dot), *signature, secret))
    {
        return std::unexpected(invalid_token);
    }
    if (!valid_expiration(member(claims, "exp")))
    {
        return std::unexpected(invalid_token);
    }

    auto *subject_value = member(claims, "sub");
    auto *username_value = member(claims, "username");
    const auto subject = text_value(subject_value);
    if (!yyjson_is_str(subject_value) || !parsing::ascii_digits(subject) ||
        subject.find_first_not_of('0') == std::string_view::npos || !yyjson_is_str(username_value))
    {
        return std::unexpected(invalid_payload);
    }

    std::optional<std::int64_t> user_id;
    if (const auto parsed = parsing::integer<std::int64_t>(subject))
    {
        user_id = *parsed;
    }

    return Identity{user_id, std::string(text_value(username_value))};
}

} // namespace tds
