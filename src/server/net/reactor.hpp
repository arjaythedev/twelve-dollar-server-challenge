#pragma once

#include "../telemetry/config.hpp"
#include "ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <vector>

namespace tds::net
{

enum class CompletionKind
{
    control,
    received,
    sent
};

struct Completion
{
    CompletionKind kind = CompletionKind::control;
    Connection *connection = nullptr;
    int result = 0;
    ReceiveBufferLease buffer;
#if TDS_TELEMETRY_LEVEL == 2
    std::uint64_t serviced_ns = 0;
#endif
};

class Reactor
{
  public:
    // Create, drive and destroy the reactor on the same issuer thread
    Reactor(std::string_view host, std::uint16_t port);
    Reactor(const Reactor &) = delete;
    Reactor &operator=(const Reactor &) = delete;

    void start();
    unsigned pending_count() const noexcept;
    void begin_turn();
    // Consume the returned completion before next advances or retires another record
    bool next(Completion &completion);
    void finish_turn();

    bool stopping() const noexcept;
    void receive(Connection &connection);
    void send(Connection &connection, msghdr &message);
    void close(Connection &connection);
    void request_stop() const noexcept;
    int stop_descriptor() const noexcept;
    std::uint16_t port() const noexcept;
    std::span<const std::unique_ptr<Ring>> rings() const noexcept;

  private:
    void receive(Connection &connection, bool poll_first);
    void accept();
    void poll_satellite(Ring &owner);
    void retry_receives(Ring &owner);
    void cancel(Ring &owner, Operation &operation);
    void stop_all();
    void advance_shutdown(Ring &owner);
    void complete_wake(Ring &owner, int result);
    void complete_accept(AcceptTicket &ticket, int result);
    void complete_shutdown(Connection &connection, int result);
    void complete_close(Ring &owner, Connection &connection, int result);
    void complete_io(Connection &connection, Event event, int result, unsigned flags,
                     ReceiveBufferLease buffer, Completion &completion);
    void complete(Ring &owner, Operation *operation, int result, unsigned flags,
                  Completion &completion);
    int wait();
    void finish_ring();
    void finish_wakeups();

    struct CqeCursor
    {
        io_uring_cqe *cqes = nullptr;
        unsigned mask = 0;
        unsigned head = 0;
        unsigned tail = 0;
    };

    Descriptor listener_;
    Descriptor stop_;
    static constexpr std::size_t max_rings = telemetry::io_ring_capacity;
    // Tickets outlive the rings that still have their addresses in CQEs
    std::array<AcceptTicket, 64> accept_tickets_;
    std::vector<std::unique_ptr<Ring>> rings_;
    Operation stop_operation_{Event::stop};
    unsigned table_size_ = 0;
    std::uint16_t port_ = 0;
    bool stopping_ = false;
    bool closing_wakeups_ = false;
    unsigned previous_count_ = 0;
    bool accepted_ = false;
    unsigned turn_count_ = 0;
    std::size_t ring_index_ = 0;
    CqeCursor cq_cursor_{};
    unsigned drained_ = 0;
    bool ring_started_ = false;
};

} // namespace tds::net
