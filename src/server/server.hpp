#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tds
{

class Server
{
  public:
    Server(std::string_view host, std::uint16_t port, std::string database_path,
           std::string secret);
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
