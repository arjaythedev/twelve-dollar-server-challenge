#include "api.hpp"
#include "auth.hpp"
#include "json.hpp"
#include "parsing.hpp"

#include <expected>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace tds
{
namespace
{

enum class Endpoint
{
    health,
    feed,
    post,
    create,
    like,
    missing
};

struct Route
{
    Endpoint endpoint;
    std::string_view id;
};

Route route(const Request &request)
{
    if (request.method == "GET" && request.path == "/health")
    {
        return {Endpoint::health, {}};
    }
    if (request.method == "GET" && request.path == "/feed")
    {
        return {Endpoint::feed, {}};
    }
    if (request.method == "POST" && request.path == "/posts")
    {
        return {Endpoint::create, {}};
    }
    if (request.path.starts_with("/posts/"))
    {
        auto id = std::string_view(request.path).substr(7);
        const bool like = id.ends_with("/like");
        if (like)
        {
            id.remove_suffix(5);
        }
        if (!id.empty() && id.find('/') == std::string_view::npos)
        {
            if (request.method == "GET" && !like)
            {
                return {Endpoint::post, id};
            }
            if (request.method == "POST" && like)
            {
                return {Endpoint::like, id};
            }
        }
    }

    return {Endpoint::missing, {}};
}

Response error_response(ApiError error)
{
    std::string body = "{\"error\":";
    quote(body, error.message);
    body += '}';

    return {error.status, std::move(body)};
}

std::expected<std::int64_t, ApiError> parse_post_id(std::string_view text)
{
    if (!parsing::ascii_digits(text) || text.find_first_not_of('0') == std::string_view::npos)
    {
        return std::unexpected(ApiError{Status::bad_request, "invalid post id"});
    }

    const auto parsed = parsing::integer<std::int64_t>(text);
    if (!parsed)
    {
        const auto status = parsed.error() == parsing::NumberError::out_of_range
                                ? Status::not_found
                                : Status::bad_request;
        const auto message = status == Status::not_found ? "post not found" : "invalid post id";

        return std::unexpected(ApiError{status, message});
    }

    return *parsed;
}

bool body_whitespace(std::uint32_t value)
{
    return (value >= 9 && value <= 13) || value == 32 || value == 0xA0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200A) || value == 0x2028 || value == 0x2029 ||
           value == 0x202F || value == 0x205F || value == 0x3000 || value == 0xFEFF;
}

std::string_view trimmed_body(std::string_view body)
{
    std::size_t first = body.size();
    std::size_t last = 0;
    for (std::size_t offset = 0; offset < body.size();)
    {
        const auto point = parsing::code_point(body, offset);
        if (!point)
        {
            throw std::runtime_error("post body is not valid UTF-8");
        }

        if (!body_whitespace(point->value))
        {
            if (first == body.size())
            {
                first = offset;
            }
            last = offset + point->width;
        }
        offset += point->width;
    }

    return last ? body.substr(first, last - first) : std::string_view{};
}

std::int64_t database_user_id(const Identity &identity)
{
    if (!identity.user_id)
    {
        throw std::runtime_error("user id exceeds SQLite integer range");
    }

    return *identity.user_id;
}

} // namespace

Api::Api(std::string database_path, std::string secret)
    : database_path_(std::move(database_path)), secret_(std::move(secret)),
      started_(std::chrono::steady_clock::now())
{
}

Database &Api::database()
{
    if (!database_)
    {
        database_ = std::make_unique<Database>(database_path_);
    }

    return *database_;
}

Response Api::health()
{
    try
    {
        database().health();
    }
    catch (const std::exception &error)
    {
        std::string output = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":";
        quote(output, error.what());
        output += '}';

        return {Status::unavailable, std::move(output)};
    }

    const auto elapsed = std::chrono::steady_clock::now() - started_;
    std::string output = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":";
    integer(output, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    output += '}';

    return {Status::ok, std::move(output)};
}

Response Api::get_post(std::string_view id)
{
    const auto parsed = parse_post_id(id);
    if (!parsed)
    {
        return error_response(parsed.error());
    }

    auto post = database().post(*parsed);
    if (!post)
    {
        return error_response({Status::not_found, "post not found"});
    }

    return {Status::ok, std::move(*post)};
}

Response Api::create_post(const Request &request)
{
    const auto &identity = request.identity.value();

    const auto document = read_json(request.body);
    if (!document)
    {
        return error_response({Status::bad_request, "malformed JSON body"});
    }

    auto *value = member(yyjson_doc_get_root(document->get()), "body");
    if (!yyjson_is_str(value))
    {
        return error_response({Status::bad_request, "body is required"});
    }

    const auto body = trimmed_body(text_value(value));
    if (body.empty())
    {
        return error_response({Status::bad_request, "body is required"});
    }

    std::size_t characters = 0;
    for (std::size_t offset = 0; offset < body.size(); ++characters)
    {
        offset += parsing::code_point(body, offset)->width;
    }
    if (characters > 500)
    {
        return error_response({Status::bad_request, "body must be at most 500 characters"});
    }

    const auto user_id = database_user_id(identity);

    return {Status::created, database().create(user_id, identity.username, body)};
}

Response Api::like_post(const Request &request, std::string_view id)
{
    const auto &identity = request.identity.value();

    const auto parsed = parse_post_id(id);
    if (!parsed)
    {
        return error_response(parsed.error());
    }

    const auto created = database().like(database_user_id(identity), *parsed);
    if (!created)
    {
        return error_response({Status::not_found, "post not found"});
    }

    std::string output = *created ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                                  : "{\"liked\":true,\"already_liked\":true,\"post_id\":";
    integer(output, *parsed);
    output += '}';

    return {*created ? Status::created : Status::ok, std::move(output)};
}

std::optional<Response> Api::authorize(Request &request) const
{
    const auto endpoint = route(request).endpoint;
    if (endpoint != Endpoint::create && endpoint != Endpoint::like)
    {
        return std::nullopt;
    }

    auto identity = authenticate(request.authorization, secret_);
    if (!identity)
    {
        return error_response(identity.error());
    }
    request.identity = std::move(*identity);

    return std::nullopt;
}

Response Api::handle(const Request &request)
{
    const auto target = route(request);
    switch (target.endpoint)
    {
    case Endpoint::health:
        return health();
    case Endpoint::feed:
        return {Status::ok, database().feed()};
    case Endpoint::post:
        return get_post(target.id);
    case Endpoint::create:
        return create_post(request);
    case Endpoint::like:
        return like_post(request, target.id);
    case Endpoint::missing:
        return error_response({Status::not_found, "not found"});
    }

    throw std::logic_error("unknown endpoint");
}

} // namespace tds
