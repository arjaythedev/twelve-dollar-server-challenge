#pragma once

#ifdef TDS_DIAGNOSTICS

#include "timing.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

extern "C" int __real_fsync(int descriptor);
extern "C" int __real_fdatasync(int descriptor);

namespace tds::database_timing
{

struct SyncMetrics
{
    timing::Histogram wall;
    timing::Histogram thread_cpu;
    timing::Histogram logging_wall;
    std::uint64_t cpu_clock_failures = 0;
};

class DatabaseTimings
{
  public:
    explicit DatabaseTimings(const char *owner) : owner_(owner)
    {
    }

    ~DatabaseTimings()
    {
        report(true);
    }

    void report(bool final = false)
    {
        const auto now = timing::now_ns();
        if (!final && now - last_report_ < 5'000'000'000ULL)
        {
            return;
        }

        struct Series
        {
            const char *name;
            timing::Histogram &values;
            const char *unit = "ns";
        };

        const Series series[] = {{"feed_db_and_json_ns", feed},
                                 {"post_db_and_json_ns", post},
                                 {"create_db_and_json_ns", create},
                                 {"like_db_ns", like},
                                 {"write_commit_ns", commit},
                                 {"checkpoint_ns", checkpoint},
                                 {"wal_uncheckpointed_frames", backlog, "frames"},
                                 {"fsync_wall_ns", sync[0].wall},
                                 {"fsync_thread_cpu_ns", sync[0].thread_cpu},
                                 {"fsync_logging_ns", sync[0].logging_wall},
                                 {"fdatasync_wall_ns", sync[1].wall},
                                 {"fdatasync_thread_cpu_ns", sync[1].thread_cpu},
                                 {"fdatasync_logging_ns", sync[1].logging_wall}};
        const auto write_started = timing::now_ns();
        ::flockfile(stderr);
        std::fprintf(stderr,
                     "TDS_TIMING {\"source\":\"%s\",\"time_ns\":%llu,\"interval_ns\":%llu,"
                     "\"previous_report_write_ns\":%llu,\"final\":%s,\"metrics\":{",
                     owner_, static_cast<unsigned long long>(now),
                     static_cast<unsigned long long>(now - last_report_),
                     static_cast<unsigned long long>(last_report_write_ns_),
                     final ? "true" : "false");
        bool first = true;
        for (const auto &entry : series)
        {
            if (!entry.values.count)
            {
                continue;
            }
            if (!first)
            {
                std::fputc(',', stderr);
            }
            first = false;
            entry.values.write(stderr, entry.name, entry.unit);
            entry.values.reset();
        }
        std::fprintf(stderr, "},\"sync_cpu_clock_failures\":%llu}\n",
                     static_cast<unsigned long long>(sync[0].cpu_clock_failures +
                                                     sync[1].cpu_clock_failures));
        ::funlockfile(stderr);
        last_report_write_ns_ = timing::now_ns() - write_started;
        sync[0].cpu_clock_failures = 0;
        sync[1].cpu_clock_failures = 0;
        last_report_ = now;
    }

    timing::Histogram feed;
    timing::Histogram post;
    timing::Histogram create;
    timing::Histogram like;
    timing::Histogram commit;
    timing::Histogram checkpoint;
    timing::Histogram backlog;
    std::array<SyncMetrics, 2> sync;

  private:
    const char *owner_;
    std::uint64_t last_report_ = timing::now_ns();
    std::uint64_t last_report_write_ns_ = 0;
};

class SyncScope;
inline thread_local SyncScope *active_sync = nullptr;

class SyncScope
{
  public:
    SyncScope(DatabaseTimings &owner, const char *label) noexcept
        : timings(owner), role(label), previous_(active_sync)
    {
        active_sync = this;
    }

    ~SyncScope()
    {
        active_sync = previous_;
    }

    SyncScope(const SyncScope &) = delete;
    SyncScope &operator=(const SyncScope &) = delete;

    DatabaseTimings &timings;
    const char *role;

  private:
    SyncScope *previous_;
};

struct SyncClock
{
    std::uint64_t ns = 0;
    bool available = false;
};

inline SyncClock sync_clock(clockid_t clock_id) noexcept
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

inline const char *sync_file_kind(int descriptor) noexcept
{
    std::array<char, 64> link{};
    std::snprintf(link.data(), link.size(), "/proc/self/fd/%d", descriptor);
    std::array<char, 4096> target{};
    const auto length = ::readlink(link.data(), target.data(), target.size());
    if (length < 0 || static_cast<std::size_t>(length) == target.size())
    {
        return "unknown";
    }
    const std::string_view name(target.data(), static_cast<std::size_t>(length));
    return name.ends_with("-wal") ? "wal" : "main_or_other";
}

using SyncCall = int (*)(int);
inline constinit std::array<SyncCall, 2> original_sync{::__real_fsync, ::__real_fdatasync};

inline int measured_sync(int descriptor, std::size_t index) noexcept
{
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
    auto &metrics = scope->timings.sync[index];
    const bool cpu_available =
        cpu_started.available && cpu_finished.available && cpu_finished.ns >= cpu_started.ns;
    if (cpu_available)
    {
        metrics.thread_cpu.record(cpu_finished.ns - cpu_started.ns);
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
            const auto logging_started = sync_clock(CLOCK_MONOTONIC);
            const char *kind = sync_file_kind(descriptor);
            ::flockfile(stderr);
            std::fprintf(stderr,
                         "TDS_TIMING {\"source\":\"sync\",\"role\":\"%s\",\"syscall\":\"%s\","
                         "\"time_ns\":%llu,\"start_ns\":%llu,\"end_ns\":%llu,\"wall_ns\":%llu,"
                         "\"thread_cpu_ns\":",
                         scope->role, index == 0 ? "fsync" : "fdatasync",
                         static_cast<unsigned long long>(finished.ns),
                         static_cast<unsigned long long>(started.ns),
                         static_cast<unsigned long long>(finished.ns),
                         static_cast<unsigned long long>(wall));
            if (cpu_available)
            {
                std::fprintf(stderr, "%llu",
                             static_cast<unsigned long long>(cpu_finished.ns - cpu_started.ns));
            }
            else
            {
                std::fputs("null", stderr);
            }
            std::fprintf(stderr, ",\"tid\":%ld,\"fd_kind\":\"%s\",\"rc\":%d,\"errno\":%d}\n",
                         ::syscall(SYS_gettid), kind, result, saved_errno);
            ::funlockfile(stderr);
            const auto logging_finished = sync_clock(CLOCK_MONOTONIC);
            if (logging_started.available && logging_finished.available &&
                logging_finished.ns >= logging_started.ns)
            {
                metrics.logging_wall.record(logging_finished.ns - logging_started.ns);
            }
        }
    }
    errno = saved_errno;
    return result;
}

inline int measured_fsync(int descriptor) noexcept
{
    return measured_sync(descriptor, 0);
}

inline int measured_fdatasync(int descriptor) noexcept
{
    return measured_sync(descriptor, 1);
}

void install_sync_hooks();

} // namespace tds::database_timing

#endif
