#include "app/post_writer.hpp"
#include "storage/database.hpp"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
using tds::CreatedPost;
using tds::CreatePost;
using tds::LikePost;
using tds::LikeResult;
using tds::PostId;
using tds::PostShape;
using tds::PostWriter;
using tds::Store;
using tds::UserId;
using tds::WriteCommand;
using tds::WriteErrorCode;
using tds::WriteOutcome;

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

std::vector<WriteOutcome> run_batch(Store &store, std::span<const WriteCommand> commands)
{
    std::vector<WriteOutcome> outcomes(commands.size());
    const auto result = store.commit(commands, outcomes);
    if (!result)
    {
        for (const auto &outcome : outcomes)
        {
            require(!outcome && outcome.error().code == result.error().code,
                    "transaction failure left a different provisional outcome");
        }
    }

    return outcomes;
}

std::string read_feed(Store &store)
{
    std::string output;
    PostWriter writer(output, PostShape::feed);
    store.feed(writer);

    return output;
}

std::optional<std::string> read_post(Store &store, PostId id)
{
    std::string output;
    PostWriter writer(output, PostShape::single);
    if (!store.post(id, writer))
    {
        return std::nullopt;
    }

    return output;
}

const CreatedPost &created(const WriteOutcome &result)
{
    require(result.has_value(), "create result failed");
    const auto *value = std::get_if<CreatedPost>(&*result);
    require(value != nullptr, "create result has wrong type");

    return *value;
}

LikeResult liked(const WriteOutcome &result)
{
    require(result.has_value(), "like result failed");
    const auto *value = std::get_if<LikeResult>(&*result);
    require(value != nullptr, "like result has wrong type");

    return *value;
}

void all_failed(const std::vector<WriteOutcome> &results, std::size_t count)
{
    require(results.size() == count, "failed batch lost results");
    for (const auto &result : results)
    {
        require(!result && !result.error().message.empty(),
                "failed transaction published a provisional success");
        require(result.error().code == WriteErrorCode::transaction,
                "failed transaction did not retain its typed error");
    }
}

void verify_created(sqlite3 *reader, const CreatedPost &post, std::int64_t id,
                    std::string_view body, std::string_view author)
{
    std::string json;
    PostWriter writer(json, PostShape::single);
    writer.append(post.id, body, {post.created_at.data(), post.created_at.size()}, author, 0);
    writer.finish();
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
    Store db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    const std::string body = "quote \" slash \\ line\n\t\r\b\f\x01 caf\xc3\xa9";
    const std::array<WriteCommand, 9> jobs{
        CreatePost{UserId{1}, "first", body},
        LikePost{UserId{2}, PostId{2}},
        LikePost{UserId{2}, PostId{2}},
        LikePost{UserId{1}, PostId{999}},
        CreatePost{UserId{999}, "missing", "bad foreign key"},
        LikePost{UserId{999}, PostId{1}},
        CreatePost{UserId{1}, "first", ""},
        CreatePost{UserId{2}, "second", "after failed neighbors"},
        LikePost{UserId{1}, PostId{3}},
    };
    const auto results = run_batch(db, jobs);
    require(results.size() == jobs.size(), "batch lost results");
    require(control.commits == 1 && control.savepoints == 9 && control.rollbacks_to == 3 &&
                control.releases == 9,
            "batch did not isolate its writes in one transaction");
    require(control.visible_posts == 1 && control.visible_likes == 0,
            "external reader saw uncommitted batch writes");
    verify_created(reader.get(), created(results[0]), 2, body, "first");
    require(liked(results[1]) == LikeResult::created &&
                liked(results[2]) == LikeResult::already_liked && !results[3] &&
                results[3].error().code == WriteErrorCode::post_not_found &&
                liked(results[8]) == LikeResult::created,
            "FIFO create/like, duplicate or missing-post behavior changed");
    for (const auto index : {4U, 5U, 6U})
    {
        require(!results[index] && results[index].error().code == WriteErrorCode::constraint,
                "invalid write unexpectedly succeeded");
    }
    verify_created(reader.get(), created(results[7]), 3, "after failed neighbors", "second");
    require(scalar(reader.get(), "SELECT count(*) FROM posts") == 3 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 2,
            "failed writes changed stored counts");
    const auto post = read_post(db, PostId{2});
    require(post && scalar(reader.get(), "SELECT json_extract(?,'$.post.like_count')", *post) == 1,
            "committed like is not visible to post reads");
    require(read_feed(db).find("after failed neighbors") != std::string::npos,
            "committed create is not visible to feed reads");
}

