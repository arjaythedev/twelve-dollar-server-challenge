#include "database.hpp"
#include "database_timing.hpp"
#include "json.hpp"
#include "sqlite_read.h"
#include "timing.hpp"

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

namespace tds
{
namespace
{

#ifdef TDS_DIAGNOSTICS
using database_timing::DatabaseTimings;
using database_timing::install_sync_hooks;
using database_timing::SyncScope;
#endif

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

using Connection = std::unique_ptr<sqlite3, Close>;

struct SqlError : std::runtime_error
{
    SqlError(sqlite3 *db, int result) : std::runtime_error(sqlite3_errmsg(db)), code(result & 0xff)
    {
    }

    int code;
};

void execute(sqlite3 *db, const char *sql)
{
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK)
    {
        throw SqlError(db, result);
    }
}

Connection open(const std::string &path)
{
#ifdef TDS_DIAGNOSTICS
    install_sync_hooks();
#endif
    sqlite3 *raw = nullptr;
    const int result =
        sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
    Connection db(raw);
    if (result != SQLITE_OK)
    {
        throw std::runtime_error(raw ? sqlite3_errmsg(raw) : "database open failed");
    }
    execute(raw, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"
                 "PRAGMA cache_size=-16384;");
    return db;
}

class Checkpoints
{
  public:
    explicit Checkpoints(const std::string &path)
        : db_(open(path)), worker_([this](std::stop_token stop) {
              run(stop);
          })
    {
    }

    ~Checkpoints()
    {
        worker_.request_stop();
        worker_.join();
    }

    static int committed(void *context, sqlite3 *, const char *, int frames) noexcept
    {
        auto &owner = *static_cast<Checkpoints *>(context);
        if (frames >= frame_threshold)
        {
            bool notify = false;
            {
                std::lock_guard lock(owner.mutex_);
                notify = !owner.pending_;
                owner.pending_ = true;
            }
            if (notify)
            {
                owner.ready_.notify_one();
            }
        }
        return SQLITE_OK;
    }

  private:
    void run(std::stop_token stop)
    {
#ifdef TDS_DIAGNOSTICS
        DatabaseTimings timings("checkpoint");
#endif
        bool reported_busy = false;
        bool reported_error = false;
        while (!stop.stop_requested())
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait(lock, stop, [this] {
                    return pending_;
                }))
            {
                return;
            }
            pending_ = false;
            lock.unlock();

            int frames = -1;
            int checkpointed = -1;
            int result;
            {
#ifdef TDS_DIAGNOSTICS
                timing::Scope measure(timings.checkpoint);
                SyncScope sync(timings, "checkpoint");
#endif
                result = sqlite3_wal_checkpoint_v2(db_.get(), "main", SQLITE_CHECKPOINT_PASSIVE,
                                                   &frames, &checkpointed);
            }
#ifdef TDS_DIAGNOSTICS
            if (frames >= 0 && checkpointed >= 0 && checkpointed <= frames)
            {
                timings.backlog.record(static_cast<std::uint64_t>(frames - checkpointed));
            }
            timings.report();
#endif
            const bool busy = result == SQLITE_BUSY || result == SQLITE_LOCKED;
            if (busy && !reported_busy)
            {
                std::fprintf(stderr, "WAL checkpoint busy, retrying on a later write\n");
                reported_busy = true;
            }
            else if (result != SQLITE_OK && !busy && !reported_error)
            {
                std::fprintf(stderr, "WAL checkpoint failed (%d): %s, retrying on a later write\n",
                             result, sqlite3_errstr(result));
                reported_error = true;
            }
            if (result != SQLITE_OK || checkpointed < frames)
            {
                lock.lock();
                ready_.wait_for(lock, stop, retry_delay, [] {
                    return false;
                });
            }
        }
    }

    static constexpr int frame_threshold = 1000;
    static constexpr auto retry_delay = std::chrono::milliseconds(10);

    Connection db_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    bool pending_ = false;
    // The worker joins before its connection and wakeup state are destroyed
    std::jthread worker_;
};

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

struct CloseTree
{
    void operator()(tds_btree *tree) const noexcept
    {
        tds_btree_destroy(tree);
    }
};

using Tree = std::unique_ptr<tds_btree, CloseTree>;

Tree open_tree(sqlite3 *db)
{
    tds_btree *tree = nullptr;
    tds_btree_create(db, &tree);

    return Tree(tree);
}

