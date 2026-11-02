#include <sqlite3.h>

// Write-kernel experiment, build with the group_commit_probe CMake target
// Each run needs a fresh seed copy; --workload valid --batch 16 selects grouped valid writes

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <time.h>
#include <utility>

static_assert(SQLITE_VERSION_NUMBER == 3053001, "Use the pinned SQLite 3.53.1 headers");

namespace
{
using Id = std::int64_t;
constexpr std::size_t max_batch = 64;
constexpr std::uint64_t max_jobs = 2'000'000;
constexpr int checkpoint_threshold = 1000;

struct SqlError : std::runtime_error
{
    int code;

    explicit SqlError(sqlite3 *db)
        : std::runtime_error(sqlite3_errmsg(db)), code(sqlite3_extended_errcode(db))
    {
    }
};

void check(sqlite3 *db, int result)
{
    if (result != SQLITE_OK)
    {
        throw SqlError(db);
    }
}

void execute(sqlite3 *db, const char *sql)
{
    check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

using Connection = std::unique_ptr<sqlite3, Close>;

Connection open(const char *path, bool create)
{
    sqlite3 *raw = nullptr;
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (create ? SQLITE_OPEN_CREATE : 0);
    const int result = sqlite3_open_v2(path, &raw, flags, nullptr);
    Connection db(raw);
    if (result != SQLITE_OK)
    {
        throw std::runtime_error(raw ? sqlite3_errmsg(raw) : "database open failed");
    }
    execute(raw, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
                 "PRAGMA foreign_keys=ON; PRAGMA cache_size=-16384;");
    check(raw, sqlite3_wal_autocheckpoint(raw, 0));
    return db;
}

class Statement
{
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db)
    {
        check(db, sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &value_, nullptr));
    }

    ~Statement()
    {
        sqlite3_finalize(value_);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    class Use
    {
      public:
        explicit Use(Statement &owner) : owner_(owner)
        {
        }

        ~Use()
        {
            sqlite3_reset(owner_.value_);
            sqlite3_clear_bindings(owner_.value_);
        }

        Use(const Use &) = delete;
        Use &operator=(const Use &) = delete;

        void bind(int column, Id value)
        {
            check(owner_.db_, sqlite3_bind_int64(owner_.value_, column, value));
        }

        void bind(int column, std::string_view value)
        {
            check(owner_.db_, sqlite3_bind_text64(owner_.value_, column, value.data(), value.size(),
                                                  SQLITE_TRANSIENT, SQLITE_UTF8));
        }

        bool next()
        {
            const int result = sqlite3_step(owner_.value_);
            if (result == SQLITE_ROW)
            {
                return true;
            }
            if (result == SQLITE_DONE)
            {
                return false;
            }
            throw SqlError(owner_.db_);
        }

        Id number(int column) const
        {
            return sqlite3_column_int64(owner_.value_, column);
        }

        std::string_view text(int column) const
        {
            const auto *data = sqlite3_column_text(owner_.value_, column);
            if (!data)
            {
                throw std::runtime_error("database returned missing text");
            }
            return {reinterpret_cast<const char *>(data),
                    static_cast<std::size_t>(sqlite3_column_bytes(owner_.value_, column))};
        }

      private:
        Statement &owner_;
    };

    Use use()
    {
        return Use(*this);
    }

  private:
    sqlite3 *db_;
    sqlite3_stmt *value_ = nullptr;
};

Id scalar(sqlite3 *db, const char *sql)
{
    Statement statement(db, sql);
    auto query = statement.use();
    if (!query.next())
    {
        throw std::runtime_error("scalar returned no row");
    }
    const Id result = query.number(0);
    if (query.next())
    {
        throw std::runtime_error("scalar returned several rows");
    }
    return result;
}

std::string scalar_text(sqlite3 *db, const char *sql)
{
    Statement statement(db, sql);
    auto query = statement.use();
    if (!query.next())
    {
        throw std::runtime_error("text scalar returned no row");
    }
    const std::string result(query.text(0));
    if (query.next())
    {
        throw std::runtime_error("text scalar returned several rows");
    }
    return result;
}

class Transaction
{
  public:
    explicit Transaction(sqlite3 *db) : db_(db)
    {
        execute(db_, "BEGIN");
    }

