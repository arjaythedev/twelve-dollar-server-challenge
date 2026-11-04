#include "reactor.hpp"
#include "../diagnostics/io.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <exception>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace tds::net
{
using telemetry::IoTelemetry;

Reactor::Reactor(std::string_view host, std::uint16_t port)
    : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
      stop_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
{
    int enabled = 1;
    if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
    {
        throw std::system_error(errno, std::generic_category(), "reuse address");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const std::string host_string(host);
    if (::inet_pton(AF_INET, host_string.c_str(), &address.sin_addr) != 1)
    {
        throw std::runtime_error("HOST must be an IPv4 address");
    }
    if (::bind(listener_.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) <
            0 ||
        ::listen(listener_.get(), 4096) < 0)
    {
        throw std::system_error(errno, std::generic_category(), "listen");
    }

    socklen_t length = sizeof(address);
    if (::getsockname(listener_.get(), reinterpret_cast<sockaddr *>(&address), &length) < 0)
    {
        throw std::system_error(errno, std::generic_category(), "socket address");
    }
    port_ = ntohs(address.sin_port);
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) < 0)
    {
        throw std::system_error(errno, std::generic_category(), "file limit");
    }
    table_size_ = static_cast<unsigned>(std::min<rlim_t>(limit.rlim_cur, 65535));
    if (!table_size_)
    {
        throw std::runtime_error("file limit leaves no direct descriptor slots");
    }
    rings_.push_back(std::make_unique<Ring>(table_size_, true));
}

Reactor::~Reactor()
{
    // The primary owns wake READs into satellites, so stop it before their storage dies
    rings_.front().reset();
}

void Reactor::start()
{
    accept();
    io_uring_prep_poll_add(rings_.front()->entry(&stop_operation_), stop_.get(), POLLIN);
}

unsigned Reactor::pending_count() const noexcept
{
    unsigned count = 0;
    for (const auto &owner : rings_)
    {
        count += owner->pending;
    }

    return count;
}

void Reactor::begin_turn()
{
    int result;
    do
    {
        result = wait();
    } while (result == -EINTR);
    if (result < 0 && result != -ETIME && result != -EBUSY)
    {
        throw std::system_error(-result, std::generic_category(), "wait");
    }
    turn_count_ = 0;
    accepted_ = false;
    ring_index_ = 0;
    ring_started_ = false;
}

bool Reactor::next(Completion &completion)
{
    completion = Completion{};
    while (ring_index_ < rings_.size())
    {
        auto &owner = *rings_[ring_index_];
        if (!ring_started_)
        {
            IoTelemetry::drain_started(owner);
            // Ring never enables CQE32 or mixed CQEs, so each completion advances one entry
            auto &cq = owner.queue.cq;
            const auto head = *cq.khead;
            const auto ready = io_uring_smp_load_acquire(cq.ktail) - head;
            cq_cursor_ = {cq.cqes, cq.ring_mask, head, head + std::min(ready, 256U)};
            drained_ = 0;
            ring_started_ = true;
        }
        if (cq_cursor_.head == cq_cursor_.tail)
        {
            finish_ring();
            continue;
        }

        auto *cqe = &cq_cursor_.cqes[cq_cursor_.head++ & cq_cursor_.mask];
        --owner.pending;
        auto *operation = static_cast<Operation *>(io_uring_cqe_get_data(cqe));
        ++drained_;
        complete(owner, operation, cqe->res, cqe->flags, completion);

        return true;
    }

    return false;
}

void Reactor::finish_ring()
{
    auto &owner = *rings_[ring_index_];
    // The caller finishes the last CQE, including a full write batch, before CQ advance
    if (drained_)
    {
        io_uring_smp_store_release(owner.queue.cq.khead, cq_cursor_.head);
        turn_count_ += drained_;
    }
    IoTelemetry::drain_finished(owner, drained_);
    if (stopping_)
    {
        advance_shutdown(owner);
    }
    else
    {
        retry_receives(owner);
    }
    ring_started_ = false;
    ++ring_index_;
}

void Reactor::finish_turn()
{
    assert(!ring_started_ && ring_index_ == rings_.size());
    finish_wakeups();
    previous_count_ = turn_count_;
}

