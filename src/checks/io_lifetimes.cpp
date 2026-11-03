#include "net/reactor.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
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
using tds::net::ReceiveBufferLease;
using tds::net::ReceiveBuffers;
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

class FileLimit
{
  public:
    explicit FileLimit(rlim_t soft)
    {
        require(::getrlimit(RLIMIT_NOFILE, &previous_) == 0, "read descriptor limit");
        require(previous_.rlim_cur >= soft, "descriptor limit too low for fixture");
        const rlimit limited{soft, previous_.rlim_max};
        require(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "set fixture descriptor limit");
    }

    ~FileLimit()
    {
        ::setrlimit(RLIMIT_NOFILE, &previous_);
    }

    FileLimit(const FileLimit &) = delete;
    FileLimit &operator=(const FileLimit &) = delete;

  private:
    rlimit previous_{};
};

class Client
{
  public:
    explicit Client(std::uint16_t port, int receive_buffer = 0)
        : descriptor_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))
    {
        require(descriptor_ >= 0, "client socket failed");
        if (receive_buffer && ::setsockopt(descriptor_, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                                           sizeof(receive_buffer)) < 0)
        {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("client receive buffer failed");
        }
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

    std::size_t drain_bytes(char expected) const
    {
        std::array<char, 4096> buffer;
        std::size_t total = 0;
        while (true)
        {
            const auto result = ::recv(descriptor_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (result < 0 && errno == EINTR)
            {
                continue;
            }
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                return total;
            }
            require(result > 0, "primary send closed or failed before all bytes arrived");
            const auto count = static_cast<std::size_t>(result);
            for (std::size_t index = 0; index < count; ++index)
            {
                require(buffer[index] == expected, "primary send corrupted its payload");
            }
            total += count;
        }
    }

    bool closed(Clock::time_point deadline) const
    {
        while (Clock::now() < deadline)
        {
            char byte = 0;
            const auto result = ::recv(descriptor_, &byte, 1, MSG_DONTWAIT);
            if (result == 0)
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
#ifdef TDS_DIAGNOSTICS
    require(store.live_count() == 0 && store.reserved_count() == 3,
            "reservation gauges lost an unaccepted slot");
#endif
    rejects(
        [&] {
            store.reserve();
        },
        "exhausted store returned another slot");

    auto &record = store.create(owner, first);
    const auto *address = &record;
    store.create(owner, second);
    store.release(spare);
#ifdef TDS_DIAGNOSTICS
    require(store.live_count() == 2 && store.reserved_count() == 0,
            "accept and release did not update connection gauges");
#endif
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
#ifdef TDS_DIAGNOSTICS
    require(store.live_count() == 2 && store.reserved_count() == 1,
            "rejected retirement changed connection gauges");
    record.start_close();
    store.retire(first);
    store.release(spare);
    require(store.live_count() == 1 && store.reserved_count() == 0,
            "terminal close or released reservation retained a diagnostic count");
#endif
}

void primary_progress_with_ready_satellite()
{
    constexpr rlim_t slots = 64;
    FileLimit limited(slots);

    // Send storage outlives the reactor, including failure cleanup and terminal cancellation
    std::string payload(256 * 1024, 'y');
    iovec vector{payload.data(), payload.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    Reactor reactor("127.0.0.1", 0);
    bool listener_limited = false;
    for (int descriptor = 0; descriptor < static_cast<int>(slots); ++descriptor)
    {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        int listening = 0;
        socklen_t option_length = sizeof(listening);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) == 0 &&
            address.sin_family == AF_INET && address.sin_port == htons(reactor.port()) &&
            ::getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &listening, &option_length) == 0 &&
            listening)
        {
            const int size = 4096;
            require(::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0,
                    "limit primary send buffer");
            listener_limited = true;
            break;
        }
    }
    require(listener_limited, "primary progress listener not found");
    reactor.start();
    Client peer(reactor.port(), 4096);
    const auto setup_deadline = Clock::now() + 5s;
    await_completion(reactor, setup_deadline);
    reactor.begin_turn();
    drain_turn(reactor, [](const Completion &completion) {
        require(completion.kind == CompletionKind::control, "unexpected progress setup payload");
    });
    auto &connection = only_connection(reactor);
    auto &primary = *reactor.rings().front();
    require(&connection.ring == &primary && reactor.rings().size() == 2,
            "reserved direct slots did not create a real satellite ring");
    auto &satellite = *reactor.rings()[1];
    primary.submit();

    const auto add_satellite_completions = [&](unsigned count) {
        for (unsigned index = 0; index < count; ++index)
        {
            io_uring_prep_nop(satellite.entry(nullptr));
        }
        satellite.submit();
    };
    add_satellite_completions(512);
    const auto busy_turn = [&](const auto &handler) {
        require(io_uring_cq_ready(&satellite.queue) >= 512,
                "satellite CQ was not ready before the primary wait shortcut");
        // No get_events, peek_cqe or readiness helper may process primary task work here
        reactor.begin_turn();
        drain_turn(reactor, handler);
        require(io_uring_cq_ready(&satellite.queue) >= 256,
                "satellite CQ became empty during the bounded production drain");
        add_satellite_completions(256);
    };
    const auto shutdown = [&] {
        reactor.request_stop();
        const auto deadline = Clock::now() + 5s;
        while (reactor.pending_count())
        {
            // Cleanup may force events only after the critical production-path checks end
            await_completion(reactor, deadline);
            reactor.begin_turn();
            drain_turn(reactor, [](Completion &completion) {
                completion.buffer.release();
            });
        }
    };
    try
    {
        peer.send_byte();
        bool received = false;
        const auto receive_deadline = Clock::now() + 2s;
        while (!received)
        {
            require(Clock::now() < receive_deadline,
                    "ready satellite starved the primary receive completion");
            busy_turn([&](Completion &completion) {
                if (completion.kind == CompletionKind::received)
                {
                    require(!received && completion.connection == &connection &&
                                completion.result == 1 && completion.buffer.bytes().size() == 1 &&
                                completion.buffer.bytes().front() == 'x',
                            "primary receive lost its target or payload");
                    completion.buffer.release();
                    received = true;
                }
                else
                {
                    require(completion.kind == CompletionKind::control,
                            "unexpected send before the primary send was submitted");
                }
            });
        }

        reactor.send(connection, message);
        std::size_t sent = 0;
        std::size_t delivered = 0;
        bool partial_send = false;
        const auto read_after = Clock::now() + 20ms;
        const auto send_deadline = Clock::now() + 2s;
        while (sent != payload.size() || delivered != payload.size())
        {
            require(Clock::now() < send_deadline,
                    "ready satellite starved the backpressured primary send");
            busy_turn([&](Completion &completion) {
                if (completion.kind == CompletionKind::sent)
                {
                    require(completion.connection == &connection && completion.result > 0 &&
                                static_cast<std::size_t>(completion.result) <=
                                    payload.size() - sent,
                            "primary send completion lost its target or byte count");
                    sent += static_cast<std::size_t>(completion.result);
                    if (sent != payload.size())
                    {
                        partial_send = true;
                        vector.iov_base = payload.data() + sent;
                        vector.iov_len = payload.size() - sent;
                        reactor.send(connection, message);
                    }
                }
                else
                {
                    require(completion.kind == CompletionKind::control,
                            "unexpected receive while checking primary send progress");
                }
            });
            if (Clock::now() >= read_after)
            {
                delivered += peer.drain_bytes('y');
                require(delivered <= payload.size(), "primary send duplicated payload bytes");
            }
        }
        require(partial_send, "primary send did not exercise real socket backpressure");

        const auto primary_slot = connection.slot;
        reactor.receive(connection);
        reactor.request_stop();
        const auto stop_deadline = Clock::now() + 2s;
        while (!reactor.stopping() || primary.connections.at(primary_slot.value))
        {
            require(Clock::now() < stop_deadline,
                    "ready satellite starved primary stop or connection retirement");
            busy_turn([](const Completion &completion) {
                require(completion.kind == CompletionKind::control,
                        "unexpected application I/O while checking primary stop progress");
            });
        }
        require(peer.closed(stop_deadline), "primary retirement did not deliver TCP EOF");
        require(io_uring_cq_ready(&satellite.queue) >= 512,
                "satellite CQ was not continuously ready through primary TCP EOF");
    }
    catch (...)
    {
        const auto failure = std::current_exception();
        shutdown();
        std::rethrow_exception(failure);
    }
    shutdown();
}

