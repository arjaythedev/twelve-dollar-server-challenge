#include "diagnostics/database.hpp"
#include "sqlite_read.h"

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <type_traits>
#include <unistd.h>

namespace
{
static_assert(tds::telemetry::profiling);
constexpr std::uint64_t calls = 2'000;
constexpr unsigned capacity = TDS_WAL_ADVICE_EVENT_CAPACITY;
static_assert(calls % capacity == 0);

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct AdviceCalls
{
    std::uint64_t calls = 0;
    std::uint64_t errors = 0;
    std::uint64_t bytes = 0;
    std::uint64_t length = 0;
    long page_size = 0;
    bool valid = true;
} observed;

std::uint64_t cpu_ns()
{
    timespec value{};
    require(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, "read probe CPU clock");
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

class Fixture
{
  public:
    Fixture()
    {
        char path[] = "/tmp/td-wal-advice-XXXXXX";
        require(::mkdtemp(path) != nullptr, "create probe directory");
        directory_ = path;
        sqlite3 *raw = nullptr;
        const int result = sqlite3_open_v2(
            (directory_ + "/wal.db").c_str(), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, "unix-excl");
        db.reset(raw);
        require(result == SQLITE_OK, "open probe database");
        require(sqlite3_exec(db.get(),
                             "PRAGMA page_size=4096; PRAGMA journal_mode=WAL;"
                             "PRAGMA wal_autocheckpoint=0; CREATE TABLE probe(value BLOB);"
                             "INSERT INTO probe VALUES(zeroblob(16384));",
                             nullptr, nullptr, nullptr) == SQLITE_OK,
                "create probe WAL");
        int backfilled = 0;
        require(sqlite3_wal_checkpoint_v2(db.get(), "main", SQLITE_CHECKPOINT_PASSIVE, &frames,
                                          &backfilled) == SQLITE_OK &&
                    frames == backfilled && frames > 1,
                "checkpoint probe WAL");
    }

    ~Fixture()
    {
        db.reset();
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::unique_ptr<sqlite3, Close> db;
    int frames = 0;

  private:
    std::string directory_;
};

// The same source is measured before and after the explicit event-budget API
template <class Drop> void drop(Drop function, sqlite3 *db, int frames, unsigned budget)
{
    if constexpr (std::is_invocable_v<Drop, sqlite3 *, int, unsigned>)
    {
        function(db, frames, budget);
    }
    else
    {
        function(db, frames);
    }
}

template <class Diagnostics>
unsigned remaining_events(const Diagnostics &diagnostics, unsigned baseline_budget)
{
    if constexpr (requires { diagnostics.remaining_advice_events(); })
    {
        return diagnostics.remaining_advice_events();
    }
    else
    {
        return baseline_budget;
    }
}

void checkpoint(Fixture &fixture, tds::telemetry::DatabaseTelemetry &diagnostics, unsigned budget,
                tds_wal_advice_snapshot &advice)
{
    {
        const auto scope = diagnostics.measure(tds::telemetry::DatabaseOperation::checkpoint);
        drop(&tds_wal_drop_checkpointed, fixture.db.get(), fixture.frames,
             remaining_events(diagnostics, budget));
    }
    tds_wal_advice_take(&advice);
    diagnostics.wal_advice_completed(advice);
    diagnostics.checkpoint_completed(SQLITE_OK, fixture.frames, fixture.frames);
}

struct Sample
{
    double ns_per_call;
    std::uint64_t verified_calls;
    std::uint64_t verified_errors;
    std::uint64_t verified_bytes;
    std::uint64_t retained_events;
    std::uint64_t dropped_events;
};

Sample measure(Fixture &fixture, bool saturated)
{
    std::uint64_t elapsed = 0;
    std::uint64_t errors = 0;
    std::uint64_t bytes = 0;
    std::uint64_t events = 0;
    std::uint64_t dropped = 0;
    std::uint64_t measured_calls = 0;
    // Recreate the owner outside timing so every retained batch has its first 16 slots
    for (std::uint64_t batch = 0; batch < calls / capacity; ++batch)
    {
        tds::telemetry::DatabaseTelemetry diagnostics("wal_advice_probe");
        tds_wal_advice_snapshot advice;
        if (saturated)
        {
            for (unsigned index = 0; index < capacity; ++index)
            {
                checkpoint(fixture, diagnostics, capacity - index, advice);
            }
        }
        const auto before = observed;
        const auto totals_before = diagnostics.snapshot();
        errno = EDOM;
        const auto started = cpu_ns();
        for (unsigned index = 0; index < capacity; ++index)
        {
            checkpoint(fixture, diagnostics, saturated ? 0 : capacity - index, advice);
            errors += advice.errors;
            bytes += advice.requested_bytes;
            events += saturated ? 0 : advice.event_count;
            dropped += advice.dropped_events + (saturated ? advice.event_count : 0);
        }
        elapsed += cpu_ns() - started;
        require(errno == EDOM, "advice diagnostics changed errno");
        const auto totals = diagnostics.snapshot();
        require(observed.valid && observed.calls - before.calls == capacity &&
                    observed.errors - before.errors == capacity / 4 &&
                    observed.bytes - before.bytes == observed.length * capacity &&
                    totals.counters.checkpoints - totals_before.counters.checkpoints == capacity &&
                    totals.timings.checkpoint.count() - totals_before.timings.checkpoint.count() ==
                        capacity &&
                    totals.wal_advice.calls - totals_before.wal_advice.calls == capacity &&
                    totals.wal_advice.errors - totals_before.wal_advice.errors == capacity / 4 &&
                    totals.wal_advice.requested_bytes - totals_before.wal_advice.requested_bytes ==
                        observed.length * capacity,
                "probe changed advice forwarding, counters or checkpoint clock samples");
        measured_calls += capacity;
    }
    require(measured_calls == calls && errors == calls / 4 && bytes == observed.length * calls &&
                events == (saturated ? 0 : calls) && dropped == (saturated ? calls : 0),
            "probe changed retained events, errors, bytes or dropped counts");
    return {static_cast<double>(elapsed) / static_cast<double>(calls),
            measured_calls,
            errors,
            bytes,
            events,
            dropped};
}
} // namespace

// SQLite setup is real; deterministic advice isolates the diagnostics from kernel cache work
extern "C" int __wrap_posix_fadvise64(int descriptor, off64_t offset, off64_t length, int advice)
{
    const auto page_size = observed.page_size;
    observed.valid &=
        descriptor >= 0 && offset == page_size && length > 0 && length % page_size == 0 &&
        advice == POSIX_FADV_DONTNEED &&
        (observed.length == 0 || observed.length == static_cast<std::uint64_t>(length));
    observed.length = static_cast<std::uint64_t>(length);
    ++observed.calls;
    observed.bytes += static_cast<std::uint64_t>(length);
    const int result = observed.calls % 4 == 0 ? EIO : 0;
    observed.errors += result != 0;
    return result;
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2 && (std::string_view(argv[1]) == "retained" ||
                              std::string_view(argv[1]) == "saturated"),
                "usage: wal_advice_probe retained|saturated");
        require(::setenv("TDS_TELEMETRY_PATH", "/dev/null", 1) == 0,
                "set probe export destination");
        observed.page_size = ::sysconf(_SC_PAGESIZE);
        require(observed.page_size > 0, "read host page size");
        Fixture fixture;
        tds_wal_advice_snapshot initial;
        tds_wal_advice_take(&initial);
        std::array<Sample, 5> samples{};
        for (auto &sample : samples)
        {
            sample = measure(fixture, std::string_view(argv[1]) == "saturated");
        }
        std::printf("{\"scenario\":\"%s\",\"calls_per_sample\":%" PRIu64
                    ",\"advice_stub\":true,\"samples\":[",
                    argv[1], calls);
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            const auto &sample = samples[index];
            std::printf("%s{\"ns_per_call\":%.6f,\"verified_calls\":%" PRIu64
                        ",\"verified_errors\":%" PRIu64 ",\"verified_bytes\":%" PRIu64
                        ",\"retained_events\":%" PRIu64 ",\"dropped_events\":%" PRIu64 "}",
                        index ? "," : "", sample.ns_per_call, sample.verified_calls,
                        sample.verified_errors, sample.verified_bytes, sample.retained_events,
                        sample.dropped_events);
        }
        std::puts("]}");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
