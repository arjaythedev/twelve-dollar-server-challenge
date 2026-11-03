#pragma once

#include "../telemetry/config.hpp"

#include "reply.hpp"
#include "session.hpp"

#include <cstddef>
#include <cstdint>

namespace tds::http
{

struct Exchange
{
    Exchange() = default;
    Exchange(const Exchange &) = delete;
    Exchange &operator=(const Exchange &) = delete;

    std::size_t retained_capacity() const noexcept
    {
        return session.retained_capacity() + reply.retained_capacity();
    }

    HttpSession session;
    Reply reply;
#if TDS_TELEMETRY_LEVEL == 2
    std::uint64_t send_prepared_ns = 0;
    std::uint64_t receive_serviced_ns = 0;
#endif
};

} // namespace tds::http
