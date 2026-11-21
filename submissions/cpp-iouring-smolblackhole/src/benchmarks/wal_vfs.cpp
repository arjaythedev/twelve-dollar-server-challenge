#include "storage/wal_vfs.hpp"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <unordered_map>
#include <utility>

namespace
{
constexpr std::int64_t jobs = 4096;
constexpr std::int64_t batch = 32;
constexpr int checkpoint_threshold = 1000;
constexpr int rounds = 6;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint64_t nanoseconds(clockid_t clock)
{
    timespec value{};
    require(::clock_gettime(clock, &value) == 0, "read probe clock");
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

using Database = std::unique_ptr<sqlite3, Close>;

void sql(sqlite3 *db, const char *text)
{
    if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

Database open(const std::string &path, const char *vfs)
{
    sqlite3 *raw = nullptr;
    const int result = sqlite3_open_v2(path.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                       vfs);
    Database db(raw);
    require(result == SQLITE_OK, "open WAL probe database");
    return db;
}

std::int64_t scalar(sqlite3 *db, const char *text)
{
    sqlite3_stmt *raw = nullptr;
    require(sqlite3_prepare_v2(db, text, -1, &raw, nullptr) == SQLITE_OK, "prepare probe scalar");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(raw, sqlite3_finalize);
    require(sqlite3_step(raw) == SQLITE_ROW, "probe scalar returned no row");
    return sqlite3_column_int64(raw, 0);
}

class Directory
{
  public:
    Directory()
    {
        char pattern[] = "/tmp/td-wal-probe-XXXXXX";
        const auto *created = ::mkdtemp(pattern);
        require(created != nullptr, "create probe directory");
        path = created;
    }

    ~Directory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::string path;
};

void seed(const std::string &path)
{
    auto db = open(path, "unix-excl");
    sql(db.get(), R"sql(
        PRAGMA page_size=4096;
        CREATE TABLE users(id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE);
        CREATE TABLE posts(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
          body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL);
        CREATE TABLE likes(user_id INTEGER NOT NULL REFERENCES users(id),
          post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL,
          PRIMARY KEY(user_id,post_id));
        CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC,id DESC);
        CREATE INDEX posts_user_id_idx ON posts(user_id);
        CREATE INDEX likes_post_id_idx ON likes(post_id);
        WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM ids WHERE n<64)
          INSERT INTO users SELECT n,'user:'||n FROM ids;
        WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM ids WHERE n<1024)
          INSERT INTO posts SELECT n,1,'seed:'||n,'2025-01-01T00:00:00Z' FROM ids;
    )sql");
}

struct Counters
{
    std::uint64_t writes = 0;
    std::uint64_t bytes = 0;
    std::uint64_t syncs = 0;
};

struct NativeFile
{
    const sqlite3_io_methods *original;
    sqlite3_io_methods methods;
};

class CountedParent
{
  public:
    explicit CountedParent(sqlite3_vfs &parent) : parent_(parent), native_(parent)
    {
        require(active_ == nullptr, "overlapping counted WAL parents");
        active_ = this;
        native_.zName = "wal-probe-counted-parent";
        native_.pNext = nullptr;
        native_.xOpen = opened;
        require(sqlite3_vfs_register(&native_, 0) == SQLITE_OK, "register counted WAL parent");
    }

    ~CountedParent()
    {
        sqlite3_vfs_unregister(&native_);
        active_ = nullptr;
    }

    sqlite3_vfs &native()
    {
        return native_;
    }

    Counters counters;

  private:
    static int opened(sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *output)
    {
        auto &owner = *active_;
        const int result = owner.parent_.xOpen(&owner.parent_, name, file, flags, output);
        if (result == SQLITE_OK && (flags & SQLITE_OPEN_WAL))
        {
            auto [entry, inserted] = owner.files_.emplace(
                file, NativeFile{file->pMethods, *file->pMethods});
            require(inserted, "duplicate counted WAL file");
            entry->second.methods.xWrite = written;
            entry->second.methods.xSync = synced;
            entry->second.methods.xClose = closed;
            file->pMethods = &entry->second.methods;
        }
        return result;
    }

    static int written(sqlite3_file *file, const void *data, int length, sqlite3_int64 offset)
    {
        auto &owner = *active_;
        ++owner.counters.writes;
        owner.counters.bytes += static_cast<std::uint64_t>(length);
        return owner.files_.at(file).original->xWrite(file, data, length, offset);
    }

    static int synced(sqlite3_file *file, int flags)
    {
        auto &owner = *active_;
        ++owner.counters.syncs;
        return owner.files_.at(file).original->xSync(file, flags);
    }

    static int closed(sqlite3_file *file)
    {
        auto &owner = *active_;
        const auto *methods = owner.files_.at(file).original;
        const int result = methods->xClose(file);
        owner.files_.erase(file);
        return result;
    }

