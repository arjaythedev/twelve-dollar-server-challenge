#include "diagnostics/database.hpp"
#include "diagnostics/io.hpp"
#include "net/reactor.hpp"

#include <array>
#include <charconv>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <time.h>
#include <unistd.h>

namespace
{
using namespace tds::telemetry;
static_assert(profiling);
constexpr std::size_t capacity = History<IoSnapshot>::capacity;
constexpr std::uint64_t captures = 20'000;
constexpr std::size_t sample_count = 5;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint64_t cpu_ns()
{
    timespec value{};
    require(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, "read probe CPU clock");

    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t resident_bytes()
{
    auto *stream = std::fopen("/proc/self/statm", "r");
    require(stream != nullptr, "open process resident memory");
    unsigned long virtual_pages = 0;
    unsigned long resident_pages = 0;
    const auto fields = std::fscanf(stream, "%lu %lu", &virtual_pages, &resident_pages);
    std::fclose(stream);
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    require(fields == 2 && page_size > 0, "read process resident memory");

    return resident_pages * static_cast<std::uint64_t>(page_size);
}

template <typename Snapshot, typename Fill> void append(History<Snapshot> &history, Fill fill)
{
    if constexpr (requires { history.next(); })
    {
        fill(history.next());
    }
    else
    {
        Snapshot snapshot;
        fill(snapshot);
        history.push(snapshot);
    }
}

void startup(std::size_t windows)
{
    // Prime the proc reader before recording the history-only resident delta
    static_cast<void>(resident_bytes());
    const auto resident_before = resident_bytes();
    const auto construction_started = cpu_ns();
    History<IoSnapshot> io;
    History<DatabaseSnapshot> database;
    History<DatabaseSnapshot> checkpoint;
    const auto construction_cpu = cpu_ns() - construction_started;
    const auto resident_constructed = resident_bytes();
    const auto fill_started = cpu_ns();
    for (std::size_t index = 0; index < windows; ++index)
    {
        append(io, [index](IoSnapshot &snapshot) {
            snapshot.start_ns = index;
            snapshot.end_ns = index + 1;
            snapshot.ring_count = io_ring_capacity;
            snapshot.timings.handler_wall_ns.record(index + 1);
            for (std::size_t ring = 0; ring < snapshot.ring_count; ++ring)
            {
                snapshot.rings[ring].ring = ring;
                snapshot.rings[ring].counters.cqes = index + ring + 1;
                snapshot.rings[ring].timings.cq_ready_before_drain.record(index + ring + 1);
            }
        });
        for (auto *history : {&database, &checkpoint})
        {
            append(*history, [index](DatabaseSnapshot &snapshot) {
                snapshot.time_ns = index + 1;
                snapshot.interval_ns = 1;
                snapshot.counters.commits = index + 1;
                snapshot.timings.commit.record(index + 1);
            });
        }
    }
    const auto fill_cpu = cpu_ns() - fill_started;
    const auto resident_filled = resident_bytes();
    std::uint64_t checksum = 0;
    require(io.size() == windows && database.size() == windows && checkpoint.size() == windows &&
                io.overwritten() == 0 && database.overwritten() == 0 &&
                checkpoint.overwritten() == 0,
            "startup probe changed history sizes or loss accounting");
    for (std::size_t index = 0; index < windows; ++index)
    {
        const auto &snapshot = io.at(index);
        require(snapshot.start_ns == index && snapshot.end_ns == index + 1 &&
                    snapshot.ring_count == io_ring_capacity &&
                    snapshot.timings.handler_wall_ns.count() == 1 &&
                    snapshot.timings.handler_wall_ns.total == index + 1,
                "startup probe lost an IO window");
        for (std::size_t ring = 0; ring < snapshot.ring_count; ++ring)
        {
            const auto &value = snapshot.rings[ring];
            require(value.ring == ring && value.counters.cqes == index + ring + 1 &&
                        value.timings.cq_ready_before_drain.count() == 1 &&
                        value.timings.cq_ready_before_drain.total == index + ring + 1,
                    "startup probe lost initialized ring data");
            checksum += value.counters.cqes + value.timings.cq_ready_before_drain.total;
        }
        for (const auto *history : {&database, &checkpoint})
        {
            const auto &value = history->at(index);
            require(value.time_ns == index + 1 && value.interval_ns == 1 &&
                        value.counters.commits == index + 1 && value.timings.commit.count() == 1 &&
                        value.timings.commit.total == index + 1,
                    "startup probe lost a database window");
            checksum += value.counters.commits + value.timings.commit.total;
        }
    }
    std::printf("{\"mode\":\"startup\",\"windows\":%zu,\"histories\":3,"
                "\"construction_cpu_ns\":%" PRIu64 ",\"first_fill_cpu_ns\":%" PRIu64
                ",\"resident_before_bytes\":%" PRIu64 ",\"resident_constructed_bytes\":%" PRIu64
                ",\"resident_filled_bytes\":%" PRIu64 ",\"io_snapshot_bytes\":%zu,"
                "\"database_snapshot_bytes\":%zu,\"checksum\":%" PRIu64 "}\n",
                windows, construction_cpu, fill_cpu, resident_before, resident_constructed,
                resident_filled, sizeof(IoSnapshot), sizeof(DatabaseSnapshot), checksum);
}

void capture()
{
    require(::setenv("TDS_TELEMETRY_PATH", "/dev/null", 1) == 0, "set probe export destination");
    tds::net::Reactor reactor("127.0.0.1", 0);
    auto &ring = *reactor.rings().front();
    IoTelemetry io;
    const auto run = io.run(reactor);
    DatabaseTelemetry database("database");
    DatabaseTelemetry checkpoint("checkpoint");
    IoTelemetry::drain_finished(ring, 7);
    ring.statistics.interval.cq_ready_before_drain.record(3);
    {
        const auto batch = io.write_batch(5);
        const auto operation = database.measure(DatabaseOperation::feed);
        const auto checkpoint_operation = checkpoint.measure(DatabaseOperation::checkpoint);
    }
    database.write_batch(11);
    checkpoint.write_batch(13);
    auto now = tds::timing::now_ns();
    for (std::size_t index = 0; index < capacity; ++index)
    {
        io.report(reactor, true);
        now += tds::timing::report_interval_ns;
        database.record_window(now);
        checkpoint.record_window(now);
    }
    require(io.history().size() == capacity && database.windows().size() == capacity &&
                checkpoint.windows().size() == capacity,
            "capture probe did not saturate all histories");
    std::array<std::array<double, 2>, sample_count> samples{};
    for (std::size_t sample = 0; sample < sample_count; ++sample)
    {
        for (std::size_t offset = 0; offset < 2; ++offset)
        {
            const auto scenario = sample % 2 ? 1 - offset : offset;
            const auto started = cpu_ns();
            if (scenario == 0)
            {
                for (std::uint64_t index = 0; index < captures; ++index)
                {
                    io.report(reactor, true);
                }
            }
            else
            {
                for (std::uint64_t index = 0; index < captures; ++index)
                {
                    now += tds::timing::report_interval_ns;
                    database.record_window(now);
                    checkpoint.record_window(now);
                }
            }
            const auto measured_captures = captures * (scenario == 0 ? 1 : 2);
            samples[sample][scenario] =
                static_cast<double>(cpu_ns() - started) / static_cast<double>(measured_captures);
        }
    }
    const auto expected = captures * sample_count;
    for (const auto *history : {&database.windows(), &checkpoint.windows()})
    {
        require(history->size() == capacity && history->overwritten() == expected &&
                    history->at(capacity - 1).time_ns == now &&
                    history->at(0).time_ns ==
                        now - (capacity - 1) * tds::timing::report_interval_ns,
                "capture probe lost database windows, chronology or overwrite counts");
    }
    require(io.history().size() == capacity && io.history().overwritten() == expected &&
                io.totals().rings[0].counters.cqes == 7 &&
                io.totals().rings[0].timings.cq_ready_before_drain.count() == 1 &&
                io.totals().rings[0].timings.cq_ready_before_drain.total == 3 &&
                io.totals().timings.write_batch_size.count() == 1 &&
                io.totals().timings.write_batch_size.total == 5 &&
                io.totals().timings.write_batch_wall_ns.count() == 1,
            "capture probe lost IO windows or cumulative counters and timings");
    for (std::size_t index = 0; index < capacity; ++index)
    {
        const auto &window = io.history().at(index);
        require(window.ring_count == 1 && window.rings[0].ring == 0 &&
                    window.rings[0].counters.cqes == 0 &&
                    window.rings[0].timings.cq_ready_before_drain.count() == 0 &&
                    window.timings.write_batch_size.count() == 0 &&
                    window.end_ns >= window.start_ns &&
                    (index == 0 || window.start_ns == io.history().at(index - 1).end_ns),
                "capture probe changed ring gauges, interval reset or IO chronology");
    }
    const auto totals = database.snapshot();
    const auto checkpoint_totals = checkpoint.snapshot();
    require(totals.counters.feed == 1 && totals.counters.write_batches == 1 &&
                totals.counters.write_commands == 11 && totals.timings.feed.count() == 1 &&
                checkpoint_totals.counters.checkpoints == 1 &&
                checkpoint_totals.counters.write_batches == 1 &&
                checkpoint_totals.counters.write_commands == 13 &&
                checkpoint_totals.timings.checkpoint.count() == 1,
            "capture probe changed cumulative database work or timing counts");
    std::printf("{\"mode\":\"capture\",\"captures_per_sample\":%" PRIu64
                ",\"verified_io_captures\":%" PRIu64 ",\"verified_database_captures\":%" PRIu64
                ",\"verified_cqes\":7,\"verified_database_operations\":2,"
                "\"verified_write_commands\":24,\"history_capacity\":%zu,\"samples\":[",
                captures, expected, expected * 2, capacity);
    for (std::size_t sample = 0; sample < samples.size(); ++sample)
    {
        std::printf("%s{\"io_ns_per_capture\":%.6f,\"database_ns_per_capture\":%.6f}",
                    sample ? "," : "", samples[sample][0], samples[sample][1]);
    }
    std::puts("]}");
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "capture")
        {
            capture();
        }
        else
        {
            require(argc == 3 && std::string_view(argv[1]) == "startup",
                    "usage: history_probe startup <0..128> | capture");
            const std::string_view text(argv[2]);
            std::size_t windows = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), windows);
            require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                        windows <= capacity,
                    "startup windows must be an integer from 0 through 128");
            startup(windows);
        }

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());

        return 1;
    }
}
