#pragma once

#include "identity.hpp"
#include "model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tds
{

enum class Endpoint : std::uint8_t
{
    unresolved,
    health,
    feed,
    post,
    create,
    like,
    missing
};

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
    Endpoint endpoint = Endpoint::unresolved;
    bool keep_alive = true;
};

struct Response
{
    Status status;
    std::string body;
};

using RequestResult = std::variant<Response, WriteCommand>;

struct ApiError
{
    Status status;
    std::string_view message;
};

} // namespace tds