void fixed_cq_order_and_growth()
{
    FileLimit limited(32);
    Reactor reactor("127.0.0.1", 0);
    reactor.start();
    require(reactor.rings().size() == 2, "accept reservations did not create two rings");
    const auto deadline = Clock::now() + 5s;
    const auto add_nops = [](Ring &ring, unsigned count) {
        for (unsigned index = 0; index < count; ++index)
        {
            io_uring_prep_nop(ring.entry(nullptr));
        }
        ring.submit();
    };
    for (const auto &ring : reactor.rings())
    {
        add_nops(*ring, 8);
    }
    await_completion(reactor, deadline);
    reactor.begin_turn();
    drain_turn(reactor, [](const Completion &completion) {
        require(completion.kind == CompletionKind::control, "unexpected setup payload");
    });

    // The first accept appends a ring that must join the same traversal
    Client peer(reactor.port());
    bool accept_ready = false;
    while (!accept_ready)
    {
        await_completion(reactor, deadline);
        for (const auto &ring : reactor.rings())
        {
            unsigned head;
            io_uring_cqe *cqe;
            io_uring_for_each_cqe(&ring->queue, head, cqe)
            {
                auto *operation = static_cast<Operation *>(io_uring_cqe_get_data(cqe));
                accept_ready = accept_ready ||
                               (operation && operation->event == Event::accept && cqe->res >= 0);
            }
        }
        require(Clock::now() < deadline, "accept CQE was not ready before the growth turn");
    }
    constexpr std::array<std::size_t, 3> expected{0, 1, 2};
    for (unsigned turn = 0; turn < 3; ++turn)
    {
        std::array<unsigned, 3> heads{};
        for (std::size_t index = 0; index < reactor.rings().size(); ++index)
        {
            auto &ring = *reactor.rings()[index];
            heads[index] = *ring.queue.cq.khead;
            add_nops(ring, turn ? 256 : 512);
        }
        std::vector<std::size_t> order;
        const auto observe_advances = [&] {
            require(reactor.rings().size() <= heads.size(), "unexpected extra ring growth");
            for (std::size_t index = 0; index < reactor.rings().size(); ++index)
            {
                const auto head = *reactor.rings()[index]->queue.cq.khead;
                if (head != heads[index])
                {
                    require(head - heads[index] == 256, "CQ drain exceeded or missed its budget");
                    order.push_back(index);
                    heads[index] = head;
                }
            }
        };
        reactor.begin_turn();
        Completion completion;
        bool grew = false;
        while (reactor.next(completion))
        {
            require(completion.kind == CompletionKind::control, "unexpected growth payload");
            if (turn == 0 && !grew && reactor.rings().size() == 3)
            {
                add_nops(*reactor.rings()[2], 512);
                grew = true;
            }
            observe_advances();
        }
        observe_advances();
        reactor.finish_turn();
        require(reactor.rings().size() == 3, "accept did not grow a third ring during traversal");
        require(order.size() == 3, "CQ traversal missed or revisited a ring after growth");
        for (std::size_t index = 0; index < order.size(); ++index)
        {
            require(order[index] == expected[index], "CQ traversal did not preserve ring order");
        }
    }
    reactor.request_stop();
    while (reactor.pending_count())
    {
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, [](Completion &completion) {
            completion.buffer.release();
        });
    }
    require(peer.closed(deadline), "shutdown after ring growth did not close the peer");
}