    ~Transaction()
    {
        if (!committed_)
        {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit()
    {
        execute(db_, "COMMIT");
        committed_ = true;
    }

  private:
    sqlite3 *db_;
    bool committed_ = false;
};

struct Seed
{
    Id users;
    Id posts;
    Id likes;
    std::string latest_like_timestamp;
};

Seed inspect_seed(sqlite3 *db)
{
    Seed seed{scalar(db, "SELECT count(*) FROM users"), scalar(db, "SELECT count(*) FROM posts"),
              scalar(db, "SELECT count(*) FROM likes"),
              scalar_text(db, "SELECT coalesce(max(created_at),'') FROM likes")};
    if (seed.users < 2 || seed.posts < 2 || scalar(db, "SELECT min(id) FROM users") != 1 ||
        scalar(db, "SELECT max(id) FROM users") != seed.users ||
        scalar(db, "SELECT min(id) FROM posts") != 1 ||
        scalar(db, "SELECT max(id) FROM posts") != seed.posts)
    {
        throw std::runtime_error("seed users/posts must have contiguous ids beginning at 1");
    }
    if (seed.latest_like_timestamp >=
        scalar_text(db, "SELECT strftime('%Y-%m-%dT%H:%M:%fZ','now')"))
    {
        throw std::runtime_error("seed like timestamps must precede the experiment clock");
    }
    return seed;
}

class Hash
{
  public:
    void number(std::uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            byte(static_cast<unsigned char>(value & 0xffU));
            value >>= 8;
        }
    }

    void text(std::string_view value)
    {
        number(value.size());
        for (const unsigned char item : value)
        {
            byte(item);
        }
    }

    std::uint64_t value() const noexcept
    {
        return value_;
    }

  private:
    void byte(unsigned char value)
    {
        value_ ^= value;
        value_ *= 1099511628211ULL;
    }

    std::uint64_t value_ = 14695981039346656037ULL;
};

std::uint64_t mix(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

enum class Operation : std::uint64_t
{
    like,
    create
};
enum class Workload
{
    mixed,
    valid
};

struct Job
{
    std::uint64_t ordinal;
    Operation operation;
    Id user;
    Id post;
    std::string body;
};

Job make_job(std::uint64_t ordinal, const Seed &seed, Workload workload)
{
    const auto slot = ordinal % 100;
    const auto block = ordinal / 100;
    const bool create = slot < 96 && slot % 8 == 0;
    const bool mixed = workload == Workload::mixed;
    // Mixed slots 1 and 2 repeat exactly, slot 3 depends on the valid create at slot 0
    const auto key = mixed && slot == 2 ? ordinal - 1 : ordinal;
    const Id user = 1 + static_cast<Id>(mix(key) % static_cast<std::uint64_t>(seed.users));
    const Id post = 1 + static_cast<Id>(mix(key + 31) % static_cast<std::uint64_t>(seed.posts));
    Job job{ordinal, create ? Operation::create : Operation::like, user, post, {}};
    if (create)
    {
        job.body = "group-commit probe job " + std::to_string(ordinal);
        if (mixed && slot == 88)
        {
            job.user = -1;
        }
        return job;
    }
    if (!mixed)
    {
        return job;
    }
    if (slot == 3)
    {
        job.post = seed.posts + static_cast<Id>(block * 11 + 1);
    }
    if (slot == 5)
    {
        job.post = std::numeric_limits<Id>::max();
    }
    if (slot == 6)
    {
        job.user = -1;
    }
    return job;
}

void hash_job(Hash &hash, const Job &job)
{
    hash.number(job.ordinal);
    hash.number(static_cast<std::uint64_t>(job.operation));
    hash.number(static_cast<std::uint64_t>(job.user));
    hash.number(static_cast<std::uint64_t>(job.post));
    hash.text(job.body);
}

enum class Outcome : std::size_t
{
    created_post,
    created_like,
    duplicate_like,
    missing_post,
    constraint
};

struct Result
{
    Outcome outcome = Outcome::constraint;
    Id post = 0;
    int error_code = 0;
    std::string timestamp;
};

bool timestamp_shape(std::string_view value)
{
    if (value.size() != 24)
    {
        return false;
    }
    constexpr std::string_view pattern = "0000-00-00T00:00:00.000Z";
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] == '0')
        {
            if (value[i] < '0' || value[i] > '9')
            {
                return false;
            }
        }
        else if (value[i] != pattern[i])
        {
            return false;
        }
    }
    return true;
}

