#pragma once

#include <charconv>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace tds::parsing
{

enum class NumberError
{
    invalid,
    out_of_range
};

template <std::integral Integer>
    requires(!std::same_as<Integer, bool>)
std::expected<Integer, NumberError> integer(std::string_view text) noexcept
{
    Integer value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);

    if (result.ec == std::errc::result_out_of_range)
    {
        return std::unexpected(NumberError::out_of_range);
    }
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return std::unexpected(NumberError::invalid);
    }

    return value;
}

constexpr int hex_digit(char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }

    return -1;
}

struct CodePoint
{
    std::uint32_t value;
    std::size_t width;
};

bool ascii_digits(std::string_view text) noexcept;
std::optional<std::string> base64url(std::string_view text);
std::optional<CodePoint> code_point(std::string_view text, std::size_t offset) noexcept;
bool valid_utf8(std::string_view text) noexcept;

} // namespace tds::parsing
