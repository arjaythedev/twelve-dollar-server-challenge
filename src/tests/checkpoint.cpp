#include "app/post_writer.hpp"
#include "diagnostics/database.hpp"
#include "sqlite_read.h"
#include "storage/database.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

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

template <class Value> Value write_one(tds::Store &db, const tds::WriteCommand &write)
{
    std::vector<tds::WriteOutcome> results(1);
    const auto committed = db.commit(std::span<const tds::WriteCommand>(&write, 1), results);
    if (!committed)
    {
        throw std::runtime_error(committed.error().message);
    }
    auto &result = results.front();
    if (!result)
    {
        throw std::runtime_error(result.error().message);
    }
    auto *value = std::get_if<Value>(&*result);
    require(value != nullptr, "singleton write returned the wrong type");

    return std::move(*value);
}

std::string read_feed(tds::Store &store)
{
    std::string output;
    tds::PostWriter writer(output, tds::PostShape::feed);
    store.feed(writer);

    return output;
}

std::optional<std::string> read_post(tds::Store &store, tds::PostId id)
{
    std::string output;
    tds::PostWriter writer(output, tds::PostShape::single);
    if (!store.post(id, writer))
    {
        return std::nullopt;
    }

    return output;
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
    explicit Fixture(int page_size = 4096)
    {
        char directory[] = "/tmp/td-checkpoint-XXXXXX";
        const char *created = ::mkdtemp(directory);
        require(created != nullptr, "fixture directory failed");
        directory_ = created;
        path = directory_ + "/feed.db";
        auto db = open(path);
        const std::string pragma = "PRAGMA page_size=" + std::to_string(page_size);
        require(sqlite3_exec(db.get(), pragma.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
                "fixture page size failed");
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

struct AdviceProbe;
thread_local AdviceProbe *advice_probe = nullptr;

struct AdviceProbe
{
    explicit AdviceProbe(int checkpointed) : frames(checkpointed)
    {
        require(active == nullptr && advice_probe == nullptr, "overlapping advice probes");
        advice_probe = this;
    }

    ~AdviceProbe()
    {
        advice_probe = nullptr;
    }

    int frames;
    int result = 0;
    std::uint64_t calls = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t length = 0;
};

int prepare_advice(sqlite3 *db)
{
    require(sqlite3_exec(db,
                         "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
                         "CREATE TABLE advice_probe(value BLOB);"
                         "INSERT INTO advice_probe VALUES(zeroblob(16384));",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "prepare advice WAL failed");
    int frames = -1;
    int checkpointed = -1;
    require(sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_PASSIVE, &frames,
                                      &checkpointed) == SQLITE_OK &&
                frames == checkpointed && frames > 1,
            "prepare advice checkpoint failed");
    return checkpointed;
}

void advice_totals(const tds_wal_advice_snapshot &snapshot, std::uint64_t calls,
                   std::uint64_t errors, std::uint64_t length, std::uint64_t events,
                   std::uint64_t dropped)
{
    require(snapshot.calls == (tds::telemetry::enabled ? calls : 0) &&
                snapshot.errors == (tds::telemetry::enabled ? errors : 0) &&
                snapshot.requested_bytes == (tds::telemetry::enabled ? calls * length : 0) &&
                snapshot.event_count == (tds::telemetry::profiling ? events : 0) &&
                snapshot.dropped_events == (tds::telemetry::profiling ? dropped : 0),
            "advice snapshot lost mode-specific counts, bytes, events or losses");
}

void advice_empty(tds_wal_advice_snapshot &snapshot)
{
    snapshot.calls = snapshot.errors = snapshot.requested_bytes = snapshot.event_count =
        snapshot.dropped_events = std::numeric_limits<std::uint64_t>::max();
    tds_wal_advice_take(&snapshot);
    advice_totals(snapshot, 0, 0, 0, 0, 0);
}

void advice_call(sqlite3 *db, int frames, unsigned capacity)
{
    errno = EDOM;
    tds_wal_drop_checkpointed(db, frames, capacity);
    require(errno == EDOM, "advice recording changed errno");
}

void advice_storage()
{
    Fixture fixture;
    auto db = open(fixture.path);
    const int frames = prepare_advice(db.get());
    AdviceProbe probe(frames);
    tds_wal_advice_snapshot snapshot;
    advice_empty(snapshot);
    advice_call(db.get(), 0, TDS_WAL_ADVICE_EVENT_CAPACITY);
    advice_call(db.get(), -1, TDS_WAL_ADVICE_EVENT_CAPACITY);
    advice_empty(snapshot);
    require(probe.calls == 0, "empty checkpoint issued cache advice");

    const auto started = tds::timing::now_ns();
    constexpr unsigned accumulated = TDS_WAL_ADVICE_EVENT_CAPACITY + 3;
    for (unsigned index = 0; index < accumulated; ++index)
    {
        probe.result = index % 3 == 0 ? EIO : 0;
        advice_call(db.get(), frames, std::numeric_limits<unsigned>::max());
    }
    const auto finished = tds::timing::now_ns();
    tds_wal_advice_take(&snapshot);
    advice_totals(snapshot, accumulated, 7, probe.length, TDS_WAL_ADVICE_EVENT_CAPACITY, 3);
    require(probe.calls == accumulated && probe.forwarded == accumulated - 7,
            "advice did not forward successful calls exactly once");
    std::uint64_t previous = started;
    for (std::uint64_t index = 0; index < snapshot.event_count; ++index)
    {
        const auto &event = snapshot.events[index];
        require(event.time_ns >= previous && event.time_ns <= finished &&
                    event.offset == static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)) &&
                    event.length == probe.length && event.result == (index % 3 == 0 ? EIO : 0),
                "advice prefix lost monotonic time, range or result");
        previous = event.time_ns;
    }
    advice_empty(snapshot);

    probe.result = 0;
    for (unsigned index = 0; index < 5; ++index)
    {
        advice_call(db.get(), frames, 3);
    }
    tds_wal_advice_take(&snapshot);
    advice_totals(snapshot, 5, 0, probe.length, 3, 2);
    advice_empty(snapshot);

    probe.result = EIO;
    for (unsigned index = 0; index < 4; ++index)
    {
        advice_call(db.get(), frames, 0);
    }
    tds_wal_advice_take(&snapshot);
    advice_totals(snapshot, 4, 4, probe.length, 0, 4);
    advice_empty(snapshot);

    {
        tds::telemetry::DatabaseTelemetry owner("advice_capacity");
        for (unsigned index = 0; index < 20; ++index)
        {
            const unsigned remaining =
                index < TDS_WAL_ADVICE_EVENT_CAPACITY ? TDS_WAL_ADVICE_EVENT_CAPACITY - index : 0;
            require(owner.remaining_advice_events() == (tds::telemetry::profiling ? remaining : 0),
                    "advice owner reported the wrong remaining capacity");
            probe.result = index % 2 == 0 ? 0 : EIO;
            advice_call(db.get(), frames, owner.remaining_advice_events());
            tds_wal_advice_take(&snapshot);
            advice_totals(snapshot, 1, index % 2, probe.length, remaining ? 1 : 0,
                          remaining ? 0 : 1);
            owner.wal_advice_completed(snapshot);
        }
#if TDS_TELEMETRY_LEVEL >= 1
        const auto totals = owner.snapshot();
        require(totals.wal_advice.calls == 20 && totals.wal_advice.errors == 10 &&
                    totals.wal_advice.requested_bytes == 20 * probe.length,
                "saturated owner lost aggregate advice counts");
#endif
    }
    {
        tds::telemetry::DatabaseTelemetry owner("new_advice_owner");
        require(owner.remaining_advice_events() ==
                    (tds::telemetry::profiling ? TDS_WAL_ADVICE_EVENT_CAPACITY : 0),
                "new advice owner inherited an earlier owner's capacity");
        probe.result = 0;
        advice_call(db.get(), frames, owner.remaining_advice_events());
        tds_wal_advice_take(&snapshot);
        advice_totals(snapshot, 1, 0, probe.length, 1, 0);
    }
    advice_empty(snapshot);

    advice_call(db.get(), frames, TDS_WAL_ADVICE_EVENT_CAPACITY);
    advice_call(db.get(), frames, TDS_WAL_ADVICE_EVENT_CAPACITY);
    std::exception_ptr worker_error;
    std::jthread worker([&] {
        try
        {
            Fixture worker_fixture;
            auto worker_db = open(worker_fixture.path);
            const int worker_frames = prepare_advice(worker_db.get());
            AdviceProbe worker_probe(worker_frames);
            tds_wal_advice_snapshot worker_snapshot;
            advice_empty(worker_snapshot);
            for (unsigned index = 0; index < 7; ++index)
            {
                advice_call(worker_db.get(), worker_frames, TDS_WAL_ADVICE_EVENT_CAPACITY);
            }
            tds_wal_advice_take(&worker_snapshot);
            advice_totals(worker_snapshot, 7, 0, worker_probe.length, 7, 0);
            advice_empty(worker_snapshot);
        }
        catch (...)
        {
            worker_error = std::current_exception();
        }
    });
    worker.join();
    if (worker_error)
    {
        std::rethrow_exception(worker_error);
    }
    tds_wal_advice_take(&snapshot);
    advice_totals(snapshot, 2, 0, probe.length, 2, 0);
    advice_empty(snapshot);
}

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
        block_advice = false;
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
    int page_size = 4096;
    int advice_calls = 0;
    int advice_result = 0;
    bool fail_second_open = false;
    bool blocked = false;
    bool in_checkpoint = false;
    bool block_advice = false;
    bool in_advice = false;
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

void write_until_started(tds::Store &db, Control &control, int target)
{
    const std::string body(500, 'x');
    for (int count = 0; count < 1000; ++count)
    {
        write_one<tds::CreatedPost>(db, tds::CreatePost{tds::UserId{1}, "first", body});
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
        tds::Store db(fixture.path);
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

void coalescing_and_freshness(int page_size)
{
    Fixture fixture(page_size);
    Control control;
    control.page_size = page_size;
    {
        tds::Store db(fixture.path);
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
            write_one<tds::CreatedPost>(db, tds::CreatePost{tds::UserId{1}, "first",
                                                            "created while checkpoint is blocked"});
        }
        require(std::chrono::steady_clock::now() - begin < 500ms,
                "writer waits for checkpoint I/O");
        watchdog.request_stop();
        watchdog.join();
        require(control.started == 1, "checkpoint wakeups were not coalesced");
        require(write_one<tds::LikeResult>(db, tds::LikePost{tds::UserId{2}, tds::PostId{1}}) ==
                        tds::LikeResult::created &&
                    write_one<tds::LikeResult>(db, tds::LikePost{tds::UserId{2}, tds::PostId{1}}) ==
                        tds::LikeResult::already_liked,
                "like result changed");
        const auto post = read_post(db, tds::PostId{1});
        require(post && post->find("\"like_count\":1") != std::string::npos,
                "post is stale after like");
        require(read_feed(db).find("created while checkpoint is blocked") != std::string::npos,
                "feed is stale after create");
        control.release();
        control.wait([&] {
            return control.finished >= 2 && control.frames == control.backfilled &&
                   control.advice_calls >= 1;
        });
        const auto previous_wal_bytes = std::filesystem::file_size(fixture.path + "-wal");
        const auto reused = write_one<tds::CreatedPost>(
            db, tds::CreatePost{tds::UserId{1}, "first", "fresh post after WAL reuse"});
        require(std::filesystem::file_size(fixture.path + "-wal") < previous_wal_bytes,
                "WAL reuse retained evicted pages for partial overwrites");
        const auto fresh = read_post(db, reused.id);
        require(fresh && fresh->find("fresh post after WAL reuse") != std::string::npos,
                "fresh read failed after shrinking the reused WAL");
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
    auto reopened = open(fixture.path);
    require(scalar(reopened.get(),
                   "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check <> 'ok'") ==
                0,
            "cache eviction changed database integrity after reopen");
}

void checkpoint_failures()
{
    Fixture fixture;
    Control control;
    control.advice_result = EIO;
    {
        tds::Store db(fixture.path);
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
        write_one<tds::CreatedPost>(
            db, tds::CreatePost{tds::UserId{1}, "first", "committed after busy checkpoint"});
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
        write_one<tds::CreatedPost>(
            db, tds::CreatePost{tds::UserId{1}, "first", "committed after failed checkpoint"});
        control.wait([&] {
            return control.finished == previous + 2 && control.frames == control.backfilled &&
                   control.advice_calls >= 1;
        });
        require(read_feed(db).find("committed after failed checkpoint") != std::string::npos,
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
    auto db = std::make_unique<tds::Store>(fixture.path);
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
        tds::Store db(fixture.path);
        auto reader = open(fixture.path);
        require(sqlite3_exec(reader.get(), "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK,
                "reader transaction failed");
        scalar(reader.get(), "SELECT count(*) FROM posts");
        write_until_started(db, control, 1);
        control.wait([&] {
            return control.finished >= 1 && control.frames > control.backfilled;
        });
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1,
                "partial checkpoint changed the pinned reader snapshot");
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
        write_one<tds::CreatedPost>(
            db, tds::CreatePost{tds::UserId{1}, "first", "written after reader releases WAL"});
        control.wait([&] {
            return control.finished > previous && control.frames == control.backfilled;
        });
        require(read_feed(db).find("written after reader releases WAL") != std::string::npos,
                "partial checkpoint recovery hid committed write");
    }
    require(!control.wrong_owner && !control.closed_during_checkpoint,
            "partial checkpoint ownership failed");
}

void shutdown_joins_advice()
{
    Fixture fixture;
    Control control;
    auto db = std::make_unique<tds::Store>(fixture.path);
    const Unblock unblock{control};
    control.block_advice = true;
    write_until_started(*db, control, 1);
    control.wait([&] {
        return control.in_advice;
    });
    std::jthread release([&] {
        std::this_thread::sleep_for(20ms);
        control.release();
    });
    db.reset();
    require(!control.closed_during_checkpoint && control.closes == 2 && control.advice_calls >= 1,
            "shutdown released the WAL descriptor during cache eviction");
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
        if (control->opens <= 2)
        {
            require(vfs && std::string_view(vfs) == "unix-excl",
                    "issuer and checkpoint handles must both use unix-excl");
        }
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
            active->closed_during_checkpoint |= active->in_checkpoint || active->in_advice;
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

// SQLite's amalgamation enables 64-bit file offsets
extern "C" int __real_posix_fadvise64(int, off64_t, off64_t, int);

extern "C" int __wrap_posix_fadvise64(int descriptor, off64_t offset, off64_t length, int advice)
{
    if (AdviceProbe *probe = advice_probe)
    {
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        const auto end = 32 + static_cast<std::int64_t>(probe->frames) * (4096 + 24);
        require(descriptor >= 0 && offset == page_size && length > 0 && length % page_size == 0 &&
                    offset + length == end - end % page_size && advice == POSIX_FADV_DONTNEED,
                "direct advice changed the descriptor, range or operation");
        ++probe->calls;
        probe->length = static_cast<std::uint64_t>(length);
        if (probe->result)
        {
            return probe->result;
        }
        ++probe->forwarded;
        return __real_posix_fadvise64(descriptor, offset, length, advice);
    }
    Control *control = active;
    int injected = 0;
    if (control)
    {
        std::unique_lock lock(control->mutex);
        const long page_size = ::sysconf(_SC_PAGESIZE);
        const auto checkpoint_end =
            32 + static_cast<std::int64_t>(control->backfilled) * (control->page_size + 24);
        require(std::this_thread::get_id() != control->issuer && !control->in_checkpoint,
                "cache advice escaped the checkpoint worker or preceded checkpoint completion");
        require(descriptor >= 0 && advice == POSIX_FADV_DONTNEED && offset == page_size &&
                    length > 0 && length % page_size == 0 && offset + length <= checkpoint_end,
                "cache advice included the WAL header, a partial page or uncheckpointed frames");
        ++control->advice_calls;
        control->in_advice = true;
        control->changed.notify_all();
        control->changed.wait(lock, [&] {
            return !control->block_advice;
        });
        injected = control->advice_result;
    }

    const int result =
        injected ? injected : __real_posix_fadvise64(descriptor, offset, length, advice);
    if (control)
    {
        std::lock_guard lock(control->mutex);
        control->in_advice = false;
        control->changed.notify_all();
    }

    return result;
}

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
        advice_storage();
        startup_failure();
        coalescing_and_freshness(4096);
        coalescing_and_freshness(8192);
        checkpoint_failures();
        pinned_reader();
        shutdown_joins();
        shutdown_joins_advice();
        std::puts("Checkpoint startup, advice storage, coalescing, freshness, failure and shutdown "
                  "passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
