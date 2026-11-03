#include "telemetry/database_snapshot.hpp"
#include "telemetry/io_snapshot.hpp"
#include "telemetry/output.hpp"

#include <yyjson.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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
        history.push(index);
    }
    require(history.size() == history.capacity && history.overwritten() == 17,
            "history did not bound storage or account overwritten windows");
    for (std::size_t index = 0; index < history.size(); ++index)
    {
        require(history.at(index) == index + 17, "history lost chronological order after wrap");
    }
    tds::timing::Histogram first, second;
    first.record(0);
    first.record(1);
    second.record(8);
    first.merge(second);
    require(first.count == 3 && first.total == 9 && first.max == 8 && first.buckets[0] == 2 &&
                first.buckets[3] == 1,
            "merged histogram changed sample counts or bucket bounds");
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
    windows.push(totals);
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
