#pragma once

#include "../http.hpp"
#include "http_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>

namespace tds::transport
{

enum class Event
{
    accept,
    receive,
    send,
    close,
    stop,
    wake
};

struct Connection;
class Ring;

class Operation
{
  public:
    explicit Operation(Event kind) noexcept : event(kind)
    {
    }

    Operation(Event kind, Connection &owner) noexcept : event(kind), target_{.connection = &owner}
    {
    }

    Operation(Event kind, Ring &owner) noexcept : event(kind), target_{.ring = &owner}
    {
    }

    Connection &connection() const noexcept
    {
        return *target_.connection;
    }

    Ring &ring() const noexcept
    {
        return *target_.ring;
    }

    Event event;

  private:
    // The event selects the target: receive/send/close use connection, accept/wake use ring
    union Target {
        Connection *connection = nullptr;
        Ring *ring;
    } target_;
};

struct Connection
{
    Connection(Ring &owner, unsigned index)
        : ring(owner), slot(index), operation{Event::receive, *this},
          close_operation{Event::close, *this}
    {
    }

    void prepare_response(Response response);
    msghdr &prepare_send();
    void finish_response();

    void start_io(Event event) noexcept
    {
        operation.event = event;
        io_phase_ = IoPhase::active;
    }

    void finish_io() noexcept
    {
        io_phase_ = IoPhase::idle;
    }

    void start_close() noexcept
    {
        io_phase_ = IoPhase::closing;
    }

    bool io_idle() const noexcept
    {
        return io_phase_ == IoPhase::idle;
    }

    bool io_active() const noexcept
    {
        return io_phase_ == IoPhase::active;
    }

    Ring &ring;
    unsigned slot;
    Operation operation;
    Operation close_operation;
    HttpParser parser;
    std::string input;
    std::string output;
    std::string response_body;
    std::array<iovec, 2> send_vectors{};
    msghdr send_message{};
    std::size_t sent = 0;
    bool close_after = false;

  private:
    enum class IoPhase : std::uint8_t
    {
        idle,
        active,
        closing
    };

    IoPhase io_phase_ = IoPhase::idle;
};

} // namespace tds::transport