void exhausted_receive_retries()
{
    Reactor reactor("127.0.0.1", 0);
    reactor.start();
    auto &owner = *reactor.rings().front();
    constexpr unsigned clients_count = ReceiveBuffers::count + 512;
    const auto deadline = Clock::now() + 5s;
    std::vector<std::unique_ptr<Client>> clients;
    for (unsigned index = 0; index < clients_count; ++index)
    {
        clients.push_back(std::make_unique<Client>(reactor.port()));
    }
    unsigned accepted = 0;
    while (accepted != clients_count)
    {
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, [](const Completion &completion) {
            require(completion.kind == CompletionKind::control, "unexpected accept payload");
        });
        accepted = 0;
        for (std::size_t slot = 0; slot < owner.connections.size(); ++slot)
        {
            accepted += owner.connections.at(slot) != nullptr;
        }
    }
    owner.submit();
#ifdef TDS_DIAGNOSTICS
    require(owner.connections.live_count() == clients_count &&
                owner.connections.reserved_count() == 64 &&
                owner.diagnostics.receive_submissions == clients_count &&
                owner.diagnostics.receive_completions == 0,
            "idle receive submissions or connection gauges were not counted correctly");
#endif
    for (const auto &client : clients)
    {
        client->send_byte();
    }
    // Keep every selected buffer in its CQE until the kernel has exhausted the whole pool
    while (io_uring_cq_ready(&owner.queue) < clients_count && Clock::now() < deadline)
    {
        const int result = io_uring_get_events(&owner.queue);
        require(result >= 0 || result == -EINTR, "exhaustion get events failed");
    }
    require(io_uring_cq_ready(&owner.queue) == clients_count, "initial receives did not complete");
    std::vector<ReceiveBufferLease> held;
    held.reserve(ReceiveBuffers::count);
    std::vector<bool> received(owner.connections.size());
    unsigned total_received = 0;
    const auto retain = [&](Completion &completion) {
        if (completion.kind != CompletionKind::received)
        {
            require(completion.kind == CompletionKind::control, "unexpected exhaustion send");

            return;
        }
        const auto slot = completion.connection->slot.value;
        require(!received[slot] && completion.result == 1 &&
                    completion.buffer.bytes().front() == 'x',
                "exhaustion duplicated or corrupted a receive");
        received[slot] = true;
        ++total_received;
#ifdef TDS_DIAGNOSTICS
        require(completion.connection->receive_enobufs_since_ns == 0,
                "successful receive retained its first ENOBUFS timestamp");
#endif
        held.push_back(std::move(completion.buffer));
    };
    // Six bounded CQ drains consume the original 1,536 completions without returning buffers
    for (unsigned turn = 0; turn < clients_count / 256; ++turn)
    {
        reactor.begin_turn();
        drain_turn(reactor, retain);
    }
    require(held.size() == ReceiveBuffers::count, "the selected pool was not fully retained");