class Writer
{
  public:
    explicit Writer(sqlite3 *db)
        : db_(db),
          create_(db, "INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at"),
          exists_(db, "SELECT 1 FROM posts WHERE id=?"),
          like_(db, "INSERT INTO likes(user_id, post_id) VALUES(?, ?) "
                    "ON CONFLICT(user_id, post_id) DO NOTHING")
    {
    }

    Result perform(const Job &job)
    {
        if (job.operation == Operation::create)
        {
            auto query = create_.use();
            query.bind(1, job.user);
            query.bind(2, job.body);
            if (!query.next())
            {
                throw std::runtime_error("insert returned no row");
            }
            Result result{Outcome::created_post, query.number(0), 0, std::string(query.text(1))};
            while (query.next())
            {
            }
            if (!timestamp_shape(result.timestamp))
            {
                throw std::runtime_error("unexpected generated timestamp format");
            }
            return result;
        }
        bool found = false;
        {
            auto query = exists_.use();
            query.bind(1, job.post);
            found = query.next();
            while (found && query.next())
            {
            }
        }
        if (!found)
        {
            return {Outcome::missing_post, job.post, 0, {}};
        }
        {
            auto query = like_.use();
            query.bind(1, job.user);
            query.bind(2, job.post);
            while (query.next())
            {
            }
        }
        const auto outcome =
            sqlite3_changes(db_) == 1 ? Outcome::created_like : Outcome::duplicate_like;
        return {outcome, job.post, 0, {}};
    }

  private:
    sqlite3 *db_;
    Statement create_;
    Statement exists_;
    Statement like_;
};

struct Sink
{
    Hash input;
    Hash output;
    std::array<std::uint64_t, 5> outcomes{};
    std::uint64_t acknowledged = 0;
    std::uint64_t successful = 0;

    void accept(const Job &job, const Result &result)
    {
        ++acknowledged;
        ++outcomes[static_cast<std::size_t>(result.outcome)];
        if (result.outcome == Outcome::created_post || result.outcome == Outcome::created_like ||
            result.outcome == Outcome::duplicate_like)
        {
            ++successful;
        }
        hash_job(output, job);
        output.number(static_cast<std::uint64_t>(result.outcome));
        output.number(static_cast<std::uint64_t>(result.post));
        output.number(static_cast<std::uint64_t>(result.error_code));
        if (!result.timestamp.empty())
        {
            output.text("<generated-timestamp>");
        }
    }
};

struct Wal
{
    std::uint64_t appended_frames = 0;
    std::uint64_t checkpoints = 0;
    int previous_frames = 0;
    int last_frames = 0;
    int max_frames = 0;
    bool checkpoint_pending = false;

    static int committed(void *context, sqlite3 *, const char *, int frames) noexcept
    {
        auto &owner = *static_cast<Wal *>(context);
        const int appended =
            frames >= owner.previous_frames ? frames - owner.previous_frames : frames;
        owner.appended_frames += static_cast<std::uint64_t>(appended);
        owner.previous_frames = frames;
        owner.last_frames = frames;
        if (frames > owner.max_frames)
        {
            owner.max_frames = frames;
        }
        owner.checkpoint_pending = frames >= checkpoint_threshold;
        return SQLITE_OK;
    }

    void checkpoint(sqlite3 *db, bool force = false)
    {
        if (!force && !checkpoint_pending)
        {
            return;
        }
        int frames = -1;
        int copied = -1;
        check(db,
              sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &frames, &copied));
        ++checkpoints;
        if (frames != copied)
        {
            throw std::runtime_error(
                "probe requires an isolated database with no checkpoint readers");
        }
        // No readers remain, so the next writing transaction restarts the WAL
        previous_frames = 0;
        checkpoint_pending = false;
    }
};

