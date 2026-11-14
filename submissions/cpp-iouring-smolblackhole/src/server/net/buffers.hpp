#pragma once

#include <liburing.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace tds::net
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
        return storage_.get() + id * size;
    }

    void release(unsigned short id) noexcept
    {
        io_uring_buf_ring_add(buffers_, data(id), size, id, io_uring_buf_ring_mask(count), 0);
        io_uring_buf_ring_advance(buffers_, 1);
        if (retry_credits_ < count)
        {
            ++retry_credits_;
        }
    }

    unsigned take_retry_credits(unsigned limit) noexcept
    {
        // Returned buffers prove progress, not availability
        // Pending receives can consume them before a retry is submitted
        const auto credits = std::min(retry_credits_, limit);
        retry_credits_ -= credits;

        return credits;
    }

  private:
    io_uring &ring_;
    std::unique_ptr<char[]> storage_;
    io_uring_buf_ring *buffers_ = nullptr;
    unsigned retry_credits_ = 0;
};

class ReceiveBufferLease
{
  public:
    ReceiveBufferLease() = default;

    ReceiveBufferLease(ReceiveBuffers *owner, unsigned short id, std::size_t length = 0) noexcept
        : owner_(owner), length_(length), id_(id)
    {
    }

    ~ReceiveBufferLease()
    {
        release();
    }

    ReceiveBufferLease(const ReceiveBufferLease &) = delete;
    ReceiveBufferLease &operator=(const ReceiveBufferLease &) = delete;

    ReceiveBufferLease(ReceiveBufferLease &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), length_(other.length_), id_(other.id_)
    {
    }

    ReceiveBufferLease &operator=(ReceiveBufferLease &&other) noexcept
    {
        if (this != &other)
        {
            release();
            owner_ = std::exchange(other.owner_, nullptr);
            length_ = other.length_;
            id_ = other.id_;
        }

        return *this;
    }

    std::span<const char> bytes() const noexcept
    {
        if (!owner_)
        {
            return {};
        }

        return {owner_->data(id_), length_};
    }

    void release() noexcept
    {
        if (owner_)
        {
            owner_->release(id_);
            owner_ = nullptr;
        }
    }

  private:
    ReceiveBuffers *owner_ = nullptr;
    std::size_t length_ = 0;
    unsigned short id_ = 0;
};

} // namespace tds::net
