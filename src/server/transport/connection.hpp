#pragma once

#include "../http.hpp"
#include "http_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>

namespace tds::transport
{

struct Exchange
{
    Exchange() = default;
    Exchange(const Exchange &) = delete;
    Exchange &operator=(const Exchange &) = delete;

    void prepare_response(Response response, bool close_after);
    msghdr &prepare_send();
    void finish_response();

    std::size_t retained_capacity() const noexcept
    {
        return input.capacity() + output.capacity() + response_body.capacity() +
               parser.scratch_capacity();
    }

    HttpParser parser;
    std::string input;
    std::string output;
    std::string response_body;
    std::array<iovec, 2> send_vectors{};
    msghdr send_message{};
    std::size_t sent = 0;
#ifdef TDS_DIAGNOSTICS
    std::uint64_t send_prepared_ns = 0;
    std::uint64_t receive_serviced_ns = 0;
#endif
};

} // namespace tds::transport
