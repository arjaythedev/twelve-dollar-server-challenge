#pragma once

#include "http.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace tds
{

class Server
{
  public:
    using Handler = std::function<Response(const Request &)>;
    using HeaderHandler = std::function<std::optional<Response>(Request &)>;

    Server(std::string_view host, std::uint16_t port, Handler handler, HeaderHandler headers);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    void run();
    void request_stop() const noexcept;
    [[nodiscard]] int stop_descriptor() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

  private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace tds
