#pragma once

#include <cstdint>
#include <memory>

namespace tds::transport
{
struct Exchange;
}

namespace tds::net
{

struct DirectSlot
{
    unsigned value;

    bool operator==(const DirectSlot &) const = default;
};

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
    // The operation kind selects the target without an extra allocation or callback
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
    DirectSlot slot{};
    bool pending = false;
};

struct Connection
{
    Connection(Ring &owner, DirectSlot index);
    ~Connection();
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

    void start_queued_write() noexcept
    {
        io_phase_ = IoPhase::queued_write;
    }

    bool io_idle() const noexcept
    {
        return io_phase_ == IoPhase::idle;
    }

    bool io_active() const noexcept
    {
        return io_phase_ == IoPhase::active;
    }

    bool io_closing() const noexcept
    {
        return io_phase_ == IoPhase::closing;
    }

    Ring &ring;
    Operation operation;
    Operation close_operation;
    std::unique_ptr<transport::Exchange> exchange;
    DirectSlot slot;
    bool close_after = false;

  private:
    enum class IoPhase : std::uint8_t
    {
        idle,
        active,
        queued_write,
        closing
    };

    IoPhase io_phase_ = IoPhase::idle;
};

} // namespace tds::net
