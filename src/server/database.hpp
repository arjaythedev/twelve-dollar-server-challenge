#pragma once

#include "write.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
    // Results follow input order and are returned only after commit or rollback
    std::vector<WriteResult> write_batch(std::span<const WriteCommand> writes);

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace tds
