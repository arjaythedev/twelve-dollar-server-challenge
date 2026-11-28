#include "http/reply.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string wire(tds::http::Reply &reply)
{
    const auto &message = reply.message();
    std::string result;
    for (std::size_t index = 0; index < message.msg_iovlen; ++index)
    {
        result.append(static_cast<const char *>(message.msg_iov[index].iov_base),
                      message.msg_iov[index].iov_len);
    }
    return result;
}

struct StatusCase
{
    tds::Status status;
    std::string_view line;
};

constexpr std::array statuses{
    StatusCase{tds::Status::ok, "200 OK"},
    StatusCase{tds::Status::created, "201 Created"},
    StatusCase{tds::Status::bad_request, "400 Bad Request"},
    StatusCase{tds::Status::unauthorized, "401 Unauthorized"},
    StatusCase{tds::Status::not_found, "404 Not Found"},
    StatusCase{tds::Status::too_large, "413 Payload Too Large"},
    StatusCase{tds::Status::internal_error, "500 Internal Server Error"},
    StatusCase{tds::Status::unavailable, "503 Service Unavailable"},
    StatusCase{static_cast<tds::Status>(0), "0 Internal Server Error"},
    StatusCase{static_cast<tds::Status>(9), "9 Internal Server Error"},
    StatusCase{static_cast<tds::Status>(10), "10 Internal Server Error"},
    StatusCase{static_cast<tds::Status>(99), "99 Internal Server Error"},
    StatusCase{static_cast<tds::Status>(999), "999 Internal Server Error"},
    StatusCase{static_cast<tds::Status>(1000), "1000 Internal Server Error"},
    StatusCase{static_cast<tds::Status>(65535), "65535 Internal Server Error"},
};

std::string expected(std::string_view status, std::string_view body, bool close_after)
{
    std::string result = "HTTP/1.1 ";
    result += status;
    result += "\r\nContent-Type: application/json\r\nContent-Length: ";
    result += std::to_string(body.size());
    result += "\r\n";
    if (close_after)
    {
        result += "Connection: close\r\n";
    }
    result += "\r\n";
    result += body;
    return result;
}

std::string body(std::size_t size)
{
    std::string result(size, 'x');
    if (size >= 4)
    {
        result[1] = '\0';
        result[2] = static_cast<char>(0xc3);
        result[3] = static_cast<char>(0xa9);
    }
    return result;
}

void byte_contract()
{
    constexpr std::array<std::size_t, 15> sizes{0,    1,    9,    10,   99,   100,   999,  1000,
                                                4096, 8191, 8192, 8193, 9999, 10000, 65536};
    tds::http::Reply reply;
    for (const auto &status : statuses)
    {
        for (const bool close_after : {false, true})
        {
            for (const auto size : sizes)
            {
                auto content = body(size);
                const auto reference = expected(status.line, content, close_after);
                reply.prepare({status.status, std::move(content)}, close_after);
                require(wire(reply) == reference, "reply wire bytes changed");
                require(reply.advance(reference.size()), "complete reply did not finish");
                reply.reset();
            }
        }
    }
}

void partial_sends()
{
    for (const auto size : {std::size_t{0}, std::size_t{513}, std::size_t{16384}})
    {
        for (const bool close_after : {false, true})
        {
            tds::http::Reply reply;
            auto content = body(size);
            const auto reference = expected("65535 Internal Server Error", content, close_after);
            reply.prepare({static_cast<tds::Status>(65535), std::move(content)}, close_after);
            const auto header_size = reply.message().msg_iov[0].iov_len;
            const std::array chunks{std::size_t{1}, header_size - 2, std::size_t{2}, std::size_t{7},
                                    std::size_t{127}};
            std::size_t sent = 0;
            std::size_t index = 0;
            while (sent < reference.size())
            {
                require(wire(reply) == std::string_view(reference).substr(sent),
                        "partial send lost header or body bytes");
                const auto count =
                    std::min(chunks[std::min(index++, chunks.size() - 1)], reference.size() - sent);
                sent += count;
                require(reply.advance(count) == (sent == reference.size()),
                        "partial send completion boundary changed");
            }
            reply.reset();
        }
    }
}

void recycled_storage()
{
    tds::http::Reply reply;
    std::string content(1024, 'a');
    const char *storage = content.data();
    reply.prepare({tds::Status::ok, std::move(content)}, false);
    auto &message = reply.message();
    require(message.msg_iovlen == 2 && message.msg_iov[1].iov_base == storage,
            "reply did not retain moved body storage");
    require(reply.advance(message.msg_iov[0].iov_len + message.msg_iov[1].iov_len),
            "recycled response did not complete");
    reply.reset();
    content = reply.take_body();
    content.assign(1000, 'b');
    require(content.data() == storage, "recycling discarded an ordinary response allocation");
    reply.prepare({tds::Status::created, std::move(content)}, true);
    require(reply.message().msg_iov[1].iov_base == storage &&
                wire(reply) == expected("201 Created", std::string(1000, 'b'), true),
            "recycled reply changed ownership or output");
    require(reply.advance(wire(reply).size()), "recycled second response did not complete");
    reply.reset();
    reply.prepare({tds::Status::ok, std::string(16384, 'c')}, false);
    require(reply.advance(wire(reply).size()), "large response did not complete");
    reply.reset();
    require(reply.take_body().capacity() <= 8192, "large response storage was retained");
}

} // namespace

int main()
{
    try
    {
        byte_contract();
        partial_sends();
        recycled_storage();
        std::puts("Reply wire bytes, partial sends and recycled storage passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "Reply contract failed: %s\n", error.what());
        return 1;
    }
}
