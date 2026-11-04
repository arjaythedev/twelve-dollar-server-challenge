#pragma once

#include "config.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace loadgen
{

enum class EndpointKind : std::uint8_t
{
    feed,
    post,
    like,
    create
};

enum class Phase : std::uint8_t
{
    ramp_up,
    hold,
    ramp_down,
    grace
};

struct LatencyStatistics
{
    std::size_t count = 0;
    double average = 0;
    double median = 0;
    double p95 = 0;
    double p99 = 0;
    double minimum = 0;
    double maximum = 0;
};

struct LatencySummary
{
    LatencyStatistics overall;
    std::array<LatencyStatistics, 4> endpoints;
    std::array<LatencyStatistics, 4> phases;
};

class Latencies
{
  public:
    static constexpr std::size_t chunk_samples = 65'536;
    static constexpr std::size_t sample_bytes = 16;

    // Append finite durations with valid tags while measurement is active
    void record(double milliseconds, EndpointKind endpoint, Phase phase);

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    // Finalize after measurement; repeated reports reuse the exact aggregates
    void prepare();

    [[nodiscard]] const LatencySummary &summary() const noexcept;

  private:
    struct Sample
    {
        double milliseconds;
        EndpointKind endpoint;
        Phase phase;
    };

    static_assert(sizeof(Sample) == sample_bytes);

    std::vector<std::unique_ptr<Sample[]>> chunks_;
    std::size_t size_ = 0;
    LatencySummary summary_;
    bool prepared_ = false;
};

struct HistogramBounds
{
    double lower = 0;
    double upper = 0;
};

struct DiagnosticHistogram
{
    static constexpr unsigned subdivisions = 16;
    static constexpr std::size_t bin_count = 1 + 64 * subdivisions;
    std::array<std::uint64_t, bin_count> bins{};
    std::uint64_t count = 0;
    double maximum_ms = 0;

    void record(double milliseconds) noexcept
    {
        std::size_t index = 0;
        if (milliseconds > 0)
        {
            const auto units = static_cast<std::uint64_t>(std::ceil(milliseconds * 1000));
            const auto exponent = static_cast<unsigned>(std::bit_width(units) - 1);
            const auto offset = units - (std::uint64_t{1} << exponent);
            const auto subdivision =
                exponent >= 4 ? offset >> (exponent - 4) : offset << (4 - exponent);
            index = 1 + static_cast<std::size_t>(exponent) * subdivisions +
                    static_cast<std::size_t>(subdivision);
        }
        ++bins[index];
        ++count;
        maximum_ms = std::max(maximum_ms, milliseconds);
    }

    static HistogramBounds bounds(std::size_t index) noexcept
    {
        if (index == 0)
        {
            return {};
        }
        --index;
        const auto exponent = index / subdivisions;
        const auto subdivision = index % subdivisions;
        const double base = std::ldexp(1.0, static_cast<int>(exponent));
        const double lower_us =
            std::ceil(base * (1 + static_cast<double>(subdivision) / subdivisions)) - 1;
        const double upper_us =
            std::ceil(base * (1 + static_cast<double>(subdivision + 1) / subdivisions)) - 1;
        return {lower_us / 1000, upper_us / 1000};
    }

    HistogramBounds quantile(double fraction) const noexcept
    {
        if (count == 0)
        {
            return {};
        }
        const double rank = fraction * static_cast<double>(count - 1);
        const auto first = static_cast<std::uint64_t>(rank) + 1;
        const auto second = std::min(first + 1, count);
        HistogramBounds lower;
        HistogramBounds upper;
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < bins.size(); ++index)
        {
            const auto previous = cumulative;
            cumulative += bins[index];
            if (previous < first && cumulative >= first)
            {
                lower = bounds(index);
            }
            if (cumulative >= second)
            {
                upper = bounds(index);
                break;
            }
        }
        const double weight = rank - std::floor(rank);
        return {lower.lower + (upper.lower - lower.lower) * weight,
                std::min(maximum_ms, lower.upper + (upper.upper - lower.upper) * weight)};
    }
};

