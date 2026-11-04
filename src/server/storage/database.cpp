#include "database.hpp"
#include "../app/post_writer.hpp"
#include "../diagnostics/database.hpp"
#include "../sqlite_read.h"

#include <sqlite3.h>

#include <algorithm>
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

using telemetry::DatabaseOperation;
using telemetry::DatabaseTelemetry;

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
    // Statement reset during unwinding can replace SQLite's extended error code
    SqlError(sqlite3 *db, int result)
        : std::runtime_error(sqlite3_errmsg(db)), code(result & 0xff),
          extended_code(sqlite3_extended_errcode(db))
    {
    }

    int code;
    int extended_code;
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
    sqlite3 *raw = nullptr;
    // Both handles coordinate WAL locks in-process; external tools must wait until shutdown
    // locking_mode=EXCLUSIVE would also obstruct our checkpoint connection
    const int result = sqlite3_open_v2(path.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl");
    Connection db(raw);
    if (result != SQLITE_OK)
    {
        throw std::runtime_error(raw ? sqlite3_errmsg(raw) : "database open failed");
    }
    // Evicted WAL pages are cold; shrink on reuse so later writes append without rereads
    execute(raw, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"
                 "PRAGMA cache_size=-16384; PRAGMA journal_size_limit=0;");
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
        DatabaseTelemetry diagnostics("checkpoint");
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
                [[maybe_unused]] const auto sample =
                    diagnostics.measure(DatabaseOperation::checkpoint);
                result = sqlite3_wal_checkpoint_v2(db_.get(), "main", SQLITE_CHECKPOINT_PASSIVE,
                                                   &frames, &checkpointed);
                if (result == SQLITE_OK)
                {
                    tds_wal_drop_checkpointed(db_.get(), checkpointed,
                                              diagnostics.remaining_advice_events());
                }
            }
#if TDS_TELEMETRY_LEVEL >= 1
            tds_wal_advice_snapshot advice;
            tds_wal_advice_take(&advice);
            diagnostics.wal_advice_completed(advice);
#endif
            diagnostics.checkpoint_completed(result, frames, checkpointed);
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

    void execute()
    {
        auto query = use();
        while (query.next())
        {
        }
    }

  private:
    sqlite3 *db_;
    sqlite3_stmt *statement_ = nullptr;
};

class Transactions
{
  public:
    explicit Transactions(sqlite3 *db)
        : db_(db), begin_(db, "BEGIN"), commit_(db, "COMMIT"), rollback_(db, "ROLLBACK")
    {
    }

    void require_ready() const
    {
        if (!usable_)
        {
            throw std::runtime_error("database transaction recovery failed");
        }
    }

    void begin()
    {
        require_ready();
        begin_.execute();
    }

    void commit()
    {
        commit_.execute();
    }

    void rollback() noexcept
    {
        if (sqlite3_get_autocommit(db_))
        {
            return;
        }
        // Cursed: error cleanup must work even when SQLite cannot prepare another statement
        // Keep the rollback ready and quarantine the connection if its transaction survives
        try
        {
            rollback_.execute();
        }
        catch (...)
        {
        }
        usable_ = usable_ && sqlite3_get_autocommit(db_) != 0;
    }

  private:
    sqlite3 *db_;
    Statement begin_;
    Statement commit_;
    Statement rollback_;
    bool usable_ = true;
};

class Transaction
{
  public:
    Transaction(Transactions &transactions, tds_btree *tree)
        : transactions_(transactions), tree_(tree)
    {
        transactions_.begin();
    }

    ~Transaction()
    {
        tds_btree_end(tree_);
        if (!committed_)
        {
            transactions_.rollback();
        }
    }

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit()
    {
        // Cursed but required: SQLite must retire direct cursors before resolving the transaction
        tds_btree_end(tree_);
        transactions_.commit();
        committed_ = true;
    }

  private:
    Transactions &transactions_;
    tds_btree *tree_;
    bool committed_ = false;
};

class ReadScope
{
  public:
    ReadScope(Transactions &transactions, tds_btree &tree)
        : transactions_(transactions), tree_(tree)
    {
        transactions_.begin();
        status_ = tds_btree_begin(&tree_);
    }

