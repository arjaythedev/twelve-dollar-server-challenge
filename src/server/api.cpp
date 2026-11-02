#include "api.hpp"
#include "app/post_writer.hpp"
#include "auth.hpp"
#include "json.hpp"
#include "parsing.hpp"

#include <expected>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace tds
{
namespace
{

Endpoint classify_endpoint(const Request &request)
{
    if (request.method == "GET" && request.path == "/health")
    {
        return Endpoint::health;
    }
    if (request.method == "GET" && request.path == "/feed")
    {
        return Endpoint::feed;
    }
    if (request.method == "POST" && request.path == "/posts")
    {
        return Endpoint::create;
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
                return Endpoint::post;
            }
            if (request.method == "POST" && like)
            {
                return Endpoint::like;
            }
        }
    }

    return Endpoint::missing;
}

Response error_response(ApiError error)
{
    std::string body = "{\"error\":";
    quote(body, error.message);
    body += '}';

    return {error.status, std::move(body)};
}

std::expected<PostId, ApiError> parse_post_id(std::string_view text)
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

    return PostId{*parsed};
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

UserId database_user_id(const Identity &identity)
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

Store &Api::store()
{
    if (!store_)
    {
        store_ = std::make_unique<Store>(database_path_);
    }

    return *store_;
}

Response Api::health()
{
    try
    {
        store().health();
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

    std::string output;
    PostWriter writer(output, PostShape::single);
    if (!store().post(*parsed, writer))
    {
        return error_response({Status::not_found, "post not found"});
    }

    return {Status::ok, std::move(output)};
}

namespace
{

RequestResult prepare_create(const Request &request)
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

    // The yyjson document dies here, so the queued write owns its trimmed body
    return WriteCommand{CreatePost{user_id, identity.username, std::string(body)}};
}

RequestResult prepare_like(const Request &request, std::string_view id)
{
    const auto &identity = request.identity.value();

    const auto parsed = parse_post_id(id);
    if (!parsed)
    {
        return error_response(parsed.error());
    }

    return WriteCommand{LikePost{database_user_id(identity), *parsed}};
}

Response write_response(const WriteCommand &write, const WriteValue &value)
{
    if (const auto *command = std::get_if<CreatePost>(&write))
    {
        const auto &created = std::get<CreatedPost>(value);
        std::string output;
        PostWriter writer(output, PostShape::single);
        writer.append(created.id, command->body,
                      {created.created_at.data(), created.created_at.size()}, command->username, 0);
        writer.finish();

        return {Status::created, std::move(output)};
    }
    const bool created = std::get<LikeResult>(value) == LikeResult::created;
    std::string output = created ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                                 : "{\"liked\":true,\"already_liked\":true,\"post_id\":";
    integer(output, std::get<LikePost>(write).post_id.value);
    output += '}';

    return {created ? Status::created : Status::ok, std::move(output)};
}

} // namespace

void Api::handle_writes(std::span<const WriteCommand> commands, std::span<Response> responses)
{
    if (commands.size() != responses.size() || commands.size() > write_batch_limit)
    {
        throw std::logic_error("invalid HTTP write batch");
    }
    if (commands.empty())
    {
        return;
    }
    std::vector<WriteOutcome> outcomes(commands.size());
    const auto committed = store().commit(commands, outcomes);
    if (!committed)
    {
        std::cerr << "write batch failed: " << committed.error().message << '\n';
        for (auto &response : responses)
        {
            response = error_response({Status::internal_error, "internal server error"});
        }

        return;
    }
    for (std::size_t index = 0; index < commands.size(); ++index)
    {
        try
        {
            if (!outcomes[index])
            {
                const auto &error = outcomes[index].error();
                if (error.code == WriteErrorCode::post_not_found)
                {
                    responses[index] = error_response({Status::not_found, "post not found"});
                }
                else
                {
                    std::cerr << "request failed: " << error.message << '\n';
                    responses[index] =
                        error_response({Status::internal_error, "internal server error"});
                }
                continue;
            }
            responses[index] = write_response(commands[index], *outcomes[index]);
        }
        catch (const std::exception &error)
        {
            std::cerr << "request failed: " << error.what() << '\n';
            responses[index] = error_response({Status::internal_error, "internal server error"});
        }
    }
}

std::optional<Response> Api::accept_headers(Request &request) const
{
    request.endpoint = classify_endpoint(request);
    if (request.endpoint != Endpoint::create && request.endpoint != Endpoint::like)
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

RequestResult Api::handle(const Request &request)
{
    switch (request.endpoint)
    {
    case Endpoint::unresolved:
        throw std::logic_error("request headers were not accepted");
    case Endpoint::health:
        return health();
    case Endpoint::feed: {
        std::string output;
        PostWriter writer(output, PostShape::feed);
        store().feed(writer);

        return Response{Status::ok, std::move(output)};
    }
    case Endpoint::post:
        return get_post(std::string_view(request.path).substr(7));
    case Endpoint::create:
        return prepare_create(request);
    case Endpoint::like:
        return prepare_like(request,
                            std::string_view(request.path).substr(7, request.path.size() - 12));
    case Endpoint::missing:
        return error_response({Status::not_found, "not found"});
    }

    throw std::logic_error("unknown endpoint");
}

} // namespace tds