bool Reactor::stopping() const noexcept
{
    return stopping_;
}

void Reactor::receive(Connection &connection)
{
    receive(connection, true);
}

void Reactor::receive(Connection &connection, bool poll_first)
{
    connection.start_io(Event::receive);
    auto *request = connection.ring.entry(&connection.operation);
    io_uring_prep_recv(request, static_cast<int>(connection.slot.value), nullptr,
                       ReceiveBuffers::size, 0);
    request->flags |= IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
    request->buf_group = ReceiveBuffers::group;
    if (poll_first)
    {
        request->ioprio |= IORING_RECVSEND_POLL_FIRST;
    }
    IoTelemetry::receive_prepared(connection);
}

void Reactor::send(Connection &connection, msghdr &message)
{
    connection.start_io(Event::send);
    auto *request = connection.ring.entry(&connection.operation);
    io_uring_prep_sendmsg(request, static_cast<int>(connection.slot.value), &message, MSG_NOSIGNAL);
    request->flags |= IOSQE_FIXED_FILE;
}

void Reactor::close(Connection &connection)
{
    if (connection.io_active() || connection.io_closing())
    {
        throw std::logic_error("closing a connection before its original I/O completed");
    }
    connection.start_close();
    // Cursed kernel detail: unregistering a fixed slot can defer socket release behind other I/O
    // Send TCP EOF before close_direct drops the registration
    auto *request = connection.ring.entry(&connection.close_operation);
    io_uring_prep_shutdown(request, static_cast<int>(connection.slot.value), SHUT_RDWR);
    request->flags |= IOSQE_FIXED_FILE;
}