struct DiagnosticWindow
{
    static constexpr std::uint64_t unset = ~std::uint64_t{0};
    std::uint64_t sequence = unset;
    DiagnosticHistogram request_duration;
    DiagnosticHistogram timer_overdue;
    DiagnosticHistogram epoll_service_lag;
    std::uint64_t timer_batches = 0;
    unsigned timer_batch_max_services = 0;
    double timer_batch_max_ms = 0;
    std::uint64_t epoll_polls = 0;
    double epoll_repoll_max_ms = 0;
};

class Diagnostics
{
  public:
    static constexpr std::size_t window_slots = 128;
    static constexpr double window_seconds = 5;

    void request(double completed, double milliseconds) noexcept
    {
        ++requests;
        if (auto *entry = window(completed))
        {
            entry->request_duration.record(milliseconds);
        }
    }

    void timer(double serviced, double due) noexcept
    {
        ++timers;
        if (auto *entry = window(serviced))
        {
            entry->timer_overdue.record(std::max(0.0, serviced - due) * 1000);
        }
    }

    void epoll(double serviced, double batch_ready) noexcept
    {
        ++events;
        if (auto *entry = window(serviced))
        {
            entry->epoll_service_lag.record(std::max(0.0, serviced - batch_ready) * 1000);
        }
    }

    void timer_batch(double started, double ended, unsigned serviced) noexcept
    {
        if (auto *entry = window(ended))
        {
            ++entry->timer_batches;
            entry->timer_batch_max_services = std::max(entry->timer_batch_max_services, serviced);
            entry->timer_batch_max_ms =
                std::max(entry->timer_batch_max_ms, (ended - started) * 1000);
        }
    }

    void repoll(double started, double previous_return) noexcept
    {
        if (auto *entry = window(started))
        {
            ++entry->epoll_polls;
            entry->epoll_repoll_max_ms =
                std::max(entry->epoll_repoll_max_ms, (started - previous_return) * 1000);
        }
    }

    const DiagnosticWindow &retained(std::uint64_t sequence) const noexcept
    {
        return windows[sequence % window_slots];
    }

    std::uint64_t latest = DiagnosticWindow::unset;
    std::uint64_t workload_start_ns = 0;
    std::uint64_t overwritten_windows = 0;
    std::uint64_t dropped_late_samples = 0;
    std::uint64_t requests = 0;
    std::uint64_t timers = 0;
    std::uint64_t events = 0;

  private:
    DiagnosticWindow *window(double elapsed) noexcept
    {
        const auto sequence = static_cast<std::uint64_t>(elapsed / window_seconds);
        if (latest == DiagnosticWindow::unset || sequence > latest)
        {
            latest = sequence;
        }
        if (latest - sequence >= window_slots)
        {
            ++dropped_late_samples;
            return nullptr;
        }
        auto &entry = windows[sequence % window_slots];
        if (entry.sequence != sequence)
        {
            if (entry.sequence != DiagnosticWindow::unset)
            {
                ++overwritten_windows;
            }
            entry = {};
            entry.sequence = sequence;
        }
        return &entry;
    }

    std::array<DiagnosticWindow, window_slots> windows{};
};

struct Metrics
{
    Latencies latencies;
    std::unique_ptr<Diagnostics> diagnostics;
    std::uint64_t failures = 0;
    std::array<std::uint64_t, 4> checks_passed{};
    std::array<std::uint64_t, 4> checks_failed{};
    std::array<std::uint64_t, 4> endpoint_requests{};
    std::uint64_t iterations = 0;
    std::uint64_t interrupted_iterations = 0;
    std::uint64_t workflow_errors = 0;
    std::uint64_t connections = 0;
    std::uint64_t established_connections = 0;
    std::uint64_t live_connections = 0;
    std::uint64_t closed_connections = 0;
    std::uint64_t peak_live_connections = 0;
    std::uint64_t hold_peak_live_connections = 0;
    std::uint64_t vus_with_http_response = 0;
    std::uint64_t bytes_received = 0;
    std::optional<double> target_activation_seconds;
    double blocked_sum = 0;
    double elapsed = 0;
};

struct Summary
{
    std::string json;
    bool passed;
};

double percentile(std::span<const double> sorted, double fraction);
void diagnostic_summary(std::ostream &output, const Diagnostics &diagnostics, double elapsed);
Summary summarize(Metrics &metrics, const RuntimeConfig &config);

} // namespace loadgen
