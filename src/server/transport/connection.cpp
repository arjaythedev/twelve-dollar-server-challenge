#include "connection.hpp"

#include <string_view>
#include <utility>

namespace tds::transport
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

void Exchange::prepare_response(Response response, bool close_after)
{
    output.clear();
    output += "HTTP/1.1 ";
    output += std::to_string(static_cast<unsigned>(response.status));
    output += ' ';
    output += reason(response.status);
    output += "\r\nContent-Type: application/json\r\nContent-Length: ";
    output += std::to_string(response.body.size());
    output += "\r\n";
    if (close_after)
    {
        output += "Connection: close\r\n";
    }
    output += "\r\n";
    response_body = std::move(response.body);
    sent = 0;
}

msghdr &Exchange::prepare_send()
{
    std::size_t count = 0;
    std::size_t body_offset = 0;
    if (sent < output.size())
    {
        send_vectors[count++] = {output.data() + sent, output.size() - sent};
    }
    else
    {
        body_offset = sent - output.size();
    }
    if (body_offset < response_body.size())
    {
        send_vectors[count++] = {response_body.data() + body_offset,
                                 response_body.size() - body_offset};
    }
    send_message.msg_iov = send_vectors.data();
    send_message.msg_iovlen = count;
    return send_message;
}

void Exchange::finish_response()
{
    output.clear();
    std::string{}.swap(response_body);
    parser.reset();
}

} // namespace tds::transport