    static inline CountedParent *active_ = nullptr;
    sqlite3_vfs &parent_;
    sqlite3_vfs native_;
    std::unordered_map<sqlite3_file *, NativeFile> files_;
};

class Statement
{
  public:
    Statement(sqlite3 *db, const char *text)
    {
        require(sqlite3_prepare_v3(db, text, -1, SQLITE_PREPARE_PERSISTENT, &value_, nullptr) == SQLITE_OK,
                "prepare probe write");
    }

    ~Statement()
    {
        sqlite3_finalize(value_);
    }

    void execute(std::int64_t first, std::int64_t second)
    {
        require(sqlite3_bind_int64(value_, 1, first) == SQLITE_OK &&
                    sqlite3_bind_int64(value_, 2, second) == SQLITE_OK &&
                    sqlite3_step(value_) == SQLITE_DONE,
                "execute deterministic probe write");
        require(sqlite3_reset(value_) == SQLITE_OK && sqlite3_clear_bindings(value_) == SQLITE_OK,
                "reset probe write");
    }

    void execute()
    {
        require(sqlite3_step(value_) == SQLITE_DONE && sqlite3_reset(value_) == SQLITE_OK,
                "execute prepared probe transaction control");
    }

  private:
    sqlite3_stmt *value_ = nullptr;
};

struct Outcome
{
    std::int64_t posts;
    std::int64_t likes;
    std::int64_t post_ids;
    std::int64_t like_keys;
    std::int64_t body_bytes;
    bool operator==(const Outcome &) const = default;
};