class Statement
{
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db)
    {
        const int result =
            sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement_, nullptr);
        if (result != SQLITE_OK)
        {
            throw SqlError(db, result);
        }
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    class Use
    {
      public:
        explicit Use(Statement &statement) : owner_(statement)
        {
        }

        ~Use()
        {
            sqlite3_reset(owner_.statement_);
            sqlite3_clear_bindings(owner_.statement_);
        }

        Use(const Use &) = delete;
        Use &operator=(const Use &) = delete;

        void bind(int column, std::int64_t value)
        {
            check(sqlite3_bind_int64(owner_.statement_, column, value));
        }

        void bind(int column, std::string_view value)
        {
            check(sqlite3_bind_text64(owner_.statement_, column, value.data(), value.size(),
                                      SQLITE_TRANSIENT, SQLITE_UTF8));
        }

        bool next()
        {
            const int result = sqlite3_step(owner_.statement_);
            if (result == SQLITE_ROW)
            {
                return true;
            }
            if (result == SQLITE_DONE)
            {
                return false;
            }
            throw SqlError(owner_.db_, result);
        }

        std::int64_t number(int column) const
        {
            return sqlite3_column_int64(owner_.statement_, column);
        }

        std::string_view text(int column) const
        {
            const auto *data = sqlite3_column_text(owner_.statement_, column);
            if (!data)
            {
                throw std::runtime_error("database returned missing text");
            }
            return {reinterpret_cast<const char *>(data),
                    static_cast<std::size_t>(sqlite3_column_bytes(owner_.statement_, column))};
        }

      private:
        void check(int result) const
        {
            if (result != SQLITE_OK)
            {
                throw SqlError(owner_.db_, result);
            }
        }

        Statement &owner_;
    };

    Use use()
    {
        return Use(*this);
    }

  private:
    sqlite3 *db_;
    sqlite3_stmt *statement_ = nullptr;
};

class ReadScope
{
  public:
    ReadScope(sqlite3 *db, Statement &begin, Statement &commit, tds_btree &tree)
        : db_(db), commit_(commit), tree_(tree)
    {
        {
            auto query = begin.use();
            while (query.next())
            {
            }
        }
        status_ = tds_btree_begin(&tree_);
    }

    ~ReadScope()
    {
        if (!committed_)
        {
            tds_btree_end(&tree_);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    ReadScope(const ReadScope &) = delete;
    ReadScope &operator=(const ReadScope &) = delete;

    bool ready() const noexcept
    {
        return status_ == SQLITE_OK;
    }

    void commit()
    {
        tds_btree_end(&tree_);
        auto query = commit_.use();
        while (query.next())
        {
        }
        committed_ = true;
    }

  private:
    sqlite3 *db_;
    Statement &commit_;
    tds_btree &tree_;
    int status_ = SQLITE_ERROR;
    bool committed_ = false;
};

void post_object(std::string &output, std::int64_t id, std::string_view body,
                 std::string_view timestamp, std::string_view author, std::int64_t likes)
{
    output += "{\"id\":";
    integer(output, id);
    output += ",\"body\":";
    quote(output, body);
    output += ",\"created_at\":";
    quote(output, timestamp);
    output += ",\"author\":";
    quote(output, author);
    output += ",\"like_count\":";
    integer(output, likes);
    output += '}';
}

void post_object(std::string &output, const Statement::Use &row)
{
    // Column views are consumed before stepping or resetting the statement
    post_object(output, row.number(0), row.text(1), row.text(2), row.text(3), row.number(4));
}

void post_object(std::string &output, const tds_btree_post &row)
{
    post_object(output, row.id, {row.body, static_cast<std::size_t>(row.body_bytes)},
                {row.created_at, static_cast<std::size_t>(row.created_at_bytes)},
                {row.author, static_cast<std::size_t>(row.author_bytes)}, row.like_count);
}

} // namespace

class Database::State
{
  public:
    explicit State(const std::string &path)
        : db(open(path)), health(db.get(), "SELECT 1"), feed(db.get(), R"sql(
              SELECT p.id, p.body, p.created_at, u.username,
                     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
              FROM posts p JOIN users u ON u.id = p.user_id
              ORDER BY p.created_at DESC, p.id DESC
              LIMIT 20
          )sql"),
          post(db.get(), R"sql(
              SELECT p.id, p.body, p.created_at, u.username,
                     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
              FROM posts p JOIN users u ON u.id = p.user_id
              WHERE p.id = ?
          )sql"),
          create(db.get(), "INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at"),
          exists(db.get(), "SELECT 1 FROM posts WHERE id=?"), like(db.get(), R"sql(
              INSERT INTO likes(user_id, post_id) VALUES(?, ?)
              ON CONFLICT(user_id, post_id) DO NOTHING
          )sql"),
          read_begin(db.get(), "BEGIN"), read_commit(db.get(), "COMMIT"), tree(open_tree(db.get())),
          checkpoints(path)
    {
        sqlite3_wal_autocheckpoint(db.get(), 0);
        sqlite3_wal_hook(db.get(), Checkpoints::committed, &checkpoints);
    }

    ~State()
    {
        sqlite3_wal_hook(db.get(), nullptr, nullptr);
    }

    void commit(Transaction &transaction)
    {
#ifdef TDS_DIAGNOSTICS
        timing::Scope measure(timings.commit);
        SyncScope sync(timings, "issuer");
#endif
        transaction.commit();
    }

