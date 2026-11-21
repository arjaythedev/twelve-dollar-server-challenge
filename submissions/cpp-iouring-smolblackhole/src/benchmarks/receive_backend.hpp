#pragma once

#include "net/ring.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace tds::bench
{

enum class ReceiveMode
{
    one_shot,
    multishot
};

enum class ReceiveEvent : std::uint64_t
{
    receive = 1,
    send,
    cancel
};

enum class ReceiveWait
{
    immediate,
    production,
    ready_first,
    ready_one
};

struct ReceiveCompletion
{
    ReceiveEvent event = ReceiveEvent::receive;
    unsigned slot = 0;
    int result = 0;
    unsigned flags = 0;
    net::ReceiveBufferLease buffer;
};

struct ReceiveCounts
{
    std::uint64_t submissions = 0;
    std::uint64_t data = 0;
    std::uint64_t terminals = 0;
    std::uint64_t enobufs = 0;
    std::uint64_t sends = 0;
    std::uint64_t waits = 0;
    std::uint64_t ready_drains = 0;
    // Cursed: liburing can return submitted SQEs on timeout, so -ETIME undercounts deadlines
    std::uint64_t timeouts = 0;
};

// Experimental fixed-socket backend shared by the native contracts and CPU probe
class ReceiveBackend
{
  public:
    ReceiveBackend(int socket, ReceiveMode mode)
        : ReceiveBackend(std::span<const int>(&socket, 1), mode, ReceiveWait::immediate)
    {
    }

    ReceiveBackend(std::span<const int> sockets, ReceiveMode mode, ReceiveWait wait)
        : ring_(static_cast<unsigned>(sockets.size()), true), mode_(mode), wait_(wait),
          states_(sockets.size())
    {
        if (sockets.empty() || sockets.size() > 65535)
        {
            throw std::invalid_argument("invalid receive fixture socket count");
        }
        if (io_uring_register_files_update(&ring_.queue, 0, sockets.data(),
                                           static_cast<unsigned>(sockets.size())) !=
            static_cast<int>(sockets.size()))
        {
            throw std::runtime_error("register receive fixture socket");
        }
    }

    ReceiveBackend(const ReceiveBackend &) = delete;
    ReceiveBackend &operator=(const ReceiveBackend &) = delete;

    void receive(unsigned slot = 0)
    {
        auto &state = states_.at(slot);
        if (state.receiving)
        {
            throw std::logic_error("overlapping receives");
        }
        auto *entry = prepare(ReceiveEvent::receive, slot);
        if (mode_ == ReceiveMode::multishot)
        {
            // Slightly cursed: nonzero lengths work in newer headers, but 6.6/6.8 reject them
            io_uring_prep_recv_multishot(entry, static_cast<int>(slot), nullptr, 0, 0);
        }
        else
        {
            io_uring_prep_recv(entry, static_cast<int>(slot), nullptr, net::ReceiveBuffers::size,
                               0);
        }
        entry->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        entry->buf_group = net::ReceiveBuffers::group;
        entry->ioprio |= IORING_RECVSEND_POLL_FIRST;
        state.receiving = true;
#if TDS_RECEIVE_COUNTS
        ++counts.submissions;
#endif
    }

    void send(msghdr &message, unsigned slot = 0)
    {
        auto &state = states_.at(slot);
        if (state.sending)
        {
            throw std::logic_error("overlapping sends");
        }
        auto *entry = prepare(ReceiveEvent::send, slot);
        io_uring_prep_sendmsg(entry, static_cast<int>(slot), &message, MSG_NOSIGNAL);
        entry->flags |= IOSQE_FIXED_FILE;
        state.sending = true;
#if TDS_RECEIVE_COUNTS
        ++counts.sends;
#endif
    }

    void cancel_receive(unsigned slot = 0)
    {
        auto &state = states_.at(slot);
        if (!state.receiving || state.cancelling)
        {
            throw std::logic_error("receive cancellation has no live owner");
        }
        io_uring_prep_cancel64(prepare(ReceiveEvent::cancel, slot),
                               tag(ReceiveEvent::receive, slot), 0);
        state.cancelling = true;
    }

    void begin_turn(bool stopping = false, bool deferred = false)
    {
        int result;
        do
        {
#if TDS_RECEIVE_COUNTS
            ++counts.waits;
#endif
            const bool ready_first =
                wait_ == ReceiveWait::ready_first || wait_ == ReceiveWait::ready_one;
            if (ready_first && io_uring_cq_ready(&ring_.queue))
            {
                // Cursed: CQ-ready does not exclude deferred task work or unsent SQEs
                // submit still enters GETEVENTS for TASKRUN/overflow, without waiting for 128
                ring_.submit();
                result = 0;
#if TDS_RECEIVE_COUNTS
                ++counts.ready_drains;
#endif
            }
            else if (wait_ != ReceiveWait::immediate && !stopping &&
                     (wait_ != ReceiveWait::ready_one || deferred) &&
                     (previous_count_ >= 2 || deferred) &&
                     (ring_.queue.features & IORING_FEAT_EXT_ARG))
            {
                // Match the busy production ring, including its pre-6.12 timeout semantics
                __kernel_timespec timeout{0, deferred ? 4'000'000 : 8'000'000};
                io_uring_cqe *first = nullptr;
                result = io_uring_submit_and_wait_timeout(
                    &ring_.queue, &first,
                    wait_ == ReceiveWait::ready_one || previous_count_ < 2 ? 1 : 128, &timeout,
                    nullptr);
            }
            else
            {
                result = io_uring_submit_and_wait(&ring_.queue, 1);
            }
        } while (result == -EINTR);
        if (result < 0 && result != -EBUSY && result != -ETIME)
        {
            throw std::system_error(-result, std::generic_category(), "receive fixture wait");
        }
#if TDS_RECEIVE_COUNTS
        counts.timeouts += result == -ETIME;
#endif
        const auto &cq = ring_.queue.cq;
        head_ = *cq.khead;
        tail_ = head_ + std::min(io_uring_smp_load_acquire(cq.ktail) - head_, 256U);
        previous_count_ = tail_ - head_;
    }

    bool next(ReceiveCompletion &completion)
    {
        completion = {};
        if (head_ == tail_)
        {
            return false;
        }
        const auto &cq = ring_.queue.cq;
        const auto &entry = cq.cqes[head_++ & cq.ring_mask];
        const auto identity = io_uring_cqe_get_data64(&entry);
        completion.event = static_cast<ReceiveEvent>(identity & 3U);
        completion.slot = static_cast<unsigned>(identity >> 2);
        auto &state = states_.at(completion.slot);
        completion.result = entry.res;
        completion.flags = entry.flags;
        const bool more = (entry.flags & IORING_CQE_F_MORE) != 0;
        const bool selected = (entry.flags & IORING_CQE_F_BUFFER) != 0;
        const auto id = entry.flags >> IORING_CQE_BUFFER_SHIFT;
        if (selected && (id >= net::ReceiveBuffers::count ||
                         entry.res > static_cast<int>(net::ReceiveBuffers::size)))
        {
            throw std::runtime_error("invalid selected receive buffer");
        }
        if (selected)
        {
            completion.buffer = net::ReceiveBufferLease(
                ring_.receive_buffers.get(), static_cast<unsigned short>(id),
                entry.res > 0 ? static_cast<std::size_t>(entry.res) : 0);
        }
        switch (completion.event)
        {
        case ReceiveEvent::receive:
            if (!state.receiving || (more && mode_ == ReceiveMode::one_shot) ||
                (entry.res > 0 && !selected))
            {
                throw std::runtime_error("receive completion lost its live operation or buffer");
            }
            if (!more)
            {
                state.receiving = false;
                --ring_.pending;
            }
#if TDS_RECEIVE_COUNTS
            counts.data += entry.res > 0;
            counts.terminals += !more;
            counts.enobufs += entry.res == -ENOBUFS;
#endif
            break;
        case ReceiveEvent::send:
            if (!state.sending || more || selected)
            {
                throw std::runtime_error("send completion lost its live operation");
            }
            state.sending = false;
            --ring_.pending;
            break;
        case ReceiveEvent::cancel:
            if (!state.cancelling || more || selected)
            {
                throw std::runtime_error("cancel completion lost its live operation");
            }
            state.cancelling = false;
            --ring_.pending;
            break;
        default:
            throw std::runtime_error("unknown receive fixture completion");
        }

        return true;
    }

    void finish_turn()
    {
        if (head_ != tail_)
        {
            throw std::logic_error("receive fixture advanced a partially consumed CQ batch");
        }
        io_uring_smp_store_release(ring_.queue.cq.khead, head_);
    }

    bool receiving(unsigned slot = 0) const
    {
        return states_.at(slot).receiving;
    }

    bool sending(unsigned slot = 0) const
    {
        return states_.at(slot).sending;
    }

    unsigned pending() const noexcept
    {
        return ring_.pending;
    }

    net::Ring &owner() noexcept
    {
        return ring_;
    }

    ReceiveCounts counts;

  private:
    static std::uint64_t tag(ReceiveEvent event, unsigned slot) noexcept
    {
        return (static_cast<std::uint64_t>(slot) << 2) | static_cast<std::uint64_t>(event);
    }

    io_uring_sqe *prepare(ReceiveEvent event, unsigned slot)
    {
        auto *entry = ring_.entry(nullptr);
        io_uring_sqe_set_data64(entry, tag(event, slot));

        return entry;
    }

    struct State
    {
        bool receiving = false;
        bool sending = false;
        bool cancelling = false;
    };

    net::Ring ring_;
    ReceiveMode mode_;
    ReceiveWait wait_;
    std::vector<State> states_;
    unsigned previous_count_ = 0;
    unsigned head_ = 0;
    unsigned tail_ = 0;
};

} // namespace tds::bench
