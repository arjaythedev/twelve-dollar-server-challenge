#include "database.hpp"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
using Database = tds::Database;
using Write = Database::Write;

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

using Connection = std::unique_ptr<sqlite3, Close>;

Connection open(const std::string &path)
{
    sqlite3 *raw = nullptr;
    const int result =
        sqlite3_open_v2(path.c_str(), &raw,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    Connection db(raw);
    require(result == SQLITE_OK, "fixture open failed");
    return db;
}

void execute(sqlite3 *db, const char *sql)
{
    require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL failed");
}

struct Finalize
{
    void operator()(sqlite3_stmt *query) const noexcept
    {
        sqlite3_finalize(query);
    }
};

std::unique_ptr<sqlite3_stmt, Finalize> row(sqlite3 *db, const char *sql,
                                            std::string_view parameter = {})
{
    sqlite3_stmt *raw = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    std::unique_ptr<sqlite3_stmt, Finalize> query(raw);
    require(result == SQLITE_OK, "query prepare failed");
    if (sqlite3_bind_parameter_count(raw) != 0)
    {
        require(sqlite3_bind_text64(raw, 1, parameter.data(), parameter.size(), SQLITE_TRANSIENT,
                                    SQLITE_UTF8) == SQLITE_OK,
                "query bind failed");
    }
    require(sqlite3_step(raw) == SQLITE_ROW, "query row missing");
    return query;
}

std::int64_t scalar(sqlite3 *db, const char *sql, std::string_view parameter = {})
{
    const auto query = row(db, sql, parameter);
    return sqlite3_column_int64(query.get(), 0);
}

std::string text(sqlite3 *db, const char *sql, std::string_view parameter = {})
{
    const auto query = row(db, sql, parameter);
    const auto *data = sqlite3_column_text(query.get(), 0);
    require(data != nullptr, "query text missing");
    return {reinterpret_cast<const char *>(data),
            static_cast<std::size_t>(sqlite3_column_bytes(query.get(), 0))};
}

class Fixture
{
  public:
    Fixture()
    {
        char directory[] = "/tmp/td-group-commit-XXXXXX";
        const char *created = ::mkdtemp(directory);
        require(created != nullptr, "fixture directory failed");
        directory_ = created;
        path = directory_ + "/feed.db";
        auto db = open(path);
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
            INSERT INTO users(id,username) VALUES(1,'first'),(2,'second');
            INSERT INTO posts(user_id,body) VALUES(1,'seed');
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

enum class CommitFailure
{
    none,
    hook,
    busy
};

struct Control;
thread_local Control *active = nullptr;

struct Control
{
    explicit Control(sqlite3 *reader_connection) : reader(reader_connection)
    {
        require(active == nullptr, "overlapping group-commit controls");
        active = this;
    }

    ~Control()
    {
        active = nullptr;
    }

    sqlite3 *reader;
    CommitFailure commit_failure = CommitFailure::none;
    int fail_savepoint = 0;
    int begins = 0;
    int commits = 0;
    int savepoints = 0;
    int rollbacks_to = 0;
    int releases = 0;
    std::int64_t visible_posts = -1;
    std::int64_t visible_likes = -1;
};

const std::string &created(const Database::WriteResult &result)
{
    require(result.has_value(), "create result failed");
    const auto *value = std::get_if<std::string>(&*result);
    require(value != nullptr, "create result has wrong type");
    return *value;
}

std::optional<bool> liked(const Database::WriteResult &result)
{
    require(result.has_value(), "like result failed");
    const auto *value = std::get_if<std::optional<bool>>(&*result);
    require(value != nullptr, "like result has wrong type");
    return *value;
}

void all_failed(const std::vector<Database::WriteResult> &results, std::size_t count)
{
    require(results.size() == count, "failed batch lost results");
    for (const auto &result : results)
    {
        require(!result && result.error(), "failed transaction published a provisional success");
        bool rethrown = false;
        try
        {
            std::rethrow_exception(result.error());
        }
        catch (const std::runtime_error &)
        {
            rethrown = true;
        }
        require(rethrown, "failed result did not retain its exception");
    }
}

void verify_created(sqlite3 *reader, const std::string &json, std::int64_t id,
                    std::string_view body, std::string_view author)
{
    require(scalar(reader, "SELECT json_valid(?)", json) == 1, "create JSON is invalid");
    require(scalar(reader, "SELECT json_extract(?,'$.post.id')", json) == id,
            "create JSON has wrong post ID");
    require(text(reader, "SELECT json_extract(?,'$.post.body')", json) == body,
            "create JSON changed body escaping");
    require(text(reader, "SELECT json_extract(?,'$.post.author')", json) == author,
            "create JSON changed author");
    require(scalar(reader, "SELECT json_extract(?,'$.post.like_count')", json) == 0,
            "later likes changed provisional create JSON");
    const auto timestamp = text(reader, "SELECT json_extract(?,'$.post.created_at')", json);
    require(timestamp.size() == 24 && timestamp[4] == '-' && timestamp[7] == '-' &&
                timestamp[10] == 'T' && timestamp[13] == ':' && timestamp[16] == ':' &&
                timestamp[19] == '.' && timestamp[23] == 'Z',
            "create timestamp format changed");
    require(scalar(reader,
                   "SELECT count(*) FROM posts WHERE id=json_extract(?,'$.post.id') "
                   "AND body=json_extract(?1,'$.post.body') "
                   "AND created_at=json_extract(?1,'$.post.created_at')",
                   json) == 1,
            "returned create differs from committed post");
}

void fifo_isolation_and_visibility()
{
    Fixture fixture;
    Database db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    const std::string body = "quote \" slash \\ line\n\t\r\b\f\x01 caf\xc3\xa9";
    const std::array<Write, 9> jobs{
        Database::Create{1, "first", body},
        Database::Like{2, 2},
        Database::Like{2, 2},
        Database::Like{1, 999},
        Database::Create{999, "missing", "bad foreign key"},
        Database::Like{999, 1},
        Database::Create{1, "first", ""},
        Database::Create{2, "second", "after failed neighbors"},
        Database::Like{1, 3},
    };
    const auto results = db.write_batch(jobs);
    require(results.size() == jobs.size(), "batch lost results");
    require(control.commits == 1 && control.savepoints == 9 && control.rollbacks_to == 3 &&
                control.releases == 9,
            "batch did not isolate its writes in one transaction");
    require(control.visible_posts == 1 && control.visible_likes == 0,
            "external reader saw uncommitted batch writes");
    verify_created(reader.get(), created(results[0]), 2, body, "first");
    require(liked(results[1]) == true && liked(results[2]) == false &&
                !liked(results[3]).has_value() && liked(results[8]) == true,
            "FIFO create/like, duplicate or missing-post behavior changed");
    for (const auto index : {4U, 5U, 6U})
    {
        require(!results[index] && results[index].error(), "invalid write unexpectedly succeeded");
    }
    verify_created(reader.get(), created(results[7]), 3, "after failed neighbors", "second");
    require(scalar(reader.get(), "SELECT count(*) FROM posts") == 3 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 2,
            "failed writes changed stored counts");
    const auto post = db.post(2);
    require(post && scalar(reader.get(), "SELECT json_extract(?,'$.post.like_count')", *post) == 1,
            "committed like is not visible to post reads");
    require(db.feed().find("after failed neighbors") != std::string::npos,
            "committed create is not visible to feed reads");
}

void failed_commits()
{
    for (const auto failure : {CommitFailure::hook, CommitFailure::busy})
    {
        for (const bool singleton : {false, true})
        {
            Fixture fixture;
            Database db(fixture.path);
            auto reader = open(fixture.path);
            Control control(reader.get());
            control.commit_failure = failure;
            const std::array<Write, 3> jobs{
                Database::Create{1, "first", "must roll back"},
                Database::Like{2, 1},
                Database::Like{1, 999},
            };
            const auto writes = std::span<const Write>(jobs).first(singleton ? 1U : jobs.size());
            all_failed(db.write_batch(writes), writes.size());
            require(control.commits == 1 && control.visible_posts == 1 &&
                        control.visible_likes == 0,
                    "failed commit exposed uncommitted writes");
            require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                        scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
                    "failed commit retained batch writes");
            control.commit_failure = CommitFailure::none;
            const std::array<Write, 1> recovery{
                Database::Create{1, "first", "recovered after failed commit"}};
            require(!created(db.write_batch(recovery).at(0)).empty(),
                    "failed commit left writer unusable");
        }
    }
}

void automatic_rollback()
{
    Fixture fixture;
    auto reader = open(fixture.path);
    execute(reader.get(), R"sql(
        CREATE TRIGGER abort_batch BEFORE INSERT ON posts WHEN NEW.body='automatic rollback'
        BEGIN SELECT RAISE(ROLLBACK,'automatic rollback'); END;
    )sql");
    Database db(fixture.path);
    Control control(reader.get());
    const std::array<Write, 3> jobs{
        Database::Create{1, "first", "provisional"},
        Database::Create{1, "first", "automatic rollback"},
        Database::Create{1, "first", "must not run"},
    };
    all_failed(db.write_batch(jobs), jobs.size());
    require(control.commits == 0 && scalar(reader.get(), "SELECT count(*) FROM posts") == 1,
            "automatic rollback retained a batch write or continued the batch");
    const std::array<Write, 1> recovery{
        Database::Create{1, "first", "recovered after automatic rollback"}};
    require(!created(db.write_batch(recovery).at(0)).empty(),
            "automatic rollback left writer unusable");
}

void fatal_error()
{
    Fixture fixture;
    Database db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    control.fail_savepoint = 2;
    const std::array<Write, 3> jobs{
        Database::Create{1, "first", "provisional"},
        Database::Like{2, 1},
        Database::Create{1, "first", "must not run"},
    };
    all_failed(db.write_batch(jobs), jobs.size());
    require(control.commits == 0 && scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
            "fatal error retained provisional writes");
    const std::array<Write, 1> recovery{Database::Like{2, 1}};
    require(liked(db.write_batch(recovery).at(0)) == true, "fatal error left writer unusable");
}

void singleton_and_empty()
{
    Fixture fixture;
    Database db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    const std::array<Write, 0> empty{};
    require(db.write_batch(empty).empty() && control.begins == 0,
            "empty batch opened a transaction");
    const std::array<Write, 1> invalid{Database::Create{999, "missing", "bad foreign key"}};
    all_failed(db.write_batch(invalid), 1);
    const std::array<Write, 1> create{Database::Create{1, "first", "singleton"}};
    const auto created_results = db.write_batch(create);
    require(created_results.size() == 1, "singleton create lost its result");
    verify_created(reader.get(), created(created_results[0]), 2, "singleton", "first");
    const std::array<Write, 1> like{Database::Like{2, 2}};
    require(liked(db.write_batch(like).at(0)) == true && liked(db.write_batch(like).at(0)) == false,
            "singleton duplicate-like behavior changed");
    const std::array<Write, 1> missing{Database::Like{2, 999}};
    require(!liked(db.write_batch(missing).at(0)).has_value(), "singleton missing post changed");
    require(control.savepoints == 0, "singleton batches added savepoint overhead");
}
} // namespace

extern "C" int __real_sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **),
                                   void *, char **);

