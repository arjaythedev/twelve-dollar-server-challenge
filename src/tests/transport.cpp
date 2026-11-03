#include "app/application.hpp"
#include "diagnostics/database.hpp"
#include "diagnostics/io.hpp"
#include "http/exchange.hpp"
#include "net/reactor.hpp"
#include "net/ring.hpp"
#include "server.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sqlite3.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <linux/io_uring.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

template <class T>
concept ReceiveStamped = requires(T &owner) { owner.receive_enobufs_since_ns; };

template <class T>
concept ExchangeStamped = requires(T &owner) {
    owner.receive_serviced_ns;
    owner.send_prepared_ns;
};

template <class T>
concept CompletionStamped = requires(T &owner) { owner.serviced_ns; };

template <class T>
concept RingProfiled = requires(T &owner) { owner.statistics.interval; };

static_assert(sizeof(tds::net::Connection) == (tds::telemetry::profiling ? 64 : 56));
static_assert(ReceiveStamped<tds::net::Connection> == tds::telemetry::profiling);
static_assert(ExchangeStamped<tds::http::Exchange> == tds::telemetry::profiling);
static_assert(CompletionStamped<tds::net::Completion> == tds::telemetry::profiling);
static_assert(RingProfiled<tds::net::Ring> == tds::telemetry::profiling);
static_assert(std::is_empty_v<tds::telemetry::IoTelemetry> == !tds::telemetry::profiling);
static_assert(std::is_empty_v<tds::telemetry::IoTelemetry::Run> == !tds::telemetry::enabled);
static_assert(std::is_empty_v<tds::telemetry::IoTelemetry::Handler> == !tds::telemetry::profiling);
static_assert(std::is_empty_v<tds::telemetry::IoTelemetry::WriteBatch> ==
              !tds::telemetry::profiling);
static_assert(std::is_empty_v<tds::telemetry::IoTelemetry::Receive> == !tds::telemetry::profiling);
static_assert(std::is_empty_v<tds::telemetry::DatabaseTelemetry> == !tds::telemetry::enabled);
static_assert(std::is_empty_v<tds::telemetry::DatabaseTelemetry::Scope> ==
              !tds::telemetry::profiling);
constexpr std::string_view secret = "transport-secret";
constexpr std::string_view small_post =
    "{\"post\":{\"id\":1,\"body\":\"seed\",\"created_at\":\"2025-01-01T00:00:00.000Z\","
    "\"author\":\"db-user\",\"like_count\":0}}";

struct CommitGate
{
    std::binary_semaphore reached{0};
    std::binary_semaphore proceed{0};
    bool reject;
};

