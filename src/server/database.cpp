#include "database.hpp"
#include "json.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace tds
{
namespace
{

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

using Connection = std::unique_ptr<sqlite3, Close>;

void execute(sqlite3 *db, const char *sql)
{
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

Connection open(const std::string &path)
{
    sqlite3 *raw = nullptr;
    const int result =
        sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
    Connection db(raw);
    if (result != SQLITE_OK)
    {
        throw std::runtime_error(raw ? sqlite3_errmsg(raw) : "database open failed");
    }
    execute(raw, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
    return db;
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

class Statement
{
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db)
    {
        if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement_, nullptr) !=
            SQLITE_OK)
        {
            throw std::runtime_error(sqlite3_errmsg(db));
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
            throw std::runtime_error(sqlite3_errmsg(owner_.db_));
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
                throw std::runtime_error(sqlite3_errmsg(owner_.db_));
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
    // Column views are consumed before stepping or resetting the statement.
    post_object(output, row.number(0), row.text(1), row.text(2), row.text(3), row.number(4));
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
          )sql")
    {
    }

    Connection db;
    Statement health;
    Statement feed;
    Statement post;
    Statement create;
    Statement exists;
    Statement like;
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
    std::string output = "{\"posts\":[";
    output.reserve(8192);
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

std::string Database::create(std::int64_t user_id, std::string_view username, std::string_view body)
{
    Transaction transaction(state_->db.get());
    std::string output = "{\"post\":";
    {
        auto query = state_->create.use();
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
    }
    transaction.commit();

    return output;
}

std::optional<bool> Database::like(std::int64_t user_id, std::int64_t post_id)
{
    Transaction transaction(state_->db.get());
    bool found = false;
    {
        auto query = state_->exists.use();
        query.bind(1, post_id);
        found = query.next();
        while (query.next())
        {
        }
    }
    if (!found)
    {
        transaction.commit();

        return std::nullopt;
    }

    {
        auto query = state_->like.use();
        query.bind(1, user_id);
        query.bind(2, post_id);
        while (query.next())
        {
        }
    }
    const bool created = sqlite3_changes(state_->db.get()) == 1;
    transaction.commit();

    return created;
}

} // namespace tds
