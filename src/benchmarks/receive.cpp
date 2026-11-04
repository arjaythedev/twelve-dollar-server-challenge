#include "receive_backend.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace
{
using namespace tds::net;
using namespace tds::bench;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint64_t now(clockid_t clock)
{
    timespec value{};
    require(::clock_gettime(clock, &value) == 0, "probe clock failed");

    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
}

std::uint64_t micros(timeval value)
{
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000 + value.tv_usec;
}

void transfer(int socket, const char *bytes, std::size_t length)
{
    while (length)
    {
        const auto sent = ::send(socket, bytes, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        require(sent > 0, "client send failed");
        bytes += sent;
        length -= static_cast<std::size_t>(sent);
    }
}

void token(int socket, char expected)
{
    char byte = 0;
    ssize_t count;
    do
    {
        count = ::recv(socket, &byte, 1, 0);
    } while (count < 0 && errno == EINTR);
    require(count == 1 && byte == expected, "client synchronization failed");
}

struct ProducerMetrics
{
    std::uint64_t cpu_ns = 0;
    std::uint64_t wall_ns = 0;
    std::uint64_t latency_samples = 0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p95_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t max_ns = 0;
};

ProducerMetrics read_metrics(int socket)
{
    ProducerMetrics metrics;
    auto *bytes = reinterpret_cast<char *>(&metrics);
    std::size_t remaining = sizeof(metrics);
    while (remaining)
    {
        const auto count = ::recv(socket, bytes, remaining, 0);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        require(count > 0, "producer metrics missing");
        bytes += count;
        remaining -= static_cast<std::size_t>(count);
    }

    return metrics;
}

class Child
{
  public:
    explicit Child(pid_t pid) : pid_(pid)
    {
        require(pid >= 0, "fork failed");
    }

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;

    ~Child()
    {
        if (pid_ > 0)
        {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }

    void join()
    {
        int status = 0;
        pid_t waited;
        do
        {
            waited = ::waitpid(pid_, &status, 0);
        } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "wait for producer failed");
        pid_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "producer failed");
    }

  private:
    pid_t pid_;
};

void produce(std::uint16_t port, int gate, bool idle, unsigned units, unsigned connections)
{
    ::alarm(60);

    struct Client
    {
        Descriptor socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        std::array<char, 64 * 1024> payload;
        std::uint64_t left = 0;
        unsigned requests = 0;
        std::uint64_t request_started = 0;
    };

    std::vector<Client> clients(connections);
    std::vector<std::uint64_t> latencies;
    if (idle)
    {
        latencies.reserve(units);
    }
    Descriptor events(::epoll_create1(EPOLL_CLOEXEC));
    const int enabled = 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto per_socket = units / connections;
    for (unsigned slot = 0; slot < connections; ++slot)
    {
        auto &client = clients[slot];
        require(::setsockopt(client.socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled,
                             sizeof(enabled)) == 0,
                "producer TCP_NODELAY failed");
        require(::connect(client.socket.get(), reinterpret_cast<sockaddr *>(&address),
                          sizeof(address)) == 0,
                "producer connect failed");
        for (unsigned index = 0; index < client.payload.size(); ++index)
        {
            client.payload[index] = static_cast<char>(slot >> ((index % 4) * 8));
        }
        client.left = idle ? 32 : static_cast<std::uint64_t>(per_socket) * ReceiveBuffers::size;
        epoll_event event{};
        event.events = EPOLLOUT;
        event.data.u32 = slot;
        require(::epoll_ctl(events.get(), EPOLL_CTL_ADD, client.socket.get(), &event) == 0,
                "producer epoll add failed");
    }
    transfer(gate, "r", 1);
    token(gate, 'g');
    const auto cpu_start = now(CLOCK_THREAD_CPUTIME_ID);
    const auto wall_start = now(CLOCK_MONOTONIC);
    unsigned finished = 0;
    std::array<epoll_event, 256> ready;
    while (finished != connections)
    {
        const int count = ::epoll_wait(events.get(), ready.data(), ready.size(), -1);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        require(count > 0, "producer epoll wait failed");
        for (int index = 0; index < count; ++index)
        {
            const auto slot = ready[index].data.u32;
            auto &client = clients[slot];
            if (client.left)
            {
                if (idle && !client.request_started)
                {
                    client.request_started = now(CLOCK_MONOTONIC);
                }
                const auto offset =
                    idle ? 32 - client.left
                         : (static_cast<std::uint64_t>(per_socket) * ReceiveBuffers::size -
                            client.left) %
                               client.payload.size();
                const auto length =
                    std::min<std::uint64_t>(client.left, client.payload.size() - offset);
                const auto sent = ::send(client.socket.get(), client.payload.data() + offset,
                                         length, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent < 0 && (errno == EINTR || errno == EAGAIN))
                {
                    continue;
                }
                require(sent > 0, "producer send failed");
                client.left -= static_cast<std::size_t>(sent);
            }
            else
            {
                char ack = 0;
                const auto received = ::recv(client.socket.get(), &ack, 1, MSG_DONTWAIT);
                if (received < 0 && (errno == EINTR || errno == EAGAIN))
                {
                    continue;
                }
                require(idle && received == 1 && ack == 'a', "producer acknowledgement failed");
                latencies.push_back(now(CLOCK_MONOTONIC) - client.request_started);
                client.request_started = 0;
                if (++client.requests < per_socket)
                {
                    client.left = 32;
                }
            }
            epoll_event event{};
            event.data.u32 = slot;
            event.events = client.left ? EPOLLOUT : EPOLLIN;
            const bool done = idle ? client.requests == per_socket : !client.left;
            require(::epoll_ctl(events.get(), done ? EPOLL_CTL_DEL : EPOLL_CTL_MOD,
                                client.socket.get(), &event) == 0,
                    "producer epoll update failed");
            finished += done;
        }
    }
    ProducerMetrics metrics;
    metrics.wall_ns = now(CLOCK_MONOTONIC) - wall_start;
    metrics.cpu_ns = now(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    // Sort outside the measured loop and keep raw RTT separate from HTTP benchmark latency
    std::sort(latencies.begin(), latencies.end());
    metrics.latency_samples = latencies.size();
    if (!latencies.empty())
    {
        metrics.p50_ns = latencies[(latencies.size() - 1) * 50 / 100];
        metrics.p95_ns = latencies[(latencies.size() - 1) * 95 / 100];
        metrics.p99_ns = latencies[(latencies.size() - 1) * 99 / 100];
        metrics.max_ns = latencies.back();
    }
    // Keep peers open until the server has drained every original receive and cancel CQE
    token(gate, 'q');
    // A fork-local POD message, sent only after the actor stops its clocks
    transfer(gate, reinterpret_cast<const char *>(&metrics), sizeof(metrics));
}

std::uint16_t listen(int socket)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
                ::listen(socket, 4096) == 0,
            "fixture listen failed");
    socklen_t length = sizeof(address);
    require(::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "fixture port failed");

    return ntohs(address.sin_port);
}

