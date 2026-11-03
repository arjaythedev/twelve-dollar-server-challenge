#include "output.hpp"
#include "database_snapshot.hpp"
#include "io_snapshot.hpp"

#include <yyjson.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>

namespace tds::telemetry
{
namespace
{

class Json
{
  public:
    Json() : document_(yyjson_mut_doc_new(nullptr))
    {
        if (!document_)
        {
            throw std::bad_alloc();
        }
        root = object();
        yyjson_mut_doc_set_root(document_.get(), root);
    }

    yyjson_mut_val *object()
    {
        return allocated(yyjson_mut_obj(document_.get()));
    }

    yyjson_mut_val *array()
    {
        return allocated(yyjson_mut_arr(document_.get()));
    }

    void add(yyjson_mut_val *parent, const char *key, yyjson_mut_val *value)
    {
        check(yyjson_mut_obj_add_val(document_.get(), parent, key, value));
    }

    void append(yyjson_mut_val *parent, yyjson_mut_val *value)
    {
        check(yyjson_mut_arr_append(parent, value));
    }

    void number(yyjson_mut_val *parent, const char *key, std::uint64_t value)
    {
        check(yyjson_mut_obj_add_uint(document_.get(), parent, key, value));
    }

    void integer(yyjson_mut_val *parent, const char *key, int value)
    {
        check(yyjson_mut_obj_add_sint(document_.get(), parent, key, value));
    }

    void text(yyjson_mut_val *parent, const char *key, const char *value)
    {
        check(yyjson_mut_obj_add_strcpy(document_.get(), parent, key, value));
    }

    void null(yyjson_mut_val *parent, const char *key)
    {
        check(yyjson_mut_obj_add_null(document_.get(), parent, key));
    }

    void histogram(yyjson_mut_val *parent, const char *key, const timing::Histogram &values,
                   const char *unit = "ns")
    {
        auto *series = object();
        text(series, "unit", unit);
        number(series, "count", values.count);
        number(series, "total", values.total);
        number(series, "max", values.max);
        auto *buckets = array();
        for (std::size_t index = 0; index < values.buckets.size(); ++index)
        {
            if (!values.buckets[index])
            {
                continue;
            }
            auto *bucket = array();
            append(bucket, allocated(yyjson_mut_uint(document_.get(), index)));
            append(bucket, allocated(yyjson_mut_uint(document_.get(), values.buckets[index])));
            append(buckets, bucket);
        }
        add(series, "buckets", buckets);
        add(parent, key, series);
    }

    void write() const
    {
        std::size_t length = 0;
        const std::unique_ptr<char, Free> bytes(
            yyjson_mut_write(document_.get(), YYJSON_WRITE_NEWLINE_AT_END, &length));
        if (!bytes)
        {
            throw std::runtime_error("JSON encoding failed");
        }
        // The only shared lock belongs to final output, never to measured collection
        static std::mutex output_mutex;
        const std::lock_guard lock(output_mutex);
        const auto *destination = std::getenv("TDS_TELEMETRY_PATH");
        const std::unique_ptr<std::FILE, Close> file(
            std::fopen(destination ? destination : "telemetry.jsonl", "ab"));
        if (!file || std::fwrite(bytes.get(), 1, length, file.get()) != length ||
            std::fflush(file.get()) != 0)
        {
            throw std::runtime_error(std::strerror(errno));
        }
    }

    yyjson_mut_val *root = nullptr;

  private:
    struct Free
    {
        void operator()(char *value) const noexcept
        {
            std::free(value);
        }
    };

    struct Close
    {
        void operator()(std::FILE *value) const noexcept
        {
            std::fclose(value);
        }
    };

    struct Release
    {
        void operator()(yyjson_mut_doc *value) const noexcept
        {
            yyjson_mut_doc_free(value);
        }
    };

    static void check(bool success)
    {
        if (!success)
        {
            throw std::bad_alloc();
        }
    }

    static yyjson_mut_val *allocated(yyjson_mut_val *value)
    {
        check(value != nullptr);

        return value;
    }

