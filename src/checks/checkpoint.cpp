#include "database.hpp"

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
using namespace std::chrono_literals;

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

std::int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *query = nullptr;
    require(sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK, "scalar prepare failed");
    const int result = sqlite3_step(query);
    const std::int64_t value = result == SQLITE_ROW ? sqlite3_column_int64(query, 0) : -1;
    sqlite3_finalize(query);
    require(result == SQLITE_ROW, "scalar row missing");
    return value;
}

class Fixture
{
  public:
    Fixture()
    {
        char directory[] = "/tmp/td-checkpoint-XXXXXX";
        const char *created = ::mkdtemp(directory);
        require(created != nullptr, "fixture directory failed");
        directory_ = created;
        path = directory_ + "/feed.db";
        auto db = open(path);
        require(sqlite3_exec(db.get(), R"sql(
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
        )sql",
                             nullptr, nullptr, nullptr) == SQLITE_OK,
                "fixture schema failed");
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

struct Control;
Control *active = nullptr;

struct Control
{
    Control()
    {
        require(active == nullptr, "overlapping checkpoint controls");
        active = this;
    }

    ~Control()
    {
        active = nullptr;
    }

    template <class Predicate> void wait(Predicate predicate)
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 2s, predicate), "checkpoint wait timed out");
    }

    void release()
    {
        std::lock_guard lock(mutex);
        blocked = false;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    const std::thread::id issuer = std::this_thread::get_id();
    sqlite3 *writer = nullptr;
    sqlite3 *checkpointer = nullptr;
    int opens = 0;
    int closes = 0;
    int started = 0;
    int finished = 0;
    int injected_result = SQLITE_OK;
    int frames = -1;
    int backfilled = -1;
    bool fail_second_open = false;
    bool blocked = false;
    bool in_checkpoint = false;
    bool wrong_owner = false;
    bool closed_during_checkpoint = false;
    int failed_start_autocheckpoint = -1;
};

struct Unblock
{
    Control &control;

    ~Unblock()
    {
        control.release();
    }
};

void write_until_started(tds::Database &db, Control &control, int target)
{
    const std::string body(500, 'x');
    for (int count = 0; count < 1000; ++count)
    {
        db.create(1, "first", body);
        {
            std::lock_guard lock(control.mutex);
            if (control.started >= target)
            {
                return;
            }
        }
        if (count % 100 == 99)
        {
            std::this_thread::yield();
        }
    }
    control.wait([&] {
        return control.started >= target;
    });
}

void startup_failure()
{
    Fixture fixture;
    Control control;
    control.fail_second_open = true;
    bool failed = false;
    try
    {
        tds::Database db(fixture.path);
    }
    catch (const std::runtime_error &)
    {
        failed = true;
    }
    require(failed && control.opens == 2 && control.closes == 1 && control.started == 0,
            "failed worker startup did not unwind connections");
    require(control.failed_start_autocheckpoint == 1000,
            "startup failure disabled default checkpoint");
}

void coalescing_and_freshness()
{
    Fixture fixture;
    Control control;
    {
        tds::Database db(fixture.path);
        const Unblock unblock{control};
        control.blocked = true;
        write_until_started(db, control, 1);
        std::jthread watchdog([&](std::stop_token stop) {
            std::unique_lock lock(control.mutex);
            std::stop_callback wake(stop, [&] {
                control.changed.notify_all();
            });
            if (!control.changed.wait_for(lock, 1s, [&] {
                    return stop.stop_requested();
                }))
            {
                control.blocked = false;
                control.changed.notify_all();
            }
        });
        const auto begin = std::chrono::steady_clock::now();
        for (int count = 0; count < 100; ++count)
        {
            db.create(1, "first", "created while checkpoint is blocked");
        }
        require(std::chrono::steady_clock::now() - begin < 500ms,
                "writer waits for checkpoint I/O");
        watchdog.request_stop();
        watchdog.join();
        require(control.started == 1, "checkpoint wakeups were not coalesced");
        require(db.like(2, 1) == true && db.like(2, 1) == false, "like result changed");
        const auto post = db.post(1);
        require(post && post->find("\"like_count\":1") != std::string::npos,
                "post is stale after like");
        require(db.feed().find("created while checkpoint is blocked") != std::string::npos,
                "feed is stale after create");
        control.release();
        control.wait([&] {
            return control.finished >= 2 && control.frames == control.backfilled;
        });
        {
            std::unique_lock lock(control.mutex);
            require(!control.changed.wait_for(lock, 30ms,
                                              [&] {
                                                  return control.started > 2;
                                              }),
                    "checkpoint spun without another write");
        }
        require(std::filesystem::file_size(fixture.path + "-wal") < 16 * 1024 * 1024,
                "normal WAL fixture exceeded its bound");
    }
    require(!control.wrong_owner && !control.closed_during_checkpoint && control.closes == 2,
            "checkpoint ownership or teardown failed");
}

