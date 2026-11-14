#pragma once

#include "../timing.hpp"
#include "config.hpp"
#include "history.hpp"
#include "wal_advice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tds::telemetry
{

enum class SyncOperation : std::size_t
{
    fsync,
    fdatasync
};

enum class SyncRole
{
    issuer,
    checkpoint,
    other
};

struct SyncMetrics
{
    std::uint64_t calls = 0;
    std::uint64_t errors = 0;
    std::uint64_t cpu_clock_failures = 0;
    std::uint64_t wall_clock_failures = 0;
    timing::Histogram wall;
    timing::Histogram thread_cpu;
};

using SyncCounters = std::array<SyncMetrics, 2>;

struct SlowSyncEvent
{
    SyncOperation operation{};
    SyncRole role{};
    std::uint64_t start_ns = 0;
    std::uint64_t end_ns = 0;
    std::uint64_t wall_ns = 0;
    std::uint64_t thread_cpu_ns = 0;
    bool cpu_available = false;
    int result = 0;
    int error = 0;
};

struct DatabaseCounters
{
    std::uint64_t feed = 0;
    std::uint64_t post = 0;
    std::uint64_t create = 0;
    std::uint64_t like = 0;
    std::uint64_t commits = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t write_batches = 0;
    std::uint64_t write_commands = 0;
    std::uint64_t write_failures = 0;
    std::uint64_t checkpoint_busy = 0;
    std::uint64_t checkpoint_failures = 0;
};

struct WalSnapshot
{
    bool valid = false;
    std::uint64_t frames = 0;
    std::uint64_t checkpointed = 0;
    std::uint64_t backlog = 0;
    std::uint64_t observed_resets = 0;
};

struct WalAdviceCounters
{
    std::uint64_t calls = 0;
    std::uint64_t errors = 0;
    std::uint64_t requested_bytes = 0;
};

struct DatabaseTimings
{
    timing::Histogram feed;
    timing::Histogram post;
    timing::Histogram create;
    timing::Histogram like;
    timing::Histogram commit;
    timing::Histogram checkpoint;
    timing::Histogram wal_backlog;
    SyncCounters sync;
};

struct DatabaseSnapshot
{
    std::uint64_t time_ns = 0;
    std::uint64_t interval_ns = 0;
    DatabaseCounters counters;
    WalSnapshot wal;
    WalAdviceCounters wal_advice;
    DatabaseTimings timings;
};

struct DatabaseReport
{
    Level mode;
    const char *source;
    std::uint64_t started_ns;
    std::uint64_t finished_ns;
    const DatabaseSnapshot &totals;
    const History<DatabaseSnapshot> *windows = nullptr;
    std::span<const SlowSyncEvent> slow_sync_events;
    std::uint64_t slow_sync_dropped = 0;
    std::span<const tds_wal_advice_event> wal_advice_events;
    std::uint64_t wal_advice_dropped = 0;
};

} // namespace tds::telemetry
