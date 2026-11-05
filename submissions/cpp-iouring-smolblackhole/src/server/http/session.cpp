#include "session.hpp"

#include <utility>

namespace tds::http
{

void HttpSession::append(std::string_view bytes)
{
    if (consumed_ != 0 && bytes.size() > input_.capacity() - input_.size())
    {
        // Reclaim the consumed prefix only when appending would otherwise grow the buffer
        input_.erase(0, consumed_);
        consumed_ = 0;
    }
    input_.append(bytes);
}

ParseResult HttpSession::consume()
{
    const std::string_view unread(input_.data() + consumed_, input_.size() - consumed_);
    const auto result = parser_.parse(unread);
    if (!result.valid)
    {
        return result;
    }
    consumed_ += result.consumed;
    if (consumed_ == input_.size())
    {
        input_.clear();
        consumed_ = 0;
    }

    return result;
}

void HttpSession::reset()
{
    // Drop borrowed route views before reusing the parser's strings
    accepted_.reset();
    parser_.reset();
}

void HttpSession::accept_headers(AcceptedHead head)
{
    accepted_ = std::move(head);
    parser_.accept_headers();
}

} // namespace tds::http