    ~ReadScope()
    {
        if (!committed_)
        {
            tds_btree_end(&tree_);
            transactions_.rollback();
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
        transactions_.commit();
        committed_ = true;
    }

  private:
    Transactions &transactions_;
    tds_btree &tree_;
    int status_ = SQLITE_ERROR;
    bool committed_ = false;
};

void write_post(PostWriter &writer, const Statement::Use &row)
{
    // Column views are consumed before stepping or resetting the statement
    writer.append(PostId{row.number(0)}, row.text(1), row.text(2), row.text(3), row.number(4));
}

void write_post(PostWriter &writer, const tds_btree_post &row)
{
    writer.append(PostId{row.id}, {row.body, static_cast<std::size_t>(row.body_bytes)},
                  {row.created_at, static_cast<std::size_t>(row.created_at_bytes)},
                  {row.author, static_cast<std::size_t>(row.author_bytes)}, row.like_count);
}

} // namespace

class Store::State
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
          create(db.get(), "INSERT INTO posts(user_id,body) VALUES(?,?)"),
          created_at(db.get(), "SELECT created_at FROM posts WHERE id=?"),
          exists(db.get(), "SELECT 1 FROM posts WHERE id=?"), like(db.get(), R"sql(
              INSERT INTO likes(user_id, post_id) VALUES(?, ?)
              ON CONFLICT(user_id, post_id) DO NOTHING
          )sql"),
          transactions(db.get()), write_savepoint(db.get(), "SAVEPOINT write_job"),
          write_release(db.get(), "RELEASE write_job"),
          write_rollback_to(db.get(), "ROLLBACK TO write_job"), tree(open_tree(db.get())),
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
        [[maybe_unused]] const auto sample = diagnostics.measure(DatabaseOperation::commit);
        transaction.commit();
    }

    CreatedPost create_post(UserId user_id, std::string_view body)
    {
        {
            auto query = create.use();
            query.bind(1, user_id.value);
            query.bind(2, body);
            while (query.next())
            {
            }
        }

        // Read SQLite's stored default, keeping timestamp generation inside the database
        CreatedPost result{PostId{sqlite3_last_insert_rowid(db.get())}, {}};
        auto query = created_at.use();
        query.bind(1, result.id.value);
        if (!query.next())
        {
            throw std::runtime_error("inserted post could not be read");
        }

        const auto timestamp = query.text(0);
        if (timestamp.size() != result.created_at.size())
        {
            throw std::runtime_error("insert returned an invalid timestamp");
        }
        std::ranges::copy(timestamp, result.created_at.begin());
        while (query.next())
        {
        }

        return result;
    }

    bool post_exists(PostId post_id)
    {
        if (tree)
        {
            const int result = tds_btree_post_exists(tree.get(), post_id.value);
            if (result == SQLITE_ROW || result == SQLITE_DONE)
            {
                return result == SQLITE_ROW;
            }
        }
        bool found = false;
        {
            auto query = exists.use();
            query.bind(1, post_id.value);
            found = query.next();
            while (found && query.next())
            {
            }
        }

        return found;
    }

    WriteOutcome like_post(UserId user_id, PostId post_id)
    {
        try
        {
            auto query = like.use();
            query.bind(1, user_id.value);
            query.bind(2, post_id.value);
            while (query.next())
            {
            }
        }
        catch (const SqlError &error)
        {
            // Missing post wins even when both foreign keys are invalid
            if (error.code != SQLITE_CONSTRAINT ||
                error.extended_code != SQLITE_CONSTRAINT_FOREIGNKEY ||
                sqlite3_get_autocommit(db.get()) != 0 || post_exists(post_id))
            {
                throw;
            }

            return std::unexpected(WriteError{WriteErrorCode::post_not_found, "post not found"});
        }

        if (sqlite3_changes(db.get()) == 1)
        {
            return WriteValue{LikeResult::created};
        }
        if (!post_exists(post_id))
        {
            return std::unexpected(WriteError{WriteErrorCode::post_not_found, "post not found"});
        }

        return WriteValue{LikeResult::already_liked};
    }

    WriteOutcome apply(const WriteCommand &write)
    {
        if (const auto *job = std::get_if<CreatePost>(&write))
        {
            [[maybe_unused]] const auto sample = diagnostics.measure(DatabaseOperation::create);
            return WriteValue{create_post(job->user_id, job->body)};
        }
        const auto &job = std::get<LikePost>(write);
        [[maybe_unused]] const auto sample = diagnostics.measure(DatabaseOperation::like);

        return like_post(job.user_id, job.post_id);
    }

    Connection db;
    Statement health;
    Statement feed;
    Statement post;
    Statement create;
    Statement created_at;
    Statement exists;
    Statement like;
    // One synchronous issuer reuses these controls after each read or write transaction finishes
    Transactions transactions;
    Statement write_savepoint;
    Statement write_release;
    Statement write_rollback_to;
    Tree tree;
    [[no_unique_address]] DatabaseTelemetry diagnostics{"database"};
    Checkpoints checkpoints;
};

