#pragma once

#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace tds
{

class Database
{
  public:
    struct Create
    {
        std::int64_t user_id;
        std::string username;
        std::string body;
    };

    struct Like
    {
        std::int64_t user_id;
        std::int64_t post_id;
    };

    using Write = std::variant<Create, Like>;
    using WriteValue = std::variant<std::string, std::optional<bool>>;
    using WriteResult = std::expected<WriteValue, std::exception_ptr>;

    explicit Database(const std::string &path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    void health();
    std::string feed();
    std::optional<std::string> post(std::int64_t id);
    // Results follow input order and are returned only after commit or rollback
    std::vector<WriteResult> write_batch(std::span<const Write> writes);

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace tds
