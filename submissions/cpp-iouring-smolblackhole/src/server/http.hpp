#pragma once

#include <string>
#include <string_view>

namespace tds
{

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