#ifdef TDS_DIAGNOSTICS
    std::vector<std::pair<Connection *, std::uint64_t>> first_deferred;
    for (std::size_t slot = 0; slot < owner.connections.size(); ++slot)
    {
        auto *connection = owner.connections.at(slot);
        if (connection && connection->receive_enobufs_since_ns)
        {
            first_deferred.emplace_back(connection, connection->receive_enobufs_since_ns);
        }
    }
    require(first_deferred.size() == clients_count - ReceiveBuffers::count &&
                owner.diagnostics.enobufs_to_receive_service.count == 0,
            "first ENOBUFS timestamps missed deferrals or recorded unfinished recovery");
#endif

    const auto control_turn = [&](const auto &handler) {
        io_uring_prep_nop(owner.entry(nullptr));
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, handler);
    };
    constexpr unsigned control_turns = 16;
    unsigned control_cqes = 0;
    const auto started = Clock::now();
    for (unsigned turn = 0; turn < control_turns; ++turn)
    {
        control_turn([&](const Completion &completion) {
            require(completion.kind == CompletionKind::control,
                    "a receive completed without an available buffer");
            ++control_cqes;
        });
    }
    const auto elapsed = Clock::now() - started;
    const auto probes = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 4;
    std::printf("Receive retry probe: %u control CQEs in %u turns, %.3f ms\n", control_cqes,
                control_turns, std::chrono::duration<double, std::milli>(elapsed).count());
    require(control_cqes <= control_turns + 3 + static_cast<unsigned>(probes),
            "exhausted receive retries kept generating CQEs without buffer progress");

    // No refill yet: a bounded probe must still run, rather than parking the FIFO forever
    std::this_thread::sleep_for(5ms);
    control_turn(retain);
    require(owner.deferred_receives.size() == clients_count - ReceiveBuffers::count - 1,
            "timed retry did not arm exactly one exhausted receive");
#ifdef TDS_DIAGNOSTICS
    for (const auto &[connection, first] : first_deferred)
    {
        require(connection->receive_enobufs_since_ns == first,
                "repeated ENOBUFS restarted the application deferral clock");
    }
#endif

    held.clear();
    // Return once, then retain the recovered buffers so a second retry wave gets no refill event
    while (total_received != clients_count)
    {
        control_turn(retain);
    }
    require(owner.deferred_receives.empty() && held.size() == 512,
            "one refill did not resume all deferred receives across bounded turns");
