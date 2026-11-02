#include "diagnostics.hpp"

#ifdef TDS_DIAGNOSTICS

#include "ring.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdio>

namespace tds::transport
{

void TransportDiagnostics::start() noexcept
{
    diagnostics_started_ns = timing::now_ns();
    diagnostics_last_ns = diagnostics_started_ns;
}

void TransportDiagnostics::report(std::span<const std::unique_ptr<Ring>> rings, bool final) noexcept
{
    const auto now = timing::now_ns();
    const auto interval = now - diagnostics_last_ns;
    if (!final && interval < 5'000'000'000ULL)
    {
        return;
    }
    const auto write_started_ns = timing::now_ns();
    ::flockfile(stderr);
    std::fprintf(stderr,
                 "TDS_TIMING {\"source\":\"transport\",\"time_ns\":%" PRIu64
                 ",\"elapsed_ns\":%" PRIu64 ",\"interval_ns\":%" PRIu64
                 ",\"previous_report_write_ns\":%" PRIu64 ",\"final\":%s,",
                 now, now - diagnostics_started_ns, interval, previous_report_write_ns,
                 final ? "true" : "false");
    handler_wall_ns.write(stderr, "handler_wall_ns");
    handler_wall_ns.reset();
    std::fputc(',', stderr);
    write_batch_size.write(stderr, "write_batch_size", "requests");
    write_batch_size.reset();
    std::fputc(',', stderr);
    write_batch_wall_ns.write(stderr, "write_batch_wall_ns");
    write_batch_wall_ns.reset();
    std::fputc(',', stderr);
    receive_to_handler_ns.write(stderr, "receive_cqe_service_to_handler_start_ns");
    receive_to_handler_ns.reset();
    std::fputs(",\"rings\":[", stderr);
    for (std::size_t index = 0; index < rings.size(); ++index)
    {
        auto &owner = *rings[index];
        auto &sample = owner.diagnostics;
        const auto overflow = io_uring_smp_load_acquire(owner.queue.cq.koverflow);
        std::fprintf(stderr,
                     "%s{\"ring\":%zu,\"cqes\":%" PRIu64 ",\"receive_enobufs\":%" PRIu64
                     ",\"cq_empty_samples\":%" PRIu64 ",\"cq_overflow_flag_samples\":%" PRIu64
                     ",\"cq_overflow_counter_delta\":%u,",
                     index ? "," : "", index, sample.cqes, sample.receive_enobufs,
                     sample.empty_cq_samples, sample.overflow_flag_samples,
                     overflow - sample.previous_overflow);
        sample.cq_depth.write(stderr, "cq_ready_before_drain", "cqes");
        std::fputc(',', stderr);
        sample.deferred_receive_depth.write(stderr, "deferred_receive_queue_depth", "connections");
        std::fputc(',', stderr);
        // Each attempt includes submission batching and CQ service delay, not just socket I/O
        sample.send_attempt.write(stderr, "send_attempt_prepare_to_cqe_service_ns");
        std::fputc('}', stderr);
        sample.cq_depth.reset();
        sample.deferred_receive_depth.reset();
        sample.send_attempt.reset();
        sample.cqes = 0;
        sample.receive_enobufs = 0;
        sample.empty_cq_samples = 0;
        sample.overflow_flag_samples = 0;
        sample.previous_overflow = overflow;
    }
    std::fputs("]}\n", stderr);
    ::funlockfile(stderr);
    previous_report_write_ns = timing::now_ns() - write_started_ns;
    diagnostics_last_ns = now;
}

} // namespace tds::transport

#endif