Outcome inspect(sqlite3 *db)
{
    require(scalar(db, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check<>'ok'") == 0,
            "probe database failed integrity_check");
    require(scalar(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0,
            "probe database failed foreign_key_check");
    return {scalar(db, "SELECT count(*) FROM posts"), scalar(db, "SELECT count(*) FROM likes"),
            scalar(db, "SELECT sum(id) FROM posts"),
            scalar(db, "SELECT sum(user_id*1000000+post_id) FROM likes"),
            scalar(db, "SELECT sum(length(body)) FROM posts")};
}

int committed(void *context, sqlite3 *, const char *, int frames)
{
    *static_cast<int *>(context) = frames;
    return SQLITE_OK;
}

void checkpoint(sqlite3 *db, int mode)
{
    int frames = -1;
    int completed = -1;
    require(sqlite3_wal_checkpoint_v2(db, "main", mode, &frames, &completed) == SQLITE_OK &&
                frames == completed,
            "probe checkpoint incomplete");
}

struct Result
{
    std::uint64_t cpu_ns;
    std::uint64_t lifecycle_cpu_ns;
    std::uint64_t wall_ns;
    Counters wal;
    unsigned checkpoints;
    Outcome outcome;
};

Result sample(bool coalesced, bool counted)
{
    Directory directory;
    const auto path = directory.path + "/feed.db";
    seed(path);
    auto *parent = sqlite3_vfs_find("unix-excl");
    require(parent != nullptr, "unix-excl unavailable for WAL probe");
    std::optional<CountedParent> counter;
    if (counted)
    {
        counter.emplace(*parent);
        parent = &counter->native();
    }
    std::optional<tds::storage::WalVfs> adapter;
    if (coalesced)
    {
        adapter.emplace(*parent, "wal-probe-coalesced");
    }
    const char *name = adapter ? adapter->name() : parent->zName;
    const auto lifecycle_start = nanoseconds(CLOCK_PROCESS_CPUTIME_ID);
    auto db = open(path, name);
    sql(db.get(), "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"
                  "PRAGMA cache_size=-16384; PRAGMA journal_size_limit=0; PRAGMA wal_autocheckpoint=0;");
    int frames = 0;
    sqlite3_wal_hook(db.get(), committed, &frames);
    const auto counters_before = counter ? counter->counters : Counters{};
    std::uint64_t cpu_ns = 0;
    std::uint64_t wall_ns = 0;
    unsigned checkpoints = 0;
    {
        Statement post(db.get(), "INSERT INTO posts(id,user_id,body,created_at)"
                                 " VALUES(?1,1,'created:'||?2,'2025-01-02T00:00:00Z')");
        Statement like(db.get(), "INSERT INTO likes(user_id,post_id,created_at)"
                                 " VALUES(?1,?2,'2025-01-02T00:00:00Z')");
        Statement begin(db.get(), "BEGIN");
        Statement commit(db.get(), "COMMIT");
        Statement savepoint(db.get(), "SAVEPOINT write_job");
        Statement release(db.get(), "RELEASE write_job");
        const auto wall_start = nanoseconds(CLOCK_MONOTONIC);
        const auto cpu_start = nanoseconds(CLOCK_PROCESS_CPUTIME_ID);
        for (std::int64_t first = 0; first < jobs; first += batch)
        {
            begin.execute();
            for (std::int64_t index = first; index < first + batch; ++index)
            {
                savepoint.execute();
                if (index % 8 == 0)
                {
                    post.execute(1025 + index / 8, index);
                }
                else
                {
                    like.execute(1 + index / 1024, 1 + index % 1024);
                }
                release.execute();
            }
            commit.execute();
            if (frames >= checkpoint_threshold)
            {
                checkpoint(db.get(), SQLITE_CHECKPOINT_PASSIVE);
                ++checkpoints;
            }
        }
        cpu_ns = nanoseconds(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
        wall_ns = nanoseconds(CLOCK_MONOTONIC) - wall_start;
    }
    const auto counters_after = counter ? counter->counters : Counters{};
    const Counters work{counters_after.writes - counters_before.writes,
                        counters_after.bytes - counters_before.bytes,
                        counters_after.syncs - counters_before.syncs};
    checkpoint(db.get(), SQLITE_CHECKPOINT_TRUNCATE);
    db.reset();
    const auto lifecycle_cpu_ns = nanoseconds(CLOCK_PROCESS_CPUTIME_ID) - lifecycle_start;
    auto reopened = open(path, name);
    const auto outcome = inspect(reopened.get());
    require(outcome.posts == 1536 && outcome.likes == 3584,
            "fixed useful work count differs between probe variants");
    return {cpu_ns, lifecycle_cpu_ns, wall_ns, work, checkpoints, outcome};
}

const char *label(bool coalesced)
{
    return coalesced ? "coalesced" : "unix-excl";
}
} // namespace

int main()
{
    try
    {
        std::optional<Outcome> expected;
        std::printf("{\"metadata\":{\"jobs\":%lld,\"batch\":%lld,\"rounds\":%d,"
                    "\"checkpoint_threshold\":%d,\"synchronous\":\"NORMAL\","
                    "\"creates_per_batch\":4,\"likes_per_batch\":28,"
                    "\"scope\":\"SQLite write-kernel ceiling, no server throughput claim\","
                    "\"cpu_instrumentation\":\"none; parent xWrite counts use separate identical work\"},"
                    "\"samples\":[\n",
                    static_cast<long long>(jobs), static_cast<long long>(batch), rounds,
                    checkpoint_threshold);
        for (int round = 0; round < rounds; ++round)
        {
            std::array<Result, 2> counted;
            for (int order = 0; order < 2; ++order)
            {
                const bool coalesced = (round + order) % 2 != 0;
                Result clean;
                Result traced;
                if (round % 2 == 0)
                {
                    clean = sample(coalesced, false);
                    traced = sample(coalesced, true);
                }
                else
                {
                    traced = sample(coalesced, true);
                    clean = sample(coalesced, false);
                }
                require(clean.outcome == traced.outcome && clean.checkpoints == traced.checkpoints,
                        "counted and uninstrumented samples did different useful work");
                if (!expected)
                {
                    expected = clean.outcome;
                }
                require(clean.outcome == *expected, "rotated variants have different contents");
                counted[coalesced ? 1U : 0U] = traced;
                std::printf("%s{\"round\":%d,\"order\":%d,\"mode\":\"%s\","
                            "\"cpu_ns\":%llu,\"lifecycle_cpu_ns\":%llu,\"wall_ns\":%llu,"
                            "\"counted_cpu_ns\":%llu,\"wal_writes\":%llu,\"wal_bytes\":%llu,"
                            "\"wal_syncs\":%llu,\"checkpoints\":%u,\"posts\":%lld,\"likes\":%lld,"
                            "\"post_id_sum\":%lld,\"like_key_sum\":%lld,\"body_bytes\":%lld}\n",
                            round == 0 && order == 0 ? "" : ",", round, order, label(coalesced),
                            static_cast<unsigned long long>(clean.cpu_ns),
                            static_cast<unsigned long long>(clean.lifecycle_cpu_ns),
                            static_cast<unsigned long long>(clean.wall_ns),
                            static_cast<unsigned long long>(traced.cpu_ns),
                            static_cast<unsigned long long>(traced.wal.writes),
                            static_cast<unsigned long long>(traced.wal.bytes),
                            static_cast<unsigned long long>(traced.wal.syncs), traced.checkpoints,
                            static_cast<long long>(clean.outcome.posts),
                            static_cast<long long>(clean.outcome.likes),
                            static_cast<long long>(clean.outcome.post_ids),
                            static_cast<long long>(clean.outcome.like_keys),
                            static_cast<long long>(clean.outcome.body_bytes));
            }
            require(counted[0].wal.bytes == counted[1].wal.bytes &&
                        counted[0].wal.syncs == counted[1].wal.syncs &&
                        counted[0].checkpoints == counted[1].checkpoints,
                    "WAL coalescing changed written bytes, sync policy or checkpoint work");
        }
        std::puts("]}");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "WAL VFS probe failed: %s\n", error.what());
        return 1;
    }
}
