#include "application.hpp"
#include "../auth.hpp"
#include "../json.hpp"
#include "../parsing.hpp"
#include "../storage/database.hpp"
#include "post_writer.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace tds
{
namespace
{

Response error_response(ApiError error)
{
    std::string output = "{\"error\":";
    quote(output, error.message);
    output += '}';

    return {error.status, std::move(output)};
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

std::expected<UserId, ApiError> database_user_id(const Identity &identity)
{
    if (!identity.user_id)
    {
        return std::unexpected(ApiError{Status::internal_error, "internal server error"});
    }

    return *identity.user_id;
}

std::expected<PreparedRequest, ApiError> prepare_create(const Identity &identity,
                                                        std::string_view input)
{
    const auto document = read_json(input);
    if (!document)
    {
        return std::unexpected(ApiError{Status::bad_request, "malformed JSON body"});
    }
    auto *value = member(yyjson_doc_get_root(document->get()), "body");
    if (!yyjson_is_str(value))
    {
        return std::unexpected(ApiError{Status::bad_request, "body is required"});
    }
    const auto body = trimmed_body(text_value(value));
    if (body.empty())
    {
        return std::unexpected(ApiError{Status::bad_request, "body is required"});
    }
    std::size_t characters = 0;
    for (std::size_t offset = 0; offset < body.size(); ++characters)
    {
        offset += parsing::code_point(body, offset)->width;
    }
    if (characters > 500)
    {
        return std::unexpected(
            ApiError{Status::bad_request, "body must be at most 500 characters"});
    }
    const auto user_id = database_user_id(identity);
    if (!user_id)
    {
        return std::unexpected(user_id.error());
    }

    // The parsed JSON dies here, so a queued command owns its validated text
    return PreparedRequest{
        WriteCommand{CreatePost{*user_id, identity.username, std::string(body)}}};
}

} // namespace

Application::Application(std::string database_path, std::string secret)
    : database_path_(std::move(database_path)), secret_(std::move(secret)),
      started_(std::chrono::steady_clock::now())
{
}

Application::~Application() = default;

std::expected<AcceptedHead, ApiError> Application::accept_headers(RequestHeadView head) const
{
    // Cursed GCC 13 corner: moving a temporary variant trips its uninitialized-storage analysis
    // Build the selected alternative directly inside expected instead
    if (head.method == "GET" && head.path == "/health")
    {
        return std::expected<AcceptedHead, ApiError>{std::in_place, std::in_place_type<HealthHead>};
    }
    if (head.method == "GET" && head.path == "/feed")
    {
        return std::expected<AcceptedHead, ApiError>{std::in_place, std::in_place_type<FeedHead>};
    }
    if (head.method == "POST" && head.path == "/posts")
    {
        auto identity = authenticate(head.authorization, secret_);
        if (!identity)
        {
            return std::unexpected(identity.error());
        }

        return std::expected<AcceptedHead, ApiError>{std::in_place, std::in_place_type<CreateHead>,
                                                     std::move(*identity)};
    }
    if (head.path.starts_with("/posts/"))
    {
        auto id = head.path.substr(7);
        const bool like = id.ends_with("/like");
        if (like)
        {
            id.remove_suffix(5);
        }
        if (!id.empty() && id.find('/') == std::string_view::npos)
        {
            if (head.method == "GET" && !like)
            {
                return std::expected<AcceptedHead, ApiError>{std::in_place,
                                                             std::in_place_type<PostHead>, id};
            }
            if (head.method == "POST" && like)
            {
                auto identity = authenticate(head.authorization, secret_);
                if (!identity)
                {
                    return std::unexpected(identity.error());
                }

                return std::expected<AcceptedHead, ApiError>{
                    std::in_place, std::in_place_type<LikeHead>, std::move(*identity), id};
            }
        }
    }

    return std::expected<AcceptedHead, ApiError>{std::in_place, std::in_place_type<MissingHead>};
}

std::expected<PreparedRequest, ApiError> Application::prepare(const AcceptedHead &head,
                                                              std::string_view body) const
{
    if (std::holds_alternative<HealthHead>(head))
    {
        return PreparedRequest{ReadQuery{Health{}}};
    }
    if (std::holds_alternative<FeedHead>(head))
    {
        return PreparedRequest{ReadQuery{Feed{}}};
    }
    if (const auto *post = std::get_if<PostHead>(&head))
    {
        const auto id = parse_post_id(post->id);
        if (!id)
        {
            return std::unexpected(id.error());
        }

        return PreparedRequest{ReadQuery{GetPost{*id}}};
    }
    if (const auto *create = std::get_if<CreateHead>(&head))
    {
        return prepare_create(create->identity, body);
    }
    if (const auto *like = std::get_if<LikeHead>(&head))
    {
        const auto id = parse_post_id(like->id);
        if (!id)
        {
            return std::unexpected(id.error());
        }
        const auto user_id = database_user_id(like->identity);
        if (!user_id)
        {
            return std::unexpected(user_id.error());
        }

        return PreparedRequest{WriteCommand{LikePost{*user_id, *id}}};
    }

    return std::unexpected(ApiError{Status::not_found, "not found"});
}

Store &Application::store()
{
    if (!store_)
    {
        store_ = std::make_unique<Store>(database_path_);
    }

    return *store_;
}

Response Application::health()
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

Response Application::read(const ReadQuery &query)
{
    if (std::holds_alternative<Health>(query))
    {
        return health();
    }
    std::string output;
    if (std::holds_alternative<Feed>(query))
    {
        PostWriter writer(output, PostShape::feed);
        store().feed(writer);

        return {Status::ok, std::move(output)};
    }
    PostWriter writer(output, PostShape::single);
    if (!store().post(std::get<GetPost>(query).id, writer))
    {
        return error_response({Status::not_found, "post not found"});
    }

    return {Status::ok, std::move(output)};
}

std::expected<void, WriteError> Application::commit(std::span<const WriteCommand> commands,
                                                    std::span<WriteOutcome> outcomes)
{
    if (commands.size() != outcomes.size() || commands.size() > write_batch_limit)
    {
        throw std::logic_error("invalid application write batch");
    }
    if (commands.empty())
    {
        return {};
    }

    return store().commit(commands, outcomes);
}

Response Application::write_reply(const WriteCommand &command, const WriteOutcome &outcome) const
{
    if (!outcome)
    {
        if (outcome.error().code == WriteErrorCode::post_not_found)
        {
            return error_response({Status::not_found, "post not found"});
        }
        std::cerr << "request failed: " << outcome.error().message << '\n';

        return error_response({Status::internal_error, "internal server error"});
    }
    if (const auto *create = std::get_if<CreatePost>(&command))
    {
        const auto &created = std::get<CreatedPost>(*outcome);
        std::string output;
        PostWriter writer(output, PostShape::single);
        writer.append(created.id, create->body,
                      {created.created_at.data(), created.created_at.size()}, create->username, 0);
        writer.finish();

        return {Status::created, std::move(output)};
    }
    const bool created = std::get<LikeResult>(*outcome) == LikeResult::created;
    std::string output = created ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                                 : "{\"liked\":true,\"already_liked\":true,\"post_id\":";
    integer(output, std::get<LikePost>(command).post_id.value);
    output += '}';

    return {created ? Status::created : Status::ok, std::move(output)};
}

} // namespace tds
