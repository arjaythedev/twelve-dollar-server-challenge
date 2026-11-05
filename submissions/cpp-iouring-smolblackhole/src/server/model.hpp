#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>

namespace tds
{

struct UserId
{
    std::int64_t value;

    bool operator==(const UserId &) const = default;
};

struct PostId
{
    std::int64_t value;

    bool operator==(const PostId &) const = default;
};

struct Health
{
};

struct Feed
{
};

struct GetPost
{
    PostId id;
};

struct CreatePost
{
    UserId user_id;
    std::string username;
    std::string body;
};

struct LikePost
{
    UserId user_id;
    PostId post_id;
};

struct CreatedPost
{
    PostId id;
    std::array<char, 24> created_at;
};

enum class LikeResult
{
    created,
    already_liked
};

enum class WriteErrorCode
{
    post_not_found,
    constraint,
    transaction,
    internal
};

struct WriteError
{
    WriteErrorCode code;
    std::string message;
};

inline constexpr std::size_t write_batch_limit = 32;

using ReadQuery = std::variant<Health, Feed, GetPost>;
using WriteCommand = std::variant<CreatePost, LikePost>;
using PreparedRequest = std::variant<ReadQuery, WriteCommand>;
using WriteValue = std::variant<CreatedPost, LikeResult>;
using WriteOutcome = std::expected<WriteValue, WriteError>;

} // namespace tds
