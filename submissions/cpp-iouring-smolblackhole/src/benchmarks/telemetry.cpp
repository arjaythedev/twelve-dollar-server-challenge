#include "diagnostics/database.hpp"
#include "diagnostics/io.hpp"
#include "http/exchange.hpp"
#include "net/reactor.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <time.h>
#include <utility>

namespace
{
static_assert(tds::telemetry::profiling);
constexpr std::uint64_t calls = 500'000;
constexpr std::array names{"io_ns_per_call",
                           "database_ns_per_call",
                           "handler_active_ns_per_call",
                           "handler_write_ns_per_call",
                           "receive_ns_per_call",
                           "database_scope_ns_per_call",
                           "database_sync_scope_ns_per_call",
                           "histogram_record_ns_per_call"};
constexpr std::array<std::uint64_t, 16> histogram_values{0,
                                                         1,
                                                         2,
                                                         3,
                                                         4,
                                                         7,
                                                         8,
                                                         15,
                                                         16,
                                                         31,
                                                         std::uint64_t{1} << 20,
                                                         std::uint64_t{1} << 31,
                                                         std::uint64_t{1} << 32,
                                                         std::uint64_t{1} << 48,
                                                         std::uint64_t{1} << 63,
                                                         std::numeric_limits<std::uint64_t>::max()};
constexpr std::array<std::size_t, 16> histogram_buckets{0, 0, 1,  1,  2,  2,  3,  3,
                                                        4, 4, 20, 31, 32, 48, 63, 63};
static_assert(calls % histogram_values.size() == 0);
using Sample = std::array<double, names.size()>;

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

template <class Work> double measure(Work &&work)
{
    const auto started = cpu_ns();
    for (std::uint64_t call = 0; call < calls; ++call)
    {
        work(call);
    }

    return static_cast<double>(cpu_ns() - started) / static_cast<double>(calls);
}

// Keep per-call stores visible in both variants instead of benchmarking loop hoisting
[[gnu::noinline]] void record_histogram(tds::timing::Histogram &histogram,
                                        std::uint64_t value) noexcept
{
    histogram.record(value);
}
} // namespace