    std::unique_ptr<yyjson_mut_doc, Release> document_;
};

void bounds(Json &json, yyjson_mut_val *object, std::uint64_t start, std::uint64_t end)
{
    if (start)
    {
        json.number(object, "start_ns", start);
        json.number(object, "interval_ns", end - start);
    }
    else
    {
        json.null(object, "start_ns");
        json.null(object, "interval_ns");
    }
    json.number(object, "end_ns", end);
}

yyjson_mut_val *snapshot(Json &json, const IoSnapshot &value, Level level)
{
    auto *object = json.object();
    bounds(json, object, value.start_ns, value.end_ns);
    if (level == Level::profile)
    {
        auto *metrics = json.object();
        json.histogram(metrics, "handler_wall_ns", value.timings.handler_wall_ns);
        json.histogram(metrics, "receive_cqe_service_to_handler_start_ns",
                       value.timings.receive_cqe_service_to_handler_start_ns);
        json.histogram(metrics, "write_batch_size", value.timings.write_batch_size, "requests");
        json.histogram(metrics, "write_batch_wall_ns", value.timings.write_batch_wall_ns);
        json.add(object, "metrics", metrics);
    }
    auto *rings = json.array();
    for (std::size_t index = 0; index < value.ring_count; ++index)
    {
        const auto &ring = value.rings[index];
        const auto &counts = ring.counters;
        auto *row = json.object();
        json.number(row, "ring", ring.ring);
        json.number(row, "cqes", counts.cqes);
        json.number(row, "receive_submissions", counts.receive_submissions);
        json.number(row, "receive_completions", counts.receive_completions);
        json.number(row, "receive_enobufs", counts.receive_enobufs);
        json.number(row, "cq_empty_samples", counts.empty_cq_samples);
        json.number(row, "cq_overflow_flag_samples", counts.overflow_flag_samples);
        json.number(row, "cq_overflow_counter_delta", counts.cq_overflow_counter_delta);
        json.number(row, "exchange_pool_hits", counts.exchange_pool_hits);
        json.number(row, "exchange_allocations", counts.exchange_allocations);
        json.number(row, "live_connection_records", ring.live_connection_records);
        json.number(row, "reserved_accept_slots", ring.reserved_accept_slots);
        json.number(row, "deferred_receive_queue_depth", ring.deferred_receive_queue_depth);
        json.number(row, "pooled_exchanges", ring.pooled_exchanges);
        if (level == Level::profile)
        {
            json.number(row, "deferred_receive_front_enobufs_age_ns",
                        ring.deferred_receive_front_enobufs_age_ns);
            auto *metrics = json.object();
            json.histogram(metrics, "cq_ready_before_drain", ring.timings.cq_ready_before_drain,
                           "cqes");
            json.histogram(metrics, "deferred_receive_queue_depth",
                           ring.timings.deferred_receive_queue_depth, "connections");
            json.histogram(metrics, "send_attempt_prepare_to_cqe_service_ns",
                           ring.timings.send_attempt_prepare_to_cqe_service_ns);
            json.histogram(metrics, "enobufs_to_successful_receive_service_ns",
                           ring.timings.enobufs_to_successful_receive_service_ns);
            json.add(row, "metrics", metrics);
        }
        json.append(rings, row);
    }
    json.add(object, "rings", rings);

    return object;
}

yyjson_mut_val *snapshot(Json &json, const DatabaseSnapshot &value, Level level)
{
    auto *object = json.object();
    bounds(json, object, value.interval_ns ? value.time_ns - value.interval_ns : 0, value.time_ns);
    const auto &counts = value.counters;
    auto *counters = json.object();
    json.number(counters, "feed", counts.feed);
    json.number(counters, "post", counts.post);
    json.number(counters, "create", counts.create);
    json.number(counters, "like", counts.like);
    json.number(counters, "commits", counts.commits);
    json.number(counters, "checkpoints", counts.checkpoints);
    json.number(counters, "write_batches", counts.write_batches);
    json.number(counters, "write_commands", counts.write_commands);
    json.number(counters, "write_failures", counts.write_failures);
    json.number(counters, "checkpoint_busy", counts.checkpoint_busy);
    json.number(counters, "checkpoint_failures", counts.checkpoint_failures);
    json.add(object, "counters", counters);
    if (value.wal.valid)
    {
        auto *wal = json.object();
        json.number(wal, "frames", value.wal.frames);
        json.number(wal, "checkpointed", value.wal.checkpointed);
        json.number(wal, "backlog", value.wal.backlog);
        json.number(wal, "observed_resets", value.wal.observed_resets);
        json.add(object, "wal", wal);
    }
    auto *advice = json.object();
    json.number(advice, "calls", value.wal_advice.calls);
    json.number(advice, "errors", value.wal_advice.errors);
    json.number(advice, "requested_bytes", value.wal_advice.requested_bytes);
    json.add(object, "wal_advice", advice);
    if (level == Level::profile)
    {
        const auto &times = value.timings;
        auto *metrics = json.object();
        json.histogram(metrics, "feed_db_and_json_ns", times.feed);
        json.histogram(metrics, "post_db_and_json_ns", times.post);
        json.histogram(metrics, "create_db_and_json_ns", times.create);
        json.histogram(metrics, "like_db_ns", times.like);
        json.histogram(metrics, "write_commit_ns", times.commit);
        json.histogram(metrics, "checkpoint_ns", times.checkpoint);
        json.histogram(metrics, "wal_uncheckpointed_frames", times.wal_backlog, "frames");
        constexpr std::array names{"fsync", "fdatasync"};
        auto *sync = json.object();
        for (std::size_t index = 0; index < times.sync.size(); ++index)
        {
            const auto &sample = times.sync[index];
            auto *row = json.object();
            json.number(row, "calls", sample.calls);
            json.number(row, "errors", sample.errors);
            json.number(row, "cpu_clock_failures", sample.cpu_clock_failures);
            json.number(row, "wall_clock_failures", sample.wall_clock_failures);
            json.histogram(row, "wall_ns", sample.wall);
            json.histogram(row, "thread_cpu_ns", sample.thread_cpu);
            json.add(sync, names[index], row);
        }
        json.add(metrics, "sync", sync);
        json.add(object, "metrics", metrics);
    }

    return object;
}

Json report_document(Level level, const char *source)
{
    Json json;
    json.number(json.root, "schema_version", 1);
    json.text(json.root, "mode", name(level));
    json.text(json.root, "source", source);

    return json;
}

void export_failure(const std::exception &error) noexcept
{
    std::fprintf(stderr, "telemetry export failed: %s\n", error.what());
}

} // namespace

void write_report(const IoReport &report) noexcept
{
    if (report.level == Level::off)
    {
        return;
    }
    try
    {
        auto json = report_document(report.level, "transport");
        json.add(json.root, "totals", snapshot(json, report.totals, report.level));
        auto *windows = json.array();
        if (report.history)
        {
            for (std::size_t index = 0; index < report.history->size(); ++index)
            {
                json.append(windows, snapshot(json, report.history->at(index), report.level));
            }
        }
        json.add(json.root, "windows", windows);
        json.number(json.root, "overwritten_windows", report.overwritten_history);
        json.number(json.root, "dropped_events", 0);
        json.write();
    }
    catch (const std::exception &error)
    {
        export_failure(error);
    }
}

void write_report(const DatabaseReport &report) noexcept
{
    if (report.mode == Level::off)
    {
        return;
    }
    try
    {
        auto json = report_document(report.mode, report.source);
        json.add(json.root, "totals", snapshot(json, report.totals, report.mode));
        bounds(json, json.root, report.started_ns, report.finished_ns);
        auto *windows = json.array();
        if (report.windows)
        {
            for (std::size_t index = 0; index < report.windows->size(); ++index)
            {
                json.append(windows, snapshot(json, report.windows->at(index), report.mode));
            }
        }
        json.add(json.root, "windows", windows);
        json.number(json.root, "overwritten_windows",
                    report.windows ? report.windows->overwritten() : 0);
        json.number(json.root, "dropped_events",
                    report.slow_sync_dropped + report.wal_advice_dropped);
        json.number(json.root, "slow_sync_dropped", report.slow_sync_dropped);
        json.number(json.root, "wal_advice_dropped", report.wal_advice_dropped);
        auto *events = json.array();
        for (const auto &event : report.slow_sync_events)
        {
            auto *row = json.object();
            json.text(row, "operation",
                      event.operation == SyncOperation::fsync ? "fsync" : "fdatasync");
            constexpr std::array roles{"issuer", "checkpoint", "other"};
            json.text(row, "role", roles[static_cast<std::size_t>(event.role)]);
            json.number(row, "start_ns", event.start_ns);
            json.number(row, "end_ns", event.end_ns);
            json.number(row, "wall_ns", event.wall_ns);
            if (event.cpu_available)
            {
                json.number(row, "thread_cpu_ns", event.thread_cpu_ns);
            }
            else
            {
                json.null(row, "thread_cpu_ns");
            }
            json.integer(row, "rc", event.result);
            json.integer(row, "errno", event.error);
            json.append(events, row);
        }
        json.add(json.root, "slow_sync_events", events);
        auto *advice = json.array();
        for (const auto &event : report.wal_advice_events)
        {
            auto *row = json.object();
            json.number(row, "time_ns", event.time_ns);
            json.number(row, "offset", event.offset);
            json.number(row, "length", event.length);
            json.integer(row, "rc", event.result);
            json.append(advice, row);
        }
        json.add(json.root, "wal_advice_events", advice);
        json.write();
    }
    catch (const std::exception &error)
    {
        export_failure(error);
    }
}

} // namespace tds::telemetry
