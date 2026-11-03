#include "reply.hpp"

#include <string_view>
#include <utility>

namespace tds::http
{
namespace
{

std::string_view reason(Status status)
{
    switch (status)
    {
    case Status::ok:
        return "OK";
    case Status::created:
        return "Created";
    case Status::bad_request:
        return "Bad Request";
    case Status::unauthorized:
        return "Unauthorized";
    case Status::not_found:
        return "Not Found";
    case Status::too_large:
        return "Payload Too Large";
    case Status::unavailable:
        return "Service Unavailable";
    default:
        return "Internal Server Error";
    }
}

} // namespace

void Reply::prepare(Response response, bool close_after)
{
    headers_.clear();
    headers_ += "HTTP/1.1 ";
    headers_ += std::to_string(static_cast<unsigned>(response.status));
    headers_ += ' ';
    headers_ += reason(response.status);
    headers_ += "\r\nContent-Type: application/json\r\nContent-Length: ";
    headers_ += std::to_string(response.body.size());
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
