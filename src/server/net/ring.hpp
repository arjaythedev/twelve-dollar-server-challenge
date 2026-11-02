#pragma once

#include "../timing.hpp"
#include "buffers.hpp"
#include "connections.hpp"
#include "descriptor.hpp"

#include <liburing.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace tds::net
{

class Ring
{
  public:
    Ring(unsigned slots, bool primary);
    ~Ring();
    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    std::unique_ptr<transport::Exchange> acquire_exchange();
    // Only reset exchanges without pipeline input enter this ring-local pool
    void recycle_exchange(std::unique_ptr<transport::Exchange> exchange);

    void submit()
    {
        int result;
        do
        {
            result = io_uring_submit(&queue);
        } while (result == -EINTR);
        // NODROP retains overflowed CQEs, drain them before another enter
        if (result < 0 && result != -EBUSY)
        {
            throw std::system_error(-result, std::generic_category(), "submit");
        }
    }

    io_uring_sqe *entry(Operation *operation)
    {
        auto *value = io_uring_get_sqe(&queue);
        if (!value)
        {
            submit();
            value = io_uring_get_sqe(&queue);
            if (!value)
            {
                throw std::runtime_error("submission queue exhausted");
            }
        }
        io_uring_sqe_set_data(value, operation);
        ++pending;

        return value;
    }

    io_uring queue{};
    std::unique_ptr<ReceiveBuffers> receive_buffers;
    std::unique_ptr<Descriptor> notification;
    ConnectionStore connections;
    // Deferred records have no SQE in flight and remain owned by connections
    std::deque<Connection *> deferred_receives;
    Operation wake_operation;
    unsigned pending = 0;
    std::size_t shutdown_cursor = 0;
    bool wake_pending = false;
#ifdef TDS_DIAGNOSTICS
    struct Diagnostics
    {
        timing::Histogram cq_depth;
        timing::Histogram deferred_receive_depth;
        timing::Histogram send_attempt;
        std::uint64_t cqes = 0;
        std::uint64_t receive_enobufs = 0;
        std::uint64_t empty_cq_samples = 0;
        std::uint64_t overflow_flag_samples = 0;
        unsigned previous_overflow = 0;
    } diagnostics;
#endif

  private:
    static constexpr std::size_t max_pooled_exchanges = 1024;
    static constexpr std::size_t max_retained_capacity = 8192;
    std::vector<std::unique_ptr<transport::Exchange>> exchange_pool_;
};

} // namespace tds::net
