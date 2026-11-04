#include "metrics.hpp"

#include "inputs.hpp"
#include "runtime.hpp"

#include <yyjson.h>

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>

namespace loadgen
{
namespace
{

struct Quantile
{
    std::size_t lower = 0;
    std::size_t upper = 0;
    double weight = 0;
    double lower_value = 0;
    double upper_value = 0;

    Quantile(std::size_t count, double fraction)
    {
        if (count == 0)
        {
            return;
        }
        const double rank = fraction * static_cast<double>(count - 1);
        lower = static_cast<std::size_t>(rank);
        upper = std::min(lower + 1, count - 1);
        weight = rank - static_cast<double>(lower);
    }

    void observe(std::size_t index, double milliseconds) noexcept
    {
        if (index == lower)
        {
            lower_value = milliseconds;
        }
        if (index == upper)
        {
            upper_value = milliseconds;
        }
    }

    [[nodiscard]] double value() const noexcept
    {
        return lower_value + (upper_value - lower_value) * weight;
    }
};

struct Quantiles
{
    Quantile median;
    Quantile p95;
    Quantile p99;
    std::size_t observed = 0;

    Quantiles() : Quantiles(0)
    {
    }

    explicit Quantiles(std::size_t count) : median(count, 0.5), p95(count, 0.95), p99(count, 0.99)
    {
    }

    void observe(double milliseconds) noexcept
    {
        median.observe(observed, milliseconds);
        p95.observe(observed, milliseconds);
        p99.observe(observed, milliseconds);
        ++observed;
    }
};

void accumulate(LatencyStatistics &target, double milliseconds) noexcept
{
    if (target.count == 0)
    {
        target.minimum = milliseconds;
    }
    ++target.count;
    target.average += milliseconds;
    target.maximum = milliseconds;
}

void duration_groups(std::ostream &output, const std::array<LatencyStatistics, 4> &groups,
                     const std::array<std::string_view, 4> &names)
{
    output << '{';
    for (std::size_t index = 0; index < groups.size(); ++index)
    {
        const auto &samples = groups[index];
        output << (index == 0 ? "" : ",") << json_string(names[index])
               << ":{\"count\":" << samples.count << ",\"avg\":" << samples.average
               << ",\"p95\":" << samples.p95 << ",\"p99\":" << samples.p99
               << ",\"max\":" << samples.maximum << '}';
    }
    output << '}';
}

} // namespace

void Latencies::record(double milliseconds, EndpointKind endpoint, Phase phase)
{
    assert(!prepared_);
    if (size_ == max_samples)
    {
        throw std::runtime_error("exact duration sample limit reached (20 million requests, "
                                 "320 MB sample data); shorten the run");
    }
    if (size_ % chunk_samples == 0)
    {
        chunks_.push_back(std::make_unique_for_overwrite<Sample[]>(chunk_samples));
    }
    chunks_[size_ / chunk_samples][size_ % chunk_samples] = {milliseconds, endpoint, phase};
    ++size_;
}

void Latencies::prepare()
{
    if (prepared_)
    {
        return;
    }
    std::vector<Sample> sorted;
    sorted.reserve(size_);
    for (auto &chunk : chunks_)
    {
        const auto count = std::min(chunk_samples, size_ - sorted.size());
        sorted.insert(sorted.end(), chunk.get(), chunk.get() + count);
        // Release each copied chunk before touching the next to keep resident memory bounded
        chunk.reset();
    }
    chunks_.clear();
    std::sort(sorted.begin(), sorted.end(), [](const Sample &left, const Sample &right) {
        return left.milliseconds < right.milliseconds;
    });

    // Filtering one global ordering also gives each group its original sorted summation order
    std::array<LatencyStatistics, 9> groups{};
    for (const auto &sample : sorted)
    {
        accumulate(groups[0], sample.milliseconds);
        accumulate(groups[1 + static_cast<std::size_t>(sample.endpoint)], sample.milliseconds);
        accumulate(groups[5 + static_cast<std::size_t>(sample.phase)], sample.milliseconds);
    }
    std::array<Quantiles, 9> quantiles{};
    for (std::size_t index = 0; index < groups.size(); ++index)
    {
        auto &group = groups[index];
        if (group.count)
        {
            group.average /= static_cast<double>(group.count);
        }
        quantiles[index] = Quantiles(group.count);
    }
    for (const auto &sample : sorted)
    {
        quantiles[0].observe(sample.milliseconds);
        quantiles[1 + static_cast<std::size_t>(sample.endpoint)].observe(sample.milliseconds);
        quantiles[5 + static_cast<std::size_t>(sample.phase)].observe(sample.milliseconds);
    }
    for (std::size_t index = 0; index < groups.size(); ++index)
    {
        groups[index].median = quantiles[index].median.value();
        groups[index].p95 = quantiles[index].p95.value();
        groups[index].p99 = quantiles[index].p99.value();
    }
    summary_.overall = groups[0];
    std::copy_n(groups.begin() + 1, summary_.endpoints.size(), summary_.endpoints.begin());
    std::copy_n(groups.begin() + 5, summary_.phases.size(), summary_.phases.begin());
    prepared_ = true;
}

const LatencySummary &Latencies::summary() const noexcept
{
    assert(prepared_);

    return summary_;
}

double percentile(std::span<const double> sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0;
    }
    const double rank = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(rank);
    const auto upper = std::min(lower + 1, sorted.size() - 1);

