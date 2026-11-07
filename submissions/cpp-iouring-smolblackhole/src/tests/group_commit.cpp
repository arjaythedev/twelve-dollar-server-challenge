#include "app/post_writer.hpp"
#include "sqlite_read.h"
#include "storage/database.hpp"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
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
    int rollback_attempts = 0;
    int steps = 0;
    int feed_calls = 0;
    bool fail_rollback_prepare = false;
    bool fail_rollback_step = false;
    bool fail_feed = false;
    bool fail_post_exists = false;
    bool fail_created_read = false;
    sqlite3 *writer = nullptr;
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
    const std::array<WriteCommand, 10> jobs{
        CreatePost{UserId{1}, "first", body},
        LikePost{UserId{2}, PostId{2}},
        LikePost{UserId{2}, PostId{2}},
        LikePost{UserId{1}, PostId{999}},
        CreatePost{UserId{999}, "missing", "bad foreign key"},
        LikePost{UserId{999}, PostId{1}},
        CreatePost{UserId{1}, "first", ""},
        CreatePost{UserId{2}, "second", "after failed neighbors"},
        LikePost{UserId{1}, PostId{3}},
        LikePost{UserId{999}, PostId{999}},
    };
    const auto results = run_batch(db, jobs);
    require(results.size() == jobs.size(), "batch lost results");
    require(control.commits == 1 && control.savepoints == 10 && control.rollbacks_to == 3 &&
                control.releases == 10,
            "batch did not isolate its writes in one transaction");
    std::printf("canonical batch: savepoints=%d releases=%d rollback_to=%d commits=%d\n",
                control.savepoints, control.releases, control.rollbacks_to, control.commits);
    require(control.visible_posts == 1 && control.visible_likes == 0,
            "external reader saw uncommitted batch writes");
    verify_created(reader.get(), created(results[0]), 2, body, "first");
    require(liked(results[1]) == LikeResult::created &&
                liked(results[2]) == LikeResult::already_liked && !results[3] &&
                results[3].error().code == WriteErrorCode::post_not_found &&
                liked(results[8]) == LikeResult::created && !results[9] &&
                results[9].error().code == WriteErrorCode::post_not_found,
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

void full_batch_boundary()
{
    for (const bool reject_commit : {false, true})
    {
        Fixture fixture;
        Store db(fixture.path);
        auto reader = open(fixture.path);
        Control control(reader.get());
        control.commit_failure = reject_commit ? CommitFailure::busy : CommitFailure::none;

        std::vector<WriteCommand> jobs;
        jobs.reserve(tds::write_batch_limit + 1);
        for (std::size_t index = 0; index < tds::write_batch_limit; ++index)
        {
            jobs.emplace_back(CreatePost{UserId{1}, "first", "full:" + std::to_string(index)});
        }
        const auto results = run_batch(db, jobs);
        require(control.commits == 1 && control.visible_posts == 1,
                "full batch committed early or exposed provisional writes");
        if (reject_commit)
        {
            all_failed(results, jobs.size());
            require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1,
                    "failed full batch retained a write");
        }
        else
        {
            for (std::size_t index = 0; index < jobs.size(); ++index)
            {
                require(created(results[index]).id.value == static_cast<std::int64_t>(index + 2),
                        "full batch lost its FIFO result or final command");
            }
            require(scalar(reader.get(), "SELECT count(*) FROM posts") ==
                        static_cast<std::int64_t>(jobs.size() + 1),
                    "full batch did not persist every acknowledged write");
        }

        jobs.emplace_back(CreatePost{UserId{1}, "first", "over capacity"});
        std::vector<WriteOutcome> oversized(jobs.size());
        const auto steps = control.steps;
        bool rejected = false;
        try
        {
            static_cast<void>(db.commit(jobs, oversized));
        }
        catch (const std::logic_error &)
        {
            rejected = true;
        }
        require(rejected && control.steps == steps && control.commits == 1,
                "oversized batch touched SQLite before rejecting its capacity");
    }
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
        INSERT INTO likes(user_id,post_id) VALUES(2,1);
        CREATE TRIGGER abort_batch BEFORE INSERT ON posts WHEN NEW.body='automatic rollback'
        BEGIN SELECT RAISE(ROLLBACK,'automatic rollback'); END;
    )sql");
    Store db(fixture.path);
    Control control(reader.get());
    const std::array<WriteCommand, 4> jobs{
        LikePost{UserId{2}, PostId{1}},
        CreatePost{UserId{1}, "first", "provisional"},
        CreatePost{UserId{1}, "first", "automatic rollback"},
        CreatePost{UserId{1}, "first", "must not run"},
    };
    all_failed(run_batch(db, jobs), jobs.size());
    require(control.commits == 0 && scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 1,
            "automatic rollback retained a batch write or continued the batch");
    const std::array<WriteCommand, 1> recovery{
        CreatePost{UserId{1}, "first", "recovered after automatic rollback"}};
    require(created(run_batch(db, recovery).at(0)).id.value > 0,
            "automatic rollback left writer unusable");
}

