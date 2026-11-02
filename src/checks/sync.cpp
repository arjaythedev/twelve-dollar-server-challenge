#include "database_timing.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace
{
using namespace tds::database_timing;
constexpr int success_descriptor = -1;
constexpr int slow_descriptor = -2;
std::array<std::atomic<unsigned>, 2> calls{};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <std::size_t Index> int fake_sync(int descriptor)
{
    calls[Index].fetch_add(1, std::memory_order_relaxed);
    if (descriptor == slow_descriptor)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        errno = EIO;
        return -1;
    }
    if (descriptor != success_descriptor)
    {
        errno = EBADF;
        return -1;
    }
    return 0;
}

void invoke(SyncCall callback, std::size_t index, int descriptor)
{
    const auto before = calls[index].load(std::memory_order_relaxed);
    errno = EINVAL;
    const int result = callback(descriptor);
    const int returned_errno = errno;
    const bool failed = descriptor == slow_descriptor;
    require(result == (failed ? -1 : 0) && returned_errno == (failed ? EIO : EINVAL),
            "sync hook changed descriptor, return value or errno");
    require(calls[index].load(std::memory_order_relaxed) == before + 1,
            "sync hook did not forward exactly once");
}

void metrics(const DatabaseTimings &owner, std::uint64_t fsync_count, std::uint64_t fdatasync_count)
{
    const std::array expected{fsync_count, fdatasync_count};
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto &value = owner.sync[index];
        require(value.wall.count == expected[index], "sync wall attribution is wrong");
        require(value.thread_cpu.count + value.cpu_clock_failures == expected[index],
                "sync CPU sample or unavailable-clock accounting is wrong");
    }
}

void check_hooks()
{
    const std::array<SyncCall, 2> fakes{fake_sync<0>, fake_sync<1>};
    // Only this process gets fake callbacks; no database or real descriptor is used
    original_sync = fakes;
    const std::array<SyncCall, 2> callbacks{::fsync, ::fdatasync};
    install_sync_hooks();
    install_sync_hooks();
    require(original_sync == fakes, "repeated installation changed forwarding targets");
    require(sync_clock(CLOCK_MONOTONIC).available, "monotonic clock is unavailable");
    require(!sync_clock(static_cast<clockid_t>(-1)).available,
            "invalid clock was reported available");
    DatabaseTimings owner("sync-check-main"), nested("sync-check-nested"),
        worker("sync-check-worker");
    invoke(callbacks[0], 0, success_descriptor);
    invoke(callbacks[1], 1, slow_descriptor);
    metrics(owner, 0, 0);
    {
        SyncScope outer(owner, "outer");
        invoke(callbacks[0], 0, success_descriptor);
        invoke(callbacks[1], 1, slow_descriptor);
        {
            SyncScope inner(nested, "inner");
            require(active_sync == &inner, "nested scope was not activated");
            invoke(callbacks[0], 0, slow_descriptor);
        }
        require(active_sync == &outer, "nested scope did not restore outer scope");
        invoke(callbacks[0], 0, success_descriptor);
        std::exception_ptr worker_error;
        std::jthread thread([&] {
            try
            {
                require(active_sync == nullptr, "worker inherited main thread scope");
                {
                    SyncScope scope(worker, "worker");
                    invoke(callbacks[0], 0, success_descriptor);
                    invoke(callbacks[1], 1, success_descriptor);
                }
                require(active_sync == nullptr, "worker scope was not cleared");
            }
            catch (...)
            {
                worker_error = std::current_exception();
            }
        });
        thread.join();
        if (worker_error)
        {
            std::rethrow_exception(worker_error);
        }
        require(active_sync == &outer, "worker changed main thread scope");
        metrics(owner, 2, 1);
        metrics(nested, 1, 0);
        metrics(worker, 1, 1);
        require(owner.sync[1].wall.max > 10'000'000 && nested.sync[0].wall.max > 10'000'000,
                "slow failure did not exercise the diagnostic logging threshold");
    }
    require(active_sync == nullptr, "outer scope was not cleared");
    invoke(callbacks[0], 0, success_descriptor);
    invoke(callbacks[1], 1, success_descriptor);
    metrics(owner, 2, 1);
    metrics(nested, 1, 0);
    metrics(worker, 1, 1);
}
} // namespace

int main()
{
    try
    {
        check_hooks();
        std::puts("Sync forwarding, errno, attribution and repeated installation passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
