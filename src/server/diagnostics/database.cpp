#include "database.hpp"

#if TDS_TELEMETRY_LEVEL >= 1

#include "../telemetry/output.hpp"

#include <sqlite3.h>

#include <algorithm>

namespace tds::telemetry
{

#if TDS_TELEMETRY_LEVEL >= 2
namespace
{
void merge(telemetry::DatabaseTimings &target, const telemetry::DatabaseTimings &source) noexcept
{
    using Timings = telemetry::DatabaseTimings;
    for (const auto field : {&Timings::feed, &Timings::post, &Timings::create, &Timings::like,
                             &Timings::commit, &Timings::checkpoint, &Timings::wal_backlog})
    {
        (target.*field).merge(source.*field);
    }
    for (std::size_t index = 0; index < target.sync.size(); ++index)
    {
        auto &to = target.sync[index];
        const auto &from = source.sync[index];
        to.calls += from.calls;
        to.errors += from.errors;
        to.cpu_clock_failures += from.cpu_clock_failures;
        to.wall_clock_failures += from.wall_clock_failures;
        to.wall.merge(from.wall);
        to.thread_cpu.merge(from.thread_cpu);
    }
}
} // namespace
#endif

DatabaseTelemetry::~DatabaseTelemetry()
{
    auto now = timing::now_ns();
#if TDS_TELEMETRY_LEVEL >= 2
    now = std::max(now, last_report_ns_);
    flush_window(now, true);
    const auto totals = snapshot();
    telemetry::write_report({telemetry::level,
                             source_,
                             started_ns_,
                             now,
                             totals,
                             &history_,
                             slow_sync_.events(),
                             slow_sync_.dropped(),
                             {advice_events_.data(), static_cast<std::size_t>(advice_event_count_)},
                             advice_dropped_});
#else
    auto totals = snapshot();
    totals.time_ns = now;
    telemetry::write_report({telemetry::level, source_, 0, now, totals, nullptr, {}, 0, {}, 0});
#endif
}

#if TDS_TELEMETRY_LEVEL >= 2
DatabaseTelemetry::Scope::Scope(DatabaseTelemetry &owner, DatabaseOperation operation) noexcept
    : owner_(owner), operation_(operation), started_ns_(timing::now_ns())
{
    owner.count(operation);
    if (operation == DatabaseOperation::commit || operation == DatabaseOperation::checkpoint)
    {
        sync_.emplace(owner.interval_timing_.sync, owner.slow_sync_,
                      operation == DatabaseOperation::commit ? telemetry::SyncRole::issuer
                                                             : telemetry::SyncRole::checkpoint);
    }
}

DatabaseTelemetry::Scope::~Scope()
{
    // Restore outer sync attribution before ending the enclosing operation
    sync_.reset();
    owner_.histogram(operation_).record(timing::now_ns() - started_ns_);
}

timing::Histogram &DatabaseTelemetry::histogram(DatabaseOperation operation) noexcept
{
    switch (operation)
    {
    case DatabaseOperation::feed:
        return interval_timing_.feed;
    case DatabaseOperation::post:
        return interval_timing_.post;
    case DatabaseOperation::create:
        return interval_timing_.create;
    case DatabaseOperation::like:
        return interval_timing_.like;
    case DatabaseOperation::commit:
        return interval_timing_.commit;
    case DatabaseOperation::checkpoint:
        return interval_timing_.checkpoint;
    }
    std::unreachable();
}

void DatabaseTelemetry::record_window(std::uint64_t now_ns) noexcept
{
    flush_window(now_ns, false);
}

void DatabaseTelemetry::flush_window(std::uint64_t now_ns, bool final) noexcept
{
    if (now_ns < last_report_ns_ ||
        (!final && now_ns - last_report_ns_ < timing::report_interval_ns))
    {
        return;
    }
    history_.push(
        {now_ns, now_ns - last_report_ns_, interval_, wal_, interval_advice_, interval_timing_});
    merge(cumulative_timing_, interval_timing_);
    interval_ = {};
    interval_advice_ = {};
    interval_timing_ = {};
    last_report_ns_ = now_ns;
}
#endif

telemetry::DatabaseSnapshot DatabaseTelemetry::snapshot() const noexcept
{
    telemetry::DatabaseSnapshot result;
    result.counters = totals_;
    result.wal = wal_;
    result.wal_advice = wal_advice_;
#if TDS_TELEMETRY_LEVEL >= 2
    result.time_ns = last_report_ns_;
    result.interval_ns = last_report_ns_ - started_ns_;
    result.timings = cumulative_timing_;
    merge(result.timings, interval_timing_);
#endif
    return result;
}

void DatabaseTelemetry::checkpoint_completed(int result, int frames, int checkpointed) noexcept
{
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
    {
        ++totals_.checkpoint_busy;
#if TDS_TELEMETRY_LEVEL >= 2
        ++interval_.checkpoint_busy;
#endif
    }
    else if (result != SQLITE_OK)
    {
        ++totals_.checkpoint_failures;
#if TDS_TELEMETRY_LEVEL >= 2
        ++interval_.checkpoint_failures;
#endif
    }
    if (frames >= 0 && checkpointed >= 0 && checkpointed <= frames)
    {
        // Physical file size can survive a reset, keep paired logical counts
        if (wal_.valid && static_cast<std::uint64_t>(frames) < wal_.frames)
        {
            ++wal_.observed_resets;
        }
        wal_.valid = true;
        wal_.frames = static_cast<std::uint64_t>(frames);
        wal_.checkpointed = static_cast<std::uint64_t>(checkpointed);
        wal_.backlog = static_cast<std::uint64_t>(frames - checkpointed);
#if TDS_TELEMETRY_LEVEL >= 2
        interval_timing_.wal_backlog.record(wal_.backlog);
#endif
    }
    report();
}

void DatabaseTelemetry::wal_advice_completed(const tds_wal_advice_snapshot &snapshot) noexcept
{
    wal_advice_.calls += snapshot.calls;
    wal_advice_.errors += snapshot.errors;
    wal_advice_.requested_bytes += snapshot.requested_bytes;
#if TDS_TELEMETRY_LEVEL >= 2
    interval_advice_.calls += snapshot.calls;
    interval_advice_.errors += snapshot.errors;
    interval_advice_.requested_bytes += snapshot.requested_bytes;
    advice_dropped_ += snapshot.dropped_events;
    for (std::uint64_t index = 0; index < snapshot.event_count; ++index)
    {
        if (advice_event_count_ < advice_events_.size())
        {
            advice_events_[static_cast<std::size_t>(advice_event_count_++)] =
                snapshot.events[static_cast<std::size_t>(index)];
        }
        else
        {
            ++advice_dropped_;
        }
    }
#endif
}

} // namespace tds::telemetry

#endif
