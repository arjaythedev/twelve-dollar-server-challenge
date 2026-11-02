#include "transport.hpp"
#include "parsing.hpp"

#include <liburing.h>
#include <llhttp.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tds
{
namespace
{

class Descriptor
{
  public:
    explicit Descriptor(int value) : value_(value)
    {
        if (value < 0)
        {
            throw std::system_error(errno, std::generic_category());
        }
    }

    ~Descriptor()
    {
        ::close(value_);
    }

    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;

    [[nodiscard]] int get() const noexcept
    {
        return value_;
    }

  private:
    int value_;
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

struct Parser
{
    llhttp_t parser{};
    llhttp_settings_t settings{};
    Request request;
    std::string url;
    std::string field;
    std::string value;
    bool complete = false;
    bool headers_complete = false;
    bool headers_accepted = false;
    Status error = Status::bad_request;

    Parser()
    {
        llhttp_settings_init(&settings);
        settings.on_url = [](llhttp_t *p, const char *data, std::size_t length) {
            auto &self = *static_cast<Parser *>(p->data);
            self.url.append(data, length);
            return self.url.size() > 16384 ? -1 : 0;
        };
        settings.on_header_field = [](llhttp_t *p, const char *data, std::size_t length) {
            auto &self = *static_cast<Parser *>(p->data);
            self.field.append(data, length);
            return self.field.size() > 16384 ? -1 : 0;
        };
        settings.on_header_value = [](llhttp_t *p, const char *data, std::size_t length) {
            auto &self = *static_cast<Parser *>(p->data);
            self.value.append(data, length);
            return self.value.size() > 16384 ? -1 : 0;
        };
        settings.on_header_value_complete = [](llhttp_t *p) {
            auto &self = *static_cast<Parser *>(p->data);
            for (char &c : self.field)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c + ('a' - 'A'));
                }
            }
            if (self.field == "authorization")
            {
                self.request.authorization = self.value;
            }
            self.field.clear();
            self.value.clear();
            return 0;
        };
        settings.on_headers_complete = [](llhttp_t *p) {
            auto &self = *static_cast<Parser *>(p->data);
            self.headers_complete = true;
            self.request.method = llhttp_method_name(static_cast<llhttp_method_t>(p->method));
            self.request.keep_alive = llhttp_should_keep_alive(p) != 0;
            const auto end = self.url.find('?');
            const std::string_view path(self.url.data(),
                                        end == std::string::npos ? self.url.size() : end);
            for (std::size_t i = 0; i < path.size(); ++i)
            {
                if (path[i] == '%' && i + 2 < path.size())
                {
                    const int high = parsing::hex_digit(path[i + 1]);
                    const int low = parsing::hex_digit(path[i + 2]);
                    if (high < 0 || low < 0)
                    {
                        return -1;
                    }
                    self.request.path.push_back(static_cast<char>(high * 16 + low));
                    i += 2;
                }
                else
                {
                    self.request.path.push_back(path[i]);
                }
            }
            if (p->content_length != ULLONG_MAX && p->content_length > 16384)
            {
                self.error = Status::too_large;
                return -1;
            }
            return static_cast<int>(HPE_PAUSED);
        };
        settings.on_body = [](llhttp_t *p, const char *data, std::size_t length) {
            auto &self = *static_cast<Parser *>(p->data);
            if (self.request.body.size() + length > 16384)
            {
                self.error = Status::too_large;
                return -1;
            }
            self.request.body.append(data, length);
            return 0;
        };
        settings.on_message_complete = [](llhttp_t *p) {
            static_cast<Parser *>(p->data)->complete = true;
            return static_cast<int>(HPE_PAUSED);
        };
        reset();
    }

    void reset()
    {
        request = {};
        url.clear();
        field.clear();
        value.clear();
        complete = false;
        headers_complete = false;
        headers_accepted = false;
        error = Status::bad_request;
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        parser.data = this;
    }
};

std::string_view reason(Status status)
{
    switch (status)
    {
    case Status::ok:
        return "OK";
    case Status::created:
        return "Created";
    case Status::bad_request:
        return "Bad Request";
    case Status::unauthorized:
        return "Unauthorized";
    case Status::not_found:
        return "Not Found";
    case Status::too_large:
        return "Payload Too Large";
    case Status::unavailable:
        return "Service Unavailable";
    default:
        return "Internal Server Error";
    }
}

} // namespace

class Server::State
{
  public:
    class ReceiveBuffers
    {
      public:
        static constexpr unsigned count = 1024;
        static constexpr unsigned size = 4096;
        static constexpr int group = 0;

