#include "runtime.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <sys/epoll.h>

namespace
{
using namespace loadgen;

struct Simulation
{
    std::int64_t now_ns = 0;
    std::int64_t service_ns = 0;
    std::int64_t previous_return_ns = 0;
    std::int64_t longest_turn_ns = 0;
    std::uint64_t polls = 0;
};

Simulation *active = nullptr;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Result
{
    double activation_seconds;
    double longest_turn_ms;
    std::uint64_t polls;
    bool on_time;
};

Result simulate(std::int64_t service_ns)
{
    RuntimeConfig config;
    config.vus = 350'000;
    config.ramp_up = 30;
    config.hold = 100;
    config.ramp_down = 1;
    config.grace = 1;
    // Keep first-request jitter beyond the simulated ramp, without opening any sockets
    config.time_scale = 1e12;
    config.seed = 42;
    Load load(config, endpoint("http://127.0.0.1:1"), {{"token", "user"}});
    Simulation clock;
    clock.service_ns = service_ns;
    active = &clock;
    Metrics metrics;
    try
    {
        metrics = load.run(false);
    }
    catch (...)
    {
        active = nullptr;
        interrupted = 0;
        throw;
    }
    active = nullptr;
    interrupted = 0;
    require(metrics.connections == 0 && metrics.target_activation_seconds.has_value(),
            "activation simulation opened sockets or did not activate every user");
    const double deadline = config.ramp_up + activation_deadline_grace_ms / 1000.0;

    return {*metrics.target_activation_seconds, static_cast<double>(clock.longest_turn_ns) / 1e6,
            clock.polls, *metrics.target_activation_seconds <= deadline};
}
} // namespace

// Cursed but test-only: wrap libstdc++'s clock instead of adding a fake-clock API to Load
extern "C" loadgen::Clock::time_point __real__ZNSt6chrono3_V212steady_clock3nowEv();

extern "C" loadgen::Clock::time_point __wrap__ZNSt6chrono3_V212steady_clock3nowEv()
{
    if (!active)
    {
        return __real__ZNSt6chrono3_V212steady_clock3nowEv();
    }
    active->now_ns += active->service_ns;
    if (active->now_ns >= 60'000'000'000LL)
    {
        loadgen::interrupted = 1;
    }

    return loadgen::Clock::time_point(std::chrono::nanoseconds(active->now_ns));
}

extern "C" int __real_epoll_wait(int, epoll_event *, int, int);

extern "C" int __wrap_epoll_wait(int descriptor, epoll_event *events, int capacity, int timeout)
{
    if (!active)
    {
        return __real_epoll_wait(descriptor, events, capacity, timeout);
    }
    const auto turn = active->now_ns - active->previous_return_ns;
    active->longest_turn_ns = std::max(active->longest_turn_ns, turn);
    ++active->polls;
    // Model fixed I/O work between timer passes, independently of the machine's speed
    active->now_ns += static_cast<std::int64_t>(std::max(timeout, 0)) * 1'000'000 + 300'000;
    active->previous_return_ns = active->now_ns;

    return 0;
}

int main(int argc, char **argv)
{
    try
    {
        const bool probe = argc == 2 && std::string_view(argv[1]) == "--probe";
        constexpr std::array<std::int64_t, 3> costs{10'000, 70'000, 120'000};
        std::array<Result, costs.size()> results;
        for (std::size_t index = 0; index < costs.size(); ++index)
        {
            results[index] = simulate(costs[index]);
        }
        std::printf("{\"users\":350000,\"ramp_seconds\":30,\"margin_ms\":%u,\"samples\":[",
                    loadgen::activation_deadline_grace_ms);
        for (std::size_t index = 0; index < results.size(); ++index)
        {
            const auto &value = results[index];
            std::printf("%s{\"service_ns\":%" PRId64 ",\"activation_seconds\":%.6f,"
                        "\"longest_turn_ms\":%.6f,\"polls\":%" PRIu64 ",\"on_time\":%s}",
                        index ? "," : "", costs[index], value.activation_seconds,
                        value.longest_turn_ms, value.polls, value.on_time ? "true" : "false");
        }
        std::puts("]}");
        if (!probe)
        {
            require(results[0].on_time, "light timer pressure missed the activation deadline");
            require(!results[2].on_time, "overload incorrectly bypassed the activation deadline");
            for (const auto &value : results)
            {
                require(value.longest_turn_ms < 2,
                        "timer work stopped returning to I/O within its bounded turn");
            }
        }

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());

        return 1;
    }
}
