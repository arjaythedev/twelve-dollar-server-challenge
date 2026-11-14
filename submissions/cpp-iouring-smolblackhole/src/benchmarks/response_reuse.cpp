#include "app/application.hpp"
#include "http/exchange.hpp"
#include "net/ring.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace
{
thread_local bool counting = false;
thread_local std::uint64_t allocations = 0;
thread_local std::uint64_t allocated_bytes = 0;
bool count_allocations = true;

void require(bool valid, const char *message)
{
    if (!valid)
    {
        throw std::runtime_error(message);
    }
}

void execute(sqlite3 *db, const char *sql)
{
    require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL");
}

struct Fixture
{
    Fixture()
    {
        char directory[] = "/tmp/td-response-XXXXXX";
        require(::mkdtemp(directory) != nullptr, "fixture directory");
        root = directory;
        path = root + "/feed.db";
        require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "fixture database");
        execute(db, R"sql(
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
            INSERT INTO users(id,username) VALUES(1,'db-user'),(2,'second-user');
            WITH RECURSIVE ids(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM ids WHERE i<50)
              INSERT INTO posts(id,user_id,body,created_at)
              SELECT i,1,'ordinary response body with escaped "text"','2025-01-01T00:00:00.000Z' FROM ids;
        )sql");
    }

    ~Fixture()
    {
        sqlite3_close(db);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    sqlite3 *db = nullptr;
    std::string root;
    std::string path;
};

std::uint64_t cpu_ns()
{
    timespec now{};
    require(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0, "thread CPU clock");
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

tds::Response read(tds::Application &app, tds::http::Reply &reply, const tds::ReadQuery &query)
{
#ifdef TDS_RESPONSE_REUSE
    return app.read(query, reply.take_body());
#else
    static_cast<void>(reply);
    return app.read(query);
#endif
}

void finish(tds::http::Reply &reply, tds::Response response)
{
    reply.prepare(std::move(response), false);
    const auto &message = reply.message();
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < message.msg_iovlen; ++i)
    {
        bytes += message.msg_iov[i].iov_len;
    }
    require(reply.advance(bytes), "freshness response terminal completion");
    reply.reset();
}

void partial_send(tds::Application &app)
{
    tds::http::Reply reply;
    const auto expected = app.read(tds::ReadQuery{tds::GetPost{tds::PostId{50}}}).body;
    reply.prepare(read(app, reply, tds::ReadQuery{tds::GetPost{tds::PostId{50}}}), false);
    auto &first = reply.message();
    require(first.msg_iovlen == 2, "header/body vectors");
    const auto headers = first.msg_iov[0].iov_len;
    require(!reply.advance(headers - 1), "partial headers remain active");
    auto &crossing = reply.message();
    require(crossing.msg_iov[0].iov_len == 1, "last header byte preserved");
    require(!reply.advance(2), "body remains after crossing boundary");
    auto &body = reply.message();
    require(body.msg_iovlen == 1 &&
                std::string_view(static_cast<const char *>(body.msg_iov[0].iov_base),
                                 body.msg_iov[0].iov_len) == std::string_view(expected).substr(1),
            "partial send body ownership");
    require(reply.advance(expected.size() - 1), "terminal response completion");
    reply.reset();
}

void measure(tds::Application &app, tds::net::Ring &ring, std::string_view name,
             const tds::ReadQuery &query, std::size_t iterations,
             const tds::WriteCommand *command = nullptr, const tds::WriteOutcome *outcome = nullptr)
{
    const auto expected = command ? app.write_reply(*command, *outcome) : app.read(query);
    const std::string request = "GET /probe HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::size_t discarded = 0;
    std::size_t max_retained = 0;
    std::size_t pooled = 0;
    std::size_t released_buffers = 0;
    std::uint64_t produced = 0;
    const auto one = [&] {
        auto exchange = ring.acquire_exchange();
        exchange->session.append(request);
        const auto parsed = exchange->session.consume();
        require(parsed.valid && exchange->session.headers_pending(), "probe request framing");
        exchange->session.accept_headers(tds::MissingHead{});
        require(exchange->session.consume().valid && exchange->session.complete(),
                "probe request completion");
        auto response = [&] {
            if (!command)
            {
                return read(app, exchange->reply, query);
            }
#ifdef TDS_RESPONSE_REUSE
            return app.write_reply(*command, *outcome, exchange->reply.take_body());
#else
            return app.write_reply(*command, *outcome);
#endif
        }();
        require(response.status == expected.status && response.body == expected.body,
                "fresh producer bytes changed");
        produced += response.body.size();
        exchange->reply.prepare(std::move(response), false);
        auto &message = exchange->reply.message();
        std::size_t sent = 0;
        for (std::size_t i = 0; i < message.msg_iovlen; ++i)
        {
            sent += message.msg_iov[i].iov_len;
        }
        require(exchange->reply.advance(sent), "complete response send");
        const auto before_reset = exchange->reply.retained_capacity();
        exchange->reply.reset();
        released_buffers += exchange->reply.retained_capacity() < before_reset;
        exchange->session.reset();
        const auto retained = exchange->retained_capacity();
        max_retained = std::max(max_retained, retained);
        discarded += retained > 8192;
        pooled = retained <= 8192 ? retained : 0;
        ring.recycle_exchange(std::move(exchange));
    };
    for (unsigned i = 0; i < 100; ++i)
    {
        one();
    }
    allocations = 0;
    allocated_bytes = 0;
    discarded = 0;
    produced = 0;
    max_retained = 0;
    released_buffers = 0;
    const auto started = cpu_ns();
    counting = count_allocations;
    for (std::size_t i = 0; i < iterations; ++i)
    {
        one();
    }
    counting = false;
    const auto elapsed = cpu_ns() - started;
    std::printf("{\"case\":\"%.*s\",\"requests\":%zu,\"allocation_counting\":%s,\"cpp_"
                "allocations\":%llu,\"allocated_bytes\":%llu,"
                "\"produced_bytes\":%llu,\"cpu_ns\":%llu,\"pool_retained_bytes\":%zu,"
                "\"max_post_reset_capacity\":%zu,\"oversized_discards\":%zu,\"released_response_"
                "buffers\":%zu}\n",
                static_cast<int>(name.size()), name.data(), iterations,
                count_allocations ? "true" : "false", static_cast<unsigned long long>(allocations),
                static_cast<unsigned long long>(allocated_bytes),
                static_cast<unsigned long long>(produced), static_cast<unsigned long long>(elapsed),
                pooled, max_retained, discarded, released_buffers);
}
} // namespace