struct Run
{
    Sink sink;
    Wal wal;
    std::uint64_t commits = 0;
    std::uint64_t rollbacks = 0;
};

struct Pending
{
    Job job{};
    Result result{};
};

void run_jobs(sqlite3 *db, Writer &writer, const Seed &seed, std::uint64_t count,
              std::size_t batch_size, bool savepoints, Run &run,
              Workload workload = Workload::mixed)
{
    std::array<Pending, max_batch> pending;
    for (std::uint64_t first = 0; first < count;)
    {
        const auto remaining = count - first;
        const auto size = remaining < batch_size ? static_cast<std::size_t>(remaining) : batch_size;
        if (!savepoints)
        {
            auto job = make_job(first, seed, workload);
            hash_job(run.sink.input, job);
            Result result;
            {
                Transaction transaction(db);
                bool constraint = false;
                try
                {
                    result = writer.perform(job);
                }
                catch (const SqlError &error)
                {
                    if ((error.code & 0xff) != SQLITE_CONSTRAINT)
                    {
                        throw;
                    }
                    constraint = true;
                    ++run.rollbacks;
                    result = {Outcome::constraint, job.post, error.code, {}};
                }
                if (!constraint)
                {
                    transaction.commit();
                    ++run.commits;
                }
            }
            run.sink.accept(job, result);
            run.wal.checkpoint(db);
            ++first;
            continue;
        }

        {
            Transaction transaction(db);
            for (std::size_t offset = 0; offset < size; ++offset)
            {
                auto &item = pending[offset];
                item.job = make_job(first + offset, seed, workload);
                hash_job(run.sink.input, item.job);
                execute(db, "SAVEPOINT job");
                try
                {
                    item.result = writer.perform(item.job);
                }
                catch (const SqlError &error)
                {
                    if ((error.code & 0xff) != SQLITE_CONSTRAINT)
                    {
                        throw;
                    }
                    execute(db, "ROLLBACK TO job");
                    item.result = {Outcome::constraint, item.job.post, error.code, {}};
                }
                execute(db, "RELEASE job");
            }
            transaction.commit();
            ++run.commits;
        }
        // Publishing is outside the transaction scope and only follows a successful COMMIT
        for (std::size_t offset = 0; offset < size; ++offset)
        {
            run.sink.accept(pending[offset].job, pending[offset].result);
        }
        run.wal.checkpoint(db);
        first += size;
    }
}

std::uint64_t state_hash(sqlite3 *db, const Seed &seed)
{
    Hash hash;
    {
        Statement statement(db, "SELECT id,username,created_at FROM users ORDER BY id");
        auto query = statement.use();
        while (query.next())
        {
            hash.text("user");
            hash.number(static_cast<std::uint64_t>(query.number(0)));
            hash.text(query.text(1));
            hash.text(query.text(2));
        }
    }
    {
        Statement statement(db, "SELECT id,user_id,body,created_at FROM posts ORDER BY id");
        auto query = statement.use();
        while (query.next())
        {
            hash.text("post");
            hash.number(static_cast<std::uint64_t>(query.number(0)));
            hash.number(static_cast<std::uint64_t>(query.number(1)));
            hash.text(query.text(2));
            const auto timestamp = query.text(3);
            if (!timestamp_shape(timestamp))
            {
                throw std::runtime_error("invalid stored post timestamp");
            }
            hash.text(query.number(0) > seed.posts ? "<generated-timestamp>" : timestamp);
        }
    }
    {
        Statement statement(
            db, "SELECT user_id,post_id,created_at FROM likes ORDER BY post_id,user_id");
        auto query = statement.use();
        while (query.next())
        {
            hash.text("like");
            hash.number(static_cast<std::uint64_t>(query.number(0)));
            hash.number(static_cast<std::uint64_t>(query.number(1)));
            const auto timestamp = query.text(2);
            if (!timestamp_shape(timestamp))
            {
                throw std::runtime_error("invalid stored like timestamp");
            }
            hash.text(timestamp > seed.latest_like_timestamp ? "<generated-timestamp>" : timestamp);
        }
    }
    return hash.value();
}

