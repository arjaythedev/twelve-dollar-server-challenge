#include "session.hpp"

#include <utility>

namespace tds::http
{

void HttpSession::append(std::string_view bytes)
{
    input_.append(bytes);
}

ParseResult HttpSession::consume()
{
    const auto result = parser_.parse(input_);
    if (result.valid)
    {
        // A header pause leaves the body here, a message pause leaves the next pipeline request
        input_.erase(0, result.consumed);
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