    return sorted[lower] + (sorted[upper] - sorted[lower]) * (rank - static_cast<double>(lower));
}

void diagnostic_histogram(std::ostream &output, const DiagnosticHistogram &histogram)
{
    const auto p95 = histogram.quantile(0.95);
    const auto p99 = histogram.quantile(0.99);
    output << "{\"count\":" << histogram.count << ",\"max_ms\":" << histogram.maximum_ms
           << ",\"p95_lower_ms\":" << p95.lower << ",\"p95_upper_ms\":" << p95.upper
           << ",\"p99_lower_ms\":" << p99.lower << ",\"p99_upper_ms\":" << p99.upper
           << ",\"histogram\":[";
    bool first = true;
    for (std::size_t index = 0; index < histogram.bins.size(); ++index)
    {
        if (histogram.bins[index] == 0)
        {
            continue;
        }
        const auto bounds = DiagnosticHistogram::bounds(index);
        output << (first ? "" : ",") << '[' << bounds.lower << ',' << bounds.upper << ','
               << histogram.bins[index] << ']';
        first = false;
    }
    output << "]}";
}

void diagnostic_summary(std::ostream &output, const Diagnostics &diagnostics, double elapsed)
{
    const auto last = diagnostics.latest;
    const auto first = last != DiagnosticWindow::unset && last >= Diagnostics::window_slots
                           ? last - Diagnostics::window_slots + 1
                           : 0;
    std::size_t retained = 0;
    if (last != DiagnosticWindow::unset)
    {
        for (auto sequence = first; sequence <= last; ++sequence)
        {
            if (diagnostics.retained(sequence).sequence == sequence)
            {
                ++retained;
            }
        }
    }
    output << "{\"workload_start_ns\":" << diagnostics.workload_start_ns
           << ",\"clock\":\"std::chrono::steady_clock, local process clock; no cross-machine epoch "
              "guarantee\""
           << ",\"window_seconds\":" << Diagnostics::window_seconds
           << ",\"window_slots\":" << Diagnostics::window_slots
           << ",\"storage_bytes\":" << sizeof(Diagnostics) << ",\"windows_retained\":" << retained
           << ",\"windows_overwritten\":" << diagnostics.overwritten_windows
           << ",\"dropped_late_samples\":" << diagnostics.dropped_late_samples
           << ",\"requests_total\":" << diagnostics.requests
           << ",\"timer_services_total\":" << diagnostics.timers
           << ",\"epoll_events_total\":" << diagnostics.events
           << ",\"histogram_semantics\":\"sparse [lower_ms,upper_ms,count]; lower exclusive except "
              "exact zero bin; ceil to 1us, 16 subdivisions per power of two; bucket width <=6.25% "
              "plus 1us\""
           << ",\"quantile_semantics\":\"approximate bounds for linear interpolation at "
              "p*(count-1); exact count and max\""
           << ",\"window_semantics\":\"[start_s,end_s) relative to workload_start_ns; classified "
              "by completion/service time; unobserved empty windows omitted; last 128 intervals "
              "retained\""
           << ",\"request_semantics\":\"same as global http_req_duration; failures and timeouts "
              "included, canceled in-flight requests omitted\""
           << ",\"timer_semantics\":\"due-to-tick service delay for all VU activation, think, "
              "idle, retirement, grace and request/connect timeout timers\""
           << ",\"epoll_semantics\":\"epoll_wait return to start of each returned event dispatch, "
              "including stale events; not kernel readiness arrival time\""
           << ",\"timer_batch_limit\":" << max_timer_batch
           << ",\"timer_batch_budget_ms\":" << timer_batch_seconds * 1000
           << ",\"repoll_semantics\":\"previous epoll_wait return to next call; first interval "
              "starts at workload start; excludes blocking inside epoll_wait\""
           << ",\"windows\":[";
    bool comma = false;
    if (last != DiagnosticWindow::unset)
    {
        for (auto sequence = first; sequence <= last; ++sequence)
        {
            const auto &window = diagnostics.retained(sequence);
            if (window.sequence != sequence)
            {
                continue;
            }
            output << (comma ? "," : "")
                   << "{\"start_s\":" << static_cast<double>(sequence) * Diagnostics::window_seconds
                   << ",\"end_s\":"
                   << std::min(elapsed,
                               static_cast<double>(sequence + 1) * Diagnostics::window_seconds)
                   << ",\"request_duration_ms\":";
            diagnostic_histogram(output, window.request_duration);
            output << ",\"timer_overdue_ms\":";
            diagnostic_histogram(output, window.timer_overdue);
            output << ",\"epoll_service_lag_ms\":";
            diagnostic_histogram(output, window.epoll_service_lag);
            output << ",\"timer_batches\":" << window.timer_batches
                   << ",\"timer_batch_max_services\":" << window.timer_batch_max_services
                   << ",\"timer_batch_max_ms\":" << window.timer_batch_max_ms
                   << ",\"epoll_polls\":" << window.epoll_polls
                   << ",\"epoll_repoll_max_ms\":" << window.epoll_repoll_max_ms << '}';
            comma = true;
        }
    }
    output << "]}";
}

