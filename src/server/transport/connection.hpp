#pragma once

#include "../http/reply.hpp"
#include "../http/session.hpp"

#include <cstddef>
#include <cstdint>

namespace tds::transport
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

    http::HttpSession session;
    http::Reply reply;
#ifdef TDS_DIAGNOSTICS
    std::uint64_t send_prepared_ns = 0;
    std::uint64_t receive_serviced_ns = 0;
#endif
};

} // namespace tds::transport