        explicit ReceiveBuffers(io_uring &owner)
            : ring(owner), storage(std::make_unique_for_overwrite<char[]>(count * size))
        {
            int error = 0;
            buffers = io_uring_setup_buf_ring(&ring, count, group, 0, &error);
            if (!buffers)
            {
                throw std::system_error(-error, std::generic_category(), "receive buffer ring");
            }
            for (unsigned id = 0; id < count; ++id)
            {
                io_uring_buf_ring_add(buffers, data(id), size, static_cast<unsigned short>(id),
                                      io_uring_buf_ring_mask(count), static_cast<int>(id));
            }
            io_uring_buf_ring_advance(buffers, count);
        }

        ~ReceiveBuffers()
        {
            if (io_uring_free_buf_ring(&ring, buffers, count, group) < 0)
            {
                std::terminate();
            }
        }

        ReceiveBuffers(const ReceiveBuffers &) = delete;
        ReceiveBuffers &operator=(const ReceiveBuffers &) = delete;

        char *data(unsigned id) noexcept
        {
            return storage.get() + id * size;
        }

        void release(unsigned short id) noexcept
        {
            io_uring_buf_ring_add(buffers, data(id), size, id, io_uring_buf_ring_mask(count), 0);
            io_uring_buf_ring_advance(buffers, 1);
        }

      private:
        io_uring &ring;
        std::unique_ptr<char[]> storage;
        io_uring_buf_ring *buffers = nullptr;
    };

    struct Connection;
    class Ring;

    struct Operation
    {
        Event event;
        Connection *connection = nullptr;
        Ring *ring = nullptr;
    };

    struct Connection
    {
        Connection(Ring &owner, unsigned index)
            : ring(owner), slot(index), operation{Event::receive, this},
              close_operation{Event::close, this}
        {
        }

        Ring &ring;
        unsigned slot;
        Operation operation;
        Operation close_operation;
        Parser parser;
        std::string input;
        std::string output;
        std::string response_body;
        std::array<iovec, 2> send_vectors{};
        msghdr send_message{};
        std::size_t sent = 0;
        bool close_after = false;
        bool pending = false;
        bool closing = false;
    };

    class Ring
    {
      public:
        Ring(unsigned slots, bool primary)
            : connections(slots), accept_operation{Event::accept, nullptr, this},
              wake_operation{Event::wake, nullptr, this}
        {
            free_slots.reserve(slots);
            for (unsigned index = slots; index > 0; --index)
            {
                free_slots.push_back(index - 1);
            }
            if (!primary)
            {
                notification =
                    std::make_unique<Descriptor>(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
            }
            const int result = io_uring_queue_init(1024, &queue, 0);
            if (result < 0)
            {
                throw std::system_error(-result, std::generic_category(), "io_uring setup");
            }
            try
            {
                const int registered = io_uring_register_files_sparse(&queue, slots);
                if (registered < 0)
                {
                    throw std::system_error(-registered, std::generic_category(),
                                            "fixed file table");
                }
                receive_buffers = std::make_unique<ReceiveBuffers>(queue);
                if (notification)
                {
                    const int notified = io_uring_register_eventfd(&queue, notification->get());
                    if (notified < 0)
                    {
                        throw std::system_error(-notified, std::generic_category(), "ring eventfd");
                    }
                }
            }
            catch (...)
            {
                receive_buffers.reset();
                io_uring_queue_exit(&queue);
                throw;
            }
        }

        ~Ring()
        {
            // Fatal exits must stop kernel access before any connection storage is freed.
            if (pending)
            {
                io_uring_sync_cancel_reg request{};
                request.flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL;
                request.timeout.tv_sec = 5;
                const int result = io_uring_register_sync_cancel(&queue, &request);
                if (result < 0 && result != -ENOENT)
                {
                    std::terminate();
                }
            }
            receive_buffers.reset();
            io_uring_queue_exit(&queue);
        }

        Ring(const Ring &) = delete;
        Ring &operator=(const Ring &) = delete;

        void submit()
        {
            int result;
            do
            {
                result = io_uring_submit(&queue);
            } while (result == -EINTR);
            // NODROP retains overflowed CQEs. Drain the CQ before trying another enter.
            if (result < 0 && result != -EBUSY)
            {
                throw std::system_error(-result, std::generic_category(), "submit");
            }
        }

        io_uring_sqe *entry(Operation *operation)
        {
            auto *value = io_uring_get_sqe(&queue);
            if (!value)
            {
                submit();
                value = io_uring_get_sqe(&queue);
                if (!value)
                {
                    throw std::runtime_error("submission queue exhausted");
                }
            }
            io_uring_sqe_set_data(value, operation);
            ++pending;
            return value;
        }

        io_uring queue{};
        std::unique_ptr<ReceiveBuffers> receive_buffers;
        std::unique_ptr<Descriptor> notification;
        std::vector<std::unique_ptr<Connection>> connections;
        std::vector<unsigned> free_slots;
        // Deferred connections have no operation in flight and remain owned by connections.
        std::deque<Connection *> deferred_receives;
        Operation accept_operation;
        Operation wake_operation;
        unsigned accept_slot = 0;
        unsigned pending = 0;
        std::size_t shutdown_cursor = 0;
        bool wake_pending = false;
    };

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

