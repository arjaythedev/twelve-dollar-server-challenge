#include "net/reactor.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using tds::net::Completion;
using tds::net::CompletionKind;
using tds::net::Connection;
using tds::net::Event;
using tds::net::Operation;
using tds::net::Reactor;
using tds::net::ReceiveBufferLease;
using tds::net::ReceiveBuffers;
using tds::net::Ring;

constexpr unsigned clients_count = ReceiveBuffers::count + 512;
constexpr unsigned recovered_count = clients_count - ReceiveBuffers::count;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint64_t cpu_ns()
{
    timespec value{};
    require(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "CPU clock failed");

    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000 +
           static_cast<std::uint64_t>(value.tv_nsec);
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
        const auto result =
            ::connect(descriptor_, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        if (result == 0)
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

    void send() const
    {
        require(::send(descriptor_, "x", 1, MSG_NOSIGNAL) == 1, "client send failed");
    }

  private:
    int descriptor_;
};

template <class Handler> void drain_turn(Reactor &reactor, Handler &&handler)
{
    Completion completion;
    while (reactor.next(completion))
    {
        handler(completion);
    }
    reactor.finish_turn();
}

void wait_for_cqes(Ring &owner, unsigned count, Clock::time_point deadline)
{
    owner.submit();
    while (io_uring_cq_ready(&owner.queue) < count)
    {
        require(Clock::now() < deadline, "waiting for receive CQEs timed out");
        const int result = io_uring_get_events(&owner.queue);
        require(result >= 0 || result == -EINTR, "get events failed");
    }
}

unsigned exhausted_cqes(Ring &owner)
{
    unsigned count = 0;
    unsigned seen = 0;
    unsigned head = 0;
    io_uring_cqe *cqe = nullptr;
    io_uring_for_each_cqe(&owner.queue, head, cqe)
    {
        if (seen++ == 256)
        {
            break;
        }
        const auto *operation = static_cast<Operation *>(io_uring_cqe_get_data(cqe));
        count += operation && operation->event == Event::receive && cqe->res == -ENOBUFS;
    }

    return count;
}

struct Sample
{
    std::uint64_t cpu = 0;
    std::uint64_t wall = 0;
    unsigned retries = 0;
    unsigned failures = 0;
};

Sample recover(Reactor &reactor, const std::vector<std::unique_ptr<Client>> &clients,
               const std::vector<Connection *> &connections, bool initial)
{
    auto &owner = *reactor.rings().front();
    // Start each burst with the fresh-ring credit state after returning every previous lease
    owner.receive_buffers->take_retry_credits(ReceiveBuffers::count);
    if (!initial)
    {
        for (auto *connection : connections)
        {
            reactor.receive(*connection);
        }
    }
    owner.submit();
    for (const auto &client : clients)
    {
        client->send();
    }
    const auto deadline = Clock::now() + 5s;
    wait_for_cqes(owner, clients_count, deadline);
    require(io_uring_cq_ready(&owner.queue) == clients_count,
            "burst did not produce exactly one original CQE per client");

    std::vector<ReceiveBufferLease> held;
    held.reserve(ReceiveBuffers::count);
    std::vector<bool> received(owner.connections.size());
    unsigned completed = 0;
    const auto retain = [&](Completion &completion) {
        if (completion.kind == CompletionKind::control)
        {
            return;
        }
        require(completion.kind == CompletionKind::received && completion.result == 1 &&
                    completion.buffer.bytes().size() == 1 &&
                    completion.buffer.bytes().front() == 'x',
                "recovery changed the payload or completion kind");
        const auto slot = completion.connection->slot.value;
        require(!received[slot], "recovery duplicated a client payload");
        received[slot] = true;
        ++completed;
        held.push_back(std::move(completion.buffer));
    };

    unsigned failures = 0;
    for (unsigned turn = 0; turn < clients_count / 256; ++turn)
    {
        reactor.begin_turn();
        failures += exhausted_cqes(owner);
        drain_turn(reactor, retain);
    }
    require(completed == ReceiveBuffers::count && held.size() == ReceiveBuffers::count &&
                failures == recovered_count,
            "burst did not force the expected selected-buffer exhaustion");

    Sample sample;
    sample.retries = recovered_count - static_cast<unsigned>(owner.deferred_receives.size());
    const auto started = Clock::now();
    const auto cpu_started = cpu_ns();
    held.clear();
    while (completed != clients_count)
    {
        require(Clock::now() < deadline, "ENOBUFS recovery timed out");
        // Only the real production wait path progresses the recovery phase
        reactor.begin_turn();
        sample.failures += exhausted_cqes(owner);
        drain_turn(reactor, retain);
        sample.retries += io_uring_sq_ready(&owner.queue);
    }
    sample.cpu = cpu_ns() - cpu_started;
    sample.wall = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    require(owner.deferred_receives.empty() && held.size() == recovered_count,
            "recovery lost a deferred receive or retained the wrong leases");
    require(sample.retries == recovered_count + sample.failures,
            "retry submission and terminal CQE counts disagree");

    return sample;
}

void shutdown(Reactor &reactor)
{
    reactor.request_stop();
    const auto deadline = Clock::now() + 5s;
    while (reactor.pending_count())
    {
        require(Clock::now() < deadline, "shutdown timed out");
        reactor.begin_turn();
        drain_turn(reactor, [](Completion &completion) {
            completion.buffer.release();
        });
    }
}
} // namespace