void measure(ReceiveMode mode, bool idle, unsigned units, unsigned connections, ReceiveWait wait)
{
    Descriptor listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const auto port = listen(listener.get());
    std::array<int, 2> controls{};
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, controls.data()) == 0,
            "producer gate failed");
    const auto pid = ::fork();
    if (pid == 0)
    {
        ::close(controls[0]);
        try
        {
            produce(port, controls[1], idle, units, connections);
            ::_exit(0);
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "producer: %s\n", error.what());
            ::_exit(2);
        }
    }
    Child child(pid);
    ::close(controls[1]);
    Descriptor gate(controls[0]);
    // Accept concurrently with connect so setup does not depend on the listener backlog
    std::deque<Descriptor> peers;
    std::vector<int> sockets;
    sockets.reserve(connections);
    for (unsigned slot = 0; slot < connections; ++slot)
    {
        peers.emplace_back(
            ::accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        sockets.push_back(peers.back().get());
    }
    token(gate.get(), 'r');
    iovec vector{const_cast<char *>("a"), 1};
    msghdr response{};
    response.msg_iov = &vector;
    response.msg_iovlen = 1;
    ReceiveBackend backend(sockets, mode, wait);
    for (unsigned slot = 0; slot < connections; ++slot)
    {
        backend.receive(slot);
    }

    struct Progress
    {
        std::uint64_t bytes = 0;
        unsigned sends = 0;
        unsigned request_bytes = 0;
        bool ack = false;
        bool deferred = false;
    };

    std::vector<Progress> progress(connections);
    std::vector<unsigned> deferred;
    deferred.reserve(connections);
    std::vector<std::array<char, ReceiveBuffers::size>> expected(connections);
    for (unsigned slot = 0; slot < connections; ++slot)
    {
        for (unsigned index = 0; index < ReceiveBuffers::size; ++index)
        {
            expected[slot][index] = static_cast<char>(slot >> ((index % 4) * 8));
        }
    }
    const auto target = static_cast<std::uint64_t>(units) * (idle ? 32 : ReceiveBuffers::size);
    const auto socket_target = target / connections;
    std::uint64_t bytes = 0;
    std::uint64_t completions = 0;
    std::uint64_t sent = 0;
    auto next_probe = std::chrono::steady_clock::now();
    rusage before{}, after{};
    require(::getrusage(RUSAGE_SELF, &before) == 0, "read CPU before probe");
    const auto cpu_start = now(CLOCK_THREAD_CPUTIME_ID);
    const auto wall_start = now(CLOCK_MONOTONIC);
    transfer(gate.get(), "g", 1);
    while (bytes < target || (idle && sent < units))
    {
        backend.begin_turn(false, !deferred.empty());
        ReceiveCompletion completion;
        while (backend.next(completion))
        {
            const auto slot = completion.slot;
            auto &state = progress.at(slot);
            if (completion.event == ReceiveEvent::receive && completion.result > 0)
            {
                const auto data = completion.buffer.bytes();
                const auto offset = state.bytes % ReceiveBuffers::size;
                const auto first = std::min(data.size(), ReceiveBuffers::size - offset);
                require(completion.result > 0 &&
                            data.size() == static_cast<std::size_t>(completion.result) &&
                            std::memcmp(data.data(), expected[slot].data() + offset, first) == 0 &&
                            std::memcmp(data.data() + first, expected[slot].data(),
                                        data.size() - first) == 0,
                        "receive bytes differ");
                bytes += data.size();
                state.bytes += data.size();
                ++completions;
                require(state.bytes <= socket_target, "producer sent extra bytes on socket");
                completion.buffer.release();
                if (idle)
                {
                    state.request_bytes += static_cast<unsigned>(data.size());
                    require(state.request_bytes <= 32, "producer pipelined idle requests");
                    if (state.request_bytes == 32)
                    {
                        state.request_bytes = 0;
                        require(!state.ack, "producer queued unacknowledged requests");
                        state.ack = true;
                    }
                }
            }
            else if (completion.event == ReceiveEvent::send)
            {
                require(completion.result == 1, "acknowledgement send failed");
                ++sent;
                ++state.sends;
            }
            else
            {
                require(completion.event == ReceiveEvent::receive &&
                            completion.result == -ENOBUFS && !state.deferred,
                        "receive terminated before completing useful work");
                state.deferred = true;
                deferred.push_back(slot);
            }
            // A persistent receive can beat the previous SEND CQE with the next request
            if (state.ack && !backend.sending(slot))
            {
                backend.send(response, slot);
                state.ack = false;
            }
            if (state.bytes < socket_target && !state.deferred && !backend.receiving(slot) &&
                (mode == ReceiveMode::multishot || !backend.sending(slot)))
            {
                backend.receive(slot);
            }
        }
        backend.finish_turn();
        auto credits = backend.owner().receive_buffers->take_retry_credits(
            static_cast<unsigned>(deferred.size()));
        if (!deferred.empty() && !credits)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_probe)
            {
                credits = 1;
                next_probe = now + std::chrono::milliseconds(4);
            }
        }
        // Cursed: an ENOBUFS completion is terminal, but its socket is very much alive
        for (std::size_t index = 0; index < deferred.size() && credits;)
        {
            const auto slot = deferred[index];
            auto &state = progress[slot];
            if (mode == ReceiveMode::one_shot && backend.sending(slot))
            {
                ++index;
                continue;
            }
            state.deferred = false;
            deferred[index] = deferred.back();
            deferred.pop_back();
            if (state.bytes < socket_target)
            {
                backend.receive(slot);
                --credits;
            }
        }
    }
    const auto wall = now(CLOCK_MONOTONIC) - wall_start;
    const auto cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    require(::getrusage(RUSAGE_SELF, &after) == 0, "read CPU after probe");
    require(bytes == target && (!idle || sent == units), "different useful work");
    for (unsigned slot = 0; slot < connections; ++slot)
    {
        const auto &state = progress[slot];
        require(state.bytes == socket_target && !state.request_bytes && !state.ack &&
                    (!idle || state.sends == units / connections),
                "different useful work on socket");
        if (backend.receiving(slot))
        {
            backend.cancel_receive(slot);
        }
    }
    while (backend.pending())
    {
        backend.begin_turn(true);
        ReceiveCompletion completion;
        while (backend.next(completion))
        {
            require((completion.event == ReceiveEvent::receive &&
                     (completion.result == 0 || completion.result == -ECANCELED ||
                      completion.result == -ENOBUFS)) ||
                        (completion.event == ReceiveEvent::cancel &&
                         (completion.result == 0 || completion.result == -ENOENT ||
                          completion.result == -EALREADY)),
                    "teardown retained unexpected data or operations");
        }
        backend.finish_turn();
    }
    transfer(gate.get(), "q", 1);
    const auto producer = read_metrics(gate.get());
    child.join();
    require(producer.latency_samples == (idle ? units : 0), "producer RTT sample count differs");
    std::printf(
        "{\"mode\":\"%s\",\"case\":\"%s\",\"bytes\":%llu,\"receive_cqes\":%llu,\"sends\":%llu,"
        "\"server_thread_cpu_ns\":%llu,\"server_user_us\":%llu,\"server_system_us\":%llu,"
        "\"wall_ns\":%llu,\"buffer_pool_bytes\":%u,\"counted\":%d,\"receive_sqes\":%llu,"
        "\"terminal_receive_cqes\":%llu,\"enobufs\":%llu,\"connections\":%u,"
        "\"wait_policy\":\"%s\",\"cq_wait_nr\":%u,\"timeout_ms\":%u,"
        "\"waits\":%llu,\"timeouts\":%llu,\"ready_drains\":%llu,"
        "\"producer_cpu_ns\":%llu,\"producer_wall_ns\":%llu,\"rtt_samples\":%llu,"
        "\"rtt_p50_ns\":%llu,\"rtt_p95_ns\":%llu,\"rtt_p99_ns\":%llu,\"rtt_max_ns\":%llu}\n",
        mode == ReceiveMode::multishot ? "multishot" : "one_shot",
        idle ? "empty_pingpong" : "queued_stream", static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(completions), static_cast<unsigned long long>(sent),
        static_cast<unsigned long long>(cpu),
        static_cast<unsigned long long>(micros(after.ru_utime) - micros(before.ru_utime)),
        static_cast<unsigned long long>(micros(after.ru_stime) - micros(before.ru_stime)),
        static_cast<unsigned long long>(wall), ReceiveBuffers::count * ReceiveBuffers::size,
        TDS_RECEIVE_COUNTS, static_cast<unsigned long long>(backend.counts.submissions),
        static_cast<unsigned long long>(backend.counts.terminals),
        static_cast<unsigned long long>(backend.counts.enobufs), connections,
        wait == ReceiveWait::production    ? "batch128"
        : wait == ReceiveWait::ready_first ? "ready128"
                                           : "ready1",
        wait == ReceiveWait::ready_one ? 1U : 128U, wait == ReceiveWait::ready_one ? 0U : 8U,
        static_cast<unsigned long long>(backend.counts.waits),
        static_cast<unsigned long long>(backend.counts.timeouts),
        static_cast<unsigned long long>(backend.counts.ready_drains),
        static_cast<unsigned long long>(producer.cpu_ns),
        static_cast<unsigned long long>(producer.wall_ns),
        static_cast<unsigned long long>(producer.latency_samples),
        static_cast<unsigned long long>(producer.p50_ns),
        static_cast<unsigned long long>(producer.p95_ns),
        static_cast<unsigned long long>(producer.p99_ns),
        static_cast<unsigned long long>(producer.max_ns));
}
} // namespace