extern "C" int __wrap_sqlite3_exec(sqlite3 *db, const char *sql,
                                   int (*callback)(void *, int, char **, char **), void *context,
                                   char **message)
{
    Control *control = active;
    if (control)
    {
        const std::string_view command(sql);
        if (command == "BEGIN")
        {
            ++control->begins;
        }
        else if (command == "SAVEPOINT write_job")
        {
            ++control->savepoints;
            if (control->savepoints == control->fail_savepoint)
            {
                return SQLITE_IOERR;
            }
        }
        else if (command == "ROLLBACK TO write_job")
        {
            ++control->rollbacks_to;
        }
        else if (command == "RELEASE write_job")
        {
            ++control->releases;
        }
        else if (command == "COMMIT")
        {
            ++control->commits;
            control->visible_posts = scalar(control->reader, "SELECT count(*) FROM posts");
            control->visible_likes = scalar(control->reader, "SELECT count(*) FROM likes");
            if (control->commit_failure == CommitFailure::busy)
            {
                return SQLITE_BUSY;
            }
            if (control->commit_failure == CommitFailure::hook)
            {
                sqlite3_commit_hook(
                    db,
                    [](void *) -> int {
                        return 1;
                    },
                    nullptr);
                const int result = __real_sqlite3_exec(db, sql, callback, context, message);
                sqlite3_commit_hook(db, nullptr, nullptr);
                return result;
            }
        }
    }
    return __real_sqlite3_exec(db, sql, callback, context, message);
}

int main()
{
    try
    {
        fifo_isolation_and_visibility();
        failed_commits();
        automatic_rollback();
        fatal_error();
        singleton_and_empty();
        std::puts("Group-commit FIFO, isolation, visibility, failure and singleton checks passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