int main()
{
    try
    {
        // Final export is outside the timed loops and discarded by this probe
        require(::setenv("TDS_TELEMETRY_PATH", "/dev/null", 1) == 0,
                "set probe export destination");
        tds::net::Reactor reactor("127.0.0.1", 0);
        auto &ring = *reactor.rings().front();
        auto &connection = ring.connections.create(ring, ring.connections.reserve());
        connection.exchange = ring.acquire_exchange();
        tds::telemetry::IoTelemetry io;
        const auto run = io.run(reactor);
        io.receive().attach(connection);
        tds::telemetry::DatabaseTelemetry database("database");
        const auto accept = [&](tds::AcceptedHead head) {
            auto &session = connection.exchange->session;
            session.reset();
            session.append("GET /health HTTP/1.1\r\nHost: local\r\n\r\n");
            require(session.consume().valid && session.headers_pending(), "parse probe headers");
            session.accept_headers(std::move(head));
            require(session.consume().valid && session.complete(), "complete probe request");
        };
        for (unsigned index = 0; index < 5000; ++index)
        {
            io.report(reactor);
            database.report();
        }
        std::array<Sample, 5> samples{};
        tds::timing::Histogram histogram;
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            for (std::size_t offset = 0; offset < names.size(); ++offset)
            {
                // Reverse scenario order to reduce a fixed warmup/frequency bias
                const auto scenario = index % 2 ? names.size() - 1 - offset : offset;
                auto &value = samples[index][scenario];
                switch (scenario)
                {
                case 0:
                    value = measure([&](std::uint64_t) {
                        io.report(reactor);
                    });
                    break;
                case 1:
                    value = measure([&](std::uint64_t) {
                        database.report();
                    });
                    break;
                case 2:
                    accept(tds::HealthHead{});
                    value = measure([&](std::uint64_t) {
                        auto sample = io.handler(connection);
                        sample.finish();
                    });
                    break;
                case 3:
                    accept(tds::CreateHead{});
                    value = measure([&](std::uint64_t) {
                        const auto sample = io.handler(connection);
                    });
                    break;
                case 4:
                    value = measure([&](std::uint64_t) {
                        io.receive().attach(connection);
                    });
                    break;
                case 5:
                    value = measure([&](std::uint64_t call) {
                        constexpr std::array operations{tds::telemetry::DatabaseOperation::feed,
                                                        tds::telemetry::DatabaseOperation::post,
                                                        tds::telemetry::DatabaseOperation::create,
                                                        tds::telemetry::DatabaseOperation::like};
                        const auto sample = database.measure(operations[call % operations.size()]);
                    });
                    break;
                case 6:
                    value = measure([&](std::uint64_t call) {
                        const auto operation = call % 2
                                                   ? tds::telemetry::DatabaseOperation::checkpoint
                                                   : tds::telemetry::DatabaseOperation::commit;
                        const auto sample = database.measure(operation);
                    });
                    break;
                case 7:
                    value = measure([&](std::uint64_t call) {
                        record_histogram(histogram,
                                         histogram_values[call % histogram_values.size()]);
                    });
                    break;
                }
            }
        }
        require(!io.history().size() && !database.windows().size(),
                "probe crossed a report deadline, discard the sample");
        const auto expected = calls * samples.size();
        const auto repetitions = expected / histogram_values.size();
        std::array<std::uint64_t, 64> expected_buckets{};
        std::uint64_t expected_total = 0;
        for (std::size_t index = 0; index < histogram_values.size(); ++index)
        {
            expected_buckets[histogram_buckets[index]] += repetitions;
            expected_total += histogram_values[index] * repetitions;
        }
        require(histogram.buckets == expected_buckets && histogram.count() == expected &&
                    histogram.total == expected_total &&
                    histogram.max == std::numeric_limits<std::uint64_t>::max(),
                "isolated histogram probe changed buckets, counts, unsigned totals or maximum");
        io.report(reactor, true);
        const auto &io_times = io.totals().timings;
        require(io_times.handler_wall_ns.count() == expected &&
                    io_times.receive_cqe_service_to_handler_start_ns.count() == expected &&
                    connection.exchange->receive_serviced_ns > 0,
                "probe changed active/write handler counts or lost receive timestamps");
        const auto db = database.snapshot();
        require(db.counters.feed == expected / 4 && db.counters.post == expected / 4 &&
                    db.counters.create == expected / 4 && db.counters.like == expected / 4 &&
                    db.counters.commits == expected / 2 &&
                    db.counters.checkpoints == expected / 2 &&
                    db.timings.feed.count() == expected / 4 &&
                    db.timings.post.count() == expected / 4 &&
                    db.timings.create.count() == expected / 4 &&
                    db.timings.like.count() == expected / 4 &&
                    db.timings.commit.count() == expected / 2 &&
                    db.timings.checkpoint.count() == expected / 2 &&
                    db.timings.sync[0].calls == 0 && db.timings.sync[1].calls == 0,
                "probe changed database operation counts or timing samples");
        std::printf("{\"calls_per_sample\":%" PRIu64 ",\"verified_handler_samples\":%" PRIu64
                    ",\"verified_database_samples\":%" PRIu64
                    ",\"verified_histogram_samples\":%" PRIu64 ",\"samples\":[",
                    calls, io_times.handler_wall_ns.count(), expected * 2, histogram.count());
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            std::fputs(index ? ",{" : "{", stdout);
            for (std::size_t scenario = 0; scenario < names.size(); ++scenario)
            {
                std::printf("%s\"%s\":%.6f", scenario ? "," : "", names[scenario],
                            samples[index][scenario]);
            }
            std::fputc('}', stdout);
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
