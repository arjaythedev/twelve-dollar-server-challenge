#pragma once

#include "parser.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace tds::http
{

class HttpSession
{
  public:
    HttpSession() = default;
    HttpSession(const HttpSession &) = delete;
    HttpSession &operator=(const HttpSession &) = delete;

    void append(std::string_view bytes);
    ParseResult consume();

    Request &request() noexcept
    {
        return parser_.request();
    }

    const Request &request() const noexcept
    {
        return parser_.request();
    }

    bool complete() const noexcept
    {
        return parser_.complete();
    }

    bool headers_pending() const noexcept
    {
        return parser_.headers_pending();
    }

    void accept_headers() noexcept
    {
        parser_.accept_headers();
    }

    Status error() const noexcept
    {
        return parser_.error();
    }

    bool has_input() const noexcept
    {
        return !input_.empty();
    }

    std::size_t retained_capacity() const noexcept
    {
        return input_.capacity() + parser_.scratch_capacity();
    }

    // Reset one message without discarding the following pipelined request
    void reset();

  private:
    Parser parser_;
    std::string input_;
};

} // namespace tds::http
