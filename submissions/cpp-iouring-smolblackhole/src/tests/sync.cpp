#include "diagnostics/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
using namespace tds::telemetry;
constexpr int success_descriptor = -1;
constexpr int slow_descriptor = -2;
constexpr int invalid_descriptor = -3;
std::array<std::atomic<std::uint64_t>, 2> calls{};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class Artifact
{
  public:
    Artifact()
    {
        std::strcpy(path_.data(), "/tmp/tds-sync-XXXXXX");
        const int descriptor = ::mkstemp(path_.data());
        require(descriptor >= 0, "create sync artifact");
        ::close(descriptor);
        if (const auto *previous = std::getenv("TDS_TELEMETRY_PATH"))
        {
            previous_ = previous;
            had_previous_ = true;
        }
        if (::setenv("TDS_TELEMETRY_PATH", path_.data(), 1) != 0)
        {
            std::remove(path_.data());
            throw std::runtime_error("set sync artifact destination");
        }
    }

    ~Artifact()
    {
        if (had_previous_)
        {
            ::setenv("TDS_TELEMETRY_PATH", previous_.c_str(), 1);
        }
        else
        {
            ::unsetenv("TDS_TELEMETRY_PATH");
        }
        std::remove(path_.data());
    }

    Artifact(const Artifact &) = delete;
    Artifact &operator=(const Artifact &) = delete;

    std::string read() const
    {
        std::ifstream stream(path_.data());
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

  private:
    std::array<char, 64> path_{};
    std::string previous_;
    bool had_previous_ = false;
};

template <std::size_t Index> int fake_sync(int descriptor)
{
    calls[Index].fetch_add(1, std::memory_order_relaxed);
    if (descriptor == slow_descriptor)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        errno = EIO;
        return -1;
    }
    if (descriptor != success_descriptor)
    {
        errno = EBADF;
        return -1;
    }
    return 0;
}

void invoke(SyncCall callback, std::size_t index, int descriptor)
{
    const auto before = calls[index].load(std::memory_order_relaxed);
    errno = EINVAL;
    const int result = callback(descriptor);
    const int returned_errno = errno;
    const bool failed = descriptor != success_descriptor;
    const int expected_errno = descriptor == slow_descriptor ? EIO : failed ? EBADF : EINVAL;
    require(result == (failed ? -1 : 0) && returned_errno == expected_errno,
            "sync hook changed descriptor, result or errno");
    require(calls[index].load(std::memory_order_relaxed) == before + 1,
            "sync hook did not forward exactly once");
}

void metrics(const SyncCounters &owner, std::uint64_t fsync_count, std::uint64_t fdatasync_count)
{
    const std::array expected{fsync_count, fdatasync_count};
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto &value = owner[index];
        require(value.calls == expected[index], "sync call attribution is wrong");
        require(value.wall.count() + value.wall_clock_failures == expected[index],
                "sync wall availability accounting is wrong");
        require(value.thread_cpu.count() + value.cpu_clock_failures == expected[index],
                "sync CPU availability accounting is wrong");
    }
}

