#include "reactor.hpp"

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
#ifdef TDS_DIAGNOSTICS
            const auto depth = io_uring_cq_ready(&owner.queue);
            owner.diagnostics.cq_depth.record(depth);
            owner.diagnostics.empty_cq_samples += depth == 0;
            owner.diagnostics.overflow_flag_samples +=
                (io_uring_smp_load_acquire(owner.queue.sq.kflags) & IORING_SQ_CQ_OVERFLOW) != 0;
#endif
            cq_iterator_ = io_uring_cqe_iter_init(&owner.queue);
            drained_ = 0;
            ring_started_ = true;
        }
        io_uring_cqe *cqe = nullptr;
        if (drained_ == 256 || !io_uring_cqe_iter_next(&cq_iterator_, &cqe))
        {
            finish_ring();
            continue;
        }

        --owner.pending;
        auto *operation = static_cast<Operation *>(io_uring_cqe_get_data(cqe));
        accepted_ = accepted_ || (operation && operation->event == Event::accept);
        ++drained_;
        ++turn_count_;
        complete(owner, operation, cqe->res, cqe->flags, completion);

        return true;
    }

    return false;
}

void Reactor::finish_ring()
{
    auto &owner = *rings_[ring_index_];
    // The caller finishes the last CQE, including a full write batch, before CQ advance
    io_uring_cq_advance(&owner.queue, drained_);
#ifdef TDS_DIAGNOSTICS
    owner.diagnostics.cqes += drained_;
    owner.diagnostics.deferred_receive_depth.record(owner.deferred_receives.size());
#endif
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
    connection.start_io(Event::receive);
    auto *request = connection.ring.entry(&connection.operation);
    io_uring_prep_recv(request, static_cast<int>(connection.slot.value), nullptr,
                       ReceiveBuffers::size, 0);
    request->flags |= IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
    request->buf_group = ReceiveBuffers::group;
    request->ioprio |= IORING_RECVSEND_POLL_FIRST;
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
    // Cursed but required: close_direct takes a fixed slot and rejects IOSQE_FIXED_FILE
    io_uring_prep_close_direct(connection.ring.entry(&connection.close_operation),
                               connection.slot.value);
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
            poll_satellite(*accepting);
        }
        ticket.ring = accepting;
        ticket.slot = accepting->connections.reserve();
        ticket.pending = true;
        io_uring_prep_accept_direct(accepting->entry(&ticket.operation), listener_.get(), nullptr,
                                    nullptr, SOCK_NONBLOCK, ticket.slot.value);
    }
}

void Reactor::poll_satellite(Ring &owner)
{
    io_uring_prep_poll_add(rings_.front()->entry(&owner.wake_operation), owner.notification->get(),
                           POLLIN);
    owner.wake_pending = true;
}

void Reactor::retry_receives(Ring &owner)
{
    const auto count = std::min(owner.deferred_receives.size(), static_cast<std::size_t>(256));
    for (std::size_t index = 0; index < count; ++index)
    {
        auto *connection = owner.deferred_receives.front();
        owner.deferred_receives.pop_front();
        receive(*connection);
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
    if (result >= 0)
    {
        std::uint64_t notifications = 0;
        ssize_t read;
        do
        {
            read = ::read(owner.notification->get(), &notifications, sizeof(notifications));
        } while (read < 0 && errno == EINTR);
        if (read < 0 && errno != EAGAIN)
        {
            throw std::system_error(errno, std::generic_category(), "ring notification");
        }
    }
    else if (result != -ECANCELED && result != -EINTR)
    {
        throw std::system_error(-result, std::generic_category(), "ring poll");
    }
    if (!closing_wakeups_)
    {
        poll_satellite(owner);
    }
}

void Reactor::complete_accept(AcceptTicket &ticket, int result)
{
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
#ifdef TDS_DIAGNOSTICS
    if (event == Event::send)
    {
        // Close preparation may submit SQEs, so measure service before that extra work
        completion.serviced_ns = timing::now_ns();
    }
    if (event == Event::receive && result == -ENOBUFS)
    {
        ++connection.ring.diagnostics.receive_enobufs;
    }
#endif
    connection.finish_io();
    if (event == Event::send)
    {
        completion.kind = CompletionKind::sent;
        completion.connection = &connection;
        completion.result = result;
    }
    if (!stopping_ && event == Event::receive && result == -ENOBUFS)
    {
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
    auto &primary = rings_.front()->queue;
    bool deferred = false;
    for (std::size_t index = 0; index < rings_.size(); ++index)
    {
        const auto &owner = rings_[index];
        if (index)
        {
            owner->submit();
        }
        deferred = deferred || !owner->deferred_receives.empty();
    }
    // A ready satellite bypasses the primary wait, after all pending sends are submitted
    for (std::size_t index = 1; index < rings_.size(); ++index)
    {
        if (io_uring_cq_ready(&rings_[index]->queue))
        {
            rings_.front()->submit();

            return 0;
        }
    }
    const bool immediate = accepted_ || previous_count_ < 2;
    if (stopping_ || !(primary.features & IORING_FEAT_EXT_ARG) || (immediate && !deferred))
    {
        return io_uring_submit_and_wait(&primary, 1);
    }
    __kernel_timespec timeout{0, 4'000'000};
    io_uring_cqe *first = nullptr;

    return io_uring_submit_and_wait_timeout(&primary, &first, immediate ? 1 : 32, &timeout,
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
