#include "io.hpp"

#if TDS_TELEMETRY_LEVEL >= 1

#include "../http/exchange.hpp"
#include "../net/reactor.hpp"
#include "../telemetry/output.hpp"

#include <cerrno>
#include <cstddef>
#include <variant>

namespace tds::telemetry
{
namespace
{
#if TDS_TELEMETRY_LEVEL == 2
void merge(IoTimings &target, const IoTimings &sample) noexcept
{
    target.handler_wall_ns.merge(sample.handler_wall_ns);
    target.receive_cqe_service_to_handler_start_ns.merge(
        sample.receive_cqe_service_to_handler_start_ns);
    target.write_batch_size.merge(sample.write_batch_size);
    target.write_batch_wall_ns.merge(sample.write_batch_wall_ns);
}

void merge(RingTimings &target, const RingTimings &sample) noexcept
{
    target.cq_ready_before_drain.merge(sample.cq_ready_before_drain);
    target.deferred_receive_queue_depth.merge(sample.deferred_receive_queue_depth);
    target.send_attempt_prepare_to_cqe_service_ns.merge(
        sample.send_attempt_prepare_to_cqe_service_ns);
    target.enobufs_to_successful_receive_service_ns.merge(
        sample.enobufs_to_successful_receive_service_ns);
}

RingCounters difference(const RingCounters &current, const RingCounters &previous) noexcept
{
    RingCounters interval;
    interval.cqes = current.cqes - previous.cqes;
    interval.receive_submissions = current.receive_submissions - previous.receive_submissions;
    interval.receive_completions = current.receive_completions - previous.receive_completions;
    interval.receive_enobufs = current.receive_enobufs - previous.receive_enobufs;
    interval.empty_cq_samples = current.empty_cq_samples - previous.empty_cq_samples;
    interval.overflow_flag_samples = current.overflow_flag_samples - previous.overflow_flag_samples;
    interval.cq_overflow_counter_delta =
        current.cq_overflow_counter_delta - previous.cq_overflow_counter_delta;
    interval.exchange_pool_hits = current.exchange_pool_hits - previous.exchange_pool_hits;
    interval.exchange_allocations = current.exchange_allocations - previous.exchange_allocations;

    return interval;
}
#endif
} // namespace

IoTelemetry::Run::Run(IoTelemetry &owner, net::Reactor &reactor) noexcept
    : owner_(owner), reactor_(reactor)
{
#if TDS_TELEMETRY_LEVEL == 2
    owner_.start();
#endif
}

IoTelemetry::Run::~Run()
{
    owner_.finalize(reactor_);
}

#if TDS_TELEMETRY_LEVEL == 2
IoTelemetry::Handler::Handler(IoTelemetry &owner, net::Connection &connection) noexcept
    : owner_(owner)
{
    const auto &head = connection.exchange->session.accepted_head();
    active_ = !std::holds_alternative<CreateHead>(head) && !std::holds_alternative<LikeHead>(head);
    if (active_)
    {
        owner_.interval_.receive_cqe_service_to_handler_start_ns.record(
            timing::now_ns() - connection.exchange->receive_serviced_ns);
        started_ns_ = timing::now_ns();
    }
}

IoTelemetry::Handler::~Handler()
{
    finish();
}

void IoTelemetry::Handler::finish() noexcept
{
    if (active_)
    {
        owner_.interval_.handler_wall_ns.record(timing::now_ns() - started_ns_);
        active_ = false;
    }
}

IoTelemetry::WriteBatch::WriteBatch(IoTelemetry &owner, std::size_t count) noexcept : owner_(owner)
{
    owner_.interval_.write_batch_size.record(count);
    started_ns_ = timing::now_ns();
}

IoTelemetry::WriteBatch::~WriteBatch()
{
    owner_.interval_.write_batch_wall_ns.record(timing::now_ns() - started_ns_);
}

IoTelemetry::Receive::Receive() noexcept : serviced_ns_(timing::now_ns())
{
}

void IoTelemetry::Receive::attach(net::Connection &connection) const noexcept
{
    // Buffered pipeline requests keep this time through earlier response sends
    connection.exchange->receive_serviced_ns = serviced_ns_;
}

void IoTelemetry::send_prepared(net::Connection &connection) noexcept
{
    connection.exchange->send_prepared_ns = timing::now_ns();
}

void IoTelemetry::send_serviced(net::Completion &completion) noexcept
{
    // Close preparation may submit SQEs, so measure service before that extra work
    completion.serviced_ns = timing::now_ns();
}

void IoTelemetry::send_completed(const net::Completion &completion) noexcept
{
    auto &connection = *completion.connection;
    connection.ring.statistics.interval.send_attempt_prepare_to_cqe_service_ns.record(
        completion.serviced_ns - connection.exchange->send_prepared_ns);
}

void IoTelemetry::receive_deferred(net::Connection &connection) noexcept
{
    if (!connection.receive_enobufs_since_ns)
    {
        connection.receive_enobufs_since_ns = timing::now_ns();
    }
}
#endif

void IoTelemetry::drain_started(net::Ring &owner) noexcept
{
    const auto depth = io_uring_cq_ready(&owner.queue);
    auto &totals = owner.statistics.totals;
    totals.empty_cq_samples += depth == 0;
    totals.overflow_flag_samples +=
        (io_uring_smp_load_acquire(owner.queue.sq.kflags) & IORING_SQ_CQ_OVERFLOW) != 0;
#if TDS_TELEMETRY_LEVEL == 2
    owner.statistics.interval.cq_ready_before_drain.record(depth);
#endif
}

void IoTelemetry::drain_finished(net::Ring &owner, unsigned drained) noexcept
{
    owner.statistics.totals.cqes += drained;
#if TDS_TELEMETRY_LEVEL == 2
    owner.statistics.interval.deferred_receive_queue_depth.record(owner.deferred_receives.size());
#endif
}

void IoTelemetry::receive_prepared(net::Connection &connection) noexcept
{
    // Count prepared receive SQEs, including retries, rather than enter syscalls
    ++connection.ring.statistics.totals.receive_submissions;
}

void IoTelemetry::receive_completed(net::Connection &connection, int result) noexcept
{
    auto &sample = connection.ring.statistics;
    ++sample.totals.receive_completions;
    if (result == -ENOBUFS)
    {
        ++sample.totals.receive_enobufs;
    }
#if TDS_TELEMETRY_LEVEL == 2
    else if (result > 0 && connection.receive_enobufs_since_ns)
    {
        // Deferral includes retry I/O and CQ service, not socket arrival age
        sample.interval.enobufs_to_successful_receive_service_ns.record(
            timing::now_ns() - connection.receive_enobufs_since_ns);
        connection.receive_enobufs_since_ns = 0;
    }
#endif
}

void IoTelemetry::store_created(net::ConnectionStore &owner) noexcept
{
    ++owner.live_count_;
}

void IoTelemetry::store_retired(net::ConnectionStore &owner) noexcept
{
    --owner.live_count_;
}

void IoTelemetry::exchange_allocated(net::Ring &owner) noexcept
{
    ++owner.statistics.totals.exchange_allocations;
}

void IoTelemetry::exchange_reused(net::Ring &owner) noexcept
{
    ++owner.statistics.totals.exchange_pool_hits;
}

RingSnapshot IoTelemetry::snapshot_ring(net::Ring &owner, std::size_t index,
                                        [[maybe_unused]] std::uint64_t now) noexcept
{
    auto &statistics = owner.statistics;
    const auto overflow = io_uring_smp_load_acquire(owner.queue.cq.koverflow);
    statistics.totals.cq_overflow_counter_delta += overflow - statistics.previous_overflow;
    statistics.previous_overflow = overflow;
    RingSnapshot snapshot;
    snapshot.ring = index;
    snapshot.counters = statistics.totals;
    snapshot.live_connection_records = owner.connections.live_count();
    snapshot.reserved_accept_slots = owner.connections.reserved_count();
    snapshot.deferred_receive_queue_depth = owner.deferred_receives.size();
    snapshot.pooled_exchanges = owner.exchange_pool_.size();
#if TDS_TELEMETRY_LEVEL == 2
    // Retries can rejoin the FIFO, so the front need not have the oldest first ENOBUFS
    const auto *front = owner.deferred_receives.empty() ? nullptr : owner.deferred_receives.front();
    snapshot.deferred_receive_front_enobufs_age_ns =
        front ? now - front->receive_enobufs_since_ns : 0;
    snapshot.timings = statistics.interval;
#endif

    return snapshot;
}

#if TDS_TELEMETRY_LEVEL == 2
void IoTelemetry::start() noexcept
{
    totals_.start_ns = timing::now_ns();
    totals_.end_ns = totals_.start_ns;
}

void IoTelemetry::report(const net::Reactor &reactor, bool force) noexcept
{
    const auto now = timing::now_ns();
    if (!force && now - totals_.end_ns < timing::report_interval_ns)
    {
        return;
    }
    capture(reactor, now);
}

void IoTelemetry::capture(const net::Reactor &reactor, std::uint64_t now) noexcept
{
    auto &window = history_.next();
    window.start_ns = totals_.end_ns;
    window.end_ns = now;
    window.timings = interval_;
    merge(totals_.timings, interval_);
    interval_ = {};
    const auto rings = reactor.rings();
    window.ring_count = rings.size();
    totals_.ring_count = rings.size();
    for (std::size_t index = 0; index < rings.size(); ++index)
    {
        auto &owner = *rings[index];
        auto &interval = window.rings[index];
        interval = snapshot_ring(owner, index, now);

        auto cumulative_timings = totals_.rings[index].timings;
        merge(cumulative_timings, interval.timings);
        totals_.rings[index] = interval;
        totals_.rings[index].timings = cumulative_timings;
        interval.counters = difference(interval.counters, owner.statistics.previous);
        owner.statistics.previous = totals_.rings[index].counters;
        owner.statistics.interval = {};
    }
    totals_.end_ns = now;
}
#endif

void IoTelemetry::finalize(const net::Reactor &reactor) noexcept
{
#if TDS_TELEMETRY_LEVEL == 2
    report(reactor, true);
    write_report({level, totals_, &history_, history_.overwritten()});
#else
    IoSnapshot totals;
    totals.end_ns = timing::now_ns();
    const auto rings = reactor.rings();
    totals.ring_count = rings.size();
    for (std::size_t index = 0; index < rings.size(); ++index)
    {
        totals.rings[index] = snapshot_ring(*rings[index], index, totals.end_ns);
    }
    write_report({level, totals});
#endif
}

} // namespace tds::telemetry

#endif
