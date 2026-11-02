#pragma once

#ifdef TDS_DIAGNOSTICS

#include <array>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace tds::timing
{

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
        ++count;
        total += value;
        if (value > max)
        {
            max = value;
        }
    }

    void write(std::FILE *output, const char *name, const char *unit = "ns") const noexcept
    {
        std::fprintf(output,
                     "\"%s\":{\"unit\":\"%s\",\"count\":%" PRIu64 ",\"total\":%" PRIu64
                     ",\"max\":%" PRIu64 ",\"buckets\":[",
                     name, unit, count, total, max);
        bool first = true;
        for (std::size_t index = 0; index < buckets.size(); ++index)
        {
            if (buckets[index])
            {
                std::fprintf(output, "%s[%zu,%" PRIu64 "]", first ? "" : ",", index,
                             buckets[index]);
                first = false;
            }
        }
        std::fputs("]}", output);
    }

    void reset() noexcept
    {
        *this = Histogram{};
    }

    std::array<std::uint64_t, 64> buckets{};
    std::uint64_t count = 0;
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

#endif
