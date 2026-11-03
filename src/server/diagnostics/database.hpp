#pragma once

#include "sync.hpp"

#if TDS_TELEMETRY_LEVEL >= 2
#include <optional>
#endif

#include <utility>

namespace tds::telemetry
{

enum class DatabaseOperation
{
    feed,
    post,
    create,
    like,
    commit,
    checkpoint
};

class DatabaseTelemetry
{
  public:
    class Scope
    {
      public:
#if TDS_TELEMETRY_LEVEL >= 2
        Scope(DatabaseTelemetry &owner, DatabaseOperation operation) noexcept;
        ~Scope();
#elif TDS_TELEMETRY_LEVEL >= 1
        Scope(DatabaseTelemetry &owner, DatabaseOperation operation) noexcept
        {
            owner.count(operation);
        }
#else
        constexpr Scope(DatabaseTelemetry &, DatabaseOperation) noexcept
        {
        }
#endif
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

      private:
#if TDS_TELEMETRY_LEVEL >= 2
        DatabaseTelemetry &owner_;
        DatabaseOperation operation_;
        std::uint64_t started_ns_;
        std::optional<SyncScope> sync_;
#endif
    };

#if TDS_TELEMETRY_LEVEL >= 1
    explicit DatabaseTelemetry(const char *source) : source_(source)
    {
    }

    ~DatabaseTelemetry();

    void checkpoint_completed(int result, int frames, int checkpointed) noexcept;
    void wal_advice_completed(const tds_wal_advice_snapshot &snapshot) noexcept;
    telemetry::DatabaseSnapshot snapshot() const noexcept;

    void write_batch(std::uint64_t commands) noexcept
    {
        ++totals_.write_batches;
        totals_.write_commands += commands;
#if TDS_TELEMETRY_LEVEL >= 2
        ++interval_.write_batches;
        interval_.write_commands += commands;
#endif
    }

    void write_failed() noexcept
    {
        ++totals_.write_failures;
#if TDS_TELEMETRY_LEVEL >= 2
        ++interval_.write_failures;
#endif
    }
#else
    constexpr explicit DatabaseTelemetry(const char *) noexcept
    {
    }

    void checkpoint_completed(int, int, int) noexcept
    {
    }

    void wal_advice_completed(const tds_wal_advice_snapshot &) noexcept
    {
    }

    void write_batch(std::uint64_t) noexcept
    {
    }

    void write_failed() noexcept
    {
    }
#endif

#if TDS_TELEMETRY_LEVEL >= 2
    // Sampling stores a bounded interval, finalization owns all output
    void report() noexcept
    {
        record_window(timing::now_ns());
    }

    void record_window(std::uint64_t now_ns) noexcept;

    const telemetry::History<telemetry::DatabaseSnapshot> &windows() const noexcept
    {
        return history_;
    }
#else
    // Basic and off hot calls have no clocks, histories or output
    void report() noexcept
    {
    }
#endif

    DatabaseTelemetry(const DatabaseTelemetry &) = delete;
    DatabaseTelemetry &operator=(const DatabaseTelemetry &) = delete;

    [[nodiscard]] Scope measure(DatabaseOperation operation) noexcept
    {
        return Scope(*this, operation);
    }

  private:
#if TDS_TELEMETRY_LEVEL >= 1
    static std::uint64_t &operation_count(telemetry::DatabaseCounters &counters,
                                          DatabaseOperation operation) noexcept
    {
        switch (operation)
        {
        case DatabaseOperation::feed:
            return counters.feed;
        case DatabaseOperation::post:
            return counters.post;
        case DatabaseOperation::create:
            return counters.create;
        case DatabaseOperation::like:
            return counters.like;
        case DatabaseOperation::commit:
            return counters.commits;
        case DatabaseOperation::checkpoint:
            return counters.checkpoints;
        }
        std::unreachable();
    }

    void count(DatabaseOperation operation) noexcept
    {
        ++operation_count(totals_, operation);
#if TDS_TELEMETRY_LEVEL >= 2
        ++operation_count(interval_, operation);
#endif
    }

    const char *source_;
    telemetry::DatabaseCounters totals_;
    telemetry::WalSnapshot wal_;
    telemetry::WalAdviceCounters wal_advice_;
#endif

#if TDS_TELEMETRY_LEVEL >= 2
    timing::Histogram &histogram(DatabaseOperation operation) noexcept;
    void flush_window(std::uint64_t now_ns, bool final) noexcept;

    std::uint64_t started_ns_ = timing::now_ns();
    std::uint64_t last_report_ns_ = started_ns_;
    telemetry::DatabaseCounters interval_;
    telemetry::WalAdviceCounters interval_advice_;
    telemetry::DatabaseTimings cumulative_timing_;
    telemetry::DatabaseTimings interval_timing_;
    SlowSyncBuffer slow_sync_;
    std::array<tds_wal_advice_event, TDS_WAL_ADVICE_EVENT_CAPACITY> advice_events_{};
    std::uint64_t advice_event_count_ = 0;
    std::uint64_t advice_dropped_ = 0;
    telemetry::History<telemetry::DatabaseSnapshot> history_;
#endif
};

} // namespace tds::telemetry
