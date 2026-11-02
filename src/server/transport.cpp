#include "transport.hpp"
#include "transport/ring.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace tds
{
namespace
{
using transport::AcceptTicket;
using transport::Connection;
using transport::Descriptor;
using transport::Event;
using transport::Operation;
using transport::ReceiveBufferLease;
using transport::ReceiveBuffers;
using transport::Ring;
} // namespace

class Server::State
{
  public:
    State(std::string_view host, std::uint16_t port, Handler handle, HeaderHandler head)
        : listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
          stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)), handler(std::move(handle)),
          headers(std::move(head))
    {
        int enabled = 1;
        if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
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
        if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            ::listen(listener.get(), 4096) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "listen");
        }

        socklen_t length = sizeof(address);
        if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &length) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "socket address");
        }
        bound_port = ntohs(address.sin_port);
        rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "file limit");
        }
        table_size = static_cast<unsigned>(std::min<rlim_t>(limit.rlim_cur, 65535));
        if (!table_size)
        {
            throw std::runtime_error("file limit leaves no direct descriptor slots");
        }
        rings.push_back(std::make_unique<Ring>(table_size, true));
    }

    void request_stop() const noexcept
    {
        const std::uint64_t value = 1;
        ssize_t result = 0;
        do
        {
            result = ::write(stop.get(), &value, sizeof(value));
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno != EAGAIN)
        {
            std::terminate();
        }
    }

    [[nodiscard]] int stop_descriptor() const noexcept
    {
        return stop.get();
    }

    [[nodiscard]] std::uint16_t port() const noexcept
    {
        return bound_port;
    }

    void run()
    {
        accept();
        io_uring_prep_poll_add(rings.front()->entry(&stop_operation), stop.get(), POLLIN);
        unsigned previous_count = 0;
        bool accepted = false;
        while (pending_count())
        {
            const int result = wait(previous_count, accepted);
            if (result == -EINTR)
            {
                continue;
            }
            if (result < 0 && result != -ETIME && result != -EBUSY)
            {
                throw std::system_error(-result, std::generic_category(), "wait");
            }
            unsigned count = 0;
            accepted = false;
            // A busy ring cannot monopolize handlers or delay another ring's close CQEs
            for (std::size_t index = 0; index < rings.size(); ++index)
            {
                auto &owner = *rings[index];
                unsigned head = 0;
                unsigned drained = 0;
                io_uring_cqe *completion = nullptr;
                io_uring_for_each_cqe(&owner.queue, head, completion)
                {
                    --owner.pending;
                    auto *operation = static_cast<Operation *>(io_uring_cqe_get_data(completion));
                    accepted = accepted || (operation && operation->event == Event::accept);
                    complete(owner, operation, completion->res, completion->flags);
                    if (++drained == 256)
                    {
                        break;
                    }
                }
                io_uring_cq_advance(&owner.queue, drained);
                count += drained;
                if (!stopping)
                {
                    retry_receives(owner);
                }
                else
                {
                    advance_shutdown(owner);
                }
            }
            finish_wakeups();
            previous_count = count;
        }
    }

  private:
    void poll_satellite(Ring &owner)
    {
        io_uring_prep_poll_add(rings.front()->entry(&owner.wake_operation),
                               owner.notification->get(), POLLIN);
        owner.wake_pending = true;
    }

    void accept()
    {
        if (stopping)
        {
            return;
        }
        for (auto &ticket : accept_tickets)
        {
            if (ticket.pending)
            {
                continue;
            }
            Ring *accepting = nullptr;
            for (const auto &owner : rings)
            {
                if (!owner->free_slots.empty())
                {
                    accepting = owner.get();
                    break;
                }
            }
            if (!accepting)
            {
                if (rings.size() == max_rings)
                {
                    return;
                }
                auto owner = std::make_unique<Ring>(table_size, false);
                accepting = owner.get();
                rings.push_back(std::move(owner));
                poll_satellite(*accepting);
            }
            ticket.ring = accepting;
            ticket.slot = accepting->free_slots.back();
            accepting->free_slots.pop_back();
            ticket.pending = true;
            io_uring_prep_accept_direct(accepting->entry(&ticket.operation), listener.get(),
                                        nullptr, nullptr, SOCK_NONBLOCK, ticket.slot);
        }
    }

    void receive(Connection &connection)
    {
        connection.start_io(Event::receive);
        auto *request = connection.ring.entry(&connection.operation);
        io_uring_prep_recv(request, static_cast<int>(connection.slot), nullptr,
                           ReceiveBuffers::size, 0);
        request->flags |= IOSQE_BUFFER_SELECT | IOSQE_FIXED_FILE;
        request->buf_group = ReceiveBuffers::group;
        request->ioprio |= IORING_RECVSEND_POLL_FIRST;
    }

    void retry_receives(Ring &owner)
    {
        const auto count = std::min(owner.deferred_receives.size(), static_cast<std::size_t>(256));
        for (std::size_t i = 0; i < count; ++i)
        {
            auto *connection = owner.deferred_receives.front();
            owner.deferred_receives.pop_front();
            receive(*connection);
        }
    }

    void send(Connection &connection)
    {
        auto &message = connection.exchange->prepare_send();
        connection.start_io(Event::send);
        auto *request = connection.ring.entry(&connection.operation);
        io_uring_prep_sendmsg(request, static_cast<int>(connection.slot), &message, MSG_NOSIGNAL);
        request->flags |= IOSQE_FIXED_FILE;
    }

    void respond(Connection &connection, Response response)
    {
        connection.close_after =
            connection.close_after || !connection.exchange->parser.request().keep_alive;
        connection.exchange->prepare_response(std::move(response), connection.close_after);
        send(connection);
    }

    void process(Connection &connection)
    {
        auto &exchange = *connection.exchange;
        auto &parser = exchange.parser;
        try
        {
            while (true)
            {
                const auto result = parser.parse(exchange.input);
                if (!result.valid)
                {
                    connection.close_after = true;
                    respond(connection, {parser.error(), "{\"error\":\"malformed HTTP request\"}"});

                    return;
                }
                exchange.input.erase(0, result.consumed);

                if (parser.headers_pending())
                {
                    if (auto rejected = headers(parser.request()))
                    {
                        // Unread body bytes cannot become the next keep-alive request
                        connection.close_after = true;
                        respond(connection, std::move(*rejected));

                        return;
                    }
                    parser.accept_headers();
                    continue;
                }
                if (parser.complete())
                {
                    respond(connection, handler(parser.request()));
                }
                else
                {
                    receive(connection);
                }

                return;
            }
        }
        catch (const std::exception &error)
        {
            std::cerr << "request failed: " << error.what() << '\n';
            connection.close_after = true;
            respond(connection, {Status::internal_error, "{\"error\":\"internal server error\"}"});
        }
    }

    void close(Connection &connection)
    {
        connection.start_close();
        // Yes, this is cursed: close_direct takes a fixed slot but rejects IOSQE_FIXED_FILE
        io_uring_prep_close_direct(connection.ring.entry(&connection.close_operation),
                                   connection.slot);
    }

    void cancel(Ring &owner, Operation &operation)
    {
        io_uring_prep_cancel(owner.entry(nullptr), &operation, 0);
    }

    void stop_all()
    {
        stopping = true;
        for (auto &ticket : accept_tickets)
        {
            if (ticket.pending)
            {
                cancel(*ticket.ring, ticket.operation);
            }
        }
        for (const auto &owner : rings)
        {
            owner->deferred_receives.clear();
        }
    }

    void advance_shutdown(Ring &owner)
    {
        // Bound cancellation work so a large idle table cannot fill the CQ before it is drained
        unsigned count = 0;
        while (owner.shutdown_cursor < owner.connections.size() && count < 128)
        {
            const auto &connection = owner.connections[owner.shutdown_cursor++];
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

    void complete_wake(Ring &satellite, int result)
    {
        satellite.wake_pending = false;
        if (result >= 0)
        {
            std::uint64_t notifications = 0;
            ssize_t read;
            do
            {
                read = ::read(satellite.notification->get(), &notifications, sizeof(notifications));
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
        if (!closing_wakeups)
        {
            poll_satellite(satellite);
        }
    }

    void complete_accept(AcceptTicket &ticket, int result)
    {
        auto &owner = *ticket.ring;
        ticket.pending = false;
        if (result >= 0)
        {
            auto connection = std::make_unique<Connection>(owner, ticket.slot);
            auto &value = *connection;
            owner.connections[ticket.slot] = std::move(connection);
            if (stopping)
            {
                close(value);
            }
            else
            {
                receive(value);
            }
        }
        else
        {
            owner.free_slots.push_back(ticket.slot);
            if (result != -ECANCELED && result != -EINTR && result != -EAGAIN &&
                result != -EMFILE && result != -ENFILE)
            {
                throw std::system_error(-result, std::generic_category(), "accept");
            }
        }
        accept();
    }

    void complete_close(Ring &owner, Connection &connection, int result)
    {
        if (result < 0)
        {
            throw std::system_error(-result, std::generic_category(), "close direct descriptor");
        }
        const unsigned slot = connection.slot;
        owner.connections[slot].reset();
        owner.free_slots.push_back(slot);
        accept();
    }

    void complete_receive(Connection &connection, int result, unsigned flags,
                          ReceiveBufferLease &buffer)
    {
        if (!(flags & IORING_CQE_F_BUFFER) || static_cast<unsigned>(result) > ReceiveBuffers::size)
        {
            throw std::runtime_error("invalid receive buffer result");
        }
        const auto id = flags >> IORING_CQE_BUFFER_SHIFT;
        if (!connection.exchange)
        {
            connection.exchange = connection.ring.acquire_exchange();
        }
        connection.exchange->input.append(connection.ring.receive_buffers->data(id),
                                          static_cast<std::size_t>(result));
        buffer.release();
        process(connection);
    }

    void complete_send(Connection &connection, int result)
    {
        auto &exchange = *connection.exchange;
        exchange.sent += static_cast<std::size_t>(result);
        if (exchange.sent < exchange.output.size() + exchange.response_body.size())
        {
            send(connection);

            return;
        }
        if (connection.close_after)
        {
            close(connection);

            return;
        }
        exchange.finish_response();
        if (!exchange.input.empty())
        {
            process(connection);

            return;
        }
        connection.ring.recycle_exchange(std::move(connection.exchange));
        receive(connection);
    }

    void complete_io(Connection &connection, Event event, int result, unsigned flags,
                     ReceiveBufferLease &buffer)
    {
        connection.finish_io();
        if (!stopping && event == Event::receive && result == -ENOBUFS)
        {
            connection.ring.deferred_receives.push_back(&connection);

            return;
        }
        if (stopping || result == 0 || (result < 0 && result != -EAGAIN && result != -EINTR))
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
            else
            {
                send(connection);
            }

            return;
        }
        if (event == Event::receive)
        {
            complete_receive(connection, result, flags, buffer);
        }
        else
        {
            complete_send(connection, result);
        }
    }

    void complete(Ring &owner, Operation *operation, int result, unsigned flags)
    {
        if (!operation)
        {
            // Only anonymous cancel completions use a null operation, they own no receive buffer
            return;
        }
        const bool selected = (flags & IORING_CQE_F_BUFFER) != 0;
        const auto id = flags >> IORING_CQE_BUFFER_SHIFT;
        if (selected && id >= ReceiveBuffers::count)
        {
            throw std::runtime_error("invalid receive buffer id");
        }
        ReceiveBufferLease buffer(selected ? owner.receive_buffers.get() : nullptr,
                                  static_cast<unsigned short>(id));
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
            complete_io(operation->connection(), operation->event, result, flags, buffer);
            return;
        }
    }

    int wait(unsigned previous_count, bool accepted)
    {
        auto &primary = rings.front()->queue;
        bool deferred = false;
        for (std::size_t index = 0; index < rings.size(); ++index)
        {
            const auto &owner = rings[index];
            if (index)
            {
                owner->submit();
            }
            deferred = deferred || !owner->deferred_receives.empty();
        }
        // Satellite CQEs cannot count toward the primary ring's blocking wait
        // Submit the primary too before a ready satellite triggers another fair drain
        for (std::size_t index = 1; index < rings.size(); ++index)
        {
            if (io_uring_cq_ready(&rings[index]->queue))
            {
                rings.front()->submit();
                return 0;
            }
        }
        // Rearm accepts promptly; EXT_ARG avoids synthetic timeout completions
        const bool immediate = accepted || previous_count < 2;
        if (stopping || !(primary.features & IORING_FEAT_EXT_ARG) || (immediate && !deferred))
        {
            return io_uring_submit_and_wait(&primary, 1);
        }
        __kernel_timespec timeout{0, 4'000'000};
        io_uring_cqe *first = nullptr;

        return io_uring_submit_and_wait_timeout(&primary, &first, immediate ? 1 : 32, &timeout,
                                                nullptr);
    }

    unsigned pending_count() const noexcept
    {
        unsigned count = 0;
        for (const auto &owner : rings)
        {
            count += owner->pending;
        }
        return count;
    }

    void finish_wakeups()
    {
        if (!stopping || closing_wakeups)
        {
            return;
        }
        unsigned polls = 0;
        for (const auto &owner : rings)
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
        // Satellite wakes remain armed until every original and close CQE has drained
        closing_wakeups = true;
        for (const auto &owner : rings)
        {
            if (owner->wake_pending)
            {
                cancel(*rings.front(), owner->wake_operation);
            }
        }
    }

    Descriptor listener;
    Descriptor stop;
    Handler handler;
    HeaderHandler headers;
    static constexpr std::size_t max_rings = 16;
    // Ticket targets remain alive until every owning ring has exited
    std::array<AcceptTicket, 64> accept_tickets;
    std::vector<std::unique_ptr<Ring>> rings;
    Operation stop_operation{Event::stop};
    unsigned table_size = 0;
    std::uint16_t bound_port = 0;
    bool stopping = false;
    bool closing_wakeups = false;
};

Server::Server(std::string_view host, std::uint16_t port, Handler handler, HeaderHandler headers)
    : state_(std::make_unique<State>(host, port, std::move(handler), std::move(headers)))
{
}

Server::~Server() = default;

void Server::run()
{
    state_->run();
}

void Server::request_stop() const noexcept
{
    state_->request_stop();
}

int Server::stop_descriptor() const noexcept
{
    return state_->stop_descriptor();
}

std::uint16_t Server::port() const noexcept
{
    return state_->port();
}

} // namespace tds
