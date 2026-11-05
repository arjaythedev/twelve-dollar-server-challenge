#include "sqlite_read.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

void check(int result)
{
    if (result != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errstr(result));
    }
}

void sql(sqlite3 *db, const char *text)
{
    if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
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

struct CloseTree
{
    void operator()(tds_btree *tree) const noexcept
    {
        tds_btree_destroy(tree);
    }
};

using Database = std::unique_ptr<sqlite3, CloseDatabase>;
using Statement = std::unique_ptr<sqlite3_stmt, CloseStatement>;
using Tree = std::unique_ptr<tds_btree, CloseTree>;

Database open(const std::string &path)
{
    sqlite3 *raw = nullptr;
    const int result =
        sqlite3_open_v2(path.c_str(), &raw,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    Database db(raw);
    check(result);
    return db;
}

Statement prepare(sqlite3 *db, const char *text)
{
    sqlite3_stmt *raw = nullptr;
    const int result = sqlite3_prepare_v2(db, text, -1, &raw, nullptr);
    Statement statement(raw);
    check(result);
    return statement;
}

std::int64_t scalar(sqlite3 *db, const char *text)
{
    auto statement = prepare(db, text);
    require(sqlite3_step(statement.get()) == SQLITE_ROW, "SQL scalar returned no row");
    return sqlite3_column_int64(statement.get(), 0);
}

void idle(sqlite3 *db)
{
    require(sqlite3_get_autocommit(db) != 0 && sqlite3_txn_state(db, "main") == SQLITE_TXN_NONE,
            "one-shot lookup retained a SQL or B-tree transaction");
}

void empty(const tds_btree_post &row)
{
    require(row.id == 0 && row.body == nullptr && row.body_bytes == 0 &&
                row.created_at == nullptr && row.created_at_bytes == 0 && row.author == nullptr &&
                row.author_bytes == 0 && row.like_count == 0,
            "non-ROW lookup retained a poisoned or previous output field");
}

struct Row
{
    sqlite3_int64 id;
    std::string body;
    std::string created_at;
    std::string author;
    sqlite3_int64 likes;
    bool operator==(const Row &) const = default;
};

std::string text(sqlite3_stmt *statement, int column)
{
    const auto *bytes = sqlite3_column_text(statement, column);
    require(bytes != nullptr, "canonical SQL returned NULL text");
    return {reinterpret_cast<const char *>(bytes),
            static_cast<std::size_t>(sqlite3_column_bytes(statement, column))};
}

Row copy(const tds_btree_post &row)
{
    return {row.id,
            {row.body, static_cast<std::size_t>(row.body_bytes)},
            {row.created_at, static_cast<std::size_t>(row.created_at_bytes)},
            {row.author, static_cast<std::size_t>(row.author_bytes)},
            row.like_count};
}

class Fixture
{
  public:
    explicit Fixture(int page_size = 512)
    {
        char pattern[] = "/tmp/td-btree-post-XXXXXX";
        const auto *created = ::mkdtemp(pattern);
        require(created != nullptr, "create point-read fixture directory");
        directory_ = created;
        path = directory_ + "/feed.db";
        db = open(path);
        const auto pragma = "PRAGMA page_size=" + std::to_string(page_size);
        sql(db.get(), pragma.c_str());
        sql(db.get(), R"sql(
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            PRAGMA foreign_keys=ON;
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
        )sql");
        require(scalar(db.get(), "PRAGMA page_size") == page_size, "wrong fixture page size");
        refresh_reader();
    }

    ~Fixture()
    {
        tree.reset();
        db.reset();
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void refresh_reader()
    {
        tree.reset();
        tds_btree *raw = nullptr;
        check(tds_btree_create(db.get(), &raw));
        tree.reset(raw);
        require(tree != nullptr, "canonical point-read schema was declined");
        idle(db.get());
    }

    void post(sqlite3_int64 id, sqlite3_int64 user, std::string_view body,
              std::string_view timestamp = "2026-01-01T00:00:00.000Z")
    {
        auto statement = prepare(db.get(), "INSERT INTO posts(id,user_id,body,created_at)"
                                           " VALUES(?1,?2,?3,?4)");
        check(sqlite3_bind_int64(statement.get(), 1, id));
        check(sqlite3_bind_int64(statement.get(), 2, user));
        check(sqlite3_bind_text64(statement.get(), 3, body.data(), body.size(), SQLITE_TRANSIENT,
                                  SQLITE_UTF8));
        check(sqlite3_bind_text64(statement.get(), 4, timestamp.data(), timestamp.size(),
                                  SQLITE_TRANSIENT, SQLITE_UTF8));
        require(sqlite3_step(statement.get()) == SQLITE_DONE, "insert point-read fixture post");
    }

    std::optional<Row> compare(sqlite3_int64 id)
    {
        std::optional<Row> expected;
        {
            auto statement = prepare(db.get(), R"sql(
                SELECT p.id,p.body,p.created_at,u.username,
                       (SELECT count(*) FROM likes WHERE post_id=p.id)
                FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1
            )sql");
            check(sqlite3_bind_int64(statement.get(), 1, id));
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_ROW)
            {
                expected = Row{sqlite3_column_int64(statement.get(), 0), text(statement.get(), 1),
                               text(statement.get(), 2), text(statement.get(), 3),
                               sqlite3_column_int64(statement.get(), 4)};
                require(sqlite3_step(statement.get()) == SQLITE_DONE,
                        "point SQL returned two rows");
            }
            else
            {
                require(result == SQLITE_DONE, "canonical point SQL failed");
            }
        }
        idle(db.get());
        tds_btree_post row;
        std::memset(&row, 0xa5, sizeof(row));
        const int result = tds_btree_get_post(tree.get(), id, &row);
        idle(db.get());
        require(result == (expected ? SQLITE_ROW : SQLITE_DONE),
                "point lookup disagreed with SQL status");
        if (expected)
        {
            require(copy(row) == *expected, "point lookup disagreed with SQL bytes or like count");
        }
        else
        {
            empty(row);
        }
        return expected;
    }

    void expect_error(int code, sqlite3_int64 id = 1)
    {
        tds_btree_post row;
        std::memset(&row, 0xa5, sizeof(row));
        require(tds_btree_get_post(tree.get(), id, &row) == code, "unexpected point-read error");
        empty(row);
        idle(db.get());
    }

    std::string path;
    Database db;
    Tree tree;

  private:
    std::string directory_;
};

void rows_and_likes(int page_size)
{
    Fixture fixture(page_size);
    fixture.compare(1);
    sql(fixture.db.get(),
        "BEGIN; WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1"
        " FROM ids WHERE n<257) INSERT INTO users(id,username) SELECT n,'user:'||n"
        " FROM ids; UPDATE users SET username=replace(hex(zeroblob(6000)),'0','x')"
        " || char(0) || ' café\"\\' WHERE id=2;");
    constexpr std::array<sqlite3_int64, 13> ids{std::numeric_limits<sqlite3_int64>::min(),
                                                -1099511627777,
                                                -8388609,
                                                -129,
                                                -1,
                                                0,
                                                1,
                                                128,
                                                32768,
                                                8388608,
                                                2147483648,
                                                1099511627776,
                                                std::numeric_limits<sqlite3_int64>::max()};
    constexpr std::array<int, 4> likes{0, 1, 32, 257};
    const std::string escaped = "quotes\" backslash\\ tab\t line\n café 東京";
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        fixture.post(ids[index], index % 2 ? 2 : 1, escaped,
                     index == 0 ? std::string(7000, 't') : "2026-01-01T00:00:00.000Z");
        const auto insert = "INSERT INTO likes(user_id,post_id) SELECT id," +
                            std::to_string(ids[index]) +
                            " FROM users WHERE id<=" + std::to_string(likes[index % likes.size()]);
        sql(fixture.db.get(), insert.c_str());
    }
    sql(fixture.db.get(), R"sql(
        WITH RECURSIVE ids(n) AS (VALUES(10000) UNION ALL SELECT n+1 FROM ids WHERE n<10300)
          INSERT INTO posts(id,user_id,body) SELECT n,1,'filler' FROM ids;
        INSERT INTO likes(user_id,post_id) SELECT u.id,p.id FROM users u CROSS JOIN posts p
          WHERE p.id BETWEEN 10000 AND 10300;
        UPDATE likes SET rowid=-9223372036854775808 WHERE user_id=1 AND post_id=128;
        UPDATE likes SET rowid=9223372036854775807 WHERE user_id=2 AND post_id=-129;
        COMMIT;
    )sql");
    for (const auto id : ids)
    {
        fixture.compare(id);
    }
    fixture.compare(-2);
    fixture.compare(129);
    fixture.compare(10301);
    fixture.compare(10000);
    fixture.compare(10300);
    sql(fixture.db.get(), "PRAGMA foreign_keys=OFF; INSERT INTO posts(id,user_id,body)"
                          " VALUES(999999,999,'orphan'); PRAGMA foreign_keys=ON;");
    require(!fixture.compare(999999), "orphan post did not preserve inner JOIN semantics");
    sql(fixture.db.get(), "INSERT INTO users(id,username) VALUES(999,'joined later');");
    require(fixture.compare(999999)->author == "joined later", "new author was not visible");
}

void fresh_commits()
{
    Fixture fixture;
    sql(fixture.db.get(), "INSERT INTO users(id,username) VALUES(1,'original'),(2,'second');");
    fixture.post(1, 1, "first");
    require(fixture.compare(1)->likes == 0, "initial point count");
    auto writer = open(fixture.path);
    sql(writer.get(), "BEGIN; UPDATE users SET username='fresh author' WHERE id=1;"
                      "UPDATE posts SET body='fresh body' WHERE id=1;"
                      "INSERT INTO likes(user_id,post_id) VALUES(2,1); COMMIT;");
    const auto fresh = fixture.compare(1);
    require(fresh->body == "fresh body" && fresh->author == "fresh author" && fresh->likes == 1,
            "next one-shot lookup reused stale row, author or likes");
    sql(writer.get(), "DELETE FROM likes; DELETE FROM posts WHERE id=1;");
    require(!fixture.compare(1), "next one-shot lookup missed committed deletion");
    sql(writer.get(), "INSERT INTO posts(id,user_id,body) VALUES(1,2,'reinserted');");
    require(fixture.compare(1)->author == "second", "reinserted row retained its prior author");
}

void borrowed_transactions()
{
    Fixture fixture;
    sql(fixture.db.get(), "INSERT INTO users(id,username) VALUES(1,'author');");
    fixture.post(1, 1, "body");
    sql(fixture.db.get(), "BEGIN;");
    require(scalar(fixture.db.get(), "SELECT count(*) FROM posts") == 1, "borrowed snapshot setup");
    tds_btree_post row;
    std::memset(&row, 0xa5, sizeof(row));
    require(tds_btree_get_post(fixture.tree.get(), 1, &row) == SQLITE_MISUSE,
            "point lookup borrowed an existing SQL transaction");
    empty(row);
    require(sqlite3_get_autocommit(fixture.db.get()) == 0 &&
                sqlite3_txn_state(fixture.db.get(), "main") == SQLITE_TXN_READ,
            "refused lookup ended the caller's SQL snapshot");
    sql(fixture.db.get(), "COMMIT;");
    fixture.compare(1);
    {
        auto statement = prepare(fixture.db.get(), "SELECT id FROM posts");
        require(sqlite3_step(statement.get()) == SQLITE_ROW, "active SQL statement setup");
        std::memset(&row, 0xa5, sizeof(row));
        require(tds_btree_get_post(fixture.tree.get(), 1, &row) == SQLITE_MISUSE,
                "point lookup displaced an active VDBE statement");
        empty(row);
        require(sqlite3_column_int64(statement.get(), 0) == 1 &&
                    sqlite3_step(statement.get()) == SQLITE_DONE,
                "refused lookup changed its caller's active SQL statement");
    }
    idle(fixture.db.get());
    sql(fixture.db.get(), "BEGIN;");
    check(tds_btree_begin(fixture.tree.get()));
    std::memset(&row, 0xa5, sizeof(row));
    require(tds_btree_get_post(fixture.tree.get(), 1, &row) == SQLITE_MISUSE,
            "point lookup displaced existing bridge cursors");
    empty(row);
    require(tds_btree_feed_next(fixture.tree.get(), &row) == SQLITE_ROW && row.id == 1,
            "refused point lookup ended the caller's bridge cursor");
    tds_btree_end(fixture.tree.get());
    sql(fixture.db.get(), "COMMIT;");
    idle(fixture.db.get());
}

void schema_epochs()
{
    for (const bool cookie_change : {false, true})
    {
        Fixture fixture;
        sql(fixture.db.get(), "INSERT INTO users(id,username) VALUES(1,'author');");
        fixture.post(1, 1, "body");
        fixture.compare(1);
        const auto cookie = scalar(fixture.db.get(), "PRAGMA schema_version");
        if (cookie_change)
        {
            sql(fixture.db.get(), "CREATE INDEX extra_post_body ON posts(body);");
        }
        else
        {
            sql(fixture.db.get(), "PRAGMA writable_schema=RESET;");
            require(scalar(fixture.db.get(), "SELECT count(*) FROM posts") == 1 &&
                        scalar(fixture.db.get(), "PRAGMA schema_version") == cookie,
                    "generation-only reset changed the schema cookie or fixture data");
        }
        fixture.expect_error(SQLITE_SCHEMA);
        fixture.refresh_reader();
        fixture.compare(1);
    }
}

sqlite3_mem_methods original_allocator{};
thread_local sqlite3 *allocation_database = nullptr;
thread_local bool fail_read_allocation = false;
thread_local unsigned failed_allocations = 0;

bool fail_allocation()
{
    if (fail_read_allocation && allocation_database &&
        sqlite3_txn_state(allocation_database, "main") == SQLITE_TXN_READ)
    {
        ++failed_allocations;
        return true;
    }
    return false;
}

void *allocate(int bytes)
{
    return fail_allocation() ? nullptr : original_allocator.xMalloc(bytes);
}

void *reallocate(void *allocation, int bytes)
{
    return fail_allocation() ? nullptr : original_allocator.xRealloc(allocation, bytes);
}

void install_allocator()
{
    check(sqlite3_config(SQLITE_CONFIG_GETMALLOC, &original_allocator));
    auto allocator = original_allocator;
    allocator.xMalloc = allocate;
    allocator.xRealloc = reallocate;
    check(sqlite3_config(SQLITE_CONFIG_MALLOC, &allocator));
}

void failures_release_snapshot()
{
    Fixture fixture;
    sql(fixture.db.get(), "INSERT INTO users(id,username) VALUES(1,'author');");
    fixture.post(1, 1, "body");
    allocation_database = fixture.db.get();
    fail_read_allocation = true;
    tds_btree_post row;
    std::memset(&row, 0xa5, sizeof(row));
    const int result = tds_btree_get_post(fixture.tree.get(), 1, &row);
    fail_read_allocation = false;
    allocation_database = nullptr;
    require(result == SQLITE_NOMEM && failed_allocations >= 1,
            "output scratch allocation failure was hidden or injection missed its seam");
    empty(row);
    idle(fixture.db.get());
    fixture.compare(1);
    sql(fixture.db.get(), "PRAGMA ignore_check_constraints=ON; UPDATE posts SET body=zeroblob(5);");
    fixture.expect_error(SQLITE_MISMATCH);
    sql(fixture.db.get(),
        "UPDATE posts SET body='recovered'; PRAGMA ignore_check_constraints=OFF;");
    require(fixture.compare(1)->body == "recovered", "lookup did not recover after a record error");
    std::memset(&row, 0xa5, sizeof(row));
    require(tds_btree_get_post(nullptr, 1, &row) == SQLITE_MISUSE, "NULL reader was accepted");
    empty(row);
    require(tds_btree_get_post(fixture.tree.get(), 1, nullptr) == SQLITE_MISUSE,
            "NULL output was accepted");
    idle(fixture.db.get());
}
} // namespace

int main()
{
    try
    {
        install_allocator();
        rows_and_likes(512);
        rows_and_likes(4096);
        fresh_commits();
        borrowed_transactions();
        schema_epochs();
        failures_release_snapshot();
        check(sqlite3_shutdown());
        check(sqlite3_config(SQLITE_CONFIG_MALLOC, &original_allocator));
        std::puts("One-shot B-tree point rows match SQL across sparse IDs, overflow, like pages, "
                  "fresh commits, borrowed state, schema epochs and allocation/record errors");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "B-tree point checks failed: %s\n", error.what());
        return 1;
    }
}