int main(int argc, char **argv)
{
    ::alarm(70);
    try
    {
        require(argc >= 4 && argc <= 6,
                "usage: receive_probe one|multi stream|idle TOTAL_UNITS [CONNECTIONS] "
                "[batch128|ready128|ready1]");
        const std::string_view backend(argv[1]);
        require(backend == "one" || backend == "multi", "unknown receive backend");
        const std::string_view mode(argv[2]);
        require(mode == "stream" || mode == "idle", "unknown receive case");
        unsigned units = 0;
        const std::string_view argument(argv[3]);
        const auto [end, error] =
            std::from_chars(argument.data(), argument.data() + argument.size(), units);
        require(error == std::errc{} && end == argument.data() + argument.size() && units > 0,
                "invalid unit count");
        unsigned connections = 1;
        if (argc >= 5)
        {
            const std::string_view count(argv[4]);
            const auto [last, status] =
                std::from_chars(count.data(), count.data() + count.size(), connections);
            require(status == std::errc{} && last == count.data() + count.size() &&
                        connections > 0 && connections <= 4096 && units % connections == 0,
                    "connection count must divide total units and be in 1..4096");
        }
        ReceiveWait wait = ReceiveWait::production;
        if (argc == 6)
        {
            const std::string_view policy(argv[5]);
            require(policy == "batch128" || policy == "ready128" || policy == "ready1",
                    "unknown wait policy");
            wait = policy == "batch128"   ? ReceiveWait::production
                   : policy == "ready128" ? ReceiveWait::ready_first
                                          : ReceiveWait::ready_one;
        }
        measure(backend == "multi" ? ReceiveMode::multishot : ReceiveMode::one_shot, mode == "idle",
                units, connections, wait);

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "receive probe: %s\n", error.what());

        return 1;
    }
}
