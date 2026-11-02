#pragma once

#include "../app/application.hpp"
#include "parser.hpp"

#include <cstddef>
#include <optional>
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

    const Message &message() const noexcept
    {
        return parser_.message();
    }

    bool complete() const noexcept
    {
        return parser_.complete();
    }

    bool headers_pending() const noexcept
    {
        return parser_.headers_pending();
    }

    void accept_headers(AcceptedHead head);

    // The accepted context borrows the normalized path until this message resets
    const AcceptedHead &accepted_head() const noexcept
    {
        return *accepted_;
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
    std::optional<AcceptedHead> accepted_;
};

} // namespace tds::http
