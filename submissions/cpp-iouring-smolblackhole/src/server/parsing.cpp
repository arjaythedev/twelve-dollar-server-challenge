#include "parsing.hpp"

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace tds::parsing
{
namespace
{

int base64_digit(char value) noexcept
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + 52;
    }
    if (value == '-')
    {
        return 62;
    }
    if (value == '_')
    {
        return 63;
    }

    return -1;
}

std::size_t utf8_width(unsigned char first) noexcept
{
    if (first >= 0xC2 && first <= 0xDF)
    {
        return 2;
    }
    if (first >= 0xE0 && first <= 0xEF)
    {
        return 3;
    }
    if (first >= 0xF0 && first <= 0xF4)
    {
        return 4;
    }

    return 0;
}

} // namespace

bool ascii_digits(std::string_view text) noexcept
{
    if (text.empty())
    {
        return false;
    }

    for (const char value : text)
    {
        if (value < '0' || value > '9')
        {
            return false;
        }
    }

    return true;
}

std::optional<std::string> base64url(std::string_view text)
{
    if (text.empty() || text.size() % 4 == 1)
    {
        return std::nullopt;
    }

    std::string output;
    output.reserve(text.size() * 3 / 4);

    unsigned bits = 0;
    unsigned count = 0;
    for (const char character : text)
    {
        const int value = base64_digit(character);
        if (value < 0)
        {
            return std::nullopt;
        }

        bits = (bits << 6) | static_cast<unsigned>(value);
        count += 6;
        if (count >= 8)
        {
            count -= 8;
            output += static_cast<char>((bits >> count) & 255);
        }
    }

    return output;
}

std::optional<CodePoint> code_point(std::string_view text, std::size_t offset) noexcept
{
    if (offset >= text.size())
    {
        return std::nullopt;
    }

    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80)
    {
        return CodePoint{first, 1};
    }

    const auto width = utf8_width(first);
    if (!width || width > text.size() - offset)
    {
        return std::nullopt;
    }

    std::uint32_t value = first & (0x7FU >> width);
    for (std::size_t index = 1; index < width; ++index)
    {
        const auto byte = static_cast<unsigned char>(text[offset + index]);
        if ((byte & 0xC0) != 0x80)
        {
            return std::nullopt;
        }
        value = (value << 6) | (byte & 0x3F);
    }

    const bool overlong = (width == 2 && value < 0x80) || (width == 3 && value < 0x800) ||
                          (width == 4 && value < 0x10000);
    const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
    if (overlong || surrogate || value > 0x10FFFF)
    {
        return std::nullopt;
    }

    return CodePoint{value, width};
}

bool valid_utf8(std::string_view text) noexcept
{
    for (std::size_t offset = 0; offset < text.size();)
    {
#if defined(__SSE2__)
        if (text.size() - offset >= 16)
        {
            const auto block =
                _mm_loadu_si128(reinterpret_cast<const __m128i *>(text.data() + offset));
            if (!_mm_movemask_epi8(block))
            {
                offset += 16;
                continue;
            }
        }
#endif
        const auto point = code_point(text, offset);
        if (!point)
        {
            return false;
        }
        offset += point->width;
    }

    return true;
}

} // namespace tds::parsing
