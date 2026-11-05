#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tds::timing
{

inline constexpr std::uint64_t report_interval_ns = 5'000'000'000ULL;

inline std::uint64_t now_ns() noexcept
{
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

struct Histogram
{
    void record(std::uint64_t value) noexcept
    {
        // Zero and one share bucket zero; remaining buckets contain floor(log2(value))
        const auto index = value ? std::bit_width(value) - 1U : 0U;
        ++buckets[static_cast<std::size_t>(index)];
        total += value;
        if (value > max)
        {
            max = value;
        }
    }

    void merge(const Histogram &other) noexcept
    {
        for (std::size_t index = 0; index < buckets.size(); ++index)
        {
            buckets[index] += other.buckets[index];
        }
        total += other.total;
        if (other.max > max)
        {
            max = other.max;
        }
    }

    void reset() noexcept
    {
        *this = Histogram{};
    }

    [[nodiscard]] std::uint64_t count() const noexcept
    {
        // Counting is cold; unsigned addition preserves the original wraparound semantics
        std::uint64_t samples = 0;
        for (const auto bucket : buckets)
        {
            samples += bucket;
        }

        return samples;
    }

    std::array<std::uint64_t, 64> buckets{};
    std::uint64_t total = 0;
    std::uint64_t max = 0;
};

class Scope
{
  public:
    explicit Scope(Histogram &histogram) noexcept : histogram_(histogram), started_ns_(now_ns())
    {
    }

    ~Scope()
    {
        histogram_.record(now_ns() - started_ns_);
    }

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    Histogram &histogram_;
    std::uint64_t started_ns_;
};

} // namespace tds::timing
