#pragma once

#include <cerrno>
#include <system_error>
#include <unistd.h>

namespace tds::net
{

class Descriptor
{
  public:
    explicit Descriptor(int value) : value_(value)
    {
        if (value < 0)
        {
            throw std::system_error(errno, std::generic_category());
        }
    }

    ~Descriptor()
    {
        ::close(value_);
    }

    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;

    [[nodiscard]] int get() const noexcept
    {
        return value_;
    }

  private:
    int value_;
};

} // namespace tds::net