#ifdef TDS_DIAGNOSTICS
    require(owner.diagnostics.receive_enobufs >= first_deferred.size() &&
                owner.diagnostics.receive_completions ==
                    clients_count + owner.diagnostics.receive_enobufs &&
                owner.diagnostics.receive_submissions == owner.diagnostics.receive_completions,
            "receive counters included control CQEs or missed retry completions");
    require(owner.diagnostics.enobufs_to_receive_service.count == first_deferred.size() &&
                owner.diagnostics.enobufs_to_receive_service.max >= 5'000'000,
            "recovery timing missed an episode or excluded the buffer-deferred wait");
#endif
    held.clear();
    reactor.request_stop();
    while (reactor.pending_count())
    {
        await_completion(reactor, deadline);
        reactor.begin_turn();
        drain_turn(reactor, [](const Completion &completion) {
            require(completion.kind == CompletionKind::control,
                    "exhaustion shutdown reached application I/O");
        });
    }
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

    Client idle(reactor.port());
    await_completion(reactor, deadline);
    reactor.begin_turn();
    drain_turn(reactor, [](const Completion &completion) {
        require(completion.kind == CompletionKind::control, "idle accept reached application I/O");
    });
    Connection *idle_connection = nullptr;
    for (std::size_t index = 0; index < store.size(); ++index)
    {
        if (auto *candidate = store.at(index); candidate && candidate != address)
        {
            require(!idle_connection, "more than one idle connection");
            idle_connection = candidate;
        }
    }
    require(idle_connection && idle_connection->io_active(),
            "idle peer did not retain its original receive in the same ring");

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
                        "submitting close retired its record before the final close CQE");
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
        drain_turn(reactor, [&](const Completion &completion) {
            require(completion.kind == CompletionKind::control, "unexpected I/O while closing");
            if (auto *closing = store.at(slot.value))
            {
                require(closing == address && closing->io_closing(),
                        "intermediate close completion changed record ownership");
                rejects(
                    [&] {
                        reactor.close(*closing);
                    },
                    "intermediate close completion allowed a duplicate close");
            }
        });
    }

    // Cursed fixed-file table references can outlive close_direct's CQE and delay TCP EOF
    require(idle_connection->io_active(), "idle receive completed before the EOF check");
    require(initial.closed(Clock::now() + 1s),
            "retired connection did not deliver TCP EOF while an unrelated receive was pending");
    const auto reused = store.reserve();
    require(reused == slot && !store.at(reused.value), "close did not return its direct slot");
    store.release(reused);

    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        idle.send_byte();
        bool idle_received = false;
        while (!idle_received)
        {
            await_completion(reactor, deadline);
            reactor.begin_turn();
            drain_turn(reactor, [&](Completion &completion) {
                if (completion.kind == CompletionKind::received)
                {
                    require(completion.connection == idle_connection && completion.result == 1 &&
                                completion.buffer.bytes().size() == 1 &&
                                completion.buffer.bytes().front() == 'x',
                            "closing another connection damaged the idle peer's receive");
                    completion.buffer.release();
                    reactor.receive(*idle_connection);
                    idle_received = true;
                }
                else
                {
                    require(completion.kind == CompletionKind::control,
                            "unexpected send while checking the idle peer");
                }
            });
        }
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
    require(idle.closed(deadline), "idle peer remained open after shutdown");
    for (const auto &ring : reactor.rings())
    {
        auto &connections = ring->connections;
#ifdef TDS_DIAGNOSTICS
        require(connections.live_count() == 0 && connections.reserved_count() == 0,
                "shutdown retained live records or accept reservations in its gauges");
#endif
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
        primary_progress_with_ready_satellite();
        fixed_cq_order_and_growth();
        exhausted_receive_retries();
        close_and_late_accept();
        ::alarm(0);
        std::puts(
            "I/O reservation, fixed CQ order/growth, primary progress with a ready satellite, "
            "original-CQE lifetime, "
            "prompt TCP EOF and late-accept shutdown checks passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        ::alarm(0);
        std::fprintf(stderr, "I/O lifetime checks failed: %s\n", error.what());

        return 1;
    }
}
