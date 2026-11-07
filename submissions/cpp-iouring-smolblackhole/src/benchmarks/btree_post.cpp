#include "app/application.hpp"
#include "app/post_writer.hpp"

#include <sqlite3.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>
#include <vector>

namespace
{
constexpr const char *post_sql = R"sql(
    SELECT p.id, p.body, p.created_at, u.username,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
    FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?
)sql";

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
    require(::clock_gettime(clock, &value) == 0, "read point-probe clock");
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

struct CloseDatabase
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

struct CloseStatement
{
    void operator()(sqlite3_stmt *statement) const noexcept
    {
        sqlite3_finalize(statement);
    }
};

using Database = std::unique_ptr<sqlite3, CloseDatabase>;
using Statement = std::unique_ptr<sqlite3_stmt, CloseStatement>;

Database open(const std::string &path)
{
    sqlite3 *raw = nullptr;
    const int result = sqlite3_open_v2(
        path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
        "unix-excl");
    Database db(raw);
    require(result == SQLITE_OK, "open point-probe database");
    return db;
}

void sql(sqlite3 *db, const char *text)
{
    if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

Statement prepare(sqlite3 *db, const char *text)
{
    sqlite3_stmt *raw = nullptr;
    const int result = sqlite3_prepare_v3(db, text, -1, SQLITE_PREPARE_PERSISTENT, &raw, nullptr);
    Statement statement(raw);
    require(result == SQLITE_OK, "prepare point-probe SQL");
    return statement;
}

std::string_view column(sqlite3_stmt *statement, int index)
{
    const auto *text = sqlite3_column_text(statement, index);
    require(text != nullptr, "point-probe SQL returned NULL text");
    return {reinterpret_cast<const char *>(text),
            static_cast<std::size_t>(sqlite3_column_bytes(statement, index))};
}

class Fixture
{
  public:
    Fixture()
    {
        char pattern[] = "/tmp/td-point-probe-XXXXXX";
        const auto *created = ::mkdtemp(pattern);
        require(created != nullptr, "create point-probe directory");
        directory_ = created;
        path = directory_ + "/feed.db";
        auto db = open(path);
        sql(db.get(), R"sql(
            PRAGMA page_size=4096;
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            CREATE TABLE users(id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
              created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE posts(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
              body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500),
              created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE likes(user_id INTEGER NOT NULL REFERENCES users(id),
              post_id INTEGER NOT NULL REFERENCES posts(id),
              created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
              PRIMARY KEY(user_id,post_id));
            CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC,id DESC);
            CREATE INDEX posts_user_id_idx ON posts(user_id);
            CREATE INDEX likes_post_id_idx ON likes(post_id);
            BEGIN;
            WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM ids WHERE n<257)
              INSERT INTO users(id,username,created_at) SELECT n,'user:'||n,'2025-01-01' FROM ids;
            WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM ids WHERE n<64)
              INSERT INTO posts(id,user_id,body,created_at)
              SELECT n,1+n%64,'post:'||n||' quotes" slash\ café 東京 '||hex(zeroblob(40)),
                     '2025-01-02T00:00:00.000Z' FROM ids;
            INSERT INTO likes(user_id,post_id,created_at)
              SELECT u.id,p.id,'2025-01-03' FROM users u CROSS JOIN posts p
              WHERE u.id<=CASE p.id%4 WHEN 0 THEN 0 WHEN 1 THEN 1 WHEN 2 THEN 4 ELSE 16 END;
            COMMIT;
            PRAGMA wal_checkpoint(TRUNCATE);
        )sql");
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::string path;

  private:
    std::string directory_;
};

std::vector<sqlite3_int64> requests(std::size_t count)
{
    std::vector<sqlite3_int64> ids;
    ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        ids.push_back(index % 5 == 0 ? 100000 + static_cast<sqlite3_int64>(index % 13)
                                     : 1 + static_cast<sqlite3_int64>((index * 37 + 11) % 64));
    }
    return ids;
}

