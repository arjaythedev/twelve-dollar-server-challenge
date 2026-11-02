#include <liburing.h>
#include <sqlite3.h>

#include "parsing.hpp"
#include "server.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
volatile std::sig_atomic_t signal_descriptor = -1;

void stop_signal(int)
{
    const int saved = errno;
    const int fd = signal_descriptor;
    if (fd >= 0)
    {
        const std::uint64_t value = 1;
        ssize_t written = 0;
        do
        {
            written = ::write(fd, &value, sizeof(value));
        } while (written < 0 && errno == EINTR);
    }
    errno = saved;
}

class Signals
{
  public:
    explicit Signals(int fd)
    {
        struct sigaction action{};
        action.sa_handler = stop_signal;
        sigemptyset(&action.sa_mask);
        signal_descriptor = fd;
        if (::sigaction(SIGINT, &action, &interrupt_) < 0)
        {
            signal_descriptor = -1;
            throw std::runtime_error("cannot install SIGINT handler");
        }
        if (::sigaction(SIGTERM, &action, &terminate_) < 0)
        {
            signal_descriptor = -1;
            ::sigaction(SIGINT, &interrupt_, nullptr);
            throw std::runtime_error("cannot install SIGTERM handler");
        }
    }

    ~Signals()
    {
        signal_descriptor = -1;
        ::sigaction(SIGINT, &interrupt_, nullptr);
        ::sigaction(SIGTERM, &terminate_, nullptr);
    }

    Signals(const Signals &) = delete;
    Signals &operator=(const Signals &) = delete;

  private:
    struct sigaction interrupt_{};
    struct sigaction terminate_{};
};

std::string setting(const char *name, const char *fallback = nullptr)
{
    const char *value = std::getenv(name);
    if (!value)
    {
        value = fallback;
    }
    if (!value)
    {
        throw std::runtime_error(std::string("missing environment variable: ") + name);
    }
    return value;
}

class Descriptor
{
  public:
    explicit Descriptor(int value) : value_(value)
    {
        if (value < 0)
        {
            throw std::runtime_error(std::strerror(errno));
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

class Ring
{
  public:
    Ring()
    {
        const int result = io_uring_queue_init(32, &ring_, 0);
        if (result < 0)
        {
            throw std::runtime_error(std::string("io_uring setup: ") + std::strerror(-result));
        }
    }

    ~Ring()
    {
        io_uring_queue_exit(&ring_);
    }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    [[nodiscard]] io_uring *get() noexcept
    {
        return &ring_;
    }

    [[nodiscard]] io_uring_sqe *entry(unsigned long long id)
    {
        auto *entry = io_uring_get_sqe(&ring_);
        if (!entry)
        {
            throw std::runtime_error("submission queue exhausted");
        }
        io_uring_sqe_set_data64(entry, id);
        return entry;
    }

    void submit()
    {
        const int result = io_uring_submit(&ring_);
        if (result < 0)
        {
            throw std::runtime_error(std::strerror(-result));
        }
    }

    [[nodiscard]] std::pair<unsigned long long, int> completion()
    {
        io_uring_cqe *event = nullptr;
        const int result = io_uring_wait_cqe(&ring_, &event);
        if (result < 0)
        {
            throw std::runtime_error(std::strerror(-result));
        }
        const auto value = std::pair{io_uring_cqe_get_data64(event), event->res};
        io_uring_cqe_seen(&ring_, event);
        return value;
    }

  private:
    io_uring ring_{};
};

void self_test()
{
    std::array<int, 2> pair{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) < 0)
    {
        throw std::runtime_error(std::strerror(errno));
    }
    const Descriptor left(pair[0]);
    const Descriptor right(pair[1]);
    Ring ring;
    constexpr std::string_view message = "io_uring";
    std::array<char, 32> buffer{};
    io_uring_prep_recv(ring.entry(1), left.get(), buffer.data(), buffer.size(), 0);
    io_uring_prep_send(ring.entry(2), right.get(), message.data(), message.size(), 0);
    ring.submit();
    for (int i = 0; i < 2; ++i)
    {
        const auto [id, result] = ring.completion();
        if ((id != 1 && id != 2) || result != static_cast<int>(message.size()))
        {
            throw std::runtime_error("send/receive completion differs");
        }
    }
    if (std::string_view(buffer.data(), message.size()) != message)
    {
        throw std::runtime_error("received bytes differ");
    }
    io_uring_prep_recv(ring.entry(3), left.get(), buffer.data(), buffer.size(), 0);
    ring.submit();
    io_uring_prep_cancel64(ring.entry(4), 3, 0);
    ring.submit();
    for (int i = 0; i < 2; ++i)
    {
        const auto [id, result] = ring.completion();
        if (!((id == 3 && result == -ECANCELED) || (id == 4 && result == 0)))
        {
            throw std::runtime_error("cancellation completion differs");
        }
    }
    std::cout << "io_uring send, receive and cancellation passed\n";
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test")
        {
            self_test();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version")
        {
            std::cout << "{\"runtime\":\"cpp\",\"version\":\"0.1.0\",\"sqlite\":\""
                      << sqlite3_libversion() << "\",\"transport\":\"io_uring\"}\n";
            return 0;
        }
        if (argc != 1)
        {
            throw std::runtime_error("unknown command-line argument");
        }
        const std::string host = setting("HOST", "127.0.0.1");
        const std::string port_text = setting("PORT", "3000");
        const auto port = tds::parsing::integer<unsigned>(port_text);
        if (!port || *port == 0 || *port > 65535)
        {
            throw std::runtime_error("PORT must be between 1 and 65535");
        }
        tds::Server server(host, static_cast<std::uint16_t>(*port), setting("SQLITE_PATH"),
                           setting("JWT_SECRET"));
        const Signals signals(server.stop_descriptor());
        std::cout << "Listening on " << host << ':' << server.port() << " through io_uring\n"
                  << std::flush;
        server.run();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