void Reactor::request_stop() const noexcept
{
    const std::uint64_t value = 1;
    ssize_t result;
    do
    {
        result = ::write(stop_.get(), &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN)
    {
        std::terminate();
    }
}

int Reactor::stop_descriptor() const noexcept
{
    return stop_.get();
}

std::uint16_t Reactor::port() const noexcept
{
    return port_;
}

std::span<const std::unique_ptr<Ring>> Reactor::rings() const noexcept
{
    return rings_;
}

void Reactor::accept()
{
    if (stopping_)
    {
        return;
    }
    for (auto &ticket : accept_tickets_)
    {
        if (ticket.pending)
        {
            continue;
        }
        Ring *accepting = nullptr;
        for (const auto &owner : rings_)
        {
            if (owner->connections.has_free_slot())
            {
                accepting = owner.get();
                break;
            }
        }
        if (!accepting)
        {
            if (rings_.size() == max_rings)
            {
                return;
            }
            auto owner = std::make_unique<Ring>(table_size_, false);
            accepting = owner.get();
            rings_.push_back(std::move(owner));
            arm_wake(*accepting);
        }
        ticket.ring = accepting;
        ticket.slot = accepting->connections.reserve();
        ticket.pending = true;
        io_uring_prep_accept_direct(accepting->entry(&ticket.operation), listener_.get(), nullptr,
                                    nullptr, SOCK_NONBLOCK, ticket.slot.value);
    }
}

void Reactor::arm_wake(Ring &owner)
{
    // Let the pending read sleep in io_uring instead of polling and issuing another syscall
    owner.wake_counter = 0;
    io_uring_prep_read(rings_.front()->entry(&owner.wake_operation), owner.notification->get(),
                       &owner.wake_counter, sizeof(owner.wake_counter), 0);
    owner.wake_pending = true;
}

void Reactor::retry_receives(Ring &owner)
{
    if (owner.deferred_receives.empty())
    {
        return;
    }
    const auto limit = static_cast<unsigned>(
        std::min(owner.deferred_receives.size(), static_cast<std::size_t>(256)));
    auto count = owner.receive_buffers->take_retry_credits(limit);
    const auto now = std::chrono::steady_clock::now();
    if (!count)
    {
        if (now < owner.next_receive_probe)
        {
            return;
        }
        // One probe preserves progress when credits were spent on receives that used no buffer
        count = 1;
    }
    owner.next_receive_probe = now + std::chrono::milliseconds(4);
    for (unsigned index = 0; index < count; ++index)
    {
        auto *connection = owner.deferred_receives.front();
        owner.deferred_receives.pop_front();
        // Try deferred receives before arming another poll; empty sockets use normal fallback
        receive(*connection, false);
    }
}

void Reactor::cancel(Ring &owner, Operation &operation)
{
    io_uring_prep_cancel(owner.entry(nullptr), &operation, 0);
}

void Reactor::stop_all()
{
    stopping_ = true;
    for (auto &ticket : accept_tickets_)
    {
        if (ticket.pending)
        {
            cancel(*ticket.ring, ticket.operation);
        }
    }
    for (const auto &owner : rings_)
    {
        owner->deferred_receives.clear();
    }
}

void Reactor::advance_shutdown(Ring &owner)
{
    // Bound cancellation work so an idle population cannot fill the CQ before its next drain
    unsigned count = 0;
    while (owner.shutdown_cursor < owner.connections.size() && count < 128)
    {
        auto *connection = owner.connections.at(owner.shutdown_cursor++);
        if (!connection)
        {
            continue;
        }
        ++count;
        if (connection->io_idle())
        {
            close(*connection);
        }
        else if (connection->io_active())
        {
            cancel(owner, connection->operation);
        }
    }
}

void Reactor::complete_wake(Ring &owner, int result)
{
    owner.wake_pending = false;
    if (result >= 0 && result != static_cast<int>(sizeof(owner.wake_counter)))
    {
        throw std::runtime_error("short ring notification read");
    }
    if (result < 0 && result != -ECANCELED && result != -EINTR)
    {
        throw std::system_error(-result, std::generic_category(), "ring notification read");
    }
    if (!closing_wakeups_)
    {
        arm_wake(owner);
    }
}

void Reactor::complete_accept(AcceptTicket &ticket, int result)
{
    accepted_ = true;
    auto &owner = *ticket.ring;
    ticket.pending = false;
    if (result >= 0)
    {
        auto &connection = owner.connections.create(owner, ticket.slot);
        if (stopping_)
        {
            close(connection);
        }
        else
        {
            receive(connection);
        }
    }
    else
    {
        owner.connections.release(ticket.slot);
        if (result != -ECANCELED && result != -EINTR && result != -EAGAIN && result != -EMFILE &&
            result != -ENFILE)
        {
            throw std::system_error(-result, std::generic_category(), "accept");
        }
    }
    accept();
}

void Reactor::complete_shutdown(Connection &connection, int result)
{
    // A reset or already disconnected peer still needs its fixed slot released
    if (result < 0 && result != -ENOTCONN)
    {
        throw std::system_error(-result, std::generic_category(), "shutdown direct socket");
    }
    // Reuse this stable operation only after its shutdown CQE has arrived
    connection.close_operation.event = Event::close;
    io_uring_prep_close_direct(connection.ring.entry(&connection.close_operation),
                               connection.slot.value);
}

void Reactor::complete_close(Ring &owner, Connection &connection, int result)
{
    if (result < 0)
    {
        throw std::system_error(-result, std::generic_category(), "close direct descriptor");
    }
    owner.connections.retire(connection.slot);
    accept();
}

void Reactor::complete_io(Connection &connection, Event event, int result, unsigned flags,
                          ReceiveBufferLease buffer, Completion &completion)
{
    if (event == Event::send)
    {
        IoTelemetry::send_serviced(completion);
    }
    else
    {
        IoTelemetry::receive_completed(connection, result);
    }
    connection.finish_io();
    if (event == Event::send)
    {
        completion.kind = CompletionKind::sent;
        completion.connection = &connection;
        completion.result = result;
    }
    if (!stopping_ && event == Event::receive && result == -ENOBUFS)
    {
        IoTelemetry::receive_deferred(connection);
        connection.ring.deferred_receives.push_back(&connection);

        return;
    }
    if (stopping_ || result == 0 || (result < 0 && result != -EAGAIN && result != -EINTR))
    {
        close(connection);

        return;
    }
    if (result < 0)
    {
        if (event == Event::receive)
        {
            receive(connection);
        }

        return;
    }
    if (event == Event::receive)
    {
        if (!(flags & IORING_CQE_F_BUFFER) || static_cast<unsigned>(result) > ReceiveBuffers::size)
        {
            throw std::runtime_error("invalid receive buffer result");
        }
        completion.kind = CompletionKind::received;
        completion.connection = &connection;
        completion.result = result;
        completion.buffer = std::move(buffer);
    }
}

void Reactor::complete(Ring &owner, Operation *operation, int result, unsigned flags,
                       Completion &completion)
{
    if (!operation)
    {
        // Anonymous cancel CQEs acknowledge cancellation, never ownership retirement
        return;
    }
    const bool selected = (flags & IORING_CQE_F_BUFFER) != 0;
    const auto id = flags >> IORING_CQE_BUFFER_SHIFT;
    if (selected && id >= ReceiveBuffers::count)
    {
        throw std::runtime_error("invalid receive buffer id");
    }
    const auto length = result > 0 ? static_cast<std::size_t>(result) : 0;
    ReceiveBufferLease buffer(selected ? owner.receive_buffers.get() : nullptr,
                              static_cast<unsigned short>(id), length);
    switch (operation->event)
    {
    case Event::stop:
        stop_all();
        return;
    case Event::wake:
        complete_wake(operation->ring(), result);
        return;
    case Event::accept:
        complete_accept(operation->accept_ticket(), result);
        return;
    case Event::shutdown:
        complete_shutdown(operation->connection(), result);
        return;
    case Event::close:
        complete_close(owner, operation->connection(), result);
        return;
    case Event::receive:
    case Event::send:
        complete_io(operation->connection(), operation->event, result, flags, std::move(buffer),
                    completion);
        return;
    }
}

int Reactor::wait()
{
    // Accept handling can grow rings, but it never runs while wait submits or polls
    const auto owners = rings();
    auto &primary = owners.front()->queue;
    for (std::size_t index = 0; index < owners.size(); ++index)
    {
        const auto &owner = owners[index];
        if (index)
        {
            owner->submit();
        }
    }
    // Cursed: an empty SQ does not mean the primary gets a day off
    // TASKRUN and overflow flags make submit enter GETEVENTS when progress is needed
    for (std::size_t index = 1; index < owners.size(); ++index)
    {
        if (io_uring_cq_ready(&owners[index]->queue))
        {
            return io_uring_submit(&primary);
        }
    }
    const bool immediate = accepted_ || previous_count_ < 2;
    const auto has_deferred_receives = [&owners] {
        for (const auto &owner : owners)
        {
            if (!owner->deferred_receives.empty())
            {
                return true;
            }
        }

        return false;
    };
    // A quiet turn can block only when no deferred receive needs a timed probe
    if (stopping_ || !(primary.features & IORING_FEAT_EXT_ARG) ||
        (immediate && !has_deferred_receives()))
    {
        return io_uring_submit_and_wait(&primary, 1);
    }
    // Exhausted receive queues keep their 4-ms probe even while busy turns collect longer
    const auto wait_ns = !immediate && !has_deferred_receives() ? 8'000'000 : 4'000'000;
    __kernel_timespec timeout{0, wait_ns};
    io_uring_cqe *first = nullptr;

    // Keep sparse retry turns prompt; busy turns collect CQEs for larger write batches
    return io_uring_submit_and_wait_timeout(&primary, &first, immediate ? 1 : 128, &timeout,
                                            nullptr);
}

void Reactor::finish_wakeups()
{
    if (!stopping_ || closing_wakeups_)
    {
        return;
    }
    unsigned polls = 0;
    for (const auto &owner : rings_)
    {
        if (owner->shutdown_cursor != owner->connections.size())
        {
            return;
        }
        polls += static_cast<unsigned>(owner->wake_pending);
    }
    if (pending_count() != polls)
    {
        return;
    }
    // Keep wake targets alive until every original operation and close CQE has drained
    closing_wakeups_ = true;
    for (const auto &owner : rings_)
    {
        if (owner->wake_pending)
        {
            cancel(*rings_.front(), owner->wake_operation);
        }
    }
}

} // namespace tds::net