tds::Response expected(sqlite3_stmt *statement, sqlite3_int64 id)
{
    require(sqlite3_bind_int64(statement, 1, id) == SQLITE_OK, "bind SQL point oracle");
    const int result = sqlite3_step(statement);
    tds::Response response{tds::Status::not_found, "{\"error\":\"post not found\"}"};
    if (result == SQLITE_ROW)
    {
        response.status = tds::Status::ok;
        tds::PostWriter writer(response.body, tds::PostShape::single);
        writer.append(tds::PostId{sqlite3_column_int64(statement, 0)}, column(statement, 1),
                      column(statement, 2), column(statement, 3),
                      sqlite3_column_int64(statement, 4));
        writer.finish();
        require(sqlite3_step(statement) == SQLITE_DONE, "SQL point oracle returned two rows");
    }
    else
    {
        require(result == SQLITE_DONE, "SQL point oracle failed");
    }
    require(sqlite3_reset(statement) == SQLITE_OK && sqlite3_clear_bindings(statement) == SQLITE_OK,
            "reset SQL point oracle");
    return response;
}

std::uint64_t hash(std::uint64_t value, const tds::Response &response)
{
    value ^= static_cast<unsigned short>(response.status);
    value *= 1099511628211ULL;
    for (const auto byte : response.body)
    {
        value ^= static_cast<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    value ^= 0xff;
    return value * 1099511628211ULL;
}

struct Oracle
{
    std::map<sqlite3_int64, tds::Response> rows;
    std::uint64_t vm_steps = 0;
    std::uint64_t digest = 14695981039346656037ULL;
    std::uint64_t body_bytes = 0;
    std::uint64_t statuses = 0;
    std::size_t existing = 0;
};

Oracle oracle(const std::string &path, const std::vector<sqlite3_int64> &ids)
{
    auto db = open(path);
    auto statement = prepare(db.get(), post_sql);
    Oracle result;
    for (const auto id : ids)
    {
        auto response = expected(statement.get(), id);
        result.vm_steps += static_cast<std::uint64_t>(
            sqlite3_stmt_status(statement.get(), SQLITE_STMTSTATUS_VM_STEP, 1));
        result.digest = hash(result.digest, response);
        result.body_bytes += response.body.size();
        result.statuses += static_cast<unsigned short>(response.status);
        result.existing += response.status == tds::Status::ok ? 1U : 0U;
        result.rows.insert_or_assign(id, std::move(response));
    }
    return result;
}

struct Timing
{
    std::uint64_t cpu_ns;
    std::uint64_t wall_ns;
    std::uint64_t bytes;
    std::uint64_t statuses;
};

Timing timed(tds::Application &app, const std::vector<sqlite3_int64> &ids, bool reuse)
{
    std::string output;
    std::uint64_t bytes = 0;
    std::uint64_t statuses = 0;
    const auto wall_start = now(CLOCK_MONOTONIC);
    const auto cpu_start = now(CLOCK_PROCESS_CPUTIME_ID);
    for (const auto id : ids)
    {
        auto response = app.read(tds::ReadQuery{tds::GetPost{tds::PostId{id}}}, std::move(output));
        bytes += response.body.size();
        statuses += static_cast<unsigned short>(response.status);
        if (reuse)
        {
            output = std::move(response.body);
        }
    }
    return {now(CLOCK_PROCESS_CPUTIME_ID) - cpu_start, now(CLOCK_MONOTONIC) - wall_start, bytes,
            statuses};
}

std::string quoted(std::string_view text)
{
    std::string output = "\"";
    for (const auto character : text)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\')
        {
            output += '\\';
            output += character;
        }
        else if (byte < 32)
        {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
            output += escaped;
        }
        else
        {
            output += character;
        }
    }
    output += '"';
    return output;
}

