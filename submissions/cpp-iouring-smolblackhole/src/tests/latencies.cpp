#include "../load/metrics.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace loadgen;

struct Observation
{
    double milliseconds;
    EndpointKind endpoint;
    Phase phase;
};

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

// Keep the old sorted-vector calculation independent of the new store
double old_percentile(const std::vector<double> &sorted, double fraction)
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

void same_statistics(const LatencyStatistics &actual, std::vector<double> expected)
{
    std::sort(expected.begin(), expected.end());
    double sum = 0;
    for (const double sample : expected)
    {
        sum += sample;
    }
    require(actual.count == expected.size(), "exact sample count");
    require(actual.average == (expected.empty() ? 0 : sum / static_cast<double>(expected.size())),
            "mean uses the same sorted accumulation order");
    require(actual.median == old_percentile(expected, 0.5), "exact interpolated median");
    require(actual.p95 == old_percentile(expected, 0.95), "exact interpolated p95");
    require(actual.p99 == old_percentile(expected, 0.99), "exact interpolated p99");
    require(actual.maximum == (expected.empty() ? 0 : expected.back()), "exact maximum");
    require(actual.minimum == (expected.empty() ? 0 : expected.front()), "exact minimum");
}

void equivalent(std::span<const Observation> observations)
{
    Latencies recorded;
    std::vector<double> overall;
    std::array<std::vector<double>, 4> endpoints;
    std::array<std::vector<double>, 4> phases;
    for (const auto &observation : observations)
    {
        recorded.record(observation.milliseconds, observation.endpoint, observation.phase);
        overall.push_back(observation.milliseconds);
        endpoints[static_cast<std::size_t>(observation.endpoint)].push_back(
            observation.milliseconds);
        phases[static_cast<std::size_t>(observation.phase)].push_back(observation.milliseconds);
    }
    require(recorded.size() == observations.size(), "recording retains every sample");
    require(recorded.empty() == observations.empty(), "empty state before finalization");
    // Returning Metrics moves the store out of the runtime before reporting
    auto actual = std::move(recorded);
    for (unsigned repetition = 0; repetition < 2; ++repetition)
    {
        actual.prepare();
        const auto &summary = actual.summary();
        same_statistics(summary.overall, overall);
        for (std::size_t index = 0; index < endpoints.size(); ++index)
        {
            same_statistics(summary.endpoints[index], endpoints[index]);
            same_statistics(summary.phases[index], phases[index]);
        }
        require(actual.size() == observations.size(), "finalization preserves the sample count");
    }
}

void test_exact_samples()
{
    equivalent({});
    const std::array single{Observation{0.125, EndpointKind::create, Phase::grace}};
    equivalent(single);
    const std::array unsorted{
        Observation{3, EndpointKind::post, Phase::ramp_up},
        Observation{0, EndpointKind::feed, Phase::hold},
        Observation{2, EndpointKind::post, Phase::hold},
        Observation{1, EndpointKind::like, Phase::ramp_down},
    };
    equivalent(unsorted);
    const std::array repeated{
        Observation{5.5, EndpointKind::feed, Phase::hold},
        Observation{5.5, EndpointKind::feed, Phase::hold},
        Observation{5.5, EndpointKind::feed, Phase::hold},
    };
    equivalent(repeated);
    const std::array magnitudes{
        Observation{1'000'000, EndpointKind::feed, Phase::hold},
        Observation{0.1, EndpointKind::feed, Phase::hold},
        Observation{0.0000000001, EndpointKind::feed, Phase::hold},
        Observation{0.2, EndpointKind::feed, Phase::hold},
        Observation{0.3, EndpointKind::feed, Phase::hold},
    };
    equivalent(magnitudes);

    std::vector<Observation> mixed;
    std::mt19937_64 random(42);
    for (unsigned index = 0; index < 10'000; ++index)
    {
        const auto endpoint = static_cast<EndpointKind>(random() % 4);
        const auto phase = static_cast<Phase>(random() % 4);
        const double milliseconds = static_cast<double>(random() % 1'000'000) / 1000;
        mixed.push_back({milliseconds, endpoint, phase});
    }
    equivalent(mixed);
}

void test_chunk_growth()
{
    static_assert(Latencies::sample_bytes == 16);
    std::vector<Observation> observations;
    observations.reserve(Latencies::chunk_samples * 2 + 17);
    for (std::size_t index = 0; index < Latencies::chunk_samples * 2 + 17; ++index)
    {
        const double value = static_cast<double>((index * 17) % 999) / 13;
        observations.push_back(
            {value, static_cast<EndpointKind>(index % 4), static_cast<Phase>((index / 5) % 4)});
    }
    equivalent(observations);
}

} // namespace

int main()
{
    try
    {
        test_exact_samples();
        test_chunk_growth();
        std::cout << "Exact latency storage matches the original vectors\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
