#include "transport.hpp"
#include "transport/ring.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <linux/io_uring.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace
{

static_assert(sizeof(tds::transport::Connection) == 56);

class Client
{
  public:
    explicit Client(std::uint16_t port, int receive_buffer = 0, std::uint32_t source = 0)
        : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
    {
        if (fd_ < 0)
        {
            throw std::runtime_error(std::strerror(errno));
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (source)
        {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(source);
            if (::bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0)
            {
                ::close(fd_);
                throw std::runtime_error("client source address");
            }
        }
        const timeval timeout{5, 0};
        if ((receive_buffer && ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                                            sizeof(receive_buffer)) < 0) ||
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
            ::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
        {
            ::close(fd_);
            throw std::runtime_error(std::strerror(errno));
        }
    }

    ~Client()
    {
        ::close(fd_);
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void send(std::string_view text) const
    {
        while (!text.empty())
        {
            const auto size = ::send(fd_, text.data(), text.size(), MSG_NOSIGNAL);
            if (size < 0 && errno == EINTR)
            {
                continue;
            }
            if (size <= 0)
            {
                throw std::runtime_error("client send failed");
            }
            text.remove_prefix(static_cast<std::size_t>(size));
        }
    }

    std::string response(tds::Status status = tds::Status::ok)
    {
        std::size_t end = std::string::npos;
        while ((end = received_.find("\r\n\r\n")) == std::string::npos)
        {
            receive();
        }
        if (!received_.starts_with("HTTP/1.1 " +
                                   std::to_string(static_cast<unsigned short>(status)) + ' '))
        {
            throw std::runtime_error("unexpected response status");
        }
        const auto start = received_.find("Content-Length: ");
        if (start == std::string::npos)
        {
            throw std::runtime_error("missing content length");
        }
        std::size_t length = 0;
        const auto result =
            std::from_chars(received_.data() + start + 16, received_.data() + end, length);
        if (result.ec != std::errc{})
        {
            throw std::runtime_error("invalid content length");
        }
        while (received_.size() < end + 4 + length)
        {
            receive();
        }
        std::string body = received_.substr(end + 4, length);
        received_.erase(0, end + 4 + length);
        return body;
    }

    void drain_available()
    {
        while (receive(MSG_DONTWAIT))
        {
        }
    }

    void finish_writing() const
    {
        if (::shutdown(fd_, SHUT_WR) < 0)
        {
            throw std::runtime_error("client write shutdown failed");
        }
    }

    bool response_available() const
    {
        pollfd socket{fd_, POLLIN, 0};
        int result;
        do
        {
            result = ::poll(&socket, 1, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
        {
            throw std::runtime_error("poll client response");
        }
        return !received_.empty() || result > 0;
    }

    bool closed() const
    {
        char byte;
        ssize_t result;
        do
        {
            result = ::recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (result < 0 && errno == EINTR);
        return result == 0 || (result < 0 && errno == ECONNRESET);
    }

  private:
    bool receive(int flags = 0)
    {
        char buffer[4096];
        ssize_t size;
        do
        {
            size = ::recv(fd_, buffer, sizeof(buffer), flags);
        } while (size < 0 && errno == EINTR);
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && flags == MSG_DONTWAIT)
        {
            return false;
        }
        if (size <= 0)
        {
            throw std::runtime_error(size < 0 ? std::strerror(errno) : "server closed response");
        }
        received_.append(buffer, static_cast<std::size_t>(size));
        return true;
    }

    int fd_;
    std::string received_;
};

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void exercise_request_reuse()
{
    tds::transport::Ring ring(8, true);
    auto exchange = ring.acquire_exchange();
    auto &parser = exchange->parser;
    auto &request = parser.request();
    require(request.endpoint == tds::Endpoint::unresolved && !request.identity,
            "new request has no route or identity");
    auto expected_capacity = parser.scratch_capacity();
    for (auto member : {&tds::Request::method, &tds::Request::path, &tds::Request::authorization,
                        &tds::Request::body})
    {
        auto &field = request.*member;
        const auto old_capacity = field.capacity();
        field.reserve(256);
        expected_capacity += field.capacity() - old_capacity;
        require(parser.scratch_capacity() == expected_capacity,
                "retained parser capacity includes each request string");
    }

    const std::string path = "/" + std::string(128, 'p');
    const std::string authorization = "Bearer " + std::string(512, 't');
    const std::string body(2048, 'b');
    const std::string wire = "POST " + path + " HTTP/1.1\r\nAuthorization: " + authorization +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n" + body;
    const auto headers = parser.parse(wire);
    require(headers.valid && parser.headers_pending(), "reused request pauses for authorization");
    require(request.method == "POST" && request.path == path &&
                request.authorization == authorization && !request.keep_alive,
            "reused request headers parsed");
    request.identity = tds::Identity{42, "alice"};
    request.endpoint = tds::Endpoint::create;
    parser.accept_headers();
    const auto parsed_body = parser.parse(std::string_view(wire).substr(headers.consumed));
    require(parsed_body.valid && parser.complete() && request.body == body,
            "reused request body parsed");
    const auto retained_capacity = exchange->retained_capacity();
    require(retained_capacity <= 8192, "ordinary request fits exchange retention budget");
    exchange->finish_response();
    require(request.method.empty() && request.path.empty() && request.authorization.empty() &&
                request.body.empty() && !request.identity &&
                request.endpoint == tds::Endpoint::unresolved && request.keep_alive,
            "finished request clears strings, route, identity and connection policy");
    require(!parser.complete() && !parser.headers_pending() &&
                parser.error() == tds::Status::bad_request,
            "finished request resets parser state");
    require(exchange->retained_capacity() == retained_capacity,
            "finished request preserves bounded string storage");
    const auto *pooled = exchange.get();
    ring.recycle_exchange(std::move(exchange));
    exchange = ring.acquire_exchange();
    require(exchange.get() == pooled, "bounded exchange reused from pool");
    const auto next = exchange->parser.parse("GET /fresh HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(next.valid && exchange->parser.headers_pending(), "recycled parser reads next request");
    const auto &fresh = exchange->parser.request();
    require(fresh.method == "GET" && fresh.path == "/fresh" && fresh.authorization.empty() &&
                fresh.body.empty() && !fresh.identity &&
                fresh.endpoint == tds::Endpoint::unresolved && fresh.keep_alive,
            "recycled request does not inherit authorization, body, route or identity");
    exchange->parser.accept_headers();
    require(exchange->parser.parse({}).valid && exchange->parser.complete(),
            "recycled parser completes an empty body");
    exchange->finish_response();
    ring.recycle_exchange(std::move(exchange));

    for (auto member : {&tds::Request::path, &tds::Request::authorization, &tds::Request::body})
    {
        exchange = ring.acquire_exchange();
        (exchange->parser.request().*member).assign(8193, 'x');
        exchange->finish_response();
        require(exchange->retained_capacity() > 8192,
                "oversized cleared request storage exceeds retention budget");
        ring.recycle_exchange(std::move(exchange));
        exchange = ring.acquire_exchange();
        require(exchange->retained_capacity() <= 8192,
                "oversized request storage is discarded instead of pooled");
        ring.recycle_exchange(std::move(exchange));
    }
}

class RequestGate
{
  public:
    void block()
    {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] {
            return released;
        });
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] {
                                     return entered;
                                 }),
                "blocking request reached handler");
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

    void reset()
    {
        std::lock_guard lock(mutex);
        entered = false;
        released = false;
    }

  private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

void limit_listener_send_buffer(std::uint16_t port)
{
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        const auto name = entry.path().filename().string();
        int descriptor = -1;
        if (std::from_chars(name.data(), name.data() + name.size(), descriptor).ec != std::errc{})
        {
            continue;
        }
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        int listening = 0;
        socklen_t option_length = sizeof(listening);
        if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) == 0 &&
            address.sin_family == AF_INET && address.sin_port == htons(port) &&
            ::getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &listening, &option_length) == 0 &&
            listening)
        {
            const int size = 4096;
            require(::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0,
                    "listener send buffer");
            return;
        }
    }
    throw std::runtime_error("server listener not found");
}