Store::Store(const std::string &path) : state_(std::make_unique<State>(path))
{
}

Store::~Store() = default;

void Store::health()
{
    state_->transactions.require_ready();
    auto query = state_->health.use();
    while (query.next())
    {
    }
}

void Store::feed(PostWriter &writer)
{
    state_->diagnostics.report();
    [[maybe_unused]] const auto sample = state_->diagnostics.measure(DatabaseOperation::feed);
    if (state_->tree)
    {
        ReadScope read(state_->transactions, *state_->tree);
        if (read.ready())
        {
            tds_btree_post row{};
            int result = SQLITE_OK;
            while ((result = tds_btree_feed_next(state_->tree.get(), &row)) == SQLITE_ROW)
            {
                write_post(writer, row);
            }
            if (result == SQLITE_DONE)
            {
                writer.finish();
                read.commit();

                return;
            }
        }
        writer.reset();
    }
    state_->transactions.require_ready();
    auto query = state_->feed.use();
    while (query.next())
    {
        write_post(writer, query);
    }
    writer.finish();
}

bool Store::post(PostId id, PostWriter &writer)
{
    state_->transactions.require_ready();
    state_->diagnostics.report();
    [[maybe_unused]] const auto sample = state_->diagnostics.measure(DatabaseOperation::post);
    auto query = state_->post.use();
    query.bind(1, id.value);
    if (!query.next())
    {
        return false;
    }
    write_post(writer, query);
    writer.finish();
    while (query.next())
    {
    }

    return true;
}

std::expected<void, WriteError> Store::commit(std::span<const WriteCommand> commands,
                                              std::span<WriteOutcome> outcomes)
{
    if (commands.size() != outcomes.size() || commands.size() > write_batch_limit)
    {
        throw std::logic_error("invalid storage write batch");
    }
    if (commands.empty())
    {
        return {};
    }
    state_->diagnostics.report();
    state_->diagnostics.write_batch(static_cast<std::uint64_t>(commands.size()));
    try
    {
        Transaction transaction(state_->transactions, state_->tree.get());
        const bool savepoints = commands.size() > 1;
        for (std::size_t index = 0; index < commands.size(); ++index)
        {
            if (savepoints)
            {
                state_->write_savepoint.execute();
            }
            try
            {
                outcomes[index] = state_->apply(commands[index]);
            }
            catch (const SqlError &error)
            {
                if (!savepoints || error.code != SQLITE_CONSTRAINT ||
                    sqlite3_get_autocommit(state_->db.get()) != 0)
                {
                    throw;
                }
                tds_btree_end(state_->tree.get());
                state_->write_rollback_to.execute();
                outcomes[index] =
                    std::unexpected(WriteError{WriteErrorCode::constraint, error.what()});
            }
            if (savepoints)
            {
                state_->write_release.execute();
            }
        }
        state_->commit(transaction);
    }
    catch (const std::exception &error)
    {
        state_->diagnostics.write_failed();
        const WriteError failure{WriteErrorCode::transaction, error.what()};
        // Transaction unwinding rolls back before provisional outcomes are invalidated
        std::ranges::fill(outcomes, std::unexpected(failure));

        return std::unexpected(failure);
    }

    return {};
}

} // namespace tds