int main(int argc, char **argv)
{
    ::alarm(60);
    try
    {
        unsigned cycles = 100;
        if (argc == 2)
        {
            const std::string_view argument(argv[1]);
            const auto parsed =
                std::from_chars(argument.data(), argument.data() + argument.size(), cycles);
            require(parsed.ec == std::errc{} && parsed.ptr == argument.data() + argument.size() &&
                        cycles > 0 && cycles <= 1000,
                    "cycles must be in [1, 1000]");
        }
        else
        {
            require(argc == 1, "usage: retry_receive_probe [cycles]");
        }

        Reactor reactor("127.0.0.1", 0);
        reactor.start();
        auto &owner = *reactor.rings().front();
        std::vector<std::unique_ptr<Client>> clients;
        clients.reserve(clients_count);
        for (unsigned index = 0; index < clients_count; ++index)
        {
            clients.push_back(std::make_unique<Client>(reactor.port()));
        }
        std::vector<Connection *> connections;
        connections.reserve(clients_count);
        const auto deadline = Clock::now() + 5s;
        while (connections.size() != clients_count)
        {
            wait_for_cqes(owner, 1, deadline);
            reactor.begin_turn();
            drain_turn(reactor, [](const Completion &completion) {
                require(completion.kind == CompletionKind::control, "unexpected setup payload");
            });
            connections.clear();
            for (std::size_t slot = 0; slot < owner.connections.size(); ++slot)
            {
                if (auto *connection = owner.connections.at(slot))
                {
                    connections.push_back(connection);
                }
            }
        }
        require(reactor.rings().size() == 1, "probe requires one ring");

        std::vector<Sample> samples;
        samples.reserve(cycles);
        try
        {
            constexpr unsigned warmup = 8;
            for (unsigned cycle = 0; cycle < warmup + cycles; ++cycle)
            {
                const auto sample = recover(reactor, clients, connections, cycle == 0);
                if (cycle >= warmup)
                {
                    samples.push_back(sample);
                }
            }
        }
        catch (...)
        {
            const auto failure = std::current_exception();
            shutdown(reactor);
            std::rethrow_exception(failure);
        }
        shutdown(reactor);

        std::printf("{\"cycles\":%u,\"warmup_cycles\":8,\"clients\":%u,"
                    "\"completed_bytes\":%u,\"recovered_bytes\":%u,\"samples\":[",
                    cycles, clients_count, cycles * clients_count, cycles * recovered_count);
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            const auto &sample = samples[index];
            std::printf("%s{\"cpu_ns\":%llu,\"wall_ns\":%llu,\"retry_submissions\":%u,"
                        "\"retry_enobufs\":%u}",
                        index ? "," : "", static_cast<unsigned long long>(sample.cpu),
                        static_cast<unsigned long long>(sample.wall), sample.retries,
                        sample.failures);
        }
        std::puts("]}");
        ::alarm(0);

        return 0;
    }
    catch (const std::exception &error)
    {
        ::alarm(0);
        std::fprintf(stderr, "Receive retry probe failed: %s\n", error.what());

        return 1;
    }
}
