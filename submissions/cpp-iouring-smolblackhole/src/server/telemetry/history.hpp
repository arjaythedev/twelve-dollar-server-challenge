#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace tds::telemetry
{

template <typename Snapshot> class History
{
  public:
    static constexpr std::size_t capacity = 128;

    History()
    {
        slots_.reserve(capacity);
    }

    History(const History &) = delete;
    History &operator=(const History &) = delete;

    // Slots stay at fixed addresses and retain their values when reused
    [[nodiscard]] Snapshot &next() noexcept
    {
        static_assert(std::is_nothrow_default_constructible_v<Snapshot>);
        if (slots_.size() < capacity)
        {
            slots_.emplace_back();
        }
        else
        {
            ++overwritten_;
        }
        auto &snapshot = slots_[next_];
        next_ = (next_ + 1) % capacity;

        return snapshot;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return slots_.size();
    }

    [[nodiscard]] const Snapshot &at(std::size_t index) const noexcept
    {
        assert(index < size());

        return slots_[(next_ + capacity - size() + index) % capacity];
    }

    [[nodiscard]] std::uint64_t overwritten() const noexcept
    {
        return overwritten_;
    }

  private:
    // Reserve once before requests, construct each slot only on its first capture
    std::vector<Snapshot> slots_;
    std::size_t next_ = 0;
    std::uint64_t overwritten_ = 0;
};

} // namespace tds::telemetry