void check_hooks()
{
    const std::array<SyncCall, 2> callbacks{::fsync, ::fdatasync};
    require(sync_clock(CLOCK_MONOTONIC).available, "monotonic clock is unavailable");
    require(!sync_clock(static_cast<clockid_t>(-1)).available,
            "invalid clock was reported available");
    SyncCounters owner{}, nested{}, worker{};
    SlowSyncBuffer events, nested_events, worker_events;
    invoke(callbacks[0], 0, success_descriptor);
    invoke(callbacks[1], 1, slow_descriptor);
    metrics(owner, 0, 0);
    {
        SyncScope outer(owner, events, SyncRole::issuer);
        invoke(callbacks[0], 0, success_descriptor);
        invoke(callbacks[1], 1, slow_descriptor);

        struct Unwind
        {
        };

        try
        {
            SyncScope inner(nested, nested_events, SyncRole::checkpoint);
            require(active_sync == &inner, "nested scope was not activated");
            invoke(callbacks[0], 0, slow_descriptor);
            throw Unwind{};
        }
        catch (const Unwind &)
        {
        }
        require(active_sync == &outer, "unwinding did not restore outer attribution");
        invoke(callbacks[0], 0, success_descriptor);
        invoke(callbacks[1], 1, invalid_descriptor);
        std::exception_ptr worker_error;
        std::jthread thread([&] {
            try
            {
                require(active_sync == nullptr, "worker inherited main thread scope");
                {
                    SyncScope scope(worker, worker_events, SyncRole::other);
                    invoke(callbacks[0], 0, success_descriptor);
                    invoke(callbacks[1], 1, success_descriptor);
                }
                require(active_sync == nullptr, "worker scope was not cleared");
            }
            catch (...)
            {
                worker_error = std::current_exception();
            }
        });
        thread.join();
        if (worker_error)
        {
            std::rethrow_exception(worker_error);
        }
        require(active_sync == &outer, "worker changed main thread attribution");
        metrics(owner, 2, 2);
        metrics(nested, 1, 0);
        metrics(worker, 1, 1);
        require(owner[1].errors == 2 && nested[0].errors == 1 && worker[0].errors == 0,
                "sync failure counters lost attribution");
    }
    require(active_sync == nullptr, "outer scope was not cleared");
    require(events.events().size() == 1 && nested_events.events().size() == 1,
            "slow sync calls were not retained as bounded typed events");
    const auto &event = events.events().front();
    require(event.operation == SyncOperation::fdatasync && event.role == SyncRole::issuer &&
                event.wall_ns > 10'000'000 && event.end_ns >= event.start_ns &&
                event.result == -1 && event.error == EIO,
            "slow sync event lost timing, attribution or failure data");
    invoke(callbacks[0], 0, success_descriptor);
    metrics(owner, 2, 2);

    SlowSyncBuffer bounded;
    for (std::uint64_t index = 0; index < SlowSyncBuffer::capacity + 3; ++index)
    {
        auto sample = event;
        sample.start_ns = index;
        bounded.record(sample);
    }
    require(bounded.events().size() == SlowSyncBuffer::capacity && bounded.dropped() == 3 &&
                bounded.events().front().start_ns == 0 &&
                bounded.events().back().start_ns == SlowSyncBuffer::capacity - 1,
            "slow sync buffer did not preserve its bounded prefix and loss count");
}

void check_wal_snapshots()
{
    Artifact artifact;
    {
        DatabaseTelemetry owner("checkpoint");
        owner.checkpoint_completed(SQLITE_OK, 12000, 11000);
        owner.checkpoint_completed(SQLITE_OK, 14000, 14000);
        owner.checkpoint_completed(SQLITE_OK, 1000, 900);
        owner.checkpoint_completed(SQLITE_BUSY, -1, -1);
        owner.checkpoint_completed(SQLITE_IOERR, 50, 51);
        const auto sample = owner.snapshot();
        require(sample.wal.valid && sample.wal.frames == 1000 && sample.wal.checkpointed == 900 &&
                    sample.wal.backlog == 100 && sample.wal.observed_resets == 1,
                "invalid WAL observations changed the last valid paired gauge");
        require(
            sample.timings.wal_backlog.count() == 3 && sample.timings.wal_backlog.total == 1100 &&
                sample.counters.checkpoint_busy == 1 && sample.counters.checkpoint_failures == 1,
            "WAL backlog or checkpoint failure accounting is wrong");

        tds_wal_advice_snapshot advice{};
        advice.calls = TDS_WAL_ADVICE_EVENT_CAPACITY;
        advice.errors = 2;
        advice.requested_bytes = 65536;
        advice.event_count = TDS_WAL_ADVICE_EVENT_CAPACITY;
        advice.dropped_events = 3;
        for (std::uint64_t index = 0; index < advice.event_count; ++index)
        {
            advice.events[static_cast<std::size_t>(index)] = {index, 4096, 4096, 0};
        }
        owner.wal_advice_completed(advice);
        owner.wal_advice_completed(advice);
        const auto totals = owner.snapshot();
        require(totals.wal_advice.calls == 32 && totals.wal_advice.errors == 4 &&
                    totals.wal_advice.requested_bytes == 131072,
                "typed WAL advice transfer lost cumulative counts");
        const auto first = totals.time_ns + tds::timing::report_interval_ns;
        owner.record_window(first);
        owner.record_window(first + tds::timing::report_interval_ns);
        require(owner.windows().size() == 2 && owner.windows().at(0).wal_advice.calls == 32 &&
                    owner.windows().at(1).wal_advice.calls == 0 &&
                    owner.snapshot().wal_advice.calls == 32,
                "WAL advice interval reset discarded cumulative totals");
        require(artifact.read().empty(), "checkpoint hooks wrote telemetry before finalization");
    }
    const auto report = artifact.read();
    require(report.find("\"wal_advice_dropped\":22") != std::string::npos &&
                report.find("\"schema_version\":1") != std::string::npos &&
                report.find("\"source\":\"checkpoint\"") != std::string::npos,
            "final checkpoint artifact lost schema, attribution or bounded advice loss");

    tds_wal_advice_snapshot empty{};
    tds_wal_advice_take(&empty);
    require(empty.calls == 0 && empty.event_count == 0 && empty.dropped_events == 0,
            "unused WAL advice bridge was not empty");
    empty.calls = 99;
    tds_wal_advice_take(&empty);
    require(empty.calls == 0, "WAL advice bridge did not overwrite and reset its destination");
}

