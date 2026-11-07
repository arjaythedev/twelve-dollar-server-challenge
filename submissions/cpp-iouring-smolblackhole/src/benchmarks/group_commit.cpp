#include "storage/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <time.h>
#include <variant>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

struct Finalize
{
    void operator()(sqlite3_stmt *statement) const noexcept
    {
        sqlite3_finalize(statement);
    }
};

using Connection = std::unique_ptr<sqlite3, Close>;
using Statement = std::unique_ptr<sqlite3_stmt, Finalize>;

Statement prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *raw = nullptr;
    require(sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) == SQLITE_OK, "probe query prepare");

    return Statement(raw);
}

std::int64_t scalar(sqlite3 *db, const char *sql)
{
    auto statement = prepare(db, sql);
    require(sqlite3_step(statement.get()) == SQLITE_ROW, "probe scalar row");

    return sqlite3_column_int64(statement.get(), 0);
}

std::uint64_t now(clockid_t clock)
{
    timespec value{};
    require(::clock_gettime(clock, &value) == 0, "probe clock");

    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::size_t positive(std::string_view text)
{
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0,
            "positive integer required");

    return value;
}

class Digest
{
  public:
    void number(std::uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            value_ ^= value & 0xffU;
            value_ *= 1099511628211ULL;
            value >>= 8;
        }
    }

    void text(std::string_view value)
    {
        number(value.size());
        for (const unsigned char byte : value)
        {
            value_ ^= byte;
            value_ *= 1099511628211ULL;
        }
    }

    std::uint64_t value() const noexcept
    {
        return value_;
    }

  private:
    std::uint64_t value_ = 14695981039346656037ULL;
};

enum class Expected : unsigned
{
    create,
    like,
    constraint,
    missing
};

