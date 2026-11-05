#include "sqlite_read.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
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

struct TemporaryFile
{
    TemporaryFile()
    {
        std::strcpy(path.data(), "/tmp/td-btree-XXXXXX");
        const int descriptor = ::mkstemp(path.data());
        if (descriptor < 0)
        {
            throw std::system_error(errno, std::generic_category());
        }
        ::close(descriptor);
        std::snprintf(wal.data(), wal.size(), "%s-wal", path.data());
        std::snprintf(shm.data(), shm.size(), "%s-shm", path.data());
    }

    ~TemporaryFile()
    {
        ::unlink(wal.data());
        ::unlink(shm.data());
        ::unlink(path.data());
    }

    std::array<char, 32> path{};
    std::array<char, 36> wal{};
    std::array<char, 36> shm{};
};

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

Database open(const char *path)
{
    sqlite3 *raw = nullptr;
    const int result =
        sqlite3_open_v2(path, &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Database db(raw);
    check(result);
    return db;
}

void execute(sqlite3 *db, const char *sql)
{
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

Statement prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *raw = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement statement(raw);
    check(result);
    return statement;
}

std::string text(sqlite3_stmt *statement, int column)
{
    const auto *value = sqlite3_column_text(statement, column);
    require(value != nullptr, "SQL returned null text");
    return {reinterpret_cast<const char *>(value),
            static_cast<std::size_t>(sqlite3_column_bytes(statement, column))};
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

void require_empty(const tds_btree_post &row)
{
    require(row.id == 0 && row.body == nullptr && row.body_bytes == 0 &&
                row.created_at == nullptr && row.created_at_bytes == 0 && row.author == nullptr &&
                row.author_bytes == 0 && row.like_count == 0,
            "non-ROW result retained a previous output field");
}

class Fixture
{
  public:
    explicit Fixture(int page_size = 512) : db_(open(file_.path.data()))
    {
        const std::string page_setting = "PRAGMA page_size=" + std::to_string(page_size);
        execute(db_.get(), page_setting.c_str());
        execute(db_.get(), R"sql(
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            PRAGMA foreign_keys=ON;
            CREATE TABLE users(
                id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
                created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE posts(
                id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
                body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500),
                created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE likes(
                user_id INTEGER NOT NULL REFERENCES users(id),
                post_id INTEGER NOT NULL REFERENCES posts(id),
                created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                PRIMARY KEY(user_id,post_id));
            CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC,id DESC);
            CREATE INDEX posts_user_id_idx ON posts(user_id);
            CREATE INDEX likes_post_id_idx ON likes(post_id);
        )sql");
        {
            auto setting = prepare(db_.get(), "PRAGMA page_size");
            require(sqlite3_step(setting.get()) == SQLITE_ROW &&
                        sqlite3_column_int(setting.get(), 0) == page_size,
                    "fixture page size differs from requested size");
        }
        tds_btree *raw = nullptr;
        check(tds_btree_create(db_.get(), &raw));
        tree_.reset(raw);
        require(raw != nullptr, "bridge declined the canonical fixture schema");
    }

    void sql(const char *statement)
    {
        execute(db_.get(), statement);
    }

    int exists(sqlite3_int64 id)
    {
        return tds_btree_post_exists(tree_.get(), id);
    }

    void end_reads()
    {
        tds_btree_end(tree_.get());
    }

    void begin_reads()
    {
        sql("BEGIN");
        check(tds_btree_begin(tree_.get()));
    }

    void expect_next(int expected)
    {
        tds_btree_post row;
        std::memset(&row, 0xa5, sizeof(row));
        require(tds_btree_feed_next(tree_.get(), &row) == expected,
                "unexpected feed cursor result");
        if (expected != SQLITE_ROW)
        {
            require_empty(row);
        }
    }

    void post(sqlite3_int64 id, sqlite3_int64 user, std::string_view body,
              std::string_view timestamp)
    {
        auto insert =
            prepare(db_.get(), "INSERT INTO posts(id,user_id,body,created_at) VALUES(?,?,?,?)");
        check(sqlite3_bind_int64(insert.get(), 1, id));
        check(sqlite3_bind_int64(insert.get(), 2, user));
        check(sqlite3_bind_text64(insert.get(), 3, body.data(), body.size(), SQLITE_TRANSIENT,
                                  SQLITE_UTF8));
        check(sqlite3_bind_text64(insert.get(), 4, timestamp.data(), timestamp.size(),
                                  SQLITE_TRANSIENT, SQLITE_UTF8));
        require(sqlite3_step(insert.get()) == SQLITE_DONE, "fixture insert failed");
    }

    template <class Change> std::vector<Row> compare(std::size_t expected, Change change)
    {
        sql("BEGIN");
        try
        {
            check(tds_btree_begin(tree_.get()));
            std::vector<Row> actual;
            tds_btree_post row;
            int result;
            for (;;)
            {
                // Poison each output so the SQL oracle checks every successful assignment
                std::memset(&row, 0xa5, sizeof(row));
                result = tds_btree_feed_next(tree_.get(), &row);
                if (result != SQLITE_ROW)
                {
                    require_empty(row);
                    break;
                }
                actual.push_back({row.id,
                                  {row.body, static_cast<std::size_t>(row.body_bytes)},
                                  {row.created_at, static_cast<std::size_t>(row.created_at_bytes)},
                                  {row.author, static_cast<std::size_t>(row.author_bytes)},
                                  row.like_count});
                require(actual.size() <= 20, "bridge exceeded LIMIT 20");
            }
            require(result == SQLITE_DONE, "direct bridge failed instead of completing");
            change(file_.path.data());
            auto query = prepare(db_.get(), R"sql(
                SELECT p.id,p.body,p.created_at,u.username,
                       (SELECT count(*) FROM likes WHERE post_id=p.id)
                FROM posts p JOIN users u ON u.id=p.user_id
                ORDER BY p.created_at DESC,p.id DESC LIMIT 20
            )sql");
            std::vector<Row> reference;
            while ((result = sqlite3_step(query.get())) == SQLITE_ROW)
            {
                reference.push_back({sqlite3_column_int64(query.get(), 0), text(query.get(), 1),
                                     text(query.get(), 2), text(query.get(), 3),
                                     sqlite3_column_int64(query.get(), 4)});
            }
            require(result == SQLITE_DONE, "canonical SQL failed");
            require(actual.size() == expected, "unexpected direct bridge row count");
            require(actual == reference, "direct bridge differs from SQL in the same snapshot");
            tds_btree_end(tree_.get());
            sql("COMMIT");
            return actual;
        }
        catch (...)
        {
            tds_btree_end(tree_.get());
            sqlite3_exec(db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

    std::vector<Row> compare(std::size_t expected)
    {
        return compare(expected, [](const char *) {
        });
    }

  private:
    TemporaryFile file_;
    Database db_;
    Tree tree_;
};

void mixed_rows(Fixture &fixture)
{
    fixture.sql("INSERT INTO users(id,username) VALUES(-9,'negative'),(1,'first'),"
                "(2,replace(hex(zeroblob(5000)),'0','x') || char(0) || ' café'),"
                "(900001,'distant'); BEGIN");
    constexpr std::array<sqlite3_int64, 4> authors{-9, 1, 2, 900001};
    std::vector<sqlite3_int64> ids{std::numeric_limits<sqlite3_int64>::min(), -5000000, -17, -1, 0};
    for (sqlite3_int64 id = 2; id < 36; ++id)
    {
        ids.push_back(id);
    }
    ids.push_back(1000000);
    ids.push_back(std::numeric_limits<sqlite3_int64>::max());
    const std::string body(500, 'x');
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        std::array<char, 64> timestamp{};
        std::snprintf(timestamp.data(), timestamp.size(), "2026-02-%02uT00:00:00.000Z",
                      static_cast<unsigned>((7 * index) % 28 + 1));
        const auto stamp = index == 0                ? "2089-01-01T00:00:00.000Z"
                           : index + 1 == ids.size() ? "2090-01-01T00:00:00.000Z"
                                                     : timestamp.data();
        fixture.post(ids[index], authors[index % authors.size()], body, stamp);
    }
    for (sqlite3_int64 id = 10000; id < 10100; ++id)
    {
        fixture.post(id, 1, "skipped", "2000-01-01T00:00:00.000Z");
    }
    fixture.sql("INSERT INTO likes(user_id,post_id) SELECT 1,id FROM posts WHERE id<0; COMMIT");
    const auto previous = fixture.compare(20, [](const char *path) {
        auto writer = open(path);
        execute(writer.get(),
                "BEGIN; UPDATE users SET username='refreshed' WHERE id=2;"
                "INSERT INTO likes(user_id,post_id) VALUES(2,9223372036854775807);"
                "UPDATE posts SET body='fresh body',created_at='2100-01-01T00:00:00.000Z' "
                "WHERE id=1000000; COMMIT");
    });
    require(fixture.compare(20) != previous, "next request reused stale rows or author names");
}

void missing_authors(Fixture &fixture)
{
    fixture.sql("DELETE FROM likes; DELETE FROM posts; PRAGMA foreign_keys=OFF; BEGIN");
    for (int index = 1; index <= 50; ++index)
    {
        std::array<char, 64> timestamp{};
        std::snprintf(timestamp.data(), timestamp.size(), "2026-01-01T00:00:%02d.000Z", index);
        fixture.post(static_cast<sqlite3_int64>(index) * 100000 - 3000000, index > 25 ? 999 : 1,
                     "body", timestamp.data());
    }
    fixture.sql("COMMIT; PRAGMA foreign_keys=ON");
    fixture.compare(20);
    fixture.sql("INSERT INTO users(id,username) VALUES(999,'newly joined')");
    fixture.compare(20);
    fixture.sql("DELETE FROM posts");
    fixture.compare(0);
}

void post_existence()
{
    Fixture fixture;
    fixture.sql("INSERT INTO users(id,username) VALUES(1,'author')");
    fixture.post(-9, 1, "negative", "2026-01-01T00:00:00.000Z");
    fixture.post(1, 1, "first", "2026-01-01T00:00:00.000Z");
    fixture.post(std::numeric_limits<sqlite3_int64>::max(), 1, "maximum",
                 "2026-01-01T00:00:00.000Z");
    require(fixture.exists(1) == SQLITE_MISUSE, "post lookup accepted autocommit mode");
    fixture.sql("BEGIN");
    require(fixture.exists(-9) == SQLITE_ROW && fixture.exists(0) == SQLITE_DONE &&
                fixture.exists(std::numeric_limits<sqlite3_int64>::max()) == SQLITE_ROW &&
                fixture.exists(std::numeric_limits<sqlite3_int64>::min()) == SQLITE_DONE,
            "post lookup confused sparse or extreme rowids");
    fixture.end_reads();
    fixture.sql("COMMIT; BEGIN; SAVEPOINT lookup");
    require(fixture.exists(1) == SQLITE_ROW, "post cursor was not retained before a write");
    fixture.post(2, 1, "provisional", "2026-01-01T00:00:00.000Z");
    require(fixture.exists(2) == SQLITE_ROW, "post lookup missed this transaction's insert");
    fixture.end_reads();
    fixture.sql("ROLLBACK TO lookup; RELEASE lookup");
    require(fixture.exists(2) == SQLITE_DONE, "post lookup retained a rolled-back row");
    fixture.end_reads();
    fixture.sql("COMMIT");

    fixture.compare(3, [&fixture](const char *path) {
        require(fixture.exists(1) == SQLITE_MISUSE, "post lookup displaced active feed cursors");
        auto writer = open(path);
        execute(writer.get(), "INSERT INTO posts(id,user_id,body) VALUES(2,1,'committed later')");
    });
    fixture.sql("BEGIN");
    require(fixture.exists(2) == SQLITE_ROW, "post lookup missed a later committed row");
    fixture.end_reads();
    fixture.sql("COMMIT; CREATE INDEX extra_post_body ON posts(body); BEGIN");
    require(fixture.exists(1) == SQLITE_SCHEMA, "post lookup accepted stale schema metadata");
    fixture.end_reads();
    fixture.sql("ROLLBACK");
}

void likes_pages(int page_size)
{
    Fixture fixture(page_size);
    fixture.sql(R"sql(
        BEGIN;
        WITH RECURSIVE ids(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM ids WHERE id<257)
        INSERT INTO users(id,username) SELECT id,'user-' || id FROM ids;
        WITH RECURSIVE ids(id) AS (VALUES(10000) UNION ALL SELECT id+1 FROM ids WHERE id<10300)
        INSERT INTO posts(id,user_id,body,created_at) SELECT id,1,'old','2000' FROM ids;
        INSERT INTO likes(user_id,post_id) SELECT u.id,p.id FROM users u CROSS JOIN posts p;
    )sql");
    constexpr std::array<sqlite3_int64, 20> ids{std::numeric_limits<sqlite3_int64>::min(),
                                                -1099511627777,
                                                -8388609,
                                                -32769,
                                                -129,
                                                -2,
                                                -1,
                                                0,
                                                1,
                                                2,
                                                127,
                                                128,
                                                32767,
                                                32768,
                                                8388607,
                                                8388608,
                                                2147483647,
                                                2147483648,
                                                1099511627776,
                                                std::numeric_limits<sqlite3_int64>::max()};
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        const std::string timestamp = "2090-01-" + std::to_string(10 + (index * 7) % 20);
        fixture.post(ids[index], 1, "leaf boundary", timestamp);
        const std::string likes = "INSERT INTO likes(user_id,post_id) SELECT id," +
                                  std::to_string(ids[index]) +
                                  " FROM users WHERE id<=" + std::to_string(1 + (index * 53) % 257);
        fixture.sql(likes.c_str());
    }
    // Exercise the rowid serial types too, independently of the signed post IDs
    fixture.sql(R"sql(
        UPDATE likes SET rowid=-9223372036854775808 WHERE user_id=1 AND post_id=0;
        UPDATE likes SET rowid=9223372036854775807 WHERE user_id=1 AND post_id=1;
        UPDATE likes SET rowid=0 WHERE user_id=1 AND post_id=2;
        COMMIT;
    )sql");
    const auto previous = fixture.compare(20, [](const char *path) {
        auto writer = open(path);
        execute(writer.get(), "DELETE FROM likes WHERE post_id=-129;"
                              "INSERT INTO likes(user_id,post_id) VALUES(257,1)");
    });
    require(fixture.compare(20) != previous, "likes scan retained a previous snapshot");
    fixture.sql("DELETE FROM likes");
    fixture.compare(20);
}

void feed_reset()
{
    Fixture fixture;
    fixture.sql("INSERT INTO users(id,username) VALUES(1,'author');"
                "INSERT INTO posts(id,user_id,body) VALUES(1,1,'first'),(2,1,'second')");
    fixture.begin_reads();
    fixture.expect_next(SQLITE_ROW);
    fixture.end_reads();
    fixture.expect_next(SQLITE_MISUSE);
    fixture.sql("COMMIT; DELETE FROM posts");
    fixture.compare(0);
    fixture.sql("PRAGMA foreign_keys=OFF;"
                "INSERT INTO posts(id,user_id,body) VALUES(1,99,'orphan')");
    fixture.compare(0);
    fixture.sql("UPDATE posts SET user_id=1;"
                "INSERT INTO likes(user_id,post_id) VALUES(1,1.5)");
    fixture.begin_reads();
    fixture.expect_next(SQLITE_MISMATCH);
    fixture.end_reads();
    fixture.sql("ROLLBACK; DELETE FROM likes; PRAGMA foreign_keys=ON");
    fixture.compare(1);
    fixture.begin_reads();
    fixture.expect_next(SQLITE_ROW);
    fixture.expect_next(SQLITE_DONE);
    fixture.expect_next(SQLITE_DONE);
    fixture.end_reads();
    fixture.sql("COMMIT");
}

} // namespace

int main()
{
    try
    {
        Fixture fixture;
        fixture.compare(0);
        mixed_rows(fixture);
        missing_authors(fixture);
        post_existence();
        likes_pages(512);
        likes_pages(4096);
        feed_reset();
        std::cout << "B-tree feed rows, snapshots, overflow, likes pages and join limits passed\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
