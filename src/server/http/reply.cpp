#include "reply.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>
#include <utility>

namespace tds::http
{
namespace
{

void append_decimal(std::string &output, std::size_t value)
{
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 1> digits;
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    output.append(digits.data(), static_cast<std::size_t>(converted.ptr - digits.data()));
}

std::string_view status_line(Status status)
{
    switch (status)
    {
    case Status::ok:
        return "HTTP/1.1 200 OK\r\n";
    case Status::created:
        return "HTTP/1.1 201 Created\r\n";
    case Status::bad_request:
        return "HTTP/1.1 400 Bad Request\r\n";
    case Status::unauthorized:
        return "HTTP/1.1 401 Unauthorized\r\n";
    case Status::not_found:
        return "HTTP/1.1 404 Not Found\r\n";
    case Status::too_large:
        return "HTTP/1.1 413 Payload Too Large\r\n";
    case Status::internal_error:
        return "HTTP/1.1 500 Internal Server Error\r\n";
    case Status::unavailable:
        return "HTTP/1.1 503 Service Unavailable\r\n";
    default:
        return {};
    }
}

} // namespace

void Reply::prepare(Response response, bool close_after)
{
    headers_.clear();
    const auto line = status_line(response.status);
    if (line.empty())
    {
        headers_ += "HTTP/1.1 ";
        append_decimal(headers_, static_cast<std::size_t>(response.status));
        headers_ += " Internal Server Error\r\n";
    }
    else
    {
        headers_ += line;
    }
    headers_ += "Content-Type: application/json\r\nContent-Length: ";
    append_decimal(headers_, response.body.size());
    headers_ += "\r\n";
    if (close_after)
    {
        headers_ += "Connection: close\r\n";
    }
    headers_ += "\r\n";
    body_ = std::move(response.body);
    sent_ = 0;
}

msghdr &Reply::message()
{
    // The kernel borrows these vectors and strings, so the reply cannot move or reset mid-send
    std::size_t count = 0;
    std::size_t body_offset = 0;
    if (sent_ < headers_.size())
    {
        vectors_[count++] = {headers_.data() + sent_, headers_.size() - sent_};
    }
    else
    {
        body_offset = sent_ - headers_.size();
    }
    if (body_offset < body_.size())
    {
        vectors_[count++] = {body_.data() + body_offset, body_.size() - body_offset};
    }
    message_.msg_iov = vectors_.data();
    message_.msg_iovlen = count;

    return message_;
}

bool Reply::advance(std::size_t bytes) noexcept
{
    sent_ += bytes;

    return sent_ >= headers_.size() + body_.size();
}

void Reply::reset()
{
    headers_.clear();
    body_.clear();
    if (body_.capacity() > max_retained_body_capacity)
    {
        std::string{}.swap(body_);
    }
}

std::string Reply::take_body()
{
    return std::move(body_);
}

} // namespace tds::http
