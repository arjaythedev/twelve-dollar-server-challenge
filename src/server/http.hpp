#pragma once

#include "identity.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tds
{

inline constexpr std::size_t write_batch_limit = 16;

enum class Status : unsigned short
{
    ok = 200,
    created = 201,
    bad_request = 400,
    unauthorized = 401,
    not_found = 404,
    too_large = 413,
    internal_error = 500,
    unavailable = 503,
};

struct Request
{
    std::string method;
    std::string path;
    std::string authorization;
    std::string body;
    std::optional<Identity> identity;
    bool keep_alive = true;
};

struct Response
{
    Status status;
    std::string body;
};

struct ApiError
{
    Status status;
    std::string_view message;
};

} // namespace tds
