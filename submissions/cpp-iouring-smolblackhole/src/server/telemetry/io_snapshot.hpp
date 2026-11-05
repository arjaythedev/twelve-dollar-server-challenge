#pragma once

#include "../timing.hpp"
#include "config.hpp"
#include "history.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tds::telemetry
{

inline constexpr std::size_t io_ring_capacity = 16;

struct RingCounters
{
    std::uint64_t cqes = 0;
    std::uint64_t receive_submissions = 0;
    std::uint64_t receive_completions = 0;
    std::uint64_t receive_enobufs = 0;
    std::uint64_t empty_cq_samples = 0;
    std::uint64_t overflow_flag_samples = 0;
    std::uint64_t cq_overflow_counter_delta = 0;
    std::uint64_t exchange_pool_hits = 0;
    std::uint64_t exchange_allocations = 0;
};

struct RingTimings
{
    timing::Histogram cq_ready_before_drain;
    timing::Histogram deferred_receive_queue_depth;
    timing::Histogram send_attempt_prepare_to_cqe_service_ns;
    timing::Histogram enobufs_to_successful_receive_service_ns;
};

struct RingSnapshot
{
    std::size_t ring = 0;
    RingCounters counters;
    std::size_t live_connection_records = 0;
    std::size_t reserved_accept_slots = 0;
    std::size_t deferred_receive_queue_depth = 0;
    std::size_t pooled_exchanges = 0;
    std::uint64_t deferred_receive_front_enobufs_age_ns = 0;
    RingTimings timings;
};

struct IoTimings
{
    timing::Histogram handler_wall_ns;
    timing::Histogram receive_cqe_service_to_handler_start_ns;
    timing::Histogram write_batch_size;
    timing::Histogram write_batch_wall_ns;
};

struct IoSnapshot
{
    std::uint64_t start_ns = 0;
    std::uint64_t end_ns = 0;
    IoTimings timings;
    std::size_t ring_count = 0;
    std::array<RingSnapshot, io_ring_capacity> rings{};
};

struct IoReport
{
    Level level;
    const IoSnapshot &totals;
    const History<IoSnapshot> *history = nullptr;
    std::uint64_t overwritten_history = 0;
};

} // namespace tds::telemetry
