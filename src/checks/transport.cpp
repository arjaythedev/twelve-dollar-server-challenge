#include "transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

class Client
{
  public:
    explicit Client(std::uint16_t port, int receive_buffer = 0)
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
        const timeval timeout{5, 0};
        if ((receive_buffer && ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                                            sizeof(receive_buffer)) < 0) ||
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
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

    std::string response()
    {
        std::size_t end = std::string::npos;
        while ((end = received_.find("\r\n\r\n")) == std::string::npos)
        {
            receive();
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

    int limit_server_send_buffer() const
    {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&local), &length) < 0)
        {
            throw std::runtime_error("client socket address");
        }
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd"))
        {
            const auto name = entry.path().filename().string();
            int descriptor = -1;
            if (std::from_chars(name.data(), name.data() + name.size(), descriptor).ec !=
                std::errc{})
            {
                continue;
            }
            sockaddr_in peer{};
            length = sizeof(peer);
            if (::getpeername(descriptor, reinterpret_cast<sockaddr *>(&peer), &length) == 0 &&
                peer.sin_family == AF_INET && peer.sin_port == local.sin_port)
            {
                const int size = 4096;
                if (::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
                {
                    throw std::runtime_error("server send buffer");
                }
                return descriptor;
            }
        }
        throw std::runtime_error("accepted server socket not found");
    }

    void drain_available()
    {
        while (receive(MSG_DONTWAIT))
        {
        }
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

std::filesystem::path ring_fdinfo()
{
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        std::error_code error;
        const auto target = std::filesystem::read_symlink(entry.path(), error);
        if (!error && target.string().find("io_uring") != std::string::npos)
        {
            return std::filesystem::path("/proc/self/fdinfo") / entry.path().filename();
        }
    }
    throw std::runtime_error("io_uring fdinfo not found");
}

void wait_for_completion(const std::filesystem::path &info, int result)
{
    const std::string expected = "res:" + std::to_string(result) + ",";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        std::ifstream stream(info);
        for (std::string line; std::getline(stream, line);)
        {
            if (line.find(expected) != std::string::npos)
            {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("expected io_uring completion " + std::to_string(result));
}

void wait_for_blocked_send(int descriptor)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        pollfd socket{descriptor, POLLOUT, 0};
        if (::poll(&socket, 1, 0) == 0)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("large send did not reach socket backpressure");
}

void exercise_short_send(std::uint16_t port, RequestGate &gate, const std::filesystem::path &info)
{
    Client slow(port, 4096);
    slow.send("GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(slow.response() == "/ready", "slow connection established");
    const int descriptor = slow.limit_server_send_buffer();
    slow.send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n"
              "GET /after HTTP/1.1\r\nHost: localhost\r\n\r\n");
    wait_for_blocked_send(descriptor);
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
        std::ifstream stream(info);
        for (std::string line; std::getline(stream, line);)
        {
            const auto start = line.find("res:");
            int result = 0;
            if (start != std::string::npos &&
                std::from_chars(line.data() + start + 4, line.data() + line.size(), result).ec ==
                    std::errc{} &&
                result > 1024 && result < 1024 * 1024)
            {
                observed = true;
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
                   RequestGate &gate, const std::filesystem::path &info)
{
    Client blocker(port);
    blocker.send("GET /block HTTP/1.1\r\nHost: localhost\r\n\r\n");
    gate.wait();
    send_pool_requests(clients);
    // Observe a real kernel CQE while the handler cannot return buffers to the pool.
    wait_for_completion(info, -ENOBUFS);
    gate.release();
    require(blocker.response() == "/block", "blocking response");
    for (std::size_t id = 0; id < clients.size(); ++id)
    {
        require(clients[id]->response() == pool_body(id), "receive pool response ownership");
    }
}

void exercise(std::uint16_t port)
{
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

} // namespace

int main()
{
    try
    {
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
        const auto info = ring_fdinfo();
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
            exercise_short_send(server.port(), gate, info);
            // Exercise cancellation across more operations than the submission ring holds.
            for (int i = 0; i < 1536; ++i)
            {
                auto client = std::make_unique<Client>(server.port());
                client->send("GET /idle HTTP/1.1\r\nHost: localhost\r\n\r\n");
                require(client->response() == "/idle", "idle connection established");
                idle.push_back(std::move(client));
            }
            exercise_pool(server.port(), idle, gate, info);

            gate.reset();
            Client unread(server.port(), 4096);
            unread.send("GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n");
            require(unread.response() == "/ready", "unread connection established");
            const int descriptor = unread.limit_server_send_buffer();
            unread.send("GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n");
            wait_for_blocked_send(descriptor);
            Client blocker(server.port());
            blocker.send("GET /block HTTP/1.1\r\nHost: localhost\r\n\r\n");
            gate.wait();
            server.request_stop();
            wait_for_completion(info, POLLIN);
            send_pool_requests(idle);
            wait_for_completion(info, -ENOBUFS);
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
