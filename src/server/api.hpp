#pragma once

#include "database.hpp"
#include "http.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tds
{

class Api
{
  public:
    Api(std::string database_path, std::string secret);

    // Authorize after headers; only accepted requests may reach handle after the body
    std::optional<Response> authorize(Request &request) const;
    Response handle(const Request &request);
    static bool is_write(const Request &request);
    void handle_writes(std::span<const Request *const> requests, std::span<Response> responses);

  private:
    Database &database();
    Response health();
    Response get_post(std::string_view id);

    std::string database_path_;
    std::string secret_;
    std::chrono::steady_clock::time_point started_;
    std::unique_ptr<Database> database_;
};

} // namespace tds