void checkpoint_failures()
{
    Fixture fixture;
    Control control;
    {
        tds::Database db(fixture.path);
        control.injected_result = SQLITE_BUSY;
        write_until_started(db, control, 1);
        control.wait([&] {
            return control.finished >= 1;
        });
        std::this_thread::sleep_for(30ms);
        int previous = 0;
        {
            std::unique_lock lock(control.mutex);
            previous = control.started;
            require(!control.changed.wait_for(lock, 30ms,
                                              [&] {
                                                  return control.started > previous;
                                              }),
                    "busy checkpoint retried without a later write");
            control.injected_result = SQLITE_IOERR;
        }
        db.create(1, "first", "committed after busy checkpoint");
        control.wait([&] {
            return control.finished == previous + 1;
        });
        {
            std::unique_lock lock(control.mutex);
            require(!control.changed.wait_for(lock, 30ms,
                                              [&] {
                                                  return control.started > previous + 1;
                                              }),
                    "failed checkpoint retried without a later write");
            control.injected_result = SQLITE_OK;
        }
        db.create(1, "first", "committed after failed checkpoint");
        control.wait([&] {
            return control.finished == previous + 2 && control.frames == control.backfilled;
        });
        require(db.feed().find("committed after failed checkpoint") != std::string::npos,
                "checkpoint failure lost committed data");
    }
    auto db = open(fixture.path);
    require(scalar(db.get(), "SELECT count(*) FROM posts WHERE body LIKE 'committed after%'") == 2,
            "checkpoint failure changed stored writes");
}

void shutdown_joins()
{
    Fixture fixture;
    Control control;
    auto db = std::make_unique<tds::Database>(fixture.path);
    const Unblock unblock{control};
    control.blocked = true;
    write_until_started(*db, control, 1);
    std::jthread release([&] {
        std::this_thread::sleep_for(20ms);
        control.release();
    });
    db.reset();
    require(!control.closed_during_checkpoint && !control.wrong_owner && control.closes == 2 &&
                control.finished >= 1,
            "shutdown closed a connection before checkpoint joined");
}

void pinned_reader()
{
    Fixture fixture;
    Control control;
    {
        tds::Database db(fixture.path);
        auto reader = open(fixture.path);
        require(sqlite3_exec(reader.get(), "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK,
                "reader transaction failed");
        scalar(reader.get(), "SELECT count(*) FROM posts");
        write_until_started(db, control, 1);
        control.wait([&] {
            return control.finished >= 1 && control.frames > control.backfilled;
        });
        std::this_thread::sleep_for(30ms);
        int previous = 0;
        {
            std::unique_lock lock(control.mutex);
            previous = control.started;
            require(!control.changed.wait_for(lock, 30ms,
                                              [&] {
                                                  return control.started > previous;
                                              }),
                    "partial checkpoint spun without another write");
        }
        require(sqlite3_exec(reader.get(), "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK,
                "reader commit failed");
        db.create(1, "first", "written after reader releases WAL");
        control.wait([&] {
            return control.finished > previous && control.frames == control.backfilled;
        });
        require(db.feed().find("written after reader releases WAL") != std::string::npos,
                "partial checkpoint recovery hid committed write");
    }
    require(!control.wrong_owner && !control.closed_during_checkpoint,
            "partial checkpoint ownership failed");
}
} // namespace

extern "C" int __real_sqlite3_open_v2(const char *, sqlite3 **, int, const char *);

extern "C" int __wrap_sqlite3_open_v2(const char *path, sqlite3 **db, int flags, const char *vfs)
{
    Control *control = active;
    if (control)
    {
        std::lock_guard lock(control->mutex);
        ++control->opens;
        if (control->opens == 2 && control->fail_second_open)
        {
            *db = nullptr;
            return SQLITE_CANTOPEN;
        }
    }
    const int result = __real_sqlite3_open_v2(path, db, flags, vfs);
    if (control && result == SQLITE_OK)
    {
        std::lock_guard lock(control->mutex);
        if (control->opens == 1)
        {
            control->writer = *db;
        }
        else if (control->opens == 2)
        {
            control->checkpointer = *db;
        }
    }
    return result;
}

extern "C" int __real_sqlite3_close(sqlite3 *);

extern "C" int __wrap_sqlite3_close(sqlite3 *db)
{
    if (active && db)
    {
        std::lock_guard lock(active->mutex);
        if (db == active->writer || db == active->checkpointer)
        {
            ++active->closes;
            active->closed_during_checkpoint |= active->in_checkpoint;
        }
        if (db == active->writer && active->fail_second_open)
        {
            active->failed_start_autocheckpoint =
                static_cast<int>(scalar(db, "PRAGMA wal_autocheckpoint"));
        }
    }
    return __real_sqlite3_close(db);
}

extern "C" int __real_sqlite3_wal_checkpoint_v2(sqlite3 *, const char *, int, int *, int *);

extern "C" int __wrap_sqlite3_wal_checkpoint_v2(sqlite3 *db, const char *name, int mode,
                                                int *frames, int *backfilled)
{
    Control *control = active;
    if (!control)
    {
        return __real_sqlite3_wal_checkpoint_v2(db, name, mode, frames, backfilled);
    }
    int injected = SQLITE_OK;
    {
        std::unique_lock lock(control->mutex);
        ++control->started;
        control->in_checkpoint = true;
        control->wrong_owner |= db != control->checkpointer ||
                                std::this_thread::get_id() == control->issuer ||
                                mode != SQLITE_CHECKPOINT_PASSIVE;
        control->changed.notify_all();
        control->changed.wait(lock, [&] {
            return !control->blocked;
        });
        injected = control->injected_result;
    }
    const int result = injected == SQLITE_OK
                           ? __real_sqlite3_wal_checkpoint_v2(db, name, mode, frames, backfilled)
                           : injected;
    {
        std::lock_guard lock(control->mutex);
        if (result == SQLITE_OK)
        {
            control->frames = *frames;
            control->backfilled = *backfilled;
        }
        ++control->finished;
        control->in_checkpoint = false;
        control->changed.notify_all();
    }
    return result;
}

int main()
{
    try
    {
        startup_failure();
        coalescing_and_freshness();
        checkpoint_failures();
        pinned_reader();
        shutdown_joins();
        std::puts("Checkpoint startup, coalescing, freshness, failure and shutdown passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
