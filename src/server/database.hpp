#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tds
{

class Database
{
  public:
    explicit Database(const std::string &path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    void health();
    std::string feed();
    std::optional<std::string> post(std::int64_t id);
    std::string create(std::int64_t user_id, std::string_view username, std::string_view body);
    std::optional<bool> like(std::int64_t user_id, std::int64_t post_id);

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace tds