void fatal_error()
{
    Fixture fixture;
    auto reader = open(fixture.path);
    Store db(fixture.path);
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

void trigger_statement_isolation(bool temporary, bool like)
{
    Fixture fixture;
    auto reader = open(fixture.path);
    Store db(fixture.path);
    Control control(reader.get());
    const std::array<WriteCommand, 1> warm{CreatePost{UserId{1}, "first", "before schema change"}};
    require(created(run_batch(db, warm).at(0)).id.value == 2 && control.writer,
            "schema-change fixture did not capture the writer");
    const std::string trigger =
        std::string("CREATE ") + (temporary ? "TEMP " : "") +
        "TRIGGER partial_failure AFTER INSERT ON " + (temporary ? "main." : "") +
        (like ? "likes WHEN NEW.user_id=2" : "posts WHEN NEW.body='fail'") +
        " BEGIN INSERT INTO posts(user_id,body) VALUES(1,'trigger side effect'); "
        "SELECT RAISE(FAIL,'partial statement'); END";
    execute(temporary ? control.writer : reader.get(), trigger.c_str());
    const auto before_savepoints = control.savepoints;
    const auto before_rollbacks = control.rollbacks_to;
    const auto before_releases = control.releases;
    const std::array<WriteCommand, 3> jobs{
        CreatePost{UserId{1}, "first", "before failed statement"},
        like ? WriteCommand{LikePost{UserId{2}, PostId{1}}}
             : WriteCommand{CreatePost{UserId{1}, "first", "fail"}},
        like ? WriteCommand{LikePost{UserId{1}, PostId{1}}}
             : WriteCommand{CreatePost{UserId{1}, "first", "after failed statement"}},
    };
    const auto outcomes = run_batch(db, jobs);
    const auto releases = control.releases - before_releases;
    require(created(outcomes[0]).id.value == 3 && !outcomes[1] &&
                outcomes[1].error().code == WriteErrorCode::constraint && outcomes[2] &&
                control.savepoints - before_savepoints == 3 &&
                control.rollbacks_to - before_rollbacks == 1 && (releases == 0 || releases == 3),
            "main/temp trigger failed to retain per-request rollback after schema change");
    require(scalar(reader.get(),
                   "SELECT count(*) FROM posts WHERE body IN ('fail','trigger side effect')") ==
                    0 &&
                scalar(reader.get(), "SELECT count(*) FROM posts") == (like ? 3 : 4) &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == (like ? 1 : 0),
            "RAISE(FAIL) retained partial writes or discarded neighboring requests");
    if (like)
    {
        require(liked(outcomes[2]) == LikeResult::created, "later valid like was lost");
    }
    else
    {
        require(created(outcomes[2]).id.value == 4, "failed create consumed a persistent row ID");
    }
}

void full_batch_trigger_pages()
{
    Fixture fixture;
    auto reader = open(fixture.path);
    Store db(fixture.path);
    execute(reader.get(), R"sql(
        CREATE TRIGGER stress_write AFTER INSERT ON posts WHEN NEW.body LIKE 'stress:%'
        BEGIN
          INSERT INTO likes(user_id,post_id) VALUES(2,NEW.id);
          SELECT CASE WHEN NEW.body='stress:fail' THEN RAISE(FAIL,'partial trigger') END;
        END;
    )sql");
    Control control(reader.get());
    for (unsigned pass = 0; pass < 3; ++pass)
    {
        std::vector<WriteCommand> jobs;
        for (std::size_t index = 0; index < tds::write_batch_limit; ++index)
        {
            jobs.emplace_back(
                CreatePost{UserId{1}, "first",
                           index == 16 ? "stress:fail" : "stress:" + std::to_string(index)});
        }
        const auto outcomes = run_batch(db, jobs);
        std::int64_t next_id = 2 + static_cast<std::int64_t>(pass * 31);
        for (std::size_t index = 0; index < jobs.size(); ++index)
        {
            if (index == 16)
            {
                require(!outcomes[index] &&
                            outcomes[index].error().code == WriteErrorCode::constraint,
                        "middle trigger failure did not remain request-local");
            }
            else
            {
                require(created(outcomes[index]).id.value == next_id++,
                        "nested savepoints lost a successful neighbor or row ID");
            }
        }
        const auto writes = static_cast<std::int64_t>((pass + 1) * 31);
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1 + writes &&
                    scalar(reader.get(), "SELECT count(*) FROM likes") == writes &&
                    scalar(reader.get(), "SELECT count(*) FROM posts WHERE body='stress:fail'") ==
                        0,
                "nested savepoints retained failed post/like pages or failed to close at commit");
    }
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

void post_existence_fallback()
{
    Fixture fixture;
    Store db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    control.fail_post_exists = true;
    const std::array<WriteCommand, 3> jobs{
        LikePost{UserId{2}, PostId{1}},
        LikePost{UserId{2}, PostId{1}},
        LikePost{UserId{1}, PostId{999}},
    };
    const auto results = run_batch(db, jobs);
    require(liked(results[0]) == LikeResult::created &&
                liked(results[1]) == LikeResult::already_liked && !results[2] &&
                results[2].error().code == WriteErrorCode::post_not_found &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 1,
            "post-cursor failure changed write outcomes instead of using SQL");
}

void likes_record_fallback()
{
    for (const char *key : {"1.5", "zeroblob(2000)"})
    {
        Fixture fixture;
        {
            auto seed = open(fixture.path);
            execute(seed.get(), "INSERT INTO posts(id,user_id,body) VALUES(2,1,'second');"
                                "INSERT INTO likes(user_id,post_id) VALUES(1,1),(2,2)");
            // SQLite affinity permits unusual keys when the fixture bypasses foreign keys
            const std::string insert =
                "INSERT INTO likes(user_id,post_id) VALUES(1," + std::string(key) + ")";
            execute(seed.get(), insert.c_str());
        }
        Store store(fixture.path);
        auto reader = open(fixture.path);
        Control control(reader.get());
        control.fail_feed = true;
        const auto expected = read_feed(store);
        control.fail_feed = false;
        const auto before = control.feed_calls;
        require(read_feed(store) == expected && control.feed_calls > before,
                "unsupported likes record changed the SQL fallback response");
    }
}

void orphan_like_precedence()
{
    Fixture fixture;
    auto reader = open(fixture.path);
    execute(reader.get(), "INSERT INTO likes(user_id,post_id) VALUES(1,999),(999,1)");
    Store db(fixture.path);
    Control control(reader.get());
    const std::array<WriteCommand, 4> jobs{
        LikePost{UserId{1}, PostId{999}},
        LikePost{UserId{999}, PostId{1}},
        LikePost{UserId{999}, PostId{999}},
        LikePost{UserId{2}, PostId{1}},
    };
    const auto results = run_batch(db, jobs);
    require(!results[0] && results[0].error().code == WriteErrorCode::post_not_found &&
                liked(results[1]) == LikeResult::already_liked && !results[2] &&
                results[2].error().code == WriteErrorCode::post_not_found &&
                liked(results[3]) == LikeResult::created &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 3,
            "orphan duplicates changed missing-post precedence or neighboring writes");
}

void failed_create_readback()
{
    Fixture fixture;
    Store db(fixture.path);
    auto reader = open(fixture.path);
    Control control(reader.get());
    control.fail_created_read = true;
    const std::array<WriteCommand, 2> jobs{
        LikePost{UserId{2}, PostId{1}},
        CreatePost{UserId{1}, "first", "must roll back after readback failure"},
    };
    all_failed(run_batch(db, jobs), jobs.size());
    require(control.commits == 0 && scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
            "failed timestamp readback published writes or left the batch committed");
    control.fail_created_read = false;
    const std::array<WriteCommand, 1> recovery{
        CreatePost{UserId{1}, "first", "recovered after timestamp readback failure"}};
    const auto results = run_batch(db, recovery);
    verify_created(reader.get(), created(results[0]), 2,
                   "recovered after timestamp readback failure", "first");
}

void created_rowid_boundary()
{
    Fixture fixture;
    auto reader = open(fixture.path);
    execute(reader.get(), "UPDATE posts SET id=9223372036854775806");
    Store db(fixture.path);
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::array<WriteCommand, 2> jobs{
        CreatePost{UserId{1}, "first", "maximum rowid"},
        LikePost{UserId{2}, PostId{maximum}},
    };
    const auto results = run_batch(db, jobs);
    verify_created(reader.get(), created(results[0]), maximum, "maximum rowid", "first");
    require(liked(results[1]) == LikeResult::created,
            "64-bit created rowid was truncated before a following write");
}

void rollback_preparation_failure()
{
    Fixture fixture;
    auto reader = open(fixture.path);
    Control control(reader.get());
    control.fail_rollback_prepare = true;
    bool rejected = false;
    try
    {
        Store unavailable(fixture.path);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    require(rejected, "store admitted requests without a prepared rollback");
    control.fail_rollback_prepare = false;
    Store recovered(fixture.path);
    const std::array<WriteCommand, 1> jobs{
        CreatePost{UserId{1}, "first", "recovered after rollback preparation failed"}};
    require(created(run_batch(recovered, jobs).at(0)).id.value == 2,
            "rollback preparation failure leaked resources or writes");
}

void unresolved_rollback(bool read_failure)
{
    Fixture fixture;
    {
        Store db(fixture.path);
        auto reader = open(fixture.path);
        Control control(reader.get());
        control.fail_rollback_step = true;
        const auto rejects_read = [](auto &&read) {
            bool rejected = false;
            try
            {
                read();
            }
            catch (const std::exception &)
            {
                rejected = true;
            }
            require(rejected, "unresolved transaction admitted a read");
        };
        if (read_failure)
        {
            control.fail_feed = true;
            rejects_read([&] {
                read_feed(db);
            });
            require(control.feed_calls == 1, "read failure missed the B-tree cursor");
        }
        else
        {
            control.commit_failure = CommitFailure::busy;
            const std::array<WriteCommand, 2> jobs{
                CreatePost{UserId{1}, "first", "uncommitted secret"},
                LikePost{UserId{2}, PostId{1}}};
            all_failed(run_batch(db, jobs), jobs.size());
        }
        require(control.rollback_attempts == 1 && control.writer &&
                    sqlite3_get_autocommit(control.writer) == 0,
                "rollback failure did not leave a real unresolved transaction");

        // Removing the injected fault must not silently readmit the poisoned connection
        control.fail_feed = false;
        control.fail_rollback_step = false;
        control.commit_failure = CommitFailure::none;
        const int previous_steps = control.steps;
        const int previous_begins = control.begins;
        rejects_read([&] {
            db.health();
        });
        rejects_read([&] {
            read_feed(db);
        });
        rejects_read([&] {
            read_post(db, PostId{1});
        });
        const std::array<WriteCommand, 1> rejected{
            CreatePost{UserId{1}, "first", "must not enter the poisoned connection"}};
        all_failed(run_batch(db, rejected), rejected.size());
        require(control.steps == previous_steps && control.begins == previous_begins,
                "poisoned connection executed another SQLite statement");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                    scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
                "unresolved rollback published provisional writes");
    }
    Store reopened(fixture.path);
    auto reader = open(fixture.path);
    require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1 &&
                scalar(reader.get(), "SELECT count(*) FROM likes") == 0,
            "closing the quarantined connection retained provisional writes");
    const std::array<WriteCommand, 1> jobs{
        CreatePost{UserId{1}, "first", "fresh connection recovered"}};
    require(created(run_batch(reopened, jobs).at(0)).id.value == 2,
            "fresh connection could not recover after quarantine");
}
} // namespace

