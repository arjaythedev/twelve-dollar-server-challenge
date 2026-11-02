#include "session.hpp"

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
    parser_.reset();
}

} // namespace tds::http
