#pragma once

#include "http.hpp"
#include "storage/database.hpp"

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

    // Classify and authorize after headers; handle receives a complete accepted request
    std::optional<Response> accept_headers(Request &request) const;
    RequestResult handle(const Request &request);
    void handle_writes(std::span<const WriteCommand> commands, std::span<Response> responses);

  private:
    Store &store();
    Response health();
    Response get_post(std::string_view id);

    std::string database_path_;
    std::string secret_;
    std::chrono::steady_clock::time_point started_;
    std::unique_ptr<Store> store_;
};

} // namespace tds
