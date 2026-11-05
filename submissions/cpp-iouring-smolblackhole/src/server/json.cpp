#include "json.hpp"
#include "parsing.hpp"

#include <array>
#include <charconv>
#include <stdexcept>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace tds
{

namespace
{
int escape_value(std::string_view text, std::size_t offset) noexcept
{
    if (text.size() - offset < 6 || text[offset] != '\\' || text[offset + 1] != 'u')
    {
        return -1;
    }
    int value = 0;
    for (std::size_t i = 2; i < 6; ++i)
    {
        const int digit = parsing::hex_digit(text[offset + i]);
        if (digit < 0)
        {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

// Preserve Python's escaped lone-surrogate strings. Raw invalid UTF-8 is rejected first.
std::string preserve_surrogates(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    bool in_string = false;
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] == '"')
        {
            in_string = !in_string;
            result += text[i++];
        }
        else if (in_string && text[i] == '\\' && i + 1 < text.size())
        {
            const int value = escape_value(text, i);
            if (value >= 0xD800 && value <= 0xDBFF)
            {
                const int low = escape_value(text, i + 6);
                if (low >= 0xDC00 && low <= 0xDFFF)
                {
                    result.append(text.substr(i, 12));
                    i += 12;
                    continue;
                }
            }
            if (value >= 0xD800 && value <= 0xDFFF)
            {
                result += static_cast<char>(0xE0 | (value >> 12));
                result += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (value & 0x3F));
                i += 6;
            }
            else
            {
                result.append(text.substr(i, 2));
                i += 2;
            }
        }
        else
        {
            result += text[i++];
        }
    }
    return result;
}
} // namespace

std::expected<Document, JSONError> read_json(std::string_view text)
{
    if (!parsing::valid_utf8(text))
    {
        return std::unexpected(JSONError::malformed);
    }
    const std::string normalized =
        text.find("\\u") != std::string_view::npos ? preserve_surrogates(text) : std::string{};
    if (!normalized.empty())
    {
        text = normalized;
    }
    Document document(yyjson_read(text.data(), text.size(),
                                  YYJSON_READ_ALLOW_INVALID_UNICODE | YYJSON_READ_BIGNUM_AS_RAW));
    if (!document)
    {
        return std::unexpected(JSONError::malformed);
    }
    return document;
}

yyjson_val *member(yyjson_val *object, std::string_view key) noexcept
{
    if (!yyjson_is_obj(object))
    {
        return nullptr;
    }
    yyjson_val *result = nullptr;
    yyjson_val *name = nullptr;
    yyjson_val *value = nullptr;
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_obj_foreach(object, index, count, name, value)
    {
        if (text_value(name) == key)
        {
            result = value;
        }
    }
    return result;
}

std::string_view text_value(yyjson_val *value) noexcept
{
    if (!yyjson_is_str(value))
    {
        return {};
    }
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

void quote(std::string &output, std::string_view text)
{
    output += '"';
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto start = i;
#if defined(__SSE2__)
        while (text.size() - i >= 16)
        {
            const auto block = _mm_loadu_si128(reinterpret_cast<const __m128i *>(text.data() + i));
            auto bad = _mm_or_si128(_mm_cmpeq_epi8(block, _mm_set1_epi8('"')),
                                    _mm_cmpeq_epi8(block, _mm_set1_epi8('\\')));
            bad = _mm_or_si128(bad, _mm_cmpeq_epi8(_mm_min_epu8(block, _mm_set1_epi8(31)), block));
            bad = _mm_or_si128(bad, _mm_cmpeq_epi8(block, _mm_set1_epi8(static_cast<char>(0xED))));
            if (_mm_movemask_epi8(bad))
            {
                break;
            }
            i += 16;
        }
#endif
        while (i < text.size())
        {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c < 32 || c == '"' || c == '\\' || c == 0xED)
            {
                break;
            }
            ++i;
        }
        if (i != start)
        {
            output.append(text.substr(start, i - start));
        }
        if (i == text.size())
        {
            break;
        }
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c == '"' || c == '\\')
        {
            output += '\\';
            output += static_cast<char>(c);
        }
        else if (c < 32)
        {
            switch (c)
            {
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output += "\\u00";
                output += "0123456789abcdef"[c >> 4];
                output += "0123456789abcdef"[c & 15];
            }
        }
        else if (c == 0xED && text.size() - i >= 2 && static_cast<unsigned char>(text[i]) >= 0xA0 &&
                 static_cast<unsigned char>(text[i]) <= 0xBF)
        {
            const unsigned value = ((c & 15U) << 12) |
                                   ((static_cast<unsigned char>(text[i]) & 63U) << 6) |
                                   (static_cast<unsigned char>(text[i + 1]) & 63U);
            output += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4)
            {
                output += "0123456789abcdef"[(value >> shift) & 15U];
            }
            i += 2;
        }
        else
        {
            output += static_cast<char>(c);
        }
    }
    output += '"';
}

void integer(std::string &output, std::int64_t value)
{
    std::array<char, 21> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{})
    {
        throw std::runtime_error("integer encoding failed");
    }
    output.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

} // namespace tds
