#pragma once

#ifdef TDS_DIAGNOSTICS

#include "../timing.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace tds::net
{
class Ring;
}

namespace tds::transport
{

struct TransportDiagnostics
{
    void start() noexcept;
    void report(std::span<const std::unique_ptr<net::Ring>> rings, bool final) noexcept;

    timing::Histogram handler_wall_ns;
    timing::Histogram receive_to_handler_ns;
    timing::Histogram write_batch_size;
    timing::Histogram write_batch_wall_ns;
    std::uint64_t diagnostics_started_ns = 0;
    std::uint64_t diagnostics_last_ns = 0;
    std::uint64_t previous_report_write_ns = 0;
};

} // namespace tds::transport

#endif