std::vector<std::filesystem::path> ring_fdinfos(pid_t process = ::getpid())
{
    const auto directory = std::filesystem::path("/proc") / std::to_string(process);
    std::vector<std::filesystem::path> result;
    for (const auto &entry : std::filesystem::directory_iterator(directory / "fd"))
    {
        std::error_code error;
        const auto target = std::filesystem::read_symlink(entry.path(), error);
        if (!error && target.string().find("io_uring") != std::string::npos)
        {
            result.push_back(directory / "fdinfo" / entry.path().filename());
        }
    }
    require(!result.empty(), "io_uring fdinfo found");
    return result;
}

void wait_for_completion(int result, std::size_t minimum = 1)
{
    const std::string expected = "res:" + std::to_string(result) + ",";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        std::size_t observed = 0;
        for (const auto &info : ring_fdinfos())
        {
            std::ifstream stream(info);
            for (std::string line; std::getline(stream, line);)
            {
                if (line.find(expected) != std::string::npos && ++observed >= minimum)
                {
                    return;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("expected io_uring completion " + std::to_string(result));
}

class RunningServer
{
  public:
    RunningServer(tds::Server::Handler handler, tds::Server::HeaderHandler headers,
                  tds::Server::BatchHandler writes, std::function<void()> release)
        : release_(std::move(release)),
          server_("127.0.0.1", 0, std::move(handler), std::move(headers), std::move(writes)),
          thread_([this] {
              try
              {
                  server_.run();
              }
              catch (...)
              {
                  error_ = std::current_exception();
              }
          })
    {
    }

    ~RunningServer()
    {
        release_();
        server_.request_stop();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    RunningServer(const RunningServer &) = delete;
    RunningServer &operator=(const RunningServer &) = delete;

    std::uint16_t port() const
    {
        return server_.port();
    }

    void request_stop() const
    {
        server_.request_stop();
    }

    void stop()
    {
        release_();
        server_.request_stop();
        thread_.join();
        if (error_)
        {
            std::rethrow_exception(error_);
        }
    }

  private:
    std::function<void()> release_;
    tds::Server server_;
    std::exception_ptr error_;
    std::jthread thread_;
};

std::string write_request(std::string_view path, std::string_view body)
{
    return "POST " + std::string(path) +
           " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + std::string(body);
}

std::unique_ptr<Client> ready_client(std::uint16_t port)
{
    auto client = std::make_unique<Client>(port);
    client->send("GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(client->response() == "/ready", "batch client established");
    return client;
}

void exercise_write_batches()
{
    RequestGate headers_gate;
    RequestGate batch_gate;
    std::mutex batch_mutex;
    std::vector<std::size_t> batch_sizes;
    std::vector<std::string> blocked_bodies;
    const auto recorded_batches = [&] {
        std::lock_guard lock(batch_mutex);
        return batch_sizes;
    };
    RunningServer server(
        [](const tds::Request &request) -> tds::RequestResult {
            if (request.endpoint == tds::Endpoint::create)
            {
                require(request.identity && request.identity->user_id == 42 &&
                            request.identity->username == "writer",
                        "write preparation receives the current request identity");
                return tds::WriteCommand{tds::CreatePost{*request.identity->user_id,
                                                         request.identity->username, request.body}};
            }
            require(request.endpoint == tds::Endpoint::feed && !request.identity,
                    "read after a write has its own route and no stale identity");
            return tds::Response{tds::Status::ok, request.path};
        },
        [&](tds::Request &request) -> std::optional<tds::Response> {
            require(request.endpoint == tds::Endpoint::unresolved && !request.identity,
                    "keepalive request begins without a stale route or identity");
            request.endpoint =
                request.method == "POST" ? tds::Endpoint::create : tds::Endpoint::feed;
            if (request.path == "/hold")
            {
                headers_gate.block();
            }
            if (request.method == "POST")
            {
                request.identity = tds::Identity{42, "writer"};
            }
            return std::nullopt;
        },
        [&](std::span<const tds::WriteCommand> commands, std::span<tds::Response> responses) {
            require(!commands.empty() && commands.size() == responses.size() &&
                        commands.size() <= tds::write_batch_limit,
                    "write callback receives bounded matching spans");
            {
                std::lock_guard lock(batch_mutex);
                batch_sizes.push_back(commands.size());
            }
            std::vector<std::string> bodies;
            for (std::size_t index = 0; index < commands.size(); ++index)
            {
                const auto *create = std::get_if<tds::CreatePost>(&commands[index]);
                require(create && create->user_id == 42 && create->username == "writer",
                        "batched command owns the prepared identity and create data");
                bodies.push_back(create->body);
                responses[index] = {tds::Status::created, create->body};
            }
            if (bodies.front().starts_with("blocked:"))
            {
                blocked_bodies = bodies;
                batch_gate.block();
            }
            for (std::size_t index = 0; index < commands.size(); ++index)
            {
                const auto &create = std::get<tds::CreatePost>(commands[index]);
                require(create.user_id == 42 && create.username == "writer" &&
                            create.body == bodies[index],
                        "owned commands survive the batch callback and peer disconnect");
            }
            if (bodies.front() == "throw-body")
            {
                throw std::runtime_error("write callback failed after filling responses");
            }
        },
        [&] {
            headers_gate.release();
            batch_gate.release();
        });

    Client singleton(server.port());
    singleton.send(write_request("/write", "singleton"));
    require(singleton.response(tds::Status::created) == "singleton",
            "single queued write flushes without another request");
    singleton.send(write_request("/write", "first") + write_request("/write", "second") +
                   "GET /after-write HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(singleton.response(tds::Status::created) == "first", "first batched pipeline response");
    require(singleton.response(tds::Status::created) == "second",
            "second batched pipeline response");
    require(singleton.response() == "/after-write", "read follows batched pipeline writes");
    require(recorded_batches() == std::vector<std::size_t>{1, 1, 1},
            "a connection holds one write until its response completes");

    std::vector<std::unique_ptr<Client>> clients;
    std::vector<std::string> bodies;
    constexpr auto count = tds::write_batch_limit + 1;
    static_assert(tds::write_batch_limit == 16);
    for (std::size_t index = 0; index < count; ++index)
    {
        clients.push_back(ready_client(server.port()));
        bodies.push_back("blocked:" + std::string(index < 10 ? "0" : "") + std::to_string(index) +
                         ':' + std::string(1024, static_cast<char>('a' + index)));
    }
    auto blocker = ready_client(server.port());
    blocker->send("GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n");
    headers_gate.wait();
    const auto before_batches = recorded_batches().size();
    for (std::size_t index = 0; index < count; ++index)
    {
        clients[index]->send(write_request("/write-block", bodies[index]));
    }
    const auto wire_size = write_request("/write-block", bodies.front()).size();
    // Every socket already has a receive submitted before the header callback blocks
    wait_for_completion(static_cast<int>(wire_size), count);
    headers_gate.release();
    batch_gate.wait();
    require(blocked_bodies.size() == 16, "ready writes flush at the sixteen-request limit");
    for (const auto &client : clients)
    {
        require(!client->response_available(), "no write response before callback returns");
    }
    const auto disconnected = std::find(bodies.begin(), bodies.end(), blocked_bodies.front());
    require(disconnected != bodies.end(), "blocked request belongs to a live client");
    clients[static_cast<std::size_t>(disconnected - bodies.begin())].reset();
    batch_gate.release();
    require(blocker->response() == "/hold", "batch preload gate completes");
    for (std::size_t index = 0; index < count; ++index)
    {
        if (clients[index])
        {
            require(clients[index]->response(tds::Status::created) == bodies[index],
                    "batched response preserves each request body after peer disconnect");
        }
    }
    const auto after_batches = recorded_batches();
    require(after_batches.size() == before_batches + 2 && after_batches[before_batches] == 16 &&
                after_batches[before_batches + 1] == 1,
            "ready writes form one full batch and flush the final singleton");

    headers_gate.reset();
    auto failed_first = ready_client(server.port());
    auto failed_second = ready_client(server.port());
    blocker->send("GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n");
    headers_gate.wait();
    const auto before_throw = recorded_batches().size();
    const auto throwing_wire = write_request("/write-throw", "throw-body");
    failed_first->send(throwing_wire);
    failed_second->send(throwing_wire);
    wait_for_completion(static_cast<int>(throwing_wire.size()), 2);
    headers_gate.release();
    require(failed_first->response(tds::Status::internal_error) != "throw-body" &&
                failed_second->response(tds::Status::internal_error) != "throw-body",
            "throwing callback replaces every partially filled response with an error");
    require(blocker->response() == "/hold", "throwing batch preload gate completes");
    const auto after_throw = recorded_batches();
    require(after_throw.size() == before_throw + 1 && after_throw.back() == 2,
            "throwing callback consumes one complete batch");
    Client fresh(server.port());
    fresh.send(write_request("/write", "fresh-owned-body"));
    require(fresh.response(tds::Status::created) == "fresh-owned-body",
            "batch callback failure leaves no stale commands or responses");
    server.stop();
}

void exercise_queued_write_shutdown()
{
    RequestGate preload_gate;
    std::size_t queued_writes = 0;
    std::size_t batches = 0;
    RunningServer server(
        [&](const tds::Request &request) -> tds::RequestResult {
            if (request.method == "POST")
            {
                ++queued_writes;
                return tds::WriteCommand{tds::CreatePost{42, "writer", request.body}};
            }
            return tds::Response{tds::Status::ok, request.path};
        },
        [&](tds::Request &request) -> std::optional<tds::Response> {
            if (request.path == "/hold")
            {
                preload_gate.block();
            }
            return std::nullopt;
        },
        [&](std::span<const tds::WriteCommand>, std::span<tds::Response>) {
            ++batches;
        },
        [&] {
            preload_gate.release();
        });
    auto first = ready_client(server.port());
    auto second = ready_client(server.port());
    auto blocker = ready_client(server.port());
    blocker->send("GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n");
    preload_gate.wait();
    const auto first_wire = write_request("/first", "queued-body");
    first->send(first_wire);
    second->send(write_request("/other", "queued-body"));
    wait_for_completion(static_cast<int>(first_wire.size()), 2);
    first.reset();
    server.request_stop();
    wait_for_completion(POLLIN);
    // Both receives precede stop in the CQ before their drain starts, including when
    // io_uring_for_each_cqe takes a snapshot of the CQ tail for the entire drain
    preload_gate.release();
    server.stop();
    require(queued_writes == 2, "both completed writes are prepared before stop is serviced");
    require(batches == 0, "shutdown discards queued writes without calling the batch handler");
    require(second->closed(), "shutdown closes the remaining queued connection");
}

void wait_for_blocked_send(pid_t process = ::getpid())
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        for (const auto &info : ring_fdinfos(process))
        {
            std::ifstream stream(info);
            for (std::string line; std::getline(stream, line);)
            {
                if (line.find("  op=" + std::to_string(IORING_OP_SEND) + ',') !=
                        std::string::npos ||
                    line.find("  op=" + std::to_string(IORING_OP_SENDMSG) + ',') !=
                        std::string::npos)
                {
                    return;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("large send did not reach socket backpressure");
}

void exercise_short_send(std::uint16_t port, RequestGate &gate)
{
    limit_listener_send_buffer(port);
    Client slow(port, 4096);
    slow.send("GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(slow.response() == "/ready", "slow connection established");
    slow.send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
    wait_for_blocked_send();
    // Readability can arrive while a partial response still owns the data-I/O operation
    slow.send("GET /after HTTP/1.1\r\nHost: localhost\r\n\r\n");
    Client blocker(port);
    // This round trip submits the slow socket's next send before the handler blocks.
    blocker.send("GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(blocker.response() == "/ready", "short-send barrier");
    blocker.send("GET /block HTTP/1.1\r\nHost: localhost\r\n\r\n");
    gate.wait();
    bool observed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        slow.drain_available();
        for (const auto &info : ring_fdinfos())
        {
            std::ifstream stream(info);
            for (std::string line; std::getline(stream, line);)
            {
                const auto start = line.find("res:");
                int result = 0;
                if (start != std::string::npos &&
                    std::from_chars(line.data() + start + 4, line.data() + line.size(), result)
                            .ec == std::errc{} &&
                    result > 1024 && result < 1024 * 1024)
                {
                    observed = true;
                }
            }
        }
        if (!observed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while (!observed && std::chrono::steady_clock::now() < deadline);
    require(observed, "short send CQE observed before draining a 1 MiB response");
    gate.release();
    require(blocker.response() == "/block", "short-send blocking response");
    require(slow.response() == std::string(1024 * 1024, 'x'), "short-send body ownership");
    require(slow.response() == "/after", "pipeline after a short send");
    gate.reset();
}

std::string pool_body(std::size_t id)
{
    return std::to_string(id) + ':' + std::string(2048, static_cast<char>('a' + id % 26));
}

void send_pool_requests(const std::vector<std::unique_ptr<Client>> &clients)
{
    for (std::size_t id = 0; id < clients.size(); ++id)
    {
        const auto body = pool_body(id);
        clients[id]->send("POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body);
    }
}

void exercise_pool(std::uint16_t port, const std::vector<std::unique_ptr<Client>> &clients,
                   RequestGate &gate)
{
    Client blocker(port);
    blocker.send("GET /block HTTP/1.1\r\nHost: localhost\r\n\r\n");
    gate.wait();
    send_pool_requests(clients);
    // Observe a real kernel CQE while the handler cannot return buffers to the pool
    wait_for_completion(-ENOBUFS);
    gate.release();
    require(blocker.response() == "/block", "blocking response");
    for (std::size_t id = 0; id < clients.size(); ++id)
    {
        require(clients[id]->response() == pool_body(id), "receive pool response ownership");
    }
}

void exercise_parser_limits(std::uint16_t port)
{
    for (const std::size_t size : {16384U, 16385U})
    {
        const std::string path = "/" + std::string(size - 1, 'x');
        const std::array<std::string, 3> requests{
            "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n",
            "GET /limit HTTP/1.1\r\n" + std::string(size, 'x') + ": ok\r\n\r\n",
            "GET /limit HTTP/1.1\r\nX-Test: " + std::string(size, 'x') + "\r\n\r\n"};
        for (std::size_t index = 0; index < requests.size(); ++index)
        {
            Client client(port);
            client.send(requests[index]);
            if (size == 16384)
            {
                require(client.response() == (index ? "/limit" : path), "parser token at limit");
            }
            else
            {
                require(client.response(tds::Status::bad_request) ==
                            "{\"error\":\"malformed HTTP request\"}",
                        "parser rejects oversized token");
            }
        }
    }
    Client body(port);
    body.send("POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16385\r\n\r\n");
    require(body.response(tds::Status::too_large) == "{\"error\":\"malformed HTTP request\"}",
            "oversized content length rejected before body arrives");
}

void exercise(std::uint16_t port)
{
    exercise_parser_limits(port);
    Client connection(port);
    connection.send("GET /first HTTP/1.1\r\nHo");
    connection.send("st: localhost\r\n\r\n");
    require(connection.response() == "/first", "fragmented headers");
    connection.send("POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n\r\nab");
    connection.send("cdef");
    require(connection.response() == "abcdef", "fragmented body");
    connection.send(
        "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\nGET /two HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(connection.response() == "/one", "first pipelined response");
    require(connection.response() == "/two", "second pipelined response");
    connection.send("POST /body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: "
                    "chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n");
    require(connection.response() == "abcdef", "chunked body");
    connection.send("GET /%66eed?ignored=yes HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(connection.response() == "/feed", "path decoding");
    connection.send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(connection.response() == std::string(1024 * 1024, 'x'), "partial sends");
    connection.send("POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n"
                    "GET /small HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(connection.response().empty(), "header-only response");
    require(connection.response() == "/small", "small response after an empty body");
    Client half_closed(port);
    half_closed.send("GET /half-closed HTTP/1.1\r\nHost: localhost\r\n\r\n");
    half_closed.finish_writing();
    require(half_closed.response() == "/half-closed", "readable bytes survive peer half-close");
    std::vector<std::unique_ptr<Client>> burst;
    for (int i = 0; i < 128; ++i)
    {
        auto client = std::make_unique<Client>(port);
        client->send("GET /burst HTTP/1.1\r\nHost: localhost\r\n\r\n"
                     "GET /again HTTP/1.1\r\nHost: localhost\r\n\r\n");
        burst.push_back(std::move(client));
    }
    for (auto &client : burst)
    {
        require(client->response() == "/burst", "burst response");
        require(client->response() == "/again", "burst pipelined response");
    }
    burst.clear();
    for (int i = 0; i < 100; ++i)
    {
        Client disconnected(port);
        disconnected.send(
            "POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1000\r\n\r\na");
    }
    connection.send("GET /alive HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    require(connection.response() == "/alive", "server after client disconnects");
}

class Pipe
{
  public:
    Pipe()
    {
        require(::pipe(descriptors.data()) == 0, "child control pipe");
    }

    ~Pipe()
    {
        close_reader();
        close_writer();
    }

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;

    int reader() const
    {
        return descriptors[0];
    }

    int writer() const
    {
        return descriptors[1];
    }

    void close_reader()
    {
        close(descriptors[0]);
    }

    void close_writer()
    {
        close(descriptors[1]);
    }

  private:
    static void close(int &descriptor)
    {
        if (descriptor >= 0)
        {
            ::close(descriptor);
            descriptor = -1;
        }
    }

    std::array<int, 2> descriptors{-1, -1};
};

class ChildProcess
{
  public:
    explicit ChildProcess(pid_t process) : process_(process)
    {
    }

    ~ChildProcess()
    {
        if (process_ > 0)
        {
            ::kill(process_, SIGKILL);
            while (::waitpid(process_, nullptr, 0) < 0 && errno == EINTR)
            {
            }
        }
    }

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    void wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do
        {
            int status = 0;
            const auto result = ::waitpid(process_, &status, WNOHANG);
            if (result == process_)
            {
                process_ = -1;
                require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "low-limit child exited successfully");
                return;
            }
            require(result == 0 || (result < 0 && errno == EINTR), "wait for child server");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("low-limit child shutdown timed out");
    }

  private:
    pid_t process_;
};

int child_server(int ready, int stop, rlim_t descriptor_limit)
{
    try
    {
        const rlimit limit{descriptor_limit, descriptor_limit};
        require(::setrlimit(RLIMIT_NOFILE, &limit) == 0, "set child descriptor limit");
        tds::Server server(
            "127.0.0.1", 0,
            [](const tds::Request &request) -> tds::Response {
                if (request.path == "/large")
                {
                    return {tds::Status::ok, std::string(1024 * 1024, 'x')};
                }
                return {tds::Status::ok, request.path == "/body" ? request.body : request.path};
            },
            [](tds::Request &request) -> std::optional<tds::Response> {
                if (request.path == "/auth" && request.authorization != "Bearer accepted")
                {
                    return tds::Response{tds::Status::unauthorized, "rejected before body"};
                }
                return std::nullopt;
            });
        limit_listener_send_buffer(server.port());
        const auto port = server.port();
        ssize_t written;
        do
        {
            written = ::write(ready, &port, sizeof(port));
        } while (written < 0 && errno == EINTR);
        require(written == static_cast<ssize_t>(sizeof(port)), "publish child server port");
        std::jthread control([&](std::stop_token token) {
            while (!token.stop_requested())
            {
                pollfd pipe{stop, POLLIN, 0};
                const int result = ::poll(&pipe, 1, 50);
                if (result > 0 || (result < 0 && errno != EINTR))
                {
                    server.request_stop();
                    return;
                }
            }
        });
        server.run();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "child server: " << error.what() << '\n';
        return 1;
    }
}

std::optional<std::size_t> registered_files(const std::vector<std::filesystem::path> &infos)
{
    std::size_t count = 0;
    for (const auto &info : infos)
    {
        std::ifstream stream(info);
        bool files = false;
        std::size_t expected = 0;
        std::size_t observed = 0;
        for (std::string line; std::getline(stream, line);)
        {
            if (line.starts_with("UserFiles:"))
            {
                const auto start = line.find_first_of("0123456789");
                require(
                    start != std::string::npos &&
                        std::from_chars(line.data() + start, line.data() + line.size(), expected)
                                .ec == std::errc{},
                    "registered file table size");
                files = true;
            }
            else if (line.starts_with("UserBufs:"))
            {
                break;
            }
            else if (files)
            {
                ++observed;
                if (line.find("<none>") == std::string::npos)
                {
                    ++count;
                }
            }
        }
        // fdinfo omits table entries when its nonblocking kernel lock attempt fails.
        if (!files || observed != expected)
        {
            return std::nullopt;
        }
    }
    return count;
}

void exercise_capacity(std::size_t count, rlim_t descriptor_limit, bool progress = false)
{
    require(count > descriptor_limit, "capacity clients exceed a single direct table");
    const auto stage = [progress](std::string_view message) {
        if (progress)
        {
            std::cout << "capacity: " << message << '\n' << std::flush;
        }
    };
    Pipe ready;
    Pipe stop;
    const auto process = ::fork();
    require(process >= 0, "fork low-limit server");
    if (process == 0)
    {
        ready.close_reader();
        stop.close_writer();
        const auto result = child_server(ready.writer(), stop.reader(), descriptor_limit);
        ::_exit(result);
    }
    ChildProcess child(process);
    ready.close_writer();
    stop.close_reader();
    pollfd startup{ready.reader(), POLLIN, 0};
    int available;
    do
    {
        available = ::poll(&startup, 1, 10000);
    } while (available < 0 && errno == EINTR);
    require(available > 0, "child startup timed out");
    std::uint16_t port = 0;
    ssize_t received;
    do
    {
        received = ::read(ready.reader(), &port, sizeof(port));
    } while (received < 0 && errno == EINTR);
    require(received == static_cast<ssize_t>(sizeof(port)), "read child server port");
    ready.close_reader();

    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(count);
    for (std::size_t id = 0; id < count; ++id)
    {
        // Distinct loopback source addresses avoid ephemeral tuple exhaustion in opt-in runs.
        const auto source = INADDR_LOOPBACK + 1U + static_cast<std::uint32_t>(id / 16000);
        // Keep the final unread response bounded even after its earlier 1 MiB round trip.
        auto client = std::make_unique<Client>(port, id + 1 == count ? 4096 : 0, source);
        client->send("GET /idle HTTP/1.1\r\nHost: localhost\r\n\r\n");
        require(client->response() == "/idle", "direct connection established");
        clients.push_back(std::move(client));
    }
    const auto infos = ring_fdinfos(process);
    require(infos.size() > 1, "multiple io_uring rings used at low descriptor limit");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto files_before = registered_files(infos);
    while (!files_before || *files_before < count)
    {
        require(std::chrono::steady_clock::now() < deadline,
                "all client sockets are registered direct descriptors");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        files_before = registered_files(infos);
    }
    stage("all clients accepted and registered across multiple rings");
    const auto removed = std::min(static_cast<std::size_t>(descriptor_limit), count / 2);
    for (std::size_t id = 0; id < removed; ++id)
    {
        clients[id].reset();
    }
    auto &satellite = *clients.back();
    satellite.send("POST /body HTTP/1.1\r\nHo");
    satellite.send("st: localhost\r\nContent-Length: 6\r\n\r\nab");
    satellite.send("cdefGET /satellite HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(satellite.response() == "abcdef", "satellite fragmented request body");
    require(satellite.response() == "/satellite", "satellite pipeline after primary closes");
    satellite.send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(satellite.response() == std::string(1024 * 1024, 'x'), "satellite partial writes");
    {
        Client rejected(port);
        rejected.send("POST /auth HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999\r\n\r\n");
        require(rejected.response(tds::Status::unauthorized) == "rejected before body",
                "direct socket rejects authorization before an absent body");
    }
    stage("satellite protocol and authorization checks passed after primary closes");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true)
    {
        const auto remaining = registered_files(infos);
        if (remaining && *remaining <= *files_before - removed)
        {
            break;
        }
        require(std::chrono::steady_clock::now() < deadline, "closed direct slots released");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (std::size_t id = 0; id < removed; ++id)
    {
        const auto source = INADDR_LOOPBACK + 1024U + static_cast<std::uint32_t>(id / 16000);
        auto client = std::make_unique<Client>(port, 0, source);
        client->send("GET /reused HTTP/1.1\r\nHost: localhost\r\n\r\n");
        require(client->response() == "/reused", "reused direct slot responds");
        clients[id] = std::move(client);
    }
    require(ring_fdinfos(process).size() == infos.size(), "reuse does not allocate another ring");
    stage("closed direct slots reused without allocating another ring");
    for (std::size_t id = 0; id + 1 < clients.size(); id += 2)
    {
        clients[id]->send("POST /body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999\r\n\r\na");
        clients[id].reset();
    }
    clients.back()->send("GET /alive HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(clients.back()->response() == "/alive", "satellite survives abrupt partial requests");
    stage("abrupt partial closes passed, waiting for unread satellite send");
    clients.back()->send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
    wait_for_blocked_send(process);
    stage("satellite send backpressure observed, stopping with live sockets");
    stop.close_writer();
    child.wait();
    std::cout << count << " external clients, " << descriptor_limit << " ordinary-fd limit, "
              << infos.size() << " rings, slot reuse and live-socket shutdown passed\n";
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc > 1)
        {
            require((argc == 3 || argc == 5) && std::string_view(argv[1]) == "--capacity",
                    "usage: transport_tests [--capacity N [--limit L]]");
            std::size_t count = 0;
            const std::string_view argument(argv[2]);
            const auto parsed =
                std::from_chars(argument.data(), argument.data() + argument.size(), count);
            require(parsed.ec == std::errc{} && parsed.ptr == argument.data() + argument.size(),
                    "invalid capacity count");
            rlim_t limit = 65535;
            if (argc == 5)
            {
                require(std::string_view(argv[3]) == "--limit", "expected --limit L");
                const std::string_view value(argv[4]);
                const auto result =
                    std::from_chars(value.data(), value.data() + value.size(), limit);
                require(result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                            limit >= 64 && limit <= 65535,
                        "invalid child descriptor limit");
            }
            exercise_capacity(count, limit, true);
            return 0;
        }
        // Fork before this process creates a server thread or io_uring.
        exercise_capacity(768, 256);
        exercise_request_reuse();
        exercise_write_batches();
        exercise_queued_write_shutdown();
        RequestGate gate;
        tds::Server server(
            "127.0.0.1", 0,
            [&gate](const tds::Request &request) -> tds::Response {
                if (request.path == "/block")
                {
                    gate.block();
                }
                if (request.path == "/large")
                {
                    return {tds::Status::ok, std::string(1024 * 1024, 'x')};
                }
                return {tds::Status::ok, request.path == "/body" ? request.body : request.path};
            },
            [](tds::Request &) -> std::optional<tds::Response> {
                return std::nullopt;
            });
        std::exception_ptr server_error;
        std::jthread thread([&] {
            try
            {
                server.run();
            }
            catch (...)
            {
                server_error = std::current_exception();
            }
        });
        std::vector<std::unique_ptr<Client>> idle;
        try
        {
            exercise(server.port());
            exercise_short_send(server.port(), gate);
            // Exercise cancellation across more operations than the submission ring holds.
            for (int i = 0; i < 1536; ++i)
            {
                auto client = std::make_unique<Client>(server.port());
                client->send("GET /idle HTTP/1.1\r\nHost: localhost\r\n\r\n");
                require(client->response() == "/idle", "idle connection established");
                idle.push_back(std::move(client));
            }
            exercise_pool(server.port(), idle, gate);

            gate.reset();
            Client unread(server.port(), 4096);
            unread.send("GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n");
            require(unread.response() == "/ready", "unread connection established");
            unread.send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
            wait_for_blocked_send();
            Client blocker(server.port());
            blocker.send("GET /block HTTP/1.1\r\nHost: localhost\r\n\r\n");
            gate.wait();
            send_pool_requests(idle);
            wait_for_completion(-ENOBUFS);
            // Receive completions precede stop so shutdown also covers already completed reads
            server.request_stop();
            wait_for_completion(POLLIN);
            gate.release();
        }
        catch (...)
        {
            gate.release();
            server.request_stop();
            thread.join();
            if (server_error)
            {
                std::rethrow_exception(server_error);
            }
            throw;
        }
        server.request_stop();
        thread.join();
        if (server_error)
        {
            std::rethrow_exception(server_error);
        }
        std::cout << "HTTP fragmentation, pipelining, pool exhaustion, partial sends and "
                     "cancellation passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