void check_operation_scopes()
{
    Artifact artifact;
    {
        DatabaseTelemetry owner("database");
        SyncCounters outside{};
        SlowSyncBuffer outside_events;
        SyncScope outer(outside, outside_events, SyncRole::other);
        for (const auto operation : {DatabaseOperation::feed, DatabaseOperation::post,
                                     DatabaseOperation::create, DatabaseOperation::like})
        {
            const auto sample = owner.measure(operation);
            require(active_sync == &outer, "read or write-body timing changed sync attribution");
        }
        {
            const auto sample = owner.measure(DatabaseOperation::commit);
            auto *commit = active_sync;
            require(commit && commit != &outer && commit->role == SyncRole::issuer,
                    "commit scope did not activate issuer attribution");
            invoke(::fsync, 0, success_descriptor);

            struct Unwind
            {
            };

            try
            {
                const auto checkpoint = owner.measure(DatabaseOperation::checkpoint);
                require(active_sync && active_sync->role == SyncRole::checkpoint,
                        "checkpoint scope did not activate checkpoint attribution");
                invoke(::fdatasync, 1, success_descriptor);
                throw Unwind{};
            }
            catch (const Unwind &)
            {
            }
            require(active_sync == commit, "checkpoint unwinding lost issuer attribution");
        }
        require(active_sync == &outer, "commit scope did not restore previous attribution");
        metrics(outside, 0, 0);
        owner.write_batch(3);
        owner.write_failed();
        const auto before = owner.snapshot();
        require(before.counters.feed == 1 && before.counters.post == 1 &&
                    before.counters.create == 1 && before.counters.like == 1 &&
                    before.counters.commits == 1 && before.counters.checkpoints == 1 &&
                    before.counters.write_batches == 1 && before.counters.write_commands == 3 &&
                    before.counters.write_failures == 1,
                "operation or write counters missed an attempt");
        metrics(before.timings.sync, 1, 1);
        require(before.timings.feed.count() == 1 && before.timings.post.count() == 1 &&
                    before.timings.create.count() == 1 && before.timings.like.count() == 1 &&
                    before.timings.commit.count() == 1 && before.timings.checkpoint.count() == 1,
                "operation timing series lost a completed scope");
        const auto first = before.time_ns + tds::timing::report_interval_ns;
        owner.record_window(first - 1);
        require(owner.windows().size() == 0, "profile window closed before five seconds");
        owner.record_window(first);
        owner.record_window(first + tds::timing::report_interval_ns);
        require(owner.windows().size() == 2 && owner.windows().at(0).counters.feed == 1 &&
                    owner.windows().at(1).counters.feed == 0 &&
                    owner.windows().at(1).timings.feed.count() == 0 &&
                    owner.snapshot().timings.feed.count() == 1,
                "profile windows did not reset intervals or preserve cumulative samples");
        require(artifact.read().empty(), "operation hooks emitted telemetry before finalization");
    }
    const auto report = artifact.read();
    require(report.find("\"mode\":\"profile\"") != std::string::npos &&
                report.find("\"source\":\"database\"") != std::string::npos &&
                report.find("\"write_commands\":3") != std::string::npos,
            "final database artifact lost typed operation data");
    require(std::count(report.begin(), report.end(), '\n') == 1,
            "owner finalization did not emit exactly one telemetry record");
}