    void poll_satellite(Ring &owner)
    {
        io_uring_prep_poll_add(rings.front()->entry(&owner.wake_operation),
                               owner.notification->get(), POLLIN);
        owner.wake_pending = true;
    }

    void accept()
    {
        if (stopping || accepting)
        {
            return;
        }
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
        accepting->accept_slot = accepting->free_slots.back();
        accepting->free_slots.pop_back();
        io_uring_prep_accept_direct(accepting->entry(&accepting->accept_operation), listener.get(),
                                    nullptr, nullptr, SOCK_NONBLOCK, accepting->accept_slot);
    }

    void receive(Connection &connection)
    {
        connection.operation.event = Event::receive;
        connection.pending = true;
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
        std::size_t count = 0;
        std::size_t body_offset = 0;
        if (connection.sent < connection.output.size())
        {
            connection.send_vectors[count++] = {connection.output.data() + connection.sent,
                                                connection.output.size() - connection.sent};
        }
        else
        {
            body_offset = connection.sent - connection.output.size();
        }
        if (body_offset < connection.response_body.size())
        {
            connection.send_vectors[count++] = {connection.response_body.data() + body_offset,
                                                connection.response_body.size() - body_offset};
        }
        connection.send_message.msg_iov = connection.send_vectors.data();
        connection.send_message.msg_iovlen = count;
        connection.operation.event = Event::send;
        connection.pending = true;
        auto *request = connection.ring.entry(&connection.operation);
        io_uring_prep_sendmsg(request, static_cast<int>(connection.slot), &connection.send_message,
                              MSG_NOSIGNAL);
        request->flags |= IOSQE_FIXED_FILE;
    }

    void respond(Connection &connection, Response response)
    {
        connection.close_after = connection.close_after || !connection.parser.request.keep_alive;
        auto &output = connection.output;
        output.clear();
        output += "HTTP/1.1 ";
        output += std::to_string(static_cast<unsigned>(response.status));
        output += ' ';
        output += reason(response.status);
        output += "\r\nContent-Type: application/json\r\nContent-Length: ";
        output += std::to_string(response.body.size());
        output += "\r\n";
        if (connection.close_after)
        {
            output += "Connection: close\r\n";
        }
        output += "\r\n";
        connection.response_body = std::move(response.body);
        connection.sent = 0;
        send(connection);
    }