thread_local CommitGate *active_commit_gate = nullptr;

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

    bool response_available(int timeout_ms = 0) const
    {
        pollfd socket{fd_, POLLIN, 0};
        int result;
        do
        {
            result = ::poll(&socket, 1, timeout_ms);
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

std::string base64url(std::string_view value)
{
    std::string encoded(4 * ((value.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                     reinterpret_cast<const unsigned char *>(value.data()),
                                     static_cast<int>(value.size()));
    require(size >= 0, "encode JWT segment");
    encoded.resize(static_cast<std::size_t>(size));
    for (char &byte : encoded)
    {
        if (byte == '+')
        {
            byte = '-';
        }
        else if (byte == '/')
        {
            byte = '_';
        }
    }
    while (encoded.ends_with('='))
    {
        encoded.pop_back();
    }

    return encoded;
}

const std::string &authorization()
{
    static const std::string result = [] {
        const auto content = base64url(R"({"alg":"HS256"})") + '.' +
                             base64url(R"({"sub":"1","username":"token-user","exp":4102444800})");
        std::array<unsigned char, 32> digest{};
        unsigned length = 0;
        require(HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                     reinterpret_cast<const unsigned char *>(content.data()), content.size(),
                     digest.data(), &length) != nullptr &&
                    length == digest.size(),
                "sign transport JWT");
        const std::string_view signature(reinterpret_cast<const char *>(digest.data()), length);

        return "Bearer " + content + '.' + base64url(signature);
    }();

    return result;
}

class Fixture
{
  public:
    Fixture()
    {
        char directory[] = "/tmp/td-transport-XXXXXX";
        const auto *created = ::mkdtemp(directory);
        require(created != nullptr, "fixture directory");
        directory_ = created;
        path = directory_ + "/feed.db";
        auto db = open();
        execute(db.get(), R"sql(
            CREATE TABLE users(id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
              created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE posts(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
              body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500),
              created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE likes(user_id INTEGER NOT NULL REFERENCES users(id),
              post_id INTEGER NOT NULL REFERENCES posts(id),
              created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
              PRIMARY KEY(user_id,post_id));
            CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC,id DESC);
            CREATE INDEX posts_user_id_idx ON posts(user_id);
            CREATE INDEX likes_post_id_idx ON likes(post_id);
            INSERT INTO users(id,username) VALUES(1,'db-user');
            INSERT INTO posts(id,user_id,body,created_at)
              VALUES(1,1,'seed','2025-01-01T00:00:00.000Z');
        )sql");
        sqlite3_stmt *raw = nullptr;
        require(sqlite3_prepare_v2(db.get(), "INSERT INTO users(id,username) VALUES(2,?)", -1, &raw,
                                   nullptr) == SQLITE_OK,
                "prepare large author");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(raw, sqlite3_finalize);
        const std::string author(1024 * 1024, 'x');
        require(sqlite3_bind_text64(raw, 1, author.data(), author.size(), SQLITE_TRANSIENT,
                                    SQLITE_UTF8) == SQLITE_OK &&
                    sqlite3_step(raw) == SQLITE_DONE,
                "insert large author");
        // Username size is unrestricted by the fixed schema, giving real API send backpressure
        execute(db.get(), R"sql(
            INSERT INTO posts(id,user_id,body,created_at)
              VALUES(2,2,'large','2000-01-01T00:00:00.000Z');
        )sql");
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::int64_t scalar(const char *sql) const
    {
        auto db = open();
        sqlite3_stmt *raw = nullptr;
        require(sqlite3_prepare_v2(db.get(), sql, -1, &raw, nullptr) == SQLITE_OK,
                "prepare fixture query");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(raw, sqlite3_finalize);
        require(sqlite3_step(raw) == SQLITE_ROW, "fixture query row");

        return sqlite3_column_int64(raw, 0);
    }

    static std::string large_post()
    {
        return "{\"post\":{\"id\":2,\"body\":\"large\",\"created_at\":\"2000-01-01T00:00:00.000Z\","
               "\"author\":\"" +
               std::string(1024 * 1024, 'x') + "\",\"like_count\":0}}";
    }

    std::string path;

  private:
    struct Close
    {
        void operator()(sqlite3 *db) const noexcept
        {
            sqlite3_close(db);
        }
    };

    std::unique_ptr<sqlite3, Close> open() const
    {
        sqlite3 *raw = nullptr;
        const int result = sqlite3_open_v2(path.c_str(), &raw,
                                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        std::unique_ptr<sqlite3, Close> db(raw);
        require(result == SQLITE_OK, "open transport fixture");

        return db;
    }

    static void execute(sqlite3 *db, const char *sql)
    {
        require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK,
                "transport fixture SQL");
    }

    std::string directory_;
};

void exercise_request_reuse()
{
    Fixture fixture;
    tds::Application application(fixture.path, std::string(secret));
    tds::net::Ring ring(8, true);
    auto exchange = ring.acquire_exchange();
    auto &session = exchange->session;
    const auto &message = session.message();
    const std::string body(2048, 'b');
    const std::string wire = "POST /posts HTTP/1.1\r\nAuthorization: " + authorization() +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n" + body;
    session.append(wire);
    require(session.consume().valid && session.headers_pending(),
            "request pauses for header acceptance");
    require(message.method == "POST" && message.path == "/posts" &&
                message.authorization == authorization() && !message.keep_alive,
            "request headers parsed");
    auto accepted =
        application.accept_headers({message.method, message.path, message.authorization});
    require(accepted && std::holds_alternative<tds::CreateHead>(*accepted),
            "protected route accepted with a fresh identity");
    session.accept_headers(std::move(*accepted));
    require(std::get<tds::CreateHead>(session.accepted_head()).identity.user_id == tds::UserId{1},
            "session owns the accepted identity");
    require(session.consume().valid && session.complete() && message.body == body,
            "request body parsed");
    const auto retained = exchange->retained_capacity();
    require(retained <= 8192, "ordinary request fits exchange retention budget");
    exchange->reply.reset();
    session.reset();
    require(message.method.empty() && message.path.empty() && message.authorization.empty() &&
                message.body.empty() && message.keep_alive,
            "finished message clears fields and connection policy");
    require(!session.complete() && !session.headers_pending() &&
                session.error() == tds::Status::bad_request,
            "finished message resets parser state");
    require(exchange->retained_capacity() == retained,
            "finished message preserves bounded string storage");
    const auto *pooled = exchange.get();
    ring.recycle_exchange(std::move(exchange));
    exchange = ring.acquire_exchange();
    require(exchange.get() == pooled, "bounded exchange reused from pool");
    auto &fresh_session = exchange->session;
    fresh_session.append("GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(fresh_session.consume().valid && fresh_session.headers_pending(),
            "recycled parser reads the next request");
    const auto &fresh = fresh_session.message();
    require(fresh.method == "GET" && fresh.path == "/feed" && fresh.authorization.empty() &&
                fresh.body.empty() && fresh.keep_alive,
            "recycled message does not inherit authorization, body or close policy");
    accepted = application.accept_headers({fresh.method, fresh.path, fresh.authorization});
    require(accepted && std::holds_alternative<tds::FeedHead>(*accepted),
            "recycled session receives a fresh public route context");
    fresh_session.accept_headers(std::move(*accepted));
    require(std::holds_alternative<tds::FeedHead>(fresh_session.accepted_head()),
            "previous identity does not leak into the next message");
    require(fresh_session.consume().valid && fresh_session.complete(),
            "recycled parser completes an empty body");
    fresh_session.reset();
    ring.recycle_exchange(std::move(exchange));

    const std::string oversized(8193, 'x');
    const std::array requests{"GET /" + oversized + " HTTP/1.1\r\nHost: local\r\n\r\n",
                              "GET /feed HTTP/1.1\r\nAuthorization: " + oversized + "\r\n\r\n",
                              "POST /posts HTTP/1.1\r\nAuthorization: " + authorization() +
                                  "\r\nContent-Length: 8193\r\n\r\n" + oversized};
    for (const auto &request : requests)
    {
        exchange = ring.acquire_exchange();
        auto &large = exchange->session;
        large.append(request);
        require(large.consume().valid && large.headers_pending(),
                "large protocol field reaches the header boundary");
        const auto &head = large.message();
        accepted = application.accept_headers({head.method, head.path, head.authorization});
        require(accepted.has_value(), "large field accepted without changing protocol limits");
        large.accept_headers(std::move(*accepted));
        require(large.consume().valid && large.complete(), "large message is framed completely");
        exchange->reply.reset();
        large.reset();
        require(exchange->retained_capacity() > 8192,
                "oversized cleared storage exceeds the retention budget");
        ring.recycle_exchange(std::move(exchange));
        exchange = ring.acquire_exchange();
        require(exchange->retained_capacity() <= 8192,
                "oversized storage is discarded instead of pooled");
        ring.recycle_exchange(std::move(exchange));
    }
}

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

void wait_for_completion(int result, pid_t process, std::size_t minimum = 1)
{
    const std::string expected = "res:" + std::to_string(result) + ",";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        std::size_t observed = 0;
        for (const auto &info : ring_fdinfos(process))
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

void wait_for_receives(pid_t process, std::size_t minimum)
{
    const auto expected = "  op=" + std::to_string(IORING_OP_RECV) + ',';
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        std::size_t observed = 0;
        for (const auto &info : ring_fdinfos(process))
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
    throw std::runtime_error("expected submitted receives before pausing userspace");
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

volatile std::sig_atomic_t child_stop_descriptor = -1;
volatile std::sig_atomic_t child_gate_ready = -1;
volatile std::sig_atomic_t child_gate_release = -1;
volatile std::sig_atomic_t child_primary_ring = -1;

void stop_child(int)
{
    const int saved_errno = errno;
    const std::uint64_t value = 1;
    ssize_t result;
    do
    {
        result = ::write(child_stop_descriptor, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    errno = saved_errno;
}

void pause_child(int)
{
    const int saved_errno = errno;
    const char ready = 'r';
    ssize_t result;
    do
    {
        result = ::write(child_gate_ready, &ready, sizeof(ready));
    } while (result < 0 && errno == EINTR);
    // Cursed: pausing handlers does not put deferred kernel work on autopilot
    // Service the primary ring without consuming any application CQEs
    pollfd gate{child_gate_release, POLLIN, 0};
    while (true)
    {
        const int polled = ::poll(&gate, 1, 1);
        if (polled > 0)
        {
            char released;
            do
            {
                result = ::read(child_gate_release, &released, sizeof(released));
            } while (result < 0 && errno == EINTR);

            break;
        }
        if (polled < 0 && errno != EINTR)
        {
            ::_exit(2);
        }
        const auto entered = ::syscall(SYS_io_uring_enter, child_primary_ring, 0, 0,
                                       IORING_ENTER_GETEVENTS, nullptr, 0);
        if (entered < 0 && errno != EINTR && errno != EBUSY)
        {
            ::_exit(2);
        }
    }
    errno = saved_errno;
}

class RunningServer
{
  public:
    explicit RunningServer(const Fixture &fixture, rlim_t descriptor_limit = 65535)
    {
        Pipe ready;
        process_ = ::fork();
        require(process_ >= 0, "fork concrete server");
        if (process_ == 0)
        {
            ready.close_reader();
            gate_ready_.close_reader();
            gate_release_.close_writer();
            try
            {
                const rlimit limit{descriptor_limit, descriptor_limit};
                require(::setrlimit(RLIMIT_NOFILE, &limit) == 0, "set server descriptor limit");
                {
                    tds::Server server("127.0.0.1", 0, fixture.path, std::string(secret));
                    child_stop_descriptor = server.stop_descriptor();
                    child_gate_ready = gate_ready_.writer();
                    child_gate_release = gate_release_.reader();
                    const auto initial_rings = ring_fdinfos(::getpid());
                    require(initial_rings.size() == 1, "one primary ring before startup");
                    child_primary_ring = std::stoi(initial_rings.front().filename().string());
                    require(std::signal(SIGUSR1, stop_child) != SIG_ERR,
                            "install test stop signal");
                    require(std::signal(SIGUSR2, pause_child) != SIG_ERR,
                            "install userspace dispatch gate");
                    limit_listener_send_buffer(server.port());
                    const auto port = server.port();
                    require(::write(ready.writer(), &port, sizeof(port)) ==
                                static_cast<ssize_t>(sizeof(port)),
                            "publish server port");
                    ready.close_writer();
                    server.run();
                }
                ::_exit(0);
            }
            catch (const std::exception &error)
            {
                std::cerr << "child server: " << error.what() << '\n';
                ::_exit(1);
            }
        }
        child_ = std::make_unique<ChildProcess>(process_);
        ready.close_writer();
        gate_ready_.close_writer();
        gate_release_.close_reader();
        pollfd startup{ready.reader(), POLLIN, 0};
        int result;
        do
        {
            result = ::poll(&startup, 1, 10000);
        } while (result < 0 && errno == EINTR);
        require(result > 0, "concrete server startup");
        require(::read(ready.reader(), &port_, sizeof(port_)) ==
                    static_cast<ssize_t>(sizeof(port_)),
                "read concrete server port");
    }

    ~RunningServer()
    {
        if (child_)
        {
            try
            {
                request_stop();
                resume();
                child_->wait();
            }
            catch (...)
            {
                // ChildProcess still owns forced cleanup if graceful shutdown failed
            }
        }
    }

    std::uint16_t port() const noexcept
    {
        return port_;
    }

    pid_t process() const noexcept
    {
        return process_;
    }

    void pause()
    {
        require(!paused_, "dispatch gate already active");
        // Keep the task schedulable so io_uring task_work can complete kernel requests
        require(::syscall(SYS_tgkill, process_, process_, SIGUSR2) == 0,
                "pause main-thread dispatch");
        pollfd ready{gate_ready_.reader(), POLLIN, 0};
        int result;
        do
        {
            result = ::poll(&ready, 1, 5000);
        } while (result < 0 && errno == EINTR);
        char byte;
        require(result > 0 && ::read(gate_ready_.reader(), &byte, sizeof(byte)) ==
                                  static_cast<ssize_t>(sizeof(byte)),
                "main thread entered the dispatch gate");
        paused_ = true;
    }

    void resume()
    {
        if (!paused_)
        {
            return;
        }
        const char released = 'r';
        ssize_t result;
        do
        {
            result = ::write(gate_release_.writer(), &released, sizeof(released));
        } while (result < 0 && errno == EINTR);
        require(result == static_cast<ssize_t>(sizeof(released)), "resume userspace dispatch");
        paused_ = false;
    }

    void request_stop() const
    {
        require(::kill(process_, SIGUSR1) == 0, "request concrete server stop");
    }

    void stop()
    {
        request_stop();
        resume();
        child_->wait();
        child_.reset();
    }

  private:
    pid_t process_ = -1;
    std::unique_ptr<ChildProcess> child_;
    std::uint16_t port_ = 0;
    Pipe gate_ready_;
    Pipe gate_release_;
    bool paused_ = false;
};

std::string get_post(std::int64_t id = 1)
{
    return "GET /posts/" + std::to_string(id) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

std::string write_request(std::string_view body, std::size_t padding = 0)
{
    return "POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: " + authorization() +
           "\r\nX-Pad: " + std::string(padding, 'p') +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + std::string(body);
}

std::unique_ptr<Client> ready_client(std::uint16_t port)
{
    auto client = std::make_unique<Client>(port);
    client->send(get_post());
    require(client->response() == small_post, "concrete API connection established");

    return client;
}

void exercise_write_batches()
{
    Fixture fixture;
    RunningServer server(fixture);
    auto client = ready_client(server.port());
    client->send(write_request(R"({"body":"singleton"})"));
    require(client->response(tds::Status::created).find("\"body\":\"singleton\"") !=
                std::string::npos,
            "singleton write flushes without another request");
    client->send(write_request(R"({"body":"first"})") + write_request(R"({"body":"second"})") +
                 get_post(5));
    require(client->response(tds::Status::created).find("\"body\":\"first\"") != std::string::npos,
            "first create response keeps pipeline order");
    require(client->response(tds::Status::created).find("\"body\":\"second\"") != std::string::npos,
            "second create response keeps pipeline order");
    require(client->response().find("\"body\":\"second\"") != std::string::npos,
            "pipelined read observes the committed second write");

    std::vector<std::unique_ptr<Client>> clients;
    constexpr auto count = tds::write_batch_limit + 1;
    for (std::size_t index = 0; index < count; ++index)
    {
        clients.push_back(ready_client(server.port()));
    }
    wait_for_receives(server.process(), clients.size() + 1);
    server.pause();
    for (std::size_t index = 0; index < count; ++index)
    {
        clients[index]->send(write_request("{\"body\":\"batch:" + std::to_string(index) + "\"}"));
    }
    server.resume();
    for (std::size_t index = 0; index < count; ++index)
    {
        require(clients[index]
                        ->response(tds::Status::created)
                        .find("\"body\":\"batch:" + std::to_string(index) + "\"") !=
                    std::string::npos,
                "concurrent committed response belongs to its own command");
    }
    client->send("POST /posts/1/like HTTP/1.1\r\nHost: localhost\r\nAuthorization: " +
                 authorization() + "\r\nContent-Length: 0\r\n\r\n" + get_post());
    require(client->response(tds::Status::created) ==
                "{\"liked\":true,\"already_liked\":false,\"post_id\":1}",
            "first like has the canonical committed outcome");
    require(client->response().find("\"like_count\":1") != std::string::npos,
            "pipelined read observes committed likes");

    Client rejected(server.port());
    rejected.send("POST /posts/abc/like HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    require(rejected.response(tds::Status::unauthorized) == "{\"error\":\"missing bearer token\"}",
            "real auth precedes invalid path ID");
    server.stop();
    require(fixture.scalar("SELECT count(*) FROM posts") ==
                static_cast<std::int64_t>(2 + 3 + count),
            "acknowledged writes survive shutdown and separate SQLite inspection");
}

void exercise_shutdown_pending()
{
    Fixture fixture;
    RunningServer server(fixture);
    auto first = ready_client(server.port());
    auto second = ready_client(server.port());
    wait_for_receives(server.process(), 2);
    server.pause();
    first->send(write_request(R"({"body":"shutdown-one"})"));
    second->send(write_request(R"({"body":"shutdown-two"})"));
    wait_for_completion(static_cast<int>(write_request(R"({"body":"shutdown-one"})").size()),
                        server.process(), 2);
    first.reset();
    // Stop is queued before resuming userspace; no fake callback controls admission
    server.request_stop();
    wait_for_completion(POLLIN, server.process());
    server.stop();
    require(fixture.scalar("SELECT count(*) FROM posts WHERE body LIKE 'shutdown-%'") <= 2,
            "shutdown does not replay prepared writes");
}

void exercise_commit_publication()
{
    for (const bool reject : {false, true})
    {
        Fixture fixture;
        CommitGate gate{.reject = reject};
        std::promise<std::pair<std::uint16_t, int>> ready;
        std::exception_ptr server_error;
        std::jthread issuer([&] {
            bool announced = false;
            try
            {
                active_commit_gate = &gate;
                tds::Server server("127.0.0.1", 0, fixture.path, std::string(secret));
                ready.set_value({server.port(), server.stop_descriptor()});
                announced = true;
                server.run();
            }
            catch (...)
            {
                server_error = std::current_exception();
                if (!announced)
                {
                    ready.set_exception(server_error);
                }
            }
            active_commit_gate = nullptr;
        });
        const auto [port, stop_descriptor] = ready.get_future().get();
        const auto stop = [&] {
            const std::uint64_t value = 1;
            ssize_t written;
            do
            {
                written = ::write(stop_descriptor, &value, sizeof(value));
            } while (written < 0 && errno == EINTR);
            issuer.join();
            if (server_error)
            {
                std::rethrow_exception(server_error);
            }
            require(written == static_cast<ssize_t>(sizeof(value)), "stop publication issuer");
        };
        try
        {
            Client client(port);
            client.send(write_request(R"({"body":"publication"})"));
            require(gate.reached.try_acquire_for(std::chrono::seconds(5)),
                    "write reached COMMIT gate");
            require(!client.response_available(20), "HTTP success escaped before COMMIT resolved");
            require(fixture.scalar("SELECT count(*) FROM posts WHERE body='publication'") == 0,
                    "uncommitted row became visible to another in-process SQLite handle");
            gate.proceed.release();
            const auto body =
                client.response(reject ? tds::Status::internal_error : tds::Status::created);
            require(reject ? body == "{\"error\":\"internal server error\"}"
                           : body.find("\"body\":\"publication\"") != std::string::npos,
                    "commit outcome did not control the HTTP reply");
            require(fixture.scalar("SELECT count(*) FROM posts WHERE body='publication'") ==
                        (reject ? 0 : 1),
                    "published response disagreed with committed visibility before shutdown");
        }
        catch (...)
        {
            stop();
            throw;
        }
        stop();
    }
}

void exercise_short_send(RunningServer &server)
{
    Client slow(server.port(), 4096);
    slow.send(get_post());
    require(slow.response() == small_post, "slow concrete API connection established");
    slow.send(get_post(2));
    wait_for_blocked_send(server.process());
    slow.send(get_post());
    auto barrier = ready_client(server.port());
    server.pause();
    bool observed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        slow.drain_available();
        for (const auto &info : ring_fdinfos(server.process()))
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
    require(observed, "real short-send CQE observed before draining the response");
    server.resume();
    require(slow.response() == Fixture::large_post(), "large API response survives partial sends");
    require(slow.response() == small_post, "pipeline resumes after the partial response");
}

std::string pool_body(std::size_t id)
{
    return std::to_string(id) + ':' + std::string(128, static_cast<char>('a' + id % 26));
}

void send_pool_requests(const std::vector<std::unique_ptr<Client>> &clients)
{
    for (std::size_t id = 0; id < clients.size(); ++id)
    {
        clients[id]->send(write_request("{\"body\":\"" + pool_body(id) + "\"}", 2048));
    }
}

void exercise_pool(RunningServer &server, const std::vector<std::unique_ptr<Client>> &clients)
{
    // Receiving a response does not imply its send CQE has rearmed the next receive
    wait_for_receives(server.process(), clients.size());
    server.pause();
    send_pool_requests(clients);
    // The pipe gate blocks userspace dispatch while kernel task_work remains runnable
    wait_for_completion(-ENOBUFS, server.process());
    server.resume();
    for (std::size_t id = 0; id < clients.size(); ++id)
    {
        require(clients[id]
                        ->response(tds::Status::created)
                        .find("\"body\":\"" + pool_body(id) + "\"") != std::string::npos,
                "receive-pool recovery preserves each real create body");
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
        for (const auto &request : requests)
        {
            Client client(port);
            client.send(request);
            if (size == 16384)
            {
                require(client.response(tds::Status::not_found) == "{\"error\":\"not found\"}",
                        "at-limit token reaches real routing");
            }
            else
            {
                require(client.response(tds::Status::bad_request) ==
                            "{\"error\":\"malformed HTTP request\"}",
                        "oversized token fails framing");
            }
        }
    }
    Client body(port);
    body.send("POST /posts HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16385\r\n\r\n");
    require(body.response(tds::Status::too_large) == "{\"error\":\"malformed HTTP request\"}",
            "framing rejects oversized content length before authentication");
}

void exercise(std::uint16_t port)
{
    exercise_parser_limits(port);
    Client connection(port);
    connection.send("GET /posts/1 HTTP/1.1\r\nHo");
    connection.send("st: localhost\r\n\r\n");
    require(connection.response() == small_post, "fragmented headers");
    const auto create = write_request(R"({"body":"fragmented"})");
    connection.send(std::string_view(create).substr(0, create.size() - 5));
    connection.send(std::string_view(create).substr(create.size() - 5));
    require(connection.response(tds::Status::created).find("\"body\":\"fragmented\"") !=
                std::string::npos,
            "fragmented authenticated JSON body");
    connection.send(get_post() + get_post());
    require(connection.response() == small_post && connection.response() == small_post,
            "two concrete pipelined reads");

    connection.send("POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: " + authorization() +
                    "\r\nTransfer-Encoding: "
                    "chunked\r\n\r\n8\r\n{\"body\":\r\n9\r\n\"chunked\"\r\n1\r\n}\r\n0\r\n\r\n");
    require(connection.response(tds::Status::created).find("\"body\":\"chunked\"") !=
                std::string::npos,
            "chunked authenticated JSON body");
    connection.send("GET /%70osts/1?ignored=yes HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(connection.response() == small_post, "decoded route ignores query string");

    Client half_closed(port);
    half_closed.send(get_post());
    half_closed.finish_writing();
    require(half_closed.response() == small_post, "readable bytes survive half-close");
    std::vector<std::unique_ptr<Client>> burst;
    for (int index = 0; index < 128; ++index)
    {
        auto client = std::make_unique<Client>(port);
        client->send(get_post() + get_post());
        burst.push_back(std::move(client));
    }
    for (auto &client : burst)
    {
        require(client->response() == small_post && client->response() == small_post,
                "burst pipeline responses remain ordered");
    }
    burst.clear();
    for (int index = 0; index < 100; ++index)
    {
        Client disconnected(port);
        disconnected.send("POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: " +
                          authorization() + "\r\nContent-Length: 1000\r\n\r\na");
    }
    connection.send("GET /posts/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    require(connection.response() == small_post, "server survives incomplete client disconnects");
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
    require(count > descriptor_limit, "capacity clients exceed one direct table");
    const auto stage = [progress](std::string_view message) {
        if (progress)
        {
            std::cout << "capacity: " << message << '\n' << std::flush;
        }
    };
    Fixture fixture;
    RunningServer server(fixture, descriptor_limit);
    const auto port = server.port();
    const auto process = server.process();
    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(count);
    for (std::size_t id = 0; id < count; ++id)
    {
        const auto source = INADDR_LOOPBACK + 1U + static_cast<std::uint32_t>(id / 16000);
        auto client = std::make_unique<Client>(port, id + 1 == count ? 4096 : 0, source);
        client->send(get_post());
        require(client->response() == small_post, "direct concrete API connection established");
        clients.push_back(std::move(client));
    }
    const auto infos = ring_fdinfos(process);
    require(infos.size() > 1, "multiple rings used at the official table boundary");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto files_before = registered_files(infos);
    while (!files_before || *files_before < count)
    {
        require(std::chrono::steady_clock::now() < deadline,
                "all clients registered as direct FDs");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        files_before = registered_files(infos);
    }
    stage("all clients registered across multiple rings");
    const auto removed = std::min(static_cast<std::size_t>(descriptor_limit), count / 2);
    for (std::size_t id = 0; id < removed; ++id)
    {
        clients[id].reset();
    }

    auto &satellite = *clients.back();
    const auto wire = write_request(R"({"body":"satellite-fragments"})");
    satellite.send(std::string_view(wire).substr(0, 25));
    satellite.send(std::string_view(wire).substr(25, wire.size() - 30));
    satellite.send(std::string_view(wire).substr(wire.size() - 5));
    satellite.send(get_post());
    require(satellite.response(tds::Status::created).find("\"body\":\"satellite-fragments\"") !=
                std::string::npos,
            "satellite fragmented write");
    require(satellite.response() == small_post, "satellite pipeline after primary closes");
    satellite.send(get_post(2));
    require(satellite.response() == Fixture::large_post(), "satellite large response ownership");
    {
        Client rejected(port);
        rejected.send("POST /posts HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999\r\n\r\n");
        require(rejected.response(tds::Status::unauthorized) ==
                    "{\"error\":\"missing bearer token\"}",
                "real auth rejects an absent body on a direct socket");
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true)
    {
        const auto remaining = registered_files(infos);
        if (remaining && *remaining <= *files_before - removed)
        {
            break;
        }
        require(std::chrono::steady_clock::now() < deadline, "closed direct slots retired");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (std::size_t id = 0; id < removed; ++id)
    {
        const auto source = INADDR_LOOPBACK + 1024U + static_cast<std::uint32_t>(id / 16000);
        auto client = std::make_unique<Client>(port, 0, source);
        client->send(get_post());
        require(client->response() == small_post, "reused direct slot serves the real API");
        clients[id] = std::move(client);
    }
    require(ring_fdinfos(process).size() == infos.size(), "reuse allocates no extra ring");
    stage("direct slots reused");
    for (std::size_t id = 0; id + 1 < clients.size(); id += 2)
    {
        clients[id]->send("POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: " +
                          authorization() + "\r\nContent-Length: 999\r\n\r\na");
        clients[id].reset();
    }
    clients.back()->send(get_post());
    require(clients.back()->response() == small_post, "satellite survives incomplete closes");
    clients.back()->send(get_post(2));
    wait_for_blocked_send(process);
    stage("stopping with a backpressured satellite send");
    server.stop();
    std::cout << count << " external clients, " << descriptor_limit << " ordinary-fd limit, "
              << infos.size() << " rings, real API, slot reuse and live shutdown passed\n";
}

} // namespace

extern "C" int __real_sqlite3_step(sqlite3_stmt *);

extern "C" int __wrap_sqlite3_step(sqlite3_stmt *statement)
{
    // Gate COMMIT in-process because unix-excl intentionally excludes external readers
    const auto *sql = active_commit_gate ? sqlite3_sql(statement) : nullptr;
    if (sql && std::string_view(sql) == "COMMIT")
    {
        auto *gate = std::exchange(active_commit_gate, nullptr);
        gate->reached.release();
        if (!gate->proceed.try_acquire_for(std::chrono::seconds(5)) || gate->reject)
        {
            return SQLITE_IOERR;
        }
    }

    return __real_sqlite3_step(statement);
}

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
        exercise_capacity(768, 256);
        exercise_request_reuse();
        exercise_write_batches();
        exercise_shutdown_pending();
        exercise_commit_publication();
        Fixture fixture;
        RunningServer server(fixture);
        exercise(server.port());
        exercise_short_send(server);
        std::vector<std::unique_ptr<Client>> idle;
        for (int index = 0; index < 1536; ++index)
        {
            idle.push_back(ready_client(server.port()));
        }
        exercise_pool(server, idle);
        Client unread(server.port(), 4096);
        unread.send(get_post(2));
        wait_for_blocked_send(server.process());
        wait_for_receives(server.process(), idle.size());
        server.pause();
        send_pool_requests(idle);
        wait_for_completion(-ENOBUFS, server.process());
        server.request_stop();
        server.stop();
        std::cout << "HTTP framing, real auth/writes, pipeline visibility, pool exhaustion, "
                     "partial sends and cancellation passed\n";
        std::cout << "Layout bytes: Connection=" << sizeof(tds::net::Connection)
                  << " Exchange=" << sizeof(tds::http::Exchange)
                  << " HttpSession=" << sizeof(tds::http::HttpSession) << '\n';

        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
