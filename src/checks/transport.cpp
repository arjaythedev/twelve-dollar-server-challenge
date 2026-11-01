#include "transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{

class Client
{
  public:
    explicit Client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
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
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
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

  private:
    void receive()
    {
        char buffer[4096];
        const auto size = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (size <= 0)
        {
            throw std::runtime_error("client receive failed");
        }
        received_.append(buffer, static_cast<std::size_t>(size));
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
        tds::Server server(
            "127.0.0.1", 0,
            [](const tds::Request &request) -> tds::Response {
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
        try
        {
            exercise(server.port());
        }
        catch (...)
        {
            server.request_stop();
            thread.join();
            throw;
        }
        // Leave a receive outstanding while shutdown cancels the connections.
        Client idle(server.port());
        server.request_stop();
        thread.join();
        if (server_error)
        {
            std::rethrow_exception(server_error);
        }
        std::cout
            << "HTTP fragmentation, pipelining, chunking, partial sends and cancellation passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
