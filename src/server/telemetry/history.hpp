#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace tds::telemetry
{

template <typename Snapshot> class History
{
  public:
    static constexpr std::size_t capacity = 128;

    void push(const Snapshot &snapshot) noexcept
    {
        static_assert(std::is_nothrow_copy_assignable_v<Snapshot>);
        (*slots_)[next_] = snapshot;
        next_ = (next_ + 1) % capacity;
        if (size_ < capacity)
        {
            ++size_;
        }
        else
        {
            ++overwritten_;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] const Snapshot &at(std::size_t index) const noexcept
    {
        assert(index < size_);

        return (*slots_)[(next_ + capacity - size_ + index) % capacity];
    }

    [[nodiscard]] std::uint64_t overwritten() const noexcept
    {
        return overwritten_;
    }

  private:
    // Allocate once, before measuring, and never grow while handling requests
    std::unique_ptr<std::array<Snapshot, capacity>> slots_ =
        std::make_unique<std::array<Snapshot, capacity>>();
    std::size_t next_ = 0;
    std::size_t size_ = 0;
    std::uint64_t overwritten_ = 0;
};

} // namespace tds::telemetry