void benchmark(const char *path, std::size_t jobs, std::size_t cap, bool mixed)
{
    sqlite3 *raw = nullptr;
    const int opened =
        sqlite3_open_v2(path, &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
    Connection reader(raw);
    require(opened == SQLITE_OK, "fresh seeded database required");
    const auto users = scalar(raw, "SELECT count(*) FROM users");
    const auto posts = scalar(raw, "SELECT count(*) FROM posts");
    const auto likes = scalar(raw, "SELECT count(*) FROM likes");
    require(users >= 2 && posts >= 2 && scalar(raw, "SELECT min(id) FROM users") == 1 &&
                scalar(raw, "SELECT max(id) FROM users") == users &&
                scalar(raw, "SELECT min(id) FROM posts") == 1 &&
                scalar(raw, "SELECT max(id) FROM posts") == posts,
            "seed must have contiguous users/posts beginning at one");
    std::vector<tds::WriteCommand> commands;
    std::vector<Expected> expected;
    commands.reserve(jobs);
    expected.reserve(jobs);
    Digest input;
    for (std::size_t i = 0; i < jobs; ++i)
    {
        const auto ordinal = static_cast<std::int64_t>(i);
        const auto user = 1 + (ordinal * 17) % users;
        const auto post = 1 + (ordinal * 48271) % posts;
        const auto fault = mixed ? i % 47 : 0;
        if (fault == 11 || fault == 22 || i % 8 == 0)
        {
            std::string body = fault == 22 ? "" : "production-group-probe:" + std::to_string(i);
            const auto author = fault == 11 ? -1 : user;
            commands.emplace_back(tds::CreatePost{tds::UserId{author}, "probe-user", body});
            expected.push_back(fault == 11 || fault == 22 ? Expected::constraint
                                                          : Expected::create);
            input.number(0);
            input.number(static_cast<std::uint64_t>(author));
            input.text(body);
        }
        else
        {
            const auto author = fault == 44 ? -1 : user;
            const auto target = fault == 33 ? -1 : post;
            commands.emplace_back(tds::LikePost{tds::UserId{author}, tds::PostId{target}});
            expected.push_back(fault == 44   ? Expected::constraint
                               : fault == 33 ? Expected::missing
                                             : Expected::like);
            input.number(1);
            input.number(static_cast<std::uint64_t>(author));
            input.number(static_cast<std::uint64_t>(target));
        }
    }
    std::vector<tds::WriteOutcome> outcomes(jobs);
    std::vector<unsigned char> committed((jobs + cap - 1) / cap);
    tds::Store store(path);
    sqlite3_int64 sqlite_current = 0;
    sqlite3_int64 sqlite_highwater = 0;
    require(sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &sqlite_current, &sqlite_highwater, 1) ==
                SQLITE_OK,
            "reset SQLite allocation highwater");
    const auto wall_started = now(CLOCK_MONOTONIC);
    const auto process_started = now(CLOCK_PROCESS_CPUTIME_ID);
    const auto thread_started = now(CLOCK_THREAD_CPUTIME_ID);
    for (std::size_t offset = 0; offset < jobs; offset += cap)
    {
        const auto count = std::min(cap, jobs - offset);
        committed[offset / cap] =
            store
                .commit(std::span<const tds::WriteCommand>(commands).subspan(offset, count),
                        std::span<tds::WriteOutcome>(outcomes).subspan(offset, count))
                .has_value();
    }
    const auto thread_cpu = now(CLOCK_THREAD_CPUTIME_ID) - thread_started;
    const auto process_cpu = now(CLOCK_PROCESS_CPUTIME_ID) - process_started;
    const auto wall = now(CLOCK_MONOTONIC) - wall_started;
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "probe peak RSS");
    require(sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &sqlite_current, &sqlite_highwater, 0) ==
                SQLITE_OK,
            "read SQLite allocation highwater");

    Digest output;
    std::array<std::uint64_t, 5> counts{};
    auto stored_post = prepare(raw, "SELECT user_id,body,created_at FROM posts WHERE id=?");
    auto stored_like = prepare(raw, "SELECT count(*) FROM likes WHERE user_id=? AND post_id=?");
    for (std::size_t i = 0; i < jobs; ++i)
    {
        const auto &result = outcomes[i];
        output.number(static_cast<unsigned>(expected[i]));
        if (expected[i] == Expected::constraint || expected[i] == Expected::missing)
        {
            const bool singleton = std::min(cap, jobs - (i / cap) * cap) == 1;
            const auto error = expected[i] == Expected::missing
                                   ? tds::WriteErrorCode::post_not_found
                               : singleton ? tds::WriteErrorCode::transaction
                                           : tds::WriteErrorCode::constraint;
            require(!result && result.error().code == error,
                    "constraint/missing-post outcome changed");
            output.number(static_cast<unsigned>(result.error().code));
            ++counts[expected[i] == Expected::missing ? 3U : 4U];
            continue;
        }
        require(result.has_value(), "valid command failed");
        if (expected[i] == Expected::create)
        {
            const auto *created = std::get_if<tds::CreatedPost>(&*result);
            const auto &command = std::get<tds::CreatePost>(commands[i]);
            require(created &&
                        sqlite3_bind_int64(stored_post.get(), 1, created->id.value) == SQLITE_OK &&
                        sqlite3_step(stored_post.get()) == SQLITE_ROW,
                    "created post missing");
            const auto *body =
                reinterpret_cast<const char *>(sqlite3_column_text(stored_post.get(), 1));
            const auto *timestamp =
                reinterpret_cast<const char *>(sqlite3_column_text(stored_post.get(), 2));
            require(
                sqlite3_column_int64(stored_post.get(), 0) == command.user_id.value && body &&
                    timestamp &&
                    std::string_view(body, static_cast<std::size_t>(sqlite3_column_bytes(
                                               stored_post.get(), 1))) == command.body &&
                    std::string_view(timestamp, static_cast<std::size_t>(
                                                    sqlite3_column_bytes(stored_post.get(), 2))) ==
                        std::string_view(created->created_at.data(), created->created_at.size()),
                "create output differs from stored row or timestamp");
            output.number(static_cast<std::uint64_t>(created->id.value));
            output.text(command.body);
            sqlite3_reset(stored_post.get());
            ++counts[0];
        }
        else
        {
            const auto *like = std::get_if<tds::LikeResult>(&*result);
            const auto &command = std::get<tds::LikePost>(commands[i]);
            require(
                like &&
                    sqlite3_bind_int64(stored_like.get(), 1, command.user_id.value) == SQLITE_OK &&
                    sqlite3_bind_int64(stored_like.get(), 2, command.post_id.value) == SQLITE_OK &&
                    sqlite3_step(stored_like.get()) == SQLITE_ROW &&
                    sqlite3_column_int64(stored_like.get(), 0) == 1,
                "successful like is missing from storage");
            output.number(static_cast<unsigned>(*like));
            sqlite3_reset(stored_like.get());
            ++counts[*like == tds::LikeResult::created ? 1U : 2U];
        }
    }
    std::size_t commits = 0;
    for (std::size_t i = 0; i < committed.size(); ++i)
    {
        const auto offset = i * cap;
        const bool failed =
            std::min(cap, jobs - offset) == 1 && expected[offset] == Expected::constraint;
        require(static_cast<bool>(committed[i]) != failed, "unexpected batch transaction result");
        commits += committed[i] != 0;
    }
    require(scalar(raw, "SELECT count(*) FROM posts") ==
                    posts + static_cast<std::int64_t>(counts[0]) &&
                scalar(raw, "SELECT count(*) FROM likes") ==
                    likes + static_cast<std::int64_t>(counts[1]),
            "stored row counts differ from acknowledged outcomes");
    std::printf(
        "{\"scope\":\"production Store::commit\",\"jobs\":%zu,\"batch\":%zu,\"workload\":\"%s\","
        "\"thread_cpu_ns\":%llu,\"process_cpu_ns\":%llu,\"wall_ns\":%llu,\"peak_rss_kib\":%ld,"
        "\"commits\":%zu,"
        "\"sqlite_process_current_bytes\":%lld,\"sqlite_process_highwater_bytes\":%lld,"
        "\"created_posts\":%llu,\"created_likes\":%llu,\"duplicate_likes\":%llu,"
        "\"missing_posts\":%llu,\"constraint_errors\":%llu,\"input_hash\":%llu,"
        "\"output_hash_without_timestamps\":%llu,\"checks\":\"outcomes_rows_timestamps\"}\n",
        jobs, cap, mixed ? "mixed" : "valid", static_cast<unsigned long long>(thread_cpu),
        static_cast<unsigned long long>(process_cpu), static_cast<unsigned long long>(wall),
        usage.ru_maxrss, commits, static_cast<long long>(sqlite_current),
        static_cast<long long>(sqlite_highwater), static_cast<unsigned long long>(counts[0]),
        static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]),
        static_cast<unsigned long long>(counts[3]), static_cast<unsigned long long>(counts[4]),
        static_cast<unsigned long long>(input.value()),
        static_cast<unsigned long long>(output.value()));
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        require(argc >= 2 && argc <= 5,
                "usage: group_commit_probe FRESH_SEEDED_DB [jobs=20000] [batch=32] [valid|mixed]");
        const auto jobs = argc > 2 ? positive(argv[2]) : 20000;
        const auto cap = argc > 3 ? positive(argv[3]) : tds::write_batch_limit;
        const std::string_view workload = argc > 4 ? argv[4] : "valid";
        require(cap <= tds::write_batch_limit && jobs <= 2000000 &&
                    (workload == "valid" || workload == "mixed"),
                "invalid batch/job/workload option");
        benchmark(argv[1], jobs, cap, workload == "mixed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "group-commit probe failed: %s\n", error.what());

        return 1;
    }
}
