#include "telemetry/database_snapshot.hpp"
#include "telemetry/io_snapshot.hpp"
#include "telemetry/output.hpp"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace
{
using namespace tds::telemetry;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Release
{
    void operator()(yyjson_doc *document) const noexcept
    {
        yyjson_doc_free(document);
    }
};

using Document = std::unique_ptr<yyjson_doc, Release>;

class Destination
{
  public:
    Destination()
    {
        std::strcpy(path_.data(), "/tmp/tds-telemetry-XXXXXX");
        const int descriptor = ::mkstemp(path_.data());
        require(descriptor >= 0, "create telemetry destination");
        ::close(descriptor);
        const auto *previous = std::getenv("TDS_TELEMETRY_PATH");
        if (previous)
        {
            previous_ = previous;
            had_previous_ = true;
        }
        require(::setenv("TDS_TELEMETRY_PATH", path_.data(), 1) == 0, "set telemetry destination");
    }

    ~Destination()
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

    Destination(const Destination &) = delete;
    Destination &operator=(const Destination &) = delete;

    std::vector<Document> read() const
    {
        std::ifstream stream(path_.data());
        std::vector<Document> documents;
        std::string line;
        while (std::getline(stream, line))
        {
            Document document(yyjson_read(line.data(), line.size(), 0));
            require(document != nullptr, "exported line is invalid JSON");
            documents.push_back(std::move(document));
        }

        return documents;
    }

  private:
    std::array<char, 64> path_{};
    std::string previous_;
    bool had_previous_ = false;
};

yyjson_val *member(yyjson_val *object, const char *key)
{
    auto *value = yyjson_obj_get(object, key);
    require(value != nullptr, "missing telemetry field");

    return value;
}

void check_history_and_histograms()
{
    History<std::uint64_t> history;
    for (std::uint64_t index = 0; index < history.capacity + 17; ++index)
    {
        history.next() = index;
    }
    require(history.size() == history.capacity && history.overwritten() == 17,
            "history did not bound storage or account overwritten windows");
    for (std::size_t index = 0; index < history.size(); ++index)
    {
        require(history.at(index) == index + 17, "history lost chronological order after wrap");
    }
    tds::timing::Histogram first, second;
    require(first.count() == 0 && first.total == 0 && first.max == 0,
            "empty histogram fabricated samples");
    first.record(0);
    first.record(1);
    second.record(8);
    first.merge(second);
    require(first.count() == 3 && first.total == 9 && first.max == 8 && first.buckets[0] == 2 &&
                first.buckets[3] == 1,
            "merged histogram changed sample counts or bucket bounds");
    first.reset();
    require(first.count() == 0 && first.total == 0 && first.max == 0 &&
                first.buckets == std::array<std::uint64_t, 64>{},
            "histogram reset retained values");
    for (unsigned exponent = 0; exponent < 64; ++exponent)
    {
        first.reset();
        const auto boundary = std::uint64_t{1} << exponent;
        first.record(boundary - 1);
        first.record(boundary);
        require(first.count() == 2 && first.total == boundary + boundary - 1 &&
                    first.max == boundary && first.buckets[exponent] == (exponent == 0 ? 2U : 1U) &&
                    (exponent == 0 || first.buckets[exponent - 1] == 1),
                "power-of-two histogram boundary changed");
    }
    first.reset();
    first.record(std::numeric_limits<std::uint64_t>::max());
    first.record(1);
    require(first.count() == 2 && first.total == 0 &&
                first.max == std::numeric_limits<std::uint64_t>::max() && first.buckets[0] == 1 &&
                first.buckets[63] == 1,
            "maximum histogram value or unsigned total wrap changed");
    first.reset();
    second.reset();
    first.record(0);
    second.record(2);
    for (unsigned doubling = 0; doubling < 63; ++doubling)
    {
        first.merge(first);
        second.merge(second);
    }
    require(first.count() == (std::uint64_t{1} << 63) && first.total == 0 && second.total == 0,
            "large histogram counts or totals lost unsigned precision");
    first.merge(second);
    require(first.count() == 0 && first.max == 2 && first.buckets[0] == (std::uint64_t{1} << 63) &&
                first.buckets[1] == (std::uint64_t{1} << 63),
            "derived count did not preserve unsigned wrap across different buckets");
    first.record(3);
    first.merge(first);
    require(first.count() == 2 && first.buckets[0] == 0 && first.buckets[1] == 2 &&
                first.total == 6 && first.max == 3,
            "recording or merging a wrapped histogram changed its aggregates");
}

struct CountedSnapshot
{
    static inline std::size_t constructions = 0;
    static inline std::size_t copies = 0;
    static inline std::size_t moves = 0;
    static inline std::size_t assignments = 0;
    static inline std::size_t destructions = 0;

    CountedSnapshot() noexcept
    {
        ++constructions;
    }

    CountedSnapshot(const CountedSnapshot &other) noexcept : value(other.value)
    {
        ++copies;
    }

    CountedSnapshot(CountedSnapshot &&other) noexcept : value(other.value)
    {
        ++moves;
    }

    CountedSnapshot &operator=(const CountedSnapshot &other) noexcept
    {
        value = other.value;
        ++assignments;

        return *this;
    }

    ~CountedSnapshot()
    {
        ++destructions;
    }

    std::uint64_t value = 0;
};

void check_lazy_history_slots()
{
    using CountedHistory = History<CountedSnapshot>;
    static_assert(!std::is_copy_constructible_v<CountedHistory> &&
                  !std::is_copy_assignable_v<CountedHistory>);
    static_assert(!std::is_move_constructible_v<CountedHistory> &&
                  !std::is_move_assignable_v<CountedHistory>);
    static_assert(!std::is_nothrow_default_constructible_v<CountedHistory>);
    {
        CountedHistory history;
        require(history.size() == 0 && history.overwritten() == 0 &&
                    CountedSnapshot::constructions == 0,
                "empty history constructed snapshots before their first capture");
        std::array<CountedSnapshot *, CountedHistory::capacity> addresses{};
        constexpr std::size_t windows = CountedHistory::capacity * 3 + 17;
        for (std::size_t index = 0; index < windows; ++index)
        {
            auto &slot = history.next();
            const auto position = index % CountedHistory::capacity;
            if (index < CountedHistory::capacity)
            {
                require(slot.value == 0 && CountedSnapshot::constructions == index + 1,
                        "first capture did not construct exactly one initialized slot");
                addresses[position] = &slot;
            }
            else
            {
                require(&slot == addresses[position] &&
                            slot.value == index - CountedHistory::capacity + 1 &&
                            CountedSnapshot::constructions == CountedHistory::capacity,
                        "wrapped history moved, reconstructed or reset a retained slot");
            }
            slot.value = index + 1;
            const auto retained = std::min(index + 1, CountedHistory::capacity);
            require(history.size() == retained && history.overwritten() == index + 1 - retained,
                    "growing history changed its retained or overwritten counts");
            for (std::size_t chronological = 0; chronological < retained; ++chronological)
            {
                const auto original = index + 1 - retained + chronological;
                require(&history.at(chronological) ==
                                addresses[original % CountedHistory::capacity] &&
                            history.at(chronological).value == original + 1,
                        "history growth or wrap changed slot addresses or chronology");
            }
        }
        require(CountedSnapshot::copies == 0 && CountedSnapshot::moves == 0 &&
                    CountedSnapshot::assignments == 0 && CountedSnapshot::destructions == 0,
                "direct history capture copied, moved or destroyed a snapshot");
    }
    require(CountedSnapshot::destructions == CountedHistory::capacity,
            "history did not destroy exactly its constructed snapshots");
}

void check_transport_export()
{
    const Destination destination;
    IoSnapshot totals;
    totals.ring_count = 1;
    totals.end_ns = 20;
    totals.rings[0].counters.cqes = 9'007'199'254'740'993ULL;
    totals.timings.handler_wall_ns.record(9);
    write_report(IoReport{Level::off, totals});
    require(destination.read().empty(), "off mode created a telemetry record");
    write_report(IoReport{Level::basic, totals});
    auto records = destination.read();
    require(records.size() == 1, "basic export emitted multiple records");
    auto *root = yyjson_doc_get_root(records[0].get());
    require(std::string_view(yyjson_get_str(member(root, "mode"))) == "basic",
            "basic export mislabeled its mode");
    auto *basic = member(root, "totals");
    require(yyjson_is_null(member(basic, "start_ns")) &&
                yyjson_obj_get(basic, "metrics") == nullptr,
            "basic export fabricated timing data");
    auto *ring = yyjson_arr_get_first(member(basic, "rings"));
    require(yyjson_get_uint(member(ring, "cqes")) == totals.rings[0].counters.cqes,
            "64-bit counters lost precision during JSON export");
    History<IoSnapshot> windows;
    totals.start_ns = 10;
    windows.next() = totals;
    write_report(IoReport{Level::profile, totals, &windows, 17});
    records = destination.read();
    root = yyjson_doc_get_root(records.back().get());
    require(yyjson_get_uint(member(root, "overwritten_windows")) == 17 &&
                yyjson_arr_size(member(root, "windows")) == 1,
            "profile export lost bounded history metadata");
    auto *metrics = member(member(root, "totals"), "metrics");
    auto *handler = member(metrics, "handler_wall_ns");
    require(yyjson_get_uint(member(handler, "count")) == 1 &&
                yyjson_get_uint(member(handler, "total")) == 9,
            "profile export changed histogram values");
}

void check_database_export()
{
    const Destination destination;
    DatabaseSnapshot totals;
    totals.time_ns = 30;
    totals.interval_ns = 20;
    totals.counters.commits = 2;
    totals.wal = {true, 100, 90, 10, 1};
    totals.timings.commit.record(12);
    const std::array events{
        SlowSyncEvent{SyncOperation::fsync, SyncRole::issuer, 10, 30, 20, 0, false, -1, EIO}};
    const std::array<tds_wal_advice_event, 1> advice{{{20, 4096, 8192, EINVAL}}};
    const DatabaseReport report{Level::profile, "database\"\\\n", 10, 30,     totals,
                                nullptr,        events,           3,  advice, 4};
    write_report(report);
    const auto records = destination.read();
    require(records.size() == 1, "escaped text split a telemetry record");
    auto *root = yyjson_doc_get_root(records.front().get());
    require(std::string_view(yyjson_get_str(member(root, "source"))) == report.source,
            "serializer did not escape source text");
    require(yyjson_get_uint(member(root, "dropped_events")) == 7,
            "event-loss metadata was not preserved");
    auto *sync = yyjson_arr_get_first(member(root, "slow_sync_events"));
    require(yyjson_get_sint(member(sync, "rc")) == -1 &&
                yyjson_get_uint(member(sync, "errno")) == EIO &&
                yyjson_is_null(member(sync, "thread_cpu_ns")),
            "failed sync or unavailable CPU clock changed during export");
    auto *wal = member(member(root, "totals"), "wal");
    require(yyjson_get_uint(member(wal, "backlog")) == 10, "WAL snapshot changed during export");
}
} // namespace

int main()
{
    try
    {
        check_history_and_histograms();
        check_lazy_history_slots();
        check_transport_export();
        check_database_export();
        std::puts("Bounded history, histogram merge and typed JSON export passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());

        return 1;
    }
}
