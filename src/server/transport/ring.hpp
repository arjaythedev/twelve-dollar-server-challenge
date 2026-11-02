#pragma once

#include "../timing.hpp"
#include "connection.hpp"
#include "descriptor.hpp"

#include <liburing.h>

#include <cerrno>
#include <cstddef>
#include <deque>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace tds::transport
{

class ReceiveBuffers
{
  public:
    static constexpr unsigned count = 1024;
    static constexpr unsigned size = 4096;
    static constexpr int group = 0;

    explicit ReceiveBuffers(io_uring &owner);
    ~ReceiveBuffers();

    ReceiveBuffers(const ReceiveBuffers &) = delete;
    ReceiveBuffers &operator=(const ReceiveBuffers &) = delete;

    char *data(unsigned id) noexcept
    {
        return storage.get() + id * size;
    }

    void release(unsigned short id) noexcept
    {
        io_uring_buf_ring_add(buffers, data(id), size, id, io_uring_buf_ring_mask(count), 0);
        io_uring_buf_ring_advance(buffers, 1);
    }

  private:
    io_uring &ring;
    std::unique_ptr<char[]> storage;
    io_uring_buf_ring *buffers = nullptr;
};

class ReceiveBufferLease
{
  public:
    ReceiveBufferLease(ReceiveBuffers *owner, unsigned short id) : owner_(owner), id_(id)
    {
    }

    ~ReceiveBufferLease()
    {
        release();
    }

    ReceiveBufferLease(const ReceiveBufferLease &) = delete;
    ReceiveBufferLease &operator=(const ReceiveBufferLease &) = delete;

    void release() noexcept
    {
        if (owner_)
        {
            owner_->release(id_);
            owner_ = nullptr;
        }
    }

  private:
    ReceiveBuffers *owner_;
    unsigned short id_;
};

class Ring
{
  public:
    Ring(unsigned slots, bool primary);
    ~Ring();

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    std::unique_ptr<Exchange> acquire_exchange();
    // Completed responses enter the pool with a reset parser and no buffered input
    void recycle_exchange(std::unique_ptr<Exchange> exchange);

    void submit()
    {
        int result;
        do
        {
            result = io_uring_submit(&queue);
        } while (result == -EINTR);
        // NODROP retains overflowed CQEs, drain the CQ before trying another enter
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
    std::vector<std::unique_ptr<Connection>> connections;
    std::vector<unsigned> free_slots;
    // Deferred connections have no operation in flight and remain owned by connections
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
    std::vector<std::unique_ptr<Exchange>> exchange_pool_;
};

} // namespace tds::transport