void check_history_bounds()
{
    Artifact artifact;
    {
        DatabaseTelemetry owner("database");
        const auto started = owner.snapshot().time_ns;
        constexpr auto capacity = History<DatabaseSnapshot>::capacity;
        for (std::uint64_t index = 0; index < capacity + 2; ++index)
        {
            {
                const auto sample = owner.measure(DatabaseOperation::feed);
            }
            owner.record_window(started + (index + 1) * tds::timing::report_interval_ns);
        }
        require(owner.windows().size() == capacity && owner.windows().overwritten() == 2 &&
                    owner.windows().at(0).time_ns ==
                        started + 3 * tds::timing::report_interval_ns &&
                    owner.snapshot().counters.feed == capacity + 2 &&
                    owner.snapshot().timings.feed.count() == capacity + 2,
                "bounded profile history lost chronology or cumulative totals");
    }
}

void require_counters(const DatabaseCounters &actual, const DatabaseCounters &expected,
                      const char *message)
{
    require(actual.feed == expected.feed && actual.post == expected.post &&
                actual.create == expected.create && actual.like == expected.like &&
                actual.commits == expected.commits && actual.checkpoints == expected.checkpoints &&
                actual.write_batches == expected.write_batches &&
                actual.write_commands == expected.write_commands &&
                actual.write_failures == expected.write_failures &&
                actual.checkpoint_busy == expected.checkpoint_busy &&
                actual.checkpoint_failures == expected.checkpoint_failures,
            message);
}

void check_counter_windows()
{
    Artifact artifact;
    DatabaseTelemetry owner("database");
    const auto started = owner.snapshot().time_ns;
    const auto interval = tds::timing::report_interval_ns;
    const auto attempts = [&](const std::array<unsigned, 6> &counts) {
        constexpr std::array operations{DatabaseOperation::feed,   DatabaseOperation::post,
                                        DatabaseOperation::create, DatabaseOperation::like,
                                        DatabaseOperation::commit, DatabaseOperation::checkpoint};
        for (std::size_t index = 0; index < operations.size(); ++index)
        {
            for (unsigned count = 0; count < counts[index]; ++count)
            {
                const auto sample = owner.measure(operations[index]);
            }
        }
    };
    attempts({1, 2, 3, 4, 5, 6});
    owner.write_batch(7);
    owner.write_batch(11);
    for (unsigned count = 0; count < 3; ++count)
    {
        owner.write_failed();
    }
    owner.checkpoint_completed(SQLITE_BUSY, -1, -1);
    owner.checkpoint_completed(SQLITE_LOCKED, -1, -1);
    owner.checkpoint_completed(SQLITE_IOERR, -1, -1);
    owner.record_window(started + interval - 1);
    require(owner.windows().size() == 0, "an early report consumed pending counter changes");
    owner.record_window(started + interval);
    require_counters(owner.windows().at(0).counters,
                     {.feed = 1,
                      .post = 2,
                      .create = 3,
                      .like = 4,
                      .commits = 5,
                      .checkpoints = 6,
                      .write_batches = 2,
                      .write_commands = 18,
                      .write_failures = 3,
                      .checkpoint_busy = 2,
                      .checkpoint_failures = 1},
                     "first window missed read, write or checkpoint attempts");

    attempts({6, 5, 4, 3, 2, 1});
    owner.write_batch(13);
    owner.write_failed();
    owner.write_failed();
    for (unsigned count = 0; count < 3; ++count)
    {
        owner.checkpoint_completed(SQLITE_BUSY, -1, -1);
    }
    for (unsigned count = 0; count < 4; ++count)
    {
        owner.checkpoint_completed(SQLITE_IOERR, -1, -1);
    }
    owner.record_window(started);
    owner.record_window(started + 2 * interval - 1);
    require(owner.windows().size() == 1, "a skipped report changed the window boundary");
    owner.record_window(started + 2 * interval);
    require_counters(owner.windows().at(1).counters,
                     {.feed = 6,
                      .post = 5,
                      .create = 4,
                      .like = 3,
                      .commits = 2,
                      .checkpoints = 1,
                      .write_batches = 1,
                      .write_commands = 13,
                      .write_failures = 2,
                      .checkpoint_busy = 3,
                      .checkpoint_failures = 4},
                     "second window reused lifetime counts or lost pending changes");
    require_counters(owner.snapshot().counters,
                     {.feed = 7,
                      .post = 7,
                      .create = 7,
                      .like = 7,
                      .commits = 7,
                      .checkpoints = 7,
                      .write_batches = 3,
                      .write_commands = 31,
                      .write_failures = 5,
                      .checkpoint_busy = 5,
                      .checkpoint_failures = 5},
                     "capturing counter windows changed lifetime totals");
    owner.record_window(started + 3 * interval);
    require_counters(owner.windows().at(2).counters, {}, "an empty window repeated earlier work");
}