Summary summarize(Metrics &metrics, const RuntimeConfig &config)
{
    metrics.latencies.prepare();
    const auto &latencies = metrics.latencies.summary();
    const auto &overall = latencies.overall;
    const auto count = overall.count;
    std::uint64_t passes = 0;
    std::uint64_t fails = 0;
    for (std::size_t index = 0; index < metrics.checks_passed.size(); ++index)
    {
        passes += metrics.checks_passed[index];
        fails += metrics.checks_failed[index];
    }
    const double p95 = overall.p95;
    const double p99 = overall.p99;
    const double failure_rate =
        count == 0 ? 0 : static_cast<double>(metrics.failures) / static_cast<double>(count);
    const bool target_concurrency_reached = metrics.hold_peak_live_connections >= config.vus;
    const bool response_coverage_complete = metrics.vus_with_http_response == config.vus;
    const bool target_activation_on_time =
        metrics.target_activation_seconds &&
        *metrics.target_activation_seconds <=
            config.ramp_up + static_cast<double>(activation_deadline_grace_ms) / 1000;
    const bool http_metrics_passed = p95 < 500 && p99 < 1000 && failure_rate < 0.01;
    const bool passed = count != 0 && target_concurrency_reached && response_coverage_complete &&
                        target_activation_on_time && http_metrics_passed;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
    {
        throw std::runtime_error("getrusage: " + std::string(std::strerror(errno)));
    }
    std::ostringstream output;
    output << std::setprecision(12) << std::boolalpha;
    output << "{\n  \"metrics\": {\n    \"http_reqs\": {\"count\":" << count << ",\"rate\":"
           << (metrics.elapsed > 0 ? static_cast<double>(count) / metrics.elapsed : 0) << "},\n"
           << "    \"http_req_duration\": {\"avg\":" << overall.average
           << ",\"med\":" << overall.median << ",\"p(95)\":" << p95 << ",\"p(99)\":" << p99
           << ",\"max\":" << overall.maximum << ",\"thresholds\":{\"p(95)<500\":" << (p95 < 500)
           << ",\"p(99)<1000\":" << (p99 < 1000) << "}},\n"
           << "    \"http_req_failed\": {\"value\":" << failure_rate
           << ",\"passes\":" << metrics.failures << ",\"fails\":" << count - metrics.failures
           << ",\"thresholds\":{\"rate<0.01\":" << (failure_rate < 0.01) << "}},\n"
           << "    \"checks\": {\"passes\":" << passes << ",\"fails\":" << fails << ",\"value\":"
           << (passes + fails ? static_cast<double>(passes) / static_cast<double>(passes + fails)
                              : 0)
           << "},\n"
           << "    \"iterations\": {\"count\":" << metrics.iterations << ",\"rate\":"
           << (metrics.elapsed > 0 ? static_cast<double>(metrics.iterations) / metrics.elapsed : 0)
           << "},\n"
           << "    \"http_req_blocked\": {\"avg\":"
           << (count ? metrics.blocked_sum * 1000 / static_cast<double>(count) : 0) << "}\n  },\n"
           << "  \"generator\": {\"name\":\"td-load\",\"version\":" << json_string(version)
           << ",\"transport\":\"epoll/http1.1/IPv4\",\"source\":\"bench/load.js\",\"passed\":"
           << passed << ",\"checks_passed\":" << (fails == 0) << ",\"vus\":" << config.vus
           << ",\"ramp_up_seconds\":" << config.ramp_up << ",\"hold_seconds\":" << config.hold
           << ",\"ramp_down_seconds\":" << config.ramp_down << ",\"grace_seconds\":" << config.grace
           << ",\"request_timeout_seconds\":" << config.timeout
           << ",\"elapsed_seconds\":" << metrics.elapsed << ",\"target_activation_seconds\":";
    if (metrics.target_activation_seconds)
    {
        output << *metrics.target_activation_seconds;
    }
    else
    {
        output << "null";
    }
    output << ",\"activation_deadline_grace_ms\":" << activation_deadline_grace_ms
           << ",\"target_activation_on_time\":" << target_activation_on_time
           << ",\"connections\":" << metrics.connections
           << ",\"connections_semantics\":\"connect calls returning success or EINPROGRESS, not "
              "completed handshakes\""
           << ",\"established_connections\":" << metrics.established_connections
           << ",\"live_connections\":" << metrics.live_connections
           << ",\"closed_connections\":" << metrics.closed_connections
           << ",\"peak_live_connections\":" << metrics.peak_live_connections
           << ",\"hold_peak_live_connections\":" << metrics.hold_peak_live_connections
           << ",\"live_connections_semantics\":\"handshaken client sockets still open, including "
              "retired idle VUs; peer closes counted when observed\""
           << ",\"vus_with_http_response\":" << metrics.vus_with_http_response
           << ",\"target_concurrency_reached\":" << target_concurrency_reached
           << ",\"http_response_coverage_complete\":" << response_coverage_complete
           << ",\"http_metrics_passed\":" << http_metrics_passed
           << ",\"max_rss_bytes\":" << static_cast<std::uint64_t>(usage.ru_maxrss) * 1024
           << ",\"idle_vu_state_bytes\":" << sizeof(User) + sizeof(std::size_t)
           << ",\"interrupted_iterations\":" << metrics.interrupted_iterations
           << ",\"workflow_errors\":" << metrics.workflow_errors
           << ",\"bytes_received\":" << metrics.bytes_received << ",\"seed\":" << config.seed
           << ",\"quantiles\":\"exact linear interpolation, all completed requests, 16 "
              "bytes/request tagged chunk store with at most 1 MiB unused sample capacity, 20 "
              "million request limit\""
           << ",\"duration_semantics\":\"send+wait+receive, excludes connect; interrupted requests "
              "omitted\""
           << ",\"endpoint_requests\":{\"feed\":" << metrics.endpoint_requests[0]
           << ",\"post\":" << metrics.endpoint_requests[1]
           << ",\"like\":" << metrics.endpoint_requests[2]
           << ",\"create\":" << metrics.endpoint_requests[3] << '}' << ",\"endpoint_duration_ms\":";
    duration_groups(output, latencies.endpoints, {"feed", "post", "like", "create"});
    output << ",\"phase_duration_ms\":";
    duration_groups(output, latencies.phases, {"ramp_up", "hold", "ramp_down", "grace"});
    output << ",\"phase_semantics\":\"classification by completion timestamp; stage boundaries "
              "enter the next phase\"";
    if (metrics.diagnostics)
    {
        output << ",\"diagnostics\":";
        diagnostic_summary(output, *metrics.diagnostics, metrics.elapsed);
    }
    output << "}\n}\n";
    // Format once after measurement, so nested diagnostics stay readable too
    const auto document = json(output.str());
    if (!document)
    {
        throw std::runtime_error("cannot parse generated summary");
    }
    std::size_t length = 0;
    const std::unique_ptr<char, decltype(&std::free)> formatted(
        yyjson_write(document.get(), YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END,
                     &length),
        &std::free);
    if (!formatted)
    {
        throw std::runtime_error("cannot format generated summary");
    }

    return {std::string(formatted.get(), length), passed};
}

} // namespace loadgen
