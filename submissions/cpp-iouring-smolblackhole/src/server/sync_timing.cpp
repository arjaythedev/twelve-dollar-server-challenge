#include "diagnostics/sync.hpp"

#if TDS_TELEMETRY_LEVEL >= 2

#include <cerrno>

namespace tds::telemetry
{

SyncClock sync_clock(clockid_t clock_id) noexcept
{
    timespec value{};
    if (::clock_gettime(clock_id, &value) != 0)
    {
        return {};
    }
    return {static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
                static_cast<std::uint64_t>(value.tv_nsec),
            true};
}

int measured_sync(int descriptor, telemetry::SyncOperation operation) noexcept
{
    const auto index = static_cast<std::size_t>(operation);
    auto *scope = active_sync;
    if (!scope)
    {
        return original_sync[index](descriptor);
    }
    const int incoming_errno = errno;
    const auto started = sync_clock(CLOCK_MONOTONIC);
    const auto cpu_started = sync_clock(CLOCK_THREAD_CPUTIME_ID);
    errno = incoming_errno;
    const int result = original_sync[index](descriptor);
    const int saved_errno = errno;
    const auto cpu_finished = sync_clock(CLOCK_THREAD_CPUTIME_ID);
    const auto finished = sync_clock(CLOCK_MONOTONIC);
    auto &metrics = scope->metrics[index];
    ++metrics.calls;
    metrics.errors += result != 0;
    const bool cpu_available =
        cpu_started.available && cpu_finished.available && cpu_finished.ns >= cpu_started.ns;
    const auto cpu_ns = cpu_available ? cpu_finished.ns - cpu_started.ns : 0;
    if (cpu_available)
    {
        metrics.thread_cpu.record(cpu_ns);
    }
    else
    {
        ++metrics.cpu_clock_failures;
    }
    if (started.available && finished.available && finished.ns >= started.ns)
    {
        const auto wall = finished.ns - started.ns;
        metrics.wall.record(wall);
        if (wall > 10'000'000)
        {
            scope->slow_events.record({operation, scope->role, started.ns, finished.ns, wall,
                                       cpu_ns, cpu_available, result, saved_errno});
        }
    }
    else
    {
        ++metrics.wall_clock_failures;
    }
    errno = saved_errno;

    return result;
}

} // namespace tds::telemetry

extern "C" int __wrap_fsync(int descriptor) noexcept
{
    return tds::telemetry::measured_sync(descriptor, tds::telemetry::SyncOperation::fsync);
}

extern "C" int __wrap_fdatasync(int descriptor) noexcept
{
    return tds::telemetry::measured_sync(descriptor, tds::telemetry::SyncOperation::fdatasync);
}

#endif