void check_counter_wrap()
{
    Artifact artifact;
    DatabaseTelemetry owner("database");
    const auto started = owner.snapshot().time_ns;
    const auto interval = tds::timing::report_interval_ns;
    owner.write_batch(std::numeric_limits<std::uint64_t>::max());
    owner.record_window(started + interval);
    require_counters(
        owner.windows().at(0).counters,
        {.write_batches = 1, .write_commands = std::numeric_limits<std::uint64_t>::max()},
        "first window lost the unsigned counter limit");
    owner.write_batch(3);
    owner.record_window(started + 2 * interval);
    require_counters(owner.windows().at(1).counters, {.write_batches = 1, .write_commands = 3},
                     "a wrapped lifetime counter changed the interval count");
    require_counters(owner.snapshot().counters, {.write_batches = 2, .write_commands = 2},
                     "lifetime counters no longer use unsigned wrap semantics");
}

void check_overlapping_operation_scopes()
{
    Artifact artifact;
    DatabaseTelemetry first("database"), second("database");
    const auto started = first.snapshot().time_ns;
    {
        const auto feed = first.measure(DatabaseOperation::feed);
        {
            const auto post = second.measure(DatabaseOperation::post);
            const auto like = first.measure(DatabaseOperation::like);
        }
        const auto before = first.snapshot();
        require(before.timings.feed.count() == 0 && before.timings.like.count() == 1 &&
                    second.snapshot().timings.post.count() == 1,
                "overlapping operation scopes lost owner or operation attribution");
        first.record_window(started + tds::timing::report_interval_ns);
    }
    const auto completed = first.snapshot();
    require(completed.counters.feed == 1 && completed.timings.feed.count() == 1 &&
                completed.timings.like.count() == 1 && completed.timings.post.count() == 0 &&
                first.windows().at(0).counters.feed == 1 &&
                first.windows().at(0).timings.feed.count() == 0,
            "a reporting window reassigned or lost an in-flight operation timing");
    first.record_window(started + 2 * tds::timing::report_interval_ns);
    require(first.windows().at(1).counters.feed == 0 &&
                first.windows().at(1).timings.feed.count() == 1 &&
                second.snapshot().counters.post == 1 && second.snapshot().timings.feed.count() == 0,
            "operation completion was not retained by its owner in the finishing interval");
}

} // namespace

int main()
{
    const auto real = original_sync;
    try
    {
        original_sync = {fake_sync<0>, fake_sync<1>};
        check_hooks();
        check_wal_snapshots();
        check_operation_scopes();
        check_counter_windows();
        check_counter_wrap();
        check_overlapping_operation_scopes();
        check_history_bounds();
        original_sync = real;
        std::puts("Sync forwarding, errno, typed attribution, bounded history and final artifacts "
                  "passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        original_sync = real;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