void *operator new(std::size_t bytes)
{
    if (void *value = std::malloc(bytes ? bytes : 1))
    {
        if (counting)
        {
            ++allocations;
            allocated_bytes += bytes;
        }
        return value;
    }
    throw std::bad_alloc();
}

void operator delete(void *value) noexcept
{
    std::free(value);
}

void operator delete(void *value, std::size_t) noexcept
{
    std::free(value);
}

int main(int argc, char **argv)
{
    try
    {
        std::size_t iterations = 5000;
        if (argc >= 2)
        {
            const std::string_view text(argv[1]);
            require(std::from_chars(text.data(), text.data() + text.size(), iterations).ec ==
                            std::errc{} &&
                        iterations > 0,
                    "iteration count");
        }
        if (argc == 3)
        {
            require(std::string_view(argv[2]) == "--cpu", "expected --cpu");
            count_allocations = false;
        }
        Fixture fixture;
        tds::Application app(fixture.path, "response-probe-secret");
        tds::net::Ring ring(4, true);
        tds::http::Reply point_reply;
        tds::http::Reply feed_reply;
        auto before = read(app, point_reply, tds::ReadQuery{tds::GetPost{tds::PostId{50}}});
        const auto before_point = before.body;
#ifdef TDS_RESPONSE_REUSE
        const auto *point_storage = before.body.data();
#endif
        finish(point_reply, std::move(before));
        before = read(app, feed_reply, tds::ReadQuery{tds::Feed{}});
        const auto before_feed = before.body;
#ifdef TDS_RESPONSE_REUSE
        const auto *feed_storage = before.body.data();
#endif
        finish(feed_reply, std::move(before));
        const std::array<tds::WriteCommand, 1> commands{
            tds::LikePost{tds::UserId{2}, tds::PostId{50}}};
        std::array<tds::WriteOutcome, 1> outcomes;
        require(app.commit(commands, outcomes).has_value() && outcomes[0],
                "committed write barrier");
        execute(fixture.db, "UPDATE users SET username='changed-db-user' WHERE id=1");
        auto after = read(app, point_reply, tds::ReadQuery{tds::GetPost{tds::PostId{50}}});
        require(before_point != after.body &&
                    after.body.find("\"like_count\":1") != std::string::npos &&
                    after.body.find("changed-db-user") != std::string::npos,
                "read reused pre-commit data");
#ifdef TDS_RESPONSE_REUSE
        require(after.body.data() == point_storage,
                "point read did not reuse its prior response allocation");
#endif
        finish(point_reply, std::move(after));
        after = read(app, feed_reply, tds::ReadQuery{tds::Feed{}});
        require(before_feed != after.body &&
                    after.body.find("\"like_count\":1") != std::string::npos &&
                    after.body.find("changed-db-user") != std::string::npos,
                "feed reused pre-commit data");
#ifdef TDS_RESPONSE_REUSE
        require(after.body.data() == feed_storage,
                "feed did not reuse its prior response allocation");
#endif
        finish(feed_reply, std::move(after));
        partial_send(app);
        measure(app, ring, "feed_small", tds::ReadQuery{tds::Feed{}}, iterations);
        execute(fixture.db, "UPDATE posts SET body=replace(hex(zeroblob(75)),'0','x')");
        const auto seed_sized = app.read(tds::ReadQuery{tds::Feed{}}).body.size();
        require(seed_sized > 4096 && seed_sized < 8192, "representative feed output size");
        measure(app, ring, "feed_seed_sized", tds::ReadQuery{tds::Feed{}}, iterations);
        measure(app, ring, "post", tds::ReadQuery{tds::GetPost{tds::PostId{50}}}, iterations);
        measure(app, ring, "missing", tds::ReadQuery{tds::GetPost{tds::PostId{999}}}, iterations);
        measure(app, ring, "write_reply", tds::ReadQuery{tds::Health{}}, iterations, &commands[0],
                &outcomes[0]);
        execute(fixture.db, "UPDATE posts SET body=replace(hex(zeroblob(250)),'0',char(10))");
        measure(app, ring, "oversized_feed", tds::ReadQuery{tds::Feed{}},
                std::max<std::size_t>(1, iterations / 10));
        std::puts("fresh commit barrier and partial-send ownership passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        counting = false;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