    void process(Connection &connection)
    {
        auto &parser = connection.parser;
        try
        {
            while (true)
            {
                const auto result = llhttp_execute(&parser.parser, connection.input.data(),
                                                   connection.input.size());
                std::size_t consumed = connection.input.size();
                if (result == HPE_PAUSED)
                {
                    consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser.parser) -
                                                        connection.input.data());
                }
                else if (result != HPE_OK)
                {
                    connection.close_after = true;
                    respond(connection, {parser.error, "{\"error\":\"malformed HTTP request\"}"});

                    return;
                }
                connection.input.erase(0, consumed);

                if (parser.headers_complete && !parser.headers_accepted)
                {
                    if (auto rejected = headers(parser.request))
                    {
                        // Unread body bytes cannot become the next keep-alive request.
                        connection.close_after = true;
                        respond(connection, std::move(*rejected));

                        return;
                    }
                    parser.headers_accepted = true;
                    llhttp_resume(&parser.parser);
                    continue;
                }
                if (parser.complete)
                {
                    respond(connection, handler(parser.request));
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
        connection.closing = true;
        connection.pending = true;
        // close_direct takes a slot but must not carry IOSQE_FIXED_FILE.
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
        if (accepting)
        {
            cancel(*accepting, accepting->accept_operation);
        }
        for (const auto &owner : rings)
        {
            owner->deferred_receives.clear();
        }
    }

    void advance_shutdown(Ring &owner)
    {
        // Bound cancellation work so a large idle table cannot fill the CQ before it is drained.
        unsigned count = 0;
        while (owner.shutdown_cursor < owner.connections.size() && count < 128)
        {
            const auto &connection = owner.connections[owner.shutdown_cursor++];
            if (!connection)
            {
                continue;
            }
            ++count;
            if (!connection->pending)
            {
                close(*connection);
            }
            else if (!connection->closing)
            {
                cancel(owner, connection->operation);
            }
        }
    }

    void complete(Ring &owner, Operation *operation, int result, unsigned flags)
    {
        const bool selected = (flags & IORING_CQE_F_BUFFER) != 0;
        const auto id = flags >> IORING_CQE_BUFFER_SHIFT;
        if (selected && id >= ReceiveBuffers::count)
        {
            throw std::runtime_error("invalid receive buffer id");
        }

        struct ReturnBuffer
        {
            ReceiveBuffers *owner;
            unsigned short id;

            ~ReturnBuffer()
            {
                release();
            }

            void release() noexcept
            {
                if (owner)
                {
                    owner->release(id);
                    owner = nullptr;
                }
            }
        } buffer{selected ? owner.receive_buffers.get() : nullptr, static_cast<unsigned short>(id)};

        if (!operation)
        {
            return;
        }
        if (operation->event == Event::stop)
        {
            stop_all();
            return;
        }
        if (operation->event == Event::wake)
        {
            auto &satellite = *operation->ring;
            satellite.wake_pending = false;
            if (result >= 0)
            {
                std::uint64_t notifications = 0;
                ssize_t read;
                do
                {
                    read = ::read(satellite.notification->get(), &notifications,
                                  sizeof(notifications));
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
            return;
        }
        if (operation->event == Event::accept)
        {
            accepting = nullptr;
            if (result >= 0)
            {
                auto connection = std::make_unique<Connection>(owner, owner.accept_slot);
                auto &value = *connection;
                owner.connections[owner.accept_slot] = std::move(connection);
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
                owner.free_slots.push_back(owner.accept_slot);
                if (result != -ECANCELED && result != -EINTR && result != -EAGAIN &&
                    result != -EMFILE && result != -ENFILE)
                {
                    throw std::system_error(-result, std::generic_category(), "accept");
                }
            }
            accept();
            return;
        }
        auto &connection = *operation->connection;
        connection.pending = false;
        if (operation->event == Event::close)
        {
            if (result < 0)
            {
                throw std::system_error(-result, std::generic_category(),
                                        "close direct descriptor");
            }
            const unsigned slot = connection.slot;
            owner.connections[slot].reset();
            owner.free_slots.push_back(slot);
            accept();
            return;
        }
        if (!stopping && operation->event == Event::receive && result == -ENOBUFS)
        {
            owner.deferred_receives.push_back(&connection);
            return;
        }
        if (stopping || result == 0 || (result < 0 && result != -EAGAIN && result != -EINTR))
        {
            close(connection);
            return;
        }
        if (result < 0)
        {
            if (operation->event == Event::receive)
            {
                receive(connection);
            }
            else
            {
                send(connection);
            }
        }
        else if (operation->event == Event::receive)
        {
            if (!selected || static_cast<unsigned>(result) > ReceiveBuffers::size)
            {
                throw std::runtime_error("invalid receive buffer result");
            }
            connection.input.append(owner.receive_buffers->data(id),
                                    static_cast<std::size_t>(result));
            buffer.release();
            process(connection);
        }
        else
        {
            connection.sent += static_cast<std::size_t>(result);
            if (connection.sent < connection.output.size() + connection.response_body.size())
            {
                send(connection);
            }
            else if (connection.close_after)
            {
                close(connection);
            }
            else
            {
                connection.output.clear();
                std::string{}.swap(connection.response_body);
                connection.parser.reset();
                if (connection.input.empty())
                {
                    receive(connection);
                }
                else
                {
                    process(connection);
                }
            }
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
        // Satellite CQEs cannot count toward the primary ring's blocking wait.
        // Submit the primary too before a ready satellite triggers another fair drain.
        for (std::size_t index = 1; index < rings.size(); ++index)
        {
            if (io_uring_cq_ready(&rings[index]->queue))
            {
                rings.front()->submit();
                return 0;
            }
        }
        // Rearm accepts promptly; EXT_ARG avoids synthetic timeout completions.
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
        // Satellite wakes remain armed until every original and close CQE has drained.
        closing_wakeups = true;
        for (const auto &owner : rings)
        {
            if (owner->wake_pending)
            {
                cancel(*rings.front(), owner->wake_operation);
            }
        }
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
            // A busy ring cannot monopolize handlers or delay another ring's close CQEs.
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

    Descriptor listener;
    Descriptor stop;
    Handler handler;
    HeaderHandler headers;
    static constexpr std::size_t max_rings = 16;
    std::vector<std::unique_ptr<Ring>> rings;
    Ring *accepting = nullptr;
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
    const std::uint64_t value = 1;
    ssize_t result = 0;
    do
    {
        result = ::write(state_->stop.get(), &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN)
    {
        std::terminate();
    }
}

int Server::stop_descriptor() const noexcept
{
    return state_->stop.get();
}

std::uint16_t Server::port() const noexcept
{
    return state_->bound_port;
}

} // namespace tds