extern "C" int __real_sqlite3_step(sqlite3_stmt *);
extern "C" int __real_sqlite3_prepare_v3(sqlite3 *, const char *, int, unsigned, sqlite3_stmt **,
                                         const char **);
extern "C" int __real_tds_btree_feed_next(tds_btree *, tds_btree_post *);
extern "C" int __real_tds_btree_post_exists(tds_btree *, sqlite3_int64);

extern "C" int __wrap_tds_btree_post_exists(tds_btree *tree, sqlite3_int64 id)
{
    if (active && active->fail_post_exists)
    {
        return SQLITE_IOERR;
    }

    return __real_tds_btree_post_exists(tree, id);
}

extern "C" int __wrap_sqlite3_prepare_v3(sqlite3 *db, const char *sql, int length, unsigned flags,
                                         sqlite3_stmt **statement, const char **tail)
{
    if (active && db != active->reader && active->fail_rollback_prepare &&
        std::string_view(sql) == "ROLLBACK")
    {
        *statement = nullptr;

        return SQLITE_NOMEM;
    }

    return __real_sqlite3_prepare_v3(db, sql, length, flags, statement, tail);
}

extern "C" int __wrap_tds_btree_feed_next(tds_btree *tree, tds_btree_post *post)
{
    if (active)
    {
        ++active->feed_calls;
        if (active->fail_feed)
        {
            return SQLITE_IOERR;
        }
    }

    return __real_tds_btree_feed_next(tree, post);
}

extern "C" int __wrap_sqlite3_step(sqlite3_stmt *statement)
{
    Control *control = active;
    auto *db = sqlite3_db_handle(statement);
    if (control && db != control->reader)
    {
        control->writer = db;
        ++control->steps;
        const std::string_view command(sqlite3_sql(statement));
        if (control->fail_created_read && command == "SELECT created_at FROM posts WHERE id=?")
        {
            return SQLITE_IOERR;
        }
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
        else if (command == "ROLLBACK")
        {
            ++control->rollback_attempts;
            if (control->fail_rollback_step)
            {
                return SQLITE_IOERR;
            }
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
        full_batch_boundary();
        failed_commits();
        automatic_rollback();
        fatal_error();
        trigger_statement_isolation(false, false);
        trigger_statement_isolation(false, true);
        trigger_statement_isolation(true, false);
        trigger_statement_isolation(true, true);
        full_batch_trigger_pages();
        singleton_and_empty();
        post_existence_fallback();
        likes_record_fallback();
        orphan_like_precedence();
        failed_create_readback();
        created_rowid_boundary();
        rollback_preparation_failure();
        unresolved_rollback(false);
        unresolved_rollback(true);
        std::puts("Group-commit FIFO, isolation, visibility, failure and singleton checks passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
