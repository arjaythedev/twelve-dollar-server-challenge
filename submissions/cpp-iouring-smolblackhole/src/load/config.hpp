#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadgen
{
using Clock = std::chrono::steady_clock;
inline constexpr std::string_view version = "0.1.0";
inline constexpr std::size_t max_response_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t max_samples = 20'000'000;
inline constexpr unsigned max_timer_batch = 256;
inline constexpr double timer_batch_seconds = 0.001;
inline constexpr unsigned activation_deadline_grace_ms = 100;

struct RuntimeConfig
{
    unsigned vus = 100;
    double hold = 300;
    double ramp_up = 60;
    double ramp_down = 30;
    double grace = 10;
    double timeout = 60;
    double time_scale = 1;
    std::uint64_t seed = 0;
    bool diagnostics = false;
};
} // namespace loadgen