void failed_commits()
{
    for (const auto failure : {CommitFailure::hook, CommitFailure::busy})
    {
        for (const bool singleton : {false, true})
        {
            Fixture fixture;
            Store db(fixture.path);
            auto reader = open(fixture.path);
            Control control(reader.get());
            control.commit_failure = failure;
            const std::array<WriteCommand, 3> jobs{
                CreatePost{UserId{1}, "first", "must roll back"},
                LikePost{UserId{2}, PostId{1}},
                LikePost{UserId{1}, PostId{999}},
            };
            const auto writes =
                std::span<const WriteCommand>(jobs).first(singleton ? 1U : jobs.size());
            all_failed(run_batch(db, writes), writes.size());
            require(control.commits == 1 && control.visible_posts == 1 &&
                        control.visible_likes == 0,
                    "failed commit exposed uncommitted writes");
            require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                        scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
                    "failed commit retained batch writes");
            control.commit_failure = CommitFailure::none;
            const std::array<WriteCommand, 1> recovery{
                CreatePost{UserId{1}, "first", "recovered after failed commit"}};
            require(created(run_batch(db, recovery).at(0)).id.value > 0,
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
    Store db(fixture.path);
    Control control(reader.get());
    const std::array<WriteCommand, 3> jobs{
        CreatePost{UserId{1}, "first", "provisional"},
        CreatePost{UserId{1}, "first", "automatic rollback"},
        CreatePost{UserId{1}, "first", "must not run"},
    };
    all_failed(run_batch(db, jobs), jobs.size());
    require(control.commits == 0 && scalar(reader.get(), "SELECT count(*) FROM posts") == 1,
            "automatic rollback retained a batch write or continued the batch");
    const std::array<WriteCommand, 1> recovery{
        CreatePost{UserId{1}, "first", "recovered after automatic rollback"}};
    require(created(run_batch(db, recovery).at(0)).id.value > 0,
            "automatic rollback left writer unusable");
}

void fatal_error()
{
    Fixture fixture;
    Store db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    control.fail_savepoint = 2;
    const std::array<WriteCommand, 3> jobs{
        CreatePost{UserId{1}, "first", "provisional"},
        LikePost{UserId{2}, PostId{1}},
        CreatePost{UserId{1}, "first", "must not run"},
    };
    all_failed(run_batch(db, jobs), jobs.size());
    require(control.commits == 0 && scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
            "fatal error retained provisional writes");
    const std::array<WriteCommand, 2> recovery{
        LikePost{UserId{2}, PostId{1}},
        LikePost{UserId{1}, PostId{1}},
    };
    const auto recovered = run_batch(db, recovery);
    require(liked(recovered.at(0)) == LikeResult::created &&
                liked(recovered.at(1)) == LikeResult::created && control.savepoints == 4 &&
                control.releases == 3 && control.commits == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 2,
            "fatal error left writer unusable");
}

void singleton_and_empty()
{
    Fixture fixture;
    Store db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    const std::array<WriteCommand, 0> empty{};
    require(run_batch(db, empty).empty() && control.begins == 0,
            "empty batch opened a transaction");
    const std::array<WriteCommand, 1> invalid{
        CreatePost{UserId{999}, "missing", "bad foreign key"}};
    all_failed(run_batch(db, invalid), 1);
    const std::array<WriteCommand, 1> create{CreatePost{UserId{1}, "first", "singleton"}};
    const auto created_results = run_batch(db, create);
    require(created_results.size() == 1, "singleton create lost its result");
    verify_created(reader.get(), created(created_results[0]), 2, "singleton", "first");
    const std::array<WriteCommand, 1> like{LikePost{UserId{2}, PostId{2}}};
    require(liked(run_batch(db, like).at(0)) == LikeResult::created &&
                liked(run_batch(db, like).at(0)) == LikeResult::already_liked,
            "singleton duplicate-like behavior changed");
    const std::array<WriteCommand, 1> missing{LikePost{UserId{2}, PostId{999}}};
    const auto missing_results = run_batch(db, missing);
    require(!missing_results[0] &&
                missing_results[0].error().code == WriteErrorCode::post_not_found,
            "singleton missing post changed");
    require(control.savepoints == 0, "singleton batches added savepoint overhead");
}
} // namespace

extern "C" int __real_sqlite3_step(sqlite3_stmt *);

extern "C" int __wrap_sqlite3_step(sqlite3_stmt *statement)
{
    Control *control = active;
    auto *db = sqlite3_db_handle(statement);
    if (control && db != control->reader)
    {
        const std::string_view command(sqlite3_sql(statement));
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
                const int result = __real_sqlite3_step(statement);
                sqlite3_commit_hook(db, nullptr, nullptr);
                return result;
            }
        }
    }
    return __real_sqlite3_step(statement);
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
