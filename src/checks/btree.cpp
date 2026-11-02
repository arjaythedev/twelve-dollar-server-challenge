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

class Fixture
{
  public:
    Fixture() : db_(open(file_.path.data()))
    {
        execute(db_.get(), R"sql(
            PRAGMA page_size=512;
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
            auto page_size = prepare(db_.get(), "PRAGMA page_size");
            require(sqlite3_step(page_size.get()) == SQLITE_ROW &&
                        sqlite3_column_int(page_size.get(), 0) == 512,
                    "overflow fixture requires 512-byte pages");
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
            tds_btree_post row{};
            int result;
            while ((result = tds_btree_feed_next(tree_.get(), &row)) == SQLITE_ROW)
            {
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

} // namespace

int main()
{
    try
    {
        Fixture fixture;
        fixture.compare(0);
        mixed_rows(fixture);
        missing_authors(fixture);
        std::cout << "B-tree feed rows, snapshots, overflow and join limits passed\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
