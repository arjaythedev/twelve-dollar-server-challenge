#pragma once

#include "../http.hpp"
#include "http_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
struct AcceptTicket;
class Ring;

struct Exchange
{
    Exchange() = default;
    Exchange(const Exchange &) = delete;
    Exchange &operator=(const Exchange &) = delete;

    void prepare_response(Response response, bool close_after);
    msghdr &prepare_send();
    void finish_response();

    std::size_t retained_capacity() const noexcept
    {
        return input.capacity() + output.capacity() + response_body.capacity() +
               parser.scratch_capacity();
    }

    HttpParser parser;
    std::string input;
    std::string output;
    std::string response_body;
    std::array<iovec, 2> send_vectors{};
    msghdr send_message{};
    std::size_t sent = 0;
#ifdef TDS_DIAGNOSTICS
    std::uint64_t send_prepared_ns = 0;
    std::uint64_t receive_serviced_ns = 0;
#endif
};

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

    explicit Operation(AcceptTicket &owner) noexcept
        : event(Event::accept), target_{.accept = &owner}
    {
    }

    AcceptTicket &accept_ticket() const noexcept
    {
        return *target_.accept;
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
    // The event selects the target: receive/send/close use connection, wake uses ring
    union Target {
        Connection *connection = nullptr;
        Ring *ring;
        AcceptTicket *accept;
    } target_;
};

struct AcceptTicket
{
    AcceptTicket() = default;
    AcceptTicket(const AcceptTicket &) = delete;
    AcceptTicket &operator=(const AcceptTicket &) = delete;

    Operation operation{*this};
    Ring *ring = nullptr;
    unsigned slot = 0;
    bool pending = false;
};

struct Connection
{
    Connection(Ring &owner, unsigned index)
        : ring(owner), operation{Event::receive, *this}, close_operation{Event::close, *this},
          slot(index)
    {
    }

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

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

    void start_application() noexcept
    {
        io_phase_ = IoPhase::application;
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
    Operation operation;
    Operation close_operation;
    std::unique_ptr<Exchange> exchange;
    unsigned slot;
    bool close_after = false;

  private:
    enum class IoPhase : std::uint8_t
    {
        idle,
        active,
        application,
        closing
    };

    IoPhase io_phase_ = IoPhase::idle;
};

} // namespace tds::transport
