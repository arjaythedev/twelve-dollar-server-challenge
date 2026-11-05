#pragma once

#include "../telemetry/database_snapshot.hpp"

#if TDS_TELEMETRY_LEVEL >= 2
#include <time.h>

extern "C" int __real_fsync(int descriptor);
extern "C" int __real_fdatasync(int descriptor);
#endif

namespace tds::telemetry
{

class SlowSyncBuffer
{
  public:
    static constexpr std::size_t capacity = 64;

    void record(const telemetry::SlowSyncEvent &event) noexcept
    {
#if TDS_TELEMETRY_LEVEL >= 2
        if (size_ < capacity)
        {
            events_[static_cast<std::size_t>(size_++)] = event;
        }
        else
        {
            ++dropped_;
        }
#else
        (void)event;
#endif
    }

    std::span<const telemetry::SlowSyncEvent> events() const noexcept
    {
#if TDS_TELEMETRY_LEVEL >= 2
        return {events_.data(), static_cast<std::size_t>(size_)};
#else
        return {};
#endif
    }

    std::uint64_t dropped() const noexcept
    {
#if TDS_TELEMETRY_LEVEL >= 2
        return dropped_;
#else
        return 0;
#endif
    }

  private:
#if TDS_TELEMETRY_LEVEL >= 2
    std::array<telemetry::SlowSyncEvent, capacity> events_{};
    std::uint64_t size_ = 0;
    std::uint64_t dropped_ = 0;
#endif
};

class SyncScope;
#if TDS_TELEMETRY_LEVEL >= 2
inline thread_local SyncScope *active_sync = nullptr;
#endif

class SyncScope
{
  public:
#if TDS_TELEMETRY_LEVEL >= 2
    SyncScope(telemetry::SyncCounters &owner, SlowSyncBuffer &events,
              telemetry::SyncRole label) noexcept
        : metrics(owner), slow_events(events), role(label), previous_(active_sync)
    {
        active_sync = this;
    }

    ~SyncScope()
    {
        active_sync = previous_;
    }

    telemetry::SyncCounters &metrics;
    SlowSyncBuffer &slow_events;
    telemetry::SyncRole role;
#else
    constexpr SyncScope(telemetry::SyncCounters &, SlowSyncBuffer &, telemetry::SyncRole) noexcept
    {
    }
#endif
    SyncScope(const SyncScope &) = delete;
    SyncScope &operator=(const SyncScope &) = delete;

  private:
#if TDS_TELEMETRY_LEVEL >= 2
    SyncScope *previous_;
#endif
};

#if TDS_TELEMETRY_LEVEL >= 2
struct SyncClock
{
    std::uint64_t ns = 0;
    bool available = false;
};

using SyncCall = int (*)(int);
inline constinit std::array<SyncCall, 2> original_sync{::__real_fsync, ::__real_fdatasync};

[[nodiscard]] SyncClock sync_clock(clockid_t clock_id) noexcept;
int measured_sync(int descriptor, telemetry::SyncOperation operation) noexcept;
#endif

} // namespace tds::telemetry
