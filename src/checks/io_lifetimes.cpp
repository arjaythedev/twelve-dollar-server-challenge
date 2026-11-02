#include "net/reactor.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using tds::net::Completion;
using tds::net::CompletionKind;
using tds::net::Connection;
using tds::net::Event;
using tds::net::Operation;
using tds::net::Reactor;
using tds::net::Ring;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <class Action> void rejects(Action &&action, const char *message)
{
    try
    {
        std::forward<Action>(action)();
    }
    catch (const std::logic_error &)
    {
        return;
    }

    throw std::runtime_error(message);
}

class Client
{
  public:
    explicit Client(std::uint16_t port)
        : descriptor_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))
    {
        require(descriptor_ >= 0, "client socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int connected =
            ::connect(descriptor_, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        if (connected == 0)
        {
            return;
        }
        if (errno == EINPROGRESS)
        {
            pollfd pending{descriptor_, POLLOUT, 0};
            int error = 0;
            socklen_t length = sizeof(error);
            if (::poll(&pending, 1, 1000) > 0 &&
                ::getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error)
            {
                return;
            }
        }
        ::close(descriptor_);
        descriptor_ = -1;

        throw std::runtime_error("client connect failed");
    }

    ~Client()
    {
        ::close(descriptor_);
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void send_byte() const
    {
        require(::send(descriptor_, "x", 1, MSG_NOSIGNAL) == 1, "client send failed");
    }

    bool closed(Clock::time_point deadline) const
    {
        while (Clock::now() < deadline)
        {
            char byte = 0;
            const auto result = ::recv(descriptor_, &byte, 1, MSG_DONTWAIT);
            if (result == 0 || (result < 0 && errno == ECONNRESET))
            {
                return true;
            }
            if (result > 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            {
                return false;
            }
            pollfd pending{descriptor_, POLLIN | POLLHUP, 0};
            ::poll(&pending, 1, 1);
        }

        return false;
    }

  private:
    int descriptor_;
};

void await_completion(Reactor &reactor, Clock::time_point deadline)
{
    for (const auto &ring : reactor.rings())
    {
        ring->submit();
    }
    while (Clock::now() < deadline)
    {
        for (const auto &ring : reactor.rings())
        {
            const int result = io_uring_get_events(&ring->queue);
            require(result >= 0 || result == -EINTR, "get events failed");
            if (io_uring_cq_ready(&ring->queue))
            {
                return;
            }
        }
        std::this_thread::sleep_for(1ms);
    }

    throw std::runtime_error("waiting for a CQE timed out");
}

template <class Handler> void drain_turn(Reactor &reactor, Handler &&handler)
{
    Completion completion;
    while (reactor.next(completion))
    {
        handler(completion);
    }
    reactor.finish_turn();
}

Connection &only_connection(Reactor &reactor)
{
    Connection *found = nullptr;
    for (const auto &ring : reactor.rings())
    {
        for (std::size_t slot = 0; slot < ring->connections.size(); ++slot)
        {
            if (auto *connection = ring->connections.at(slot))
            {
                require(!found, "more than one initial connection");
                found = connection;
            }
        }
    }
    require(found != nullptr, "initial client was not accepted");

    return *found;
}

void reservation_and_record_guards()
{
    Ring owner(3, true);
    auto &store = owner.connections;
    const auto first = store.reserve();
    const auto second = store.reserve();
    const auto spare = store.reserve();
    require(!store.has_free_slot(), "reserved slots remained free");
    rejects(
        [&] {
            store.reserve();
        },
        "exhausted store returned another slot");

    auto &record = store.create(owner, first);
    const auto *address = &record;
    store.create(owner, second);
    store.release(spare);
    require(store.reserve() == spare && store.at(first.value) == address,
            "slot reuse moved a live record");

    record.start_io(Event::receive);
    rejects(
        [&] {
            store.retire(first);
        },
        "store retired an original active operation");
    require(store.at(first.value) == address && record.io_active(),
            "failed retirement changed the active record");
    record.finish_io();
    rejects(
        [&] {
            store.retire(first);
        },
        "store retired before close completion");
    require(store.at(first.value) == address, "idle retirement released the live address");
}

void close_and_late_accept()
{
    Reactor reactor("127.0.0.1", 0);
    reactor.start();
    const auto deadline = Clock::now() + 5s;
    Client initial(reactor.port());
    await_completion(reactor, deadline);
    reactor.begin_turn();
    drain_turn(reactor, [](const Completion &completion) {
        require(completion.kind == CompletionKind::control, "unexpected initial application I/O");
    });

    auto &connection = only_connection(reactor);
    auto &store = connection.ring.connections;
    const auto slot = connection.slot;
    const auto *address = &connection;
    require(connection.io_active(), "accept did not arm the original receive");
    rejects(
        [&] {
            reactor.close(connection);
        },
        "close accepted an outstanding original receive");
    rejects(
        [&] {
            store.retire(slot);
        },
        "active receive allowed slot retirement");
    require(store.at(slot.value) == address && connection.io_active(),
            "rejected close changed original-operation ownership");

    initial.send_byte();
    bool received = false;
    while (!received)
    {
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, [&](Completion &completion) {
            if (completion.kind == CompletionKind::received)
            {
                require(completion.connection == address && completion.result == 1 &&
                            completion.buffer.bytes().size() == 1 &&
                            completion.buffer.bytes().front() == 'x',
                        "original receive did not preserve its connection and buffer");
                completion.buffer.release();
                rejects(
                    [&] {
                        store.retire(slot);
                    },
                    "receive CQE retired before close CQE");
                reactor.close(*completion.connection);
                require(store.at(slot.value) == address && completion.connection->io_closing(),
                        "submitting close retired its record before the CQE");
                rejects(
                    [&] {
                        reactor.close(*completion.connection);
                    },
                    "duplicate close submitted another terminal operation");
                received = true;
            }
        });
    }
    while (store.at(slot.value))
    {
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, [](const Completion &completion) {
            require(completion.kind == CompletionKind::control, "unexpected I/O while closing");
        });
    }

    reactor.request_stop();
    await_completion(reactor, deadline);
    reactor.begin_turn();
    auto &queue = reactor.rings().front()->queue;
    io_uring_cqe *head = nullptr;
    require(io_uring_peek_cqe(&queue, &head) == 0 && head != nullptr,
            "stop did not produce a queued CQE");
    const auto *stop = static_cast<const Operation *>(io_uring_cqe_get_data(head));
    require(stop && stop->event == Event::stop && head->res >= 0,
            "stop CQE was not first before late accept");

    Client late(reactor.port());
    bool late_accepted = false;
    while (!late_accepted && Clock::now() < deadline)
    {
        const int result = io_uring_get_events(&queue);
        require(result >= 0 || result == -EINTR, "late accept get events failed");
        unsigned cursor = 0;
        io_uring_cqe *cqe = nullptr;
        io_uring_for_each_cqe(&queue, cursor, cqe)
        {
            const auto *operation = static_cast<const Operation *>(io_uring_cqe_get_data(cqe));
            late_accepted |= operation && operation->event == Event::accept && cqe->res >= 0;
        }
        if (!late_accepted)
        {
            std::this_thread::sleep_for(1ms);
        }
    }
    require(late_accepted, "successful late accept was not queued behind stop");
    const auto no_application_io = [](const Completion &completion) {
        require(completion.kind == CompletionKind::control,
                "late shutdown accept reached application I/O");
    };
    drain_turn(reactor, no_application_io);
    require(reactor.stopping(), "queued stop did not enter shutdown");
    while (reactor.pending_count())
    {
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, no_application_io);
    }

    require(late.closed(deadline), "late accepted client was left open");
    for (const auto &ring : reactor.rings())
    {
        auto &connections = ring->connections;
        std::vector<bool> returned(connections.size());
        std::size_t count = 0;
        while (connections.has_free_slot())
        {
            const auto free = connections.reserve();
            require(free.value < returned.size() && !returned[free.value] &&
                        !connections.at(free.value),
                    "shutdown duplicated a slot or retained a live connection");
            returned[free.value] = true;
            ++count;
        }
        require(count == connections.size(), "shutdown lost an accept or connection slot");
        for (unsigned index = 0; index < connections.size(); ++index)
        {
            connections.release(tds::net::DirectSlot{index});
        }
    }
}
} // namespace

int main()
{
    // A broken blocking path must fail the test process rather than hang the suite
    ::alarm(15);
    try
    {
        reservation_and_record_guards();
        close_and_late_accept();
        ::alarm(0);
        std::puts("I/O reservation, original-CQE lifetime and late-accept shutdown checks passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        ::alarm(0);
        std::fprintf(stderr, "I/O lifetime checks failed: %s\n", error.what());

        return 1;
    }
}
