#pragma once

#include "../http.hpp"
#include "../identity.hpp"
#include "../model.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace tds
{

struct RequestHeadView
{
    std::string_view method;
    std::string_view path;
    std::string_view authorization;
};

struct HealthHead
{
};

struct FeedHead
{
};

struct PostHead
{
    std::string_view id;
};

struct CreateHead
{
    Identity identity;
};

struct LikeHead
{
    Identity identity;
    std::string_view id;
};

struct MissingHead
{
};

// Borrowed IDs remain valid until prepare, while the session owns its normalized path
using AcceptedHead =
    std::variant<HealthHead, FeedHead, PostHead, CreateHead, LikeHead, MissingHead>;

class Store;

class Application
{
  public:
    Application(std::string database_path, std::string secret);
    ~Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    std::expected<AcceptedHead, ApiError> accept_headers(RequestHeadView head) const;
    std::expected<PreparedRequest, ApiError> prepare(const AcceptedHead &head,
                                                     std::string_view body) const;
    Response read(const ReadQuery &query);
    std::expected<void, WriteError> commit(std::span<const WriteCommand> commands,
                                           std::span<WriteOutcome> outcomes);
    Response write_reply(const WriteCommand &command, const WriteOutcome &outcome) const;
    static Response error_response(ApiError error);

  private:
    Store &store();
    Response health();

    std::string database_path_;
    std::string secret_;
    std::chrono::steady_clock::time_point started_;
    std::unique_ptr<Store> store_;
};

} // namespace tds
