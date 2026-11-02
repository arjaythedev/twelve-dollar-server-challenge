#pragma once

#include "../http.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>

namespace tds::http
{

class Reply
{
  public:
    Reply() = default;
    Reply(const Reply &) = delete;
    Reply &operator=(const Reply &) = delete;

    void prepare(Response response, bool close_after);
    msghdr &message();
    bool advance(std::size_t bytes) noexcept;
    // The terminal send CQE must arrive before any referenced storage is released
    void reset();

    std::size_t retained_capacity() const noexcept
    {
        return headers_.capacity() + body_.capacity();
    }

  private:
    std::string headers_;
    std::string body_;
    std::array<iovec, 2> vectors_{};
    msghdr message_{};
    std::size_t sent_ = 0;
};

} // namespace tds::http
