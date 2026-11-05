#pragma once

#include "../model.hpp"

#include <expected>
#include <memory>
#include <span>
#include <string>

namespace tds
{

class PostWriter;

class Store
{
  public:
    explicit Store(const std::string &path);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    void health();
    void feed(PostWriter &writer);
    bool post(PostId id, PostWriter &writer);
    // Outcomes follow input order and become observable after transaction resolution
    std::expected<void, WriteError> commit(std::span<const WriteCommand> commands,
                                           std::span<WriteOutcome> outcomes);

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace tds