    std::string create_post(std::int64_t user_id, std::string_view username, std::string_view body)
    {
        std::string output = "{\"post\":";
        auto query = create.use();
        query.bind(1, user_id);
        query.bind(2, body);
        if (!query.next())
        {
            throw std::runtime_error("insert returned no row");
        }

        post_object(output, query.number(0), body, query.text(1), username, 0);
        output += '}';
        while (query.next())
        {
        }
        return output;
    }

    std::optional<bool> like_post(std::int64_t user_id, std::int64_t post_id)
    {
        bool found = false;
        {
            auto query = exists.use();
            query.bind(1, post_id);
            found = query.next();
            while (found && query.next())
            {
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        {
            auto query = like.use();
            query.bind(1, user_id);
            query.bind(2, post_id);
            while (query.next())
            {
            }
        }
        return sqlite3_changes(db.get()) == 1;
    }

    WriteValue apply(const WriteCommand &write)
    {
        if (const auto *job = std::get_if<CreatePost>(&write))
        {
#ifdef TDS_DIAGNOSTICS
            timing::Scope measure(timings.create);
#endif
            return create_post(job->user_id, job->username, job->body);
        }
        const auto &job = std::get<LikePost>(write);
#ifdef TDS_DIAGNOSTICS
        timing::Scope measure(timings.like);
#endif
        return like_post(job.user_id, job.post_id);
    }

    Connection db;
    Statement health;
    Statement feed;
    Statement post;
    Statement create;
    Statement exists;
    Statement like;
    Statement read_begin;
    Statement read_commit;
    Tree tree;
#ifdef TDS_DIAGNOSTICS
    DatabaseTimings timings{"database"};
#endif
    Checkpoints checkpoints;
};

Database::Database(const std::string &path) : state_(std::make_unique<State>(path))
{
}

Database::~Database() = default;

void Database::health()
{
    auto query = state_->health.use();
    while (query.next())
    {
    }
}

std::string Database::feed()
{
#ifdef TDS_DIAGNOSTICS
    state_->timings.report();
    timing::Scope measure(state_->timings.feed);
#endif
    std::string output = "{\"posts\":[";
    output.reserve(8192);
    if (state_->tree)
    {
        ReadScope read(state_->db.get(), state_->read_begin, state_->read_commit, *state_->tree);
        if (read.ready())
        {
            tds_btree_post row{};
            int result = SQLITE_OK;
            bool first = true;
            while ((result = tds_btree_feed_next(state_->tree.get(), &row)) == SQLITE_ROW)
            {
                if (!first)
                {
                    output += ',';
                }
                first = false;
                post_object(output, row);
            }
            if (result == SQLITE_DONE)
            {
                output += "]}";
                read.commit();

                return output;
            }
        }
        output = "{\"posts\":[";
    }
    auto query = state_->feed.use();
    bool first = true;
    while (query.next())
    {
        if (!first)
        {
            output += ',';
        }
        first = false;
        post_object(output, query);
    }
    output += "]}";
    return output;
}

std::optional<std::string> Database::post(std::int64_t id)
{
#ifdef TDS_DIAGNOSTICS
    state_->timings.report();
    timing::Scope measure(state_->timings.post);
#endif
    auto query = state_->post.use();
    query.bind(1, id);
    if (!query.next())
    {
        return std::nullopt;
    }
    std::string output = "{\"post\":";
    post_object(output, query);
    output += '}';
    while (query.next())
    {
    }
    return output;
}

std::vector<WriteResult> Database::write_batch(std::span<const WriteCommand> writes)
{
    std::vector<WriteResult> results;
    results.reserve(writes.size());
    if (writes.empty())
    {
        return results;
    }
#ifdef TDS_DIAGNOSTICS
    state_->timings.report();
#endif
    try
    {
        Transaction transaction(state_->db.get());
        const bool savepoints = writes.size() > 1;
        for (const auto &write : writes)
        {
            if (savepoints)
            {
                execute(state_->db.get(), "SAVEPOINT write_job");
            }
            try
            {
                results.emplace_back(state_->apply(write));
            }
            catch (const SqlError &error)
            {
                if (!savepoints || error.code != SQLITE_CONSTRAINT ||
                    sqlite3_get_autocommit(state_->db.get()) != 0)
                {
                    throw;
                }
                execute(state_->db.get(), "ROLLBACK TO write_job");
                results.emplace_back(std::unexpected(std::current_exception()));
            }
            if (savepoints)
            {
                execute(state_->db.get(), "RELEASE write_job");
            }
        }
        state_->commit(transaction);
    }
    catch (...)
    {
        // A failed transaction invalidates every provisional result
        results.assign(writes.size(), std::unexpected(std::current_exception()));
    }
    return results;
}

} // namespace tds