std::string plan(sqlite3 *db, const char *prefix)
{
    const auto text = std::string(prefix) + post_sql;
    auto statement = prepare(db, text.c_str());
    require(sqlite3_bind_int64(statement.get(), 1, 3) == SQLITE_OK, "bind point query plan");
    std::string output = "[";
    int result;
    bool first = true;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW)
    {
        if (!first)
        {
            output += ',';
        }
        first = false;
        output += '[';
        for (int index = 0; index < sqlite3_column_count(statement.get()); ++index)
        {
            if (index)
            {
                output += ',';
            }
            output += sqlite3_column_type(statement.get(), index) == SQLITE_NULL
                          ? "null"
                          : quoted(column(statement.get(), index));
        }
        output += ']';
    }
    require(result == SQLITE_DONE, "point query plan failed");
    output += ']';
    return output;
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        std::size_t count = 50000;
        bool explain = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == "--plan")
            {
                explain = true;
            }
            else
            {
                require(argument == "--requests" && index + 1 < argc,
                        "usage: btree_post_probe [--requests N] [--plan]");
                const std::string_view value(argv[++index]);
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), count);
                require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                            count > 0 && count <= 1'000'000,
                        "invalid point-probe request count");
            }
        }
        Fixture fixture;
        const auto ids = requests(count);
        const auto reference = oracle(fixture.path, ids);
        Timing reused{};
        Timing fresh{};
        std::uint64_t response_digest = 14695981039346656037ULL;
        {
            tds::Application app(fixture.path, "point-probe-secret");
            std::string output;
            for (const auto id : ids)
            {
                auto response =
                    app.read(tds::ReadQuery{tds::GetPost{tds::PostId{id}}}, std::move(output));
                const auto &expected_response = reference.rows.at(id);
                require(response.status == expected_response.status &&
                            response.body == expected_response.body,
                        "Application point response differs byte-for-byte from pinned SQL oracle");
                response_digest = hash(response_digest, response);
                output = std::move(response.body);
            }
            reused = timed(app, ids, true);
            fresh = timed(app, ids, false);
        }
        const auto lifecycle_start = now(CLOCK_PROCESS_CPUTIME_ID);
        Timing lifecycle{};
        {
            tds::Application app(fixture.path, "point-probe-secret");
            lifecycle = timed(app, ids, true);
        }
        const auto lifecycle_cpu_ns = now(CLOCK_PROCESS_CPUTIME_ID) - lifecycle_start;
        for (const auto &sample : {reused, fresh, lifecycle})
        {
            require(sample.bytes == reference.body_bytes && sample.statuses == reference.statuses,
                    "timed Application pass changed response bytes or statuses");
        }
        require(response_digest == reference.digest, "response digest differs from pinned SQL");
        std::printf(
            "{\"fixture_like_counts\":[0,1,4,16],\"fixture_mean_likes\":5.25,"
            "\"fixture_miss_fraction\":0.2,\"requests\":%zu,\"existing\":%zu,\"missing\":%zu,"
            "\"response_digest\":%llu,\"response_bytes\":%llu,\"status_sum\":%llu,"
            "\"reused_cpu_ns\":%llu,\"reused_wall_ns\":%llu,"
            "\"fresh_output_cpu_ns\":%llu,\"fresh_output_wall_ns\":%llu,"
            "\"lifecycle_cpu_ns\":%llu,\"canonical_sql_vm_steps\":%llu,"
            "\"vm_steps_scope\":\"separate untimed prepared SQL oracle pass\","
            "\"timing_scope\":\"full Application read with fresh snapshot and JSON serialization\","
            "\"lifecycle_scope\":\"fresh Application, lazy Store initialization, full request "
            "pass, shutdown\"",
            count, reference.existing, count - reference.existing,
            static_cast<unsigned long long>(response_digest),
            static_cast<unsigned long long>(reference.body_bytes),
            static_cast<unsigned long long>(reference.statuses),
            static_cast<unsigned long long>(reused.cpu_ns),
            static_cast<unsigned long long>(reused.wall_ns),
            static_cast<unsigned long long>(fresh.cpu_ns),
            static_cast<unsigned long long>(fresh.wall_ns),
            static_cast<unsigned long long>(lifecycle_cpu_ns),
            static_cast<unsigned long long>(reference.vm_steps));
        if (explain)
        {
            auto db = open(fixture.path);
            const auto eqp = plan(db.get(), "EXPLAIN QUERY PLAN ");
            const auto vm = plan(db.get(), "EXPLAIN ");
            std::printf(",\"canonical_sql\":%s,\"eqp\":%s,\"explain\":%s", quoted(post_sql).c_str(),
                        eqp.c_str(), vm.c_str());
        }
        std::puts("}");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "B-tree point probe failed: %s\n", error.what());
        return 1;
    }
}
