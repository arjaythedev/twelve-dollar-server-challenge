#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace tds
{

inline constexpr std::size_t write_batch_limit = 16;

struct CreatePost
{
    std::int64_t user_id;
    std::string username;
    std::string body;
};

struct LikePost
{
    std::int64_t user_id;
    std::int64_t post_id;
};

using WriteCommand = std::variant<CreatePost, LikePost>;
using WriteValue = std::variant<std::string, std::optional<bool>>;
using WriteResult = std::expected<WriteValue, std::exception_ptr>;

} // namespace tds