double cpu_seconds()
{
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
    {
        throw std::runtime_error("CLOCK_PROCESS_CPUTIME_ID failed");
    }
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
}

int db_status(sqlite3 *db, int operation, bool reset)
{
    int current = 0;
    int highwater = 0;
    check(db, sqlite3_db_status(db, operation, &current, &highwater, reset ? 1 : 0));
    return current;
}

std::uintmax_t file_bytes(const std::string &path)
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
        return 0;
    }
    if (error)
    {
        throw std::runtime_error(error.message());
    }
    return bytes;
}

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void self_test(const std::string &schema_path)
{
    std::ifstream file(schema_path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("cannot open self-test schema");
    }
    const std::string schema{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    require(schema.size() < 65'536, "unexpected self-test schema size");
    const auto fixture = [&schema] {
        auto db = open(":memory:", true);
        execute(db.get(), schema.c_str());
        execute(db.get(), "INSERT INTO users(id,username,created_at) VALUES"
                          "(1,'one','2000-01-01T00:00:00.000Z'),"
                          "(2,'two','2000-01-01T00:00:00.000Z');"
                          "INSERT INTO posts(id,user_id,body,created_at) VALUES"
                          "(1,1,'one','2000-01-01T00:00:00.000Z'),"
                          "(2,2,'two','2000-01-01T00:00:00.000Z');");
        return db;
    };

    std::uint64_t expected_input = 0;
    std::uint64_t expected_output = 0;
    std::uint64_t expected_state = 0;
    constexpr std::array<std::size_t, 5> batches{0, 1, 4, 16, 64};
    for (const auto batch : batches)
    {
        auto db = fixture();
        const Seed seed = inspect_seed(db.get());
        Writer writer(db.get());
        Run run;
        run_jobs(db.get(), writer, seed, 300, batch == 0 ? 1 : batch, batch != 0, run);
        const auto state = state_hash(db.get(), seed);
        require(run.sink.acknowledged == 300, "self-test lost acknowledgements");
        require(run.sink.outcomes[static_cast<std::size_t>(Outcome::created_post)] == 33,
                "bad creates rolled back valid neighbors");
        require(run.sink.outcomes[static_cast<std::size_t>(Outcome::constraint)] == 6,
                "self-test expected six isolated foreign-key failures");
        require(run.sink.outcomes[static_cast<std::size_t>(Outcome::missing_post)] == 3,
                "self-test missing-post behavior changed");
        require(scalar(db.get(), "SELECT count(*) FROM posts") == 35,
                "self-test stored post count differs");
        if (batch == 0)
        {
            expected_input = run.sink.input.value();
            expected_output = run.sink.output.value();
            expected_state = state;
        }
        require(run.sink.input.value() == expected_input &&
                    run.sink.output.value() == expected_output && state == expected_state,
                "baseline and batch semantics differ");
    }
    for (const auto batch : std::array<std::size_t, 2>{0, 4})
    {
        auto db = fixture();
        const Seed seed = inspect_seed(db.get());
        Writer writer(db.get());
        Run run;
        sqlite3_commit_hook(
            db.get(),
            [](void *) -> int {
                return 1;
            },
            nullptr);
        bool rejected = false;
        try
        {
            run_jobs(db.get(), writer, seed, 4, batch == 0 ? 1 : batch, batch != 0, run);
        }
        catch (const SqlError &error)
        {
            rejected = error.code == SQLITE_CONSTRAINT_COMMITHOOK;
        }
        sqlite3_commit_hook(db.get(), nullptr, nullptr);
        require(rejected, "commit-hook failure was not propagated");
        require(run.commits == 0 && run.sink.acknowledged == 0 && run.sink.successful == 0,
                "failed group published an acknowledgement");
        require(scalar(db.get(), "SELECT count(*) FROM posts") == seed.posts &&
                    scalar(db.get(), "SELECT count(*) FROM likes") == seed.likes,
                "failed group retained uncommitted changes");
    }
    std::cout << "{\"self_test\":\"passed\",\"variants\":5,\"jobs_per_variant\":300,"
                 "\"commit_failure_acknowledged\":0}\n";
}

struct Options
{
    std::string path;
    std::string schema;
    std::uint64_t jobs = 100'000;
    std::size_t batch = 1;
    bool savepoints = false;
    Workload workload = Workload::mixed;
};

std::uint64_t positive(std::string_view text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
    {
        throw std::runtime_error("expected a positive integer");
    }
    return value;
}

Options options(int argc, char **argv)
{
    Options result;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view name(argv[index]);
        if (index + 1 == argc)
        {
            throw std::runtime_error("option requires a value");
        }
        const std::string_view value(argv[++index]);
        if (name == "--db")
        {
            result.path = value;
        }
        else if (name == "--self-test")
        {
            result.schema = value;
        }
        else if (name == "--jobs")
        {
            result.jobs = positive(value);
        }
        else if (name == "--workload")
        {
            if (value == "mixed")
            {
                result.workload = Workload::mixed;
            }
            else if (value == "valid")
            {
                result.workload = Workload::valid;
            }
            else
            {
                throw std::runtime_error("workload must be valid or mixed");
            }
        }
        else if (name == "--batch")
        {
            const auto batch = positive(value);
            if (batch != 1 && batch != 4 && batch != 16 && batch != 64)
            {
                throw std::runtime_error("batch must be 1, 4, 16, or 64");
            }
            result.batch = static_cast<std::size_t>(batch);
            result.savepoints = true;
        }
        else
        {
            throw std::runtime_error("unknown option");
        }
    }
    if (result.jobs > max_jobs)
    {
        throw std::runtime_error("job limit is 2000000");
    }
    if (result.path.empty() && result.schema.empty())
    {
        throw std::runtime_error("usage: probe --db fresh.db --jobs N [--batch 1|4|16|64] "
                                 "[--workload valid|mixed] "
                                 "or probe --self-test /path/schema.sql");
    }
    if (!result.path.empty() && !result.schema.empty())
    {
        throw std::runtime_error("self-test and database run are separate commands");
    }
    return result;
}

void benchmark(const Options &config)
{
    auto db = open(config.path.c_str(), false);
    require(scalar_text(db.get(), "PRAGMA journal_mode") == "wal", "WAL mode is required");
    require(scalar(db.get(), "PRAGMA synchronous") == 1, "NORMAL synchronous is required");
    require(scalar(db.get(), "PRAGMA foreign_keys") == 1, "foreign keys must be enabled");
    const Seed seed = inspect_seed(db.get());
    Writer writer(db.get());
    Run run;
    run.wal.checkpoint(db.get(), true);
    run.wal.checkpoints = 0;
    sqlite3_wal_hook(db.get(), Wal::committed, &run.wal);
    db_status(db.get(), SQLITE_DBSTATUS_CACHE_WRITE, true);
    db_status(db.get(), SQLITE_DBSTATUS_CACHE_SPILL, true);
    db_status(db.get(), SQLITE_DBSTATUS_CACHE_HIT, true);
    db_status(db.get(), SQLITE_DBSTATUS_CACHE_MISS, true);

    const auto wall_start = std::chrono::steady_clock::now();
    const auto cpu_start = cpu_seconds();
    run_jobs(db.get(), writer, seed, config.jobs, config.batch, config.savepoints, run,
             config.workload);
    // Include final checkpoint work so every variant leaves the same amount of checkpoint debt
    run.wal.checkpoint(db.get(), true);
    const auto cpu = cpu_seconds() - cpu_start;
    const auto wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    const int writes = db_status(db.get(), SQLITE_DBSTATUS_CACHE_WRITE, false);
    const int spills = db_status(db.get(), SQLITE_DBSTATUS_CACHE_SPILL, false);
    const int hits = db_status(db.get(), SQLITE_DBSTATUS_CACHE_HIT, false);
    const int misses = db_status(db.get(), SQLITE_DBSTATUS_CACHE_MISS, false);
    sqlite3_wal_hook(db.get(), nullptr, nullptr);
    const auto wal_bytes = file_bytes(config.path + "-wal");
    const auto page_size = scalar(db.get(), "PRAGMA page_size");
    const auto final_posts = scalar(db.get(), "SELECT count(*) FROM posts");
    const auto final_likes = scalar(db.get(), "SELECT count(*) FROM likes");
    require(final_posts - seed.posts == static_cast<Id>(run.sink.outcomes[0]),
            "post count does not match acknowledged creates");
    require(final_likes - seed.likes == static_cast<Id>(run.sink.outcomes[1]),
            "like count does not match acknowledged creates");
    if (config.workload == Workload::valid)
    {
        require(run.sink.outcomes[3] == 0 && run.sink.outcomes[4] == 0,
                "valid workload produced a missing post or constraint error");
    }
    require(scalar_text(db.get(), "PRAGMA integrity_check") == "ok", "integrity check failed");
    Statement foreign_keys(db.get(), "PRAGMA foreign_key_check");
    auto foreign_key_result = foreign_keys.use();
    require(!foreign_key_result.next(), "foreign-key check failed");
    const auto state = state_hash(db.get(), seed);
    const double jobs = static_cast<double>(config.jobs);

    std::cout << std::setprecision(12) << "{\"sqlite\":\"" << sqlite3_libversion()
              << "\",\"mode\":\"" << (config.savepoints ? "savepoint_batch" : "per_request")
              << "\",\"batch\":" << config.batch << ",\"jobs\":" << config.jobs
              << ",\"workload\":\"" << (config.workload == Workload::valid ? "valid" : "mixed")
              << '"' << ",\"checkpoint_policy\":\"inline_passive_at_1000_plus_final\""
              << ",\"application_cache\":false,\"sqlite_cache_kib\":16384"
              << ",\"cpu_seconds\":" << cpu << ",\"wall_seconds\":" << wall
              << ",\"cpu_us_per_job\":" << cpu * 1e6 / jobs
              << ",\"wall_us_per_job\":" << wall * 1e6 / jobs << ",\"commits\":" << run.commits
              << ",\"full_rollbacks\":" << run.rollbacks
              << ",\"checkpoints\":" << run.wal.checkpoints << ",\"cache_writes\":" << writes
              << ",\"cache_spills\":" << spills << ",\"cache_hits\":" << hits
              << ",\"cache_misses\":" << misses
              << ",\"wal_appended_frames\":" << run.wal.appended_frames
              << ",\"wal_logical_append_bytes\":"
              << run.wal.appended_frames * static_cast<std::uint64_t>(page_size + 24)
              << ",\"wal_file_bytes_logical_capacity\":" << wal_bytes
              << ",\"wal_max_frames\":" << run.wal.max_frames << ",\"page_size\":" << page_size
              << ",\"acknowledged\":" << run.sink.acknowledged
              << ",\"successful\":" << run.sink.successful
              << ",\"created_posts\":" << run.sink.outcomes[0]
              << ",\"created_likes\":" << run.sink.outcomes[1]
              << ",\"duplicate_likes\":" << run.sink.outcomes[2]
              << ",\"missing_posts\":" << run.sink.outcomes[3]
              << ",\"constraint_errors\":" << run.sink.outcomes[4]
              << ",\"final_posts\":" << final_posts << ",\"final_likes\":" << final_likes
              << ",\"hash_algorithm\":\"fnv1a64_length_prefixed_little_endian\""
              << ",\"input_hash\":" << run.sink.input.value()
              << ",\"semantic_output_hash_timestamp_normalized\":" << run.sink.output.value()
              << ",\"state_hash_timestamp_normalized\":" << state
              << ",\"checks\":\"counts_integrity_foreign_keys_passed\"}\n";
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        require(sqlite3_libversion_number() == SQLITE_VERSION_NUMBER,
                "SQLite library/header mismatch");
        const auto config = options(argc, argv);
        if (!config.schema.empty())
        {
            self_test(config.schema);
        }
        else
        {
            benchmark(config);
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "group-commit probe failed: " << error.what() << '\n';
        return 1;
    }
}
