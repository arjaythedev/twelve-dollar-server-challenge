#include "../benchmarks/receive_backend.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <memory>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
using tds::bench::ReceiveBackend;
using tds::bench::ReceiveCompletion;
using tds::bench::ReceiveEvent;
using tds::bench::ReceiveMode;
using tds::bench::ReceiveWait;
using tds::net::Descriptor;
using tds::net::ReceiveBufferLease;
using tds::net::ReceiveBuffers;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class Loopback
{
  public:
    Loopback() : client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
    {
        Descriptor listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                    0,
                "bind loopback listener");
        require(::listen(listener.get(), 1) == 0, "listen on loopback");
        socklen_t length = sizeof(address);
        require(::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &length) == 0,
                "read loopback port");
        const timeval timeout{5, 0};
        const int enabled = 1;
        require(
            ::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
                ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
                    0 &&
                ::setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) ==
                    0,
            "configure loopback client");
        require(::connect(client.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                    0,
                "connect loopback client");
        server = std::make_unique<Descriptor>(
            ::accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    }

    Descriptor client;
    std::unique_ptr<Descriptor> server;
};

void transfer(int socket, std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto sent = ::send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        require(sent > 0, "send fixture bytes");
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

std::string read_bytes(int socket, std::size_t length)
{
    std::string bytes(length, '\0');
    std::size_t offset = 0;
    while (offset < length)
    {
        const auto count = ::recv(socket, bytes.data() + offset, length - offset, 0);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        require(count > 0, "read fixture response");
        offset += static_cast<std::size_t>(count);
    }

    return bytes;
}

void wait_ready(ReceiveBackend &backend, unsigned count = 1)
{
    backend.owner().submit();
    io_uring_cqe *entry = nullptr;
    __kernel_timespec timeout{5, 0};
    int result;
    do
    {
        result = io_uring_wait_cqes(&backend.owner().queue, &entry, count, &timeout, nullptr);
    } while (result == -EINTR);
    require(result == 0 && entry != nullptr, "receive fixture completion timed out");
}

template <class Consume> void turn(ReceiveBackend &backend, Consume &&consume)
{
    wait_ready(backend);
    backend.begin_turn();
    ReceiveCompletion completion;
    while (backend.next(completion))
    {
        consume(completion);
    }
    backend.finish_turn();
}

bool more(const ReceiveCompletion &completion)
{
    return (completion.flags & IORING_CQE_F_MORE) != 0;
}

bool selected(const ReceiveCompletion &completion)
{
    return (completion.flags & IORING_CQE_F_BUFFER) != 0;
}

unsigned buffer_id(const ReceiveCompletion &completion)
{
    require(selected(completion), "completion has no selected buffer id");

    return completion.flags >> IORING_CQE_BUFFER_SHIFT;
}

void append(std::string &bytes, const ReceiveCompletion &completion)
{
    const auto buffer = completion.buffer.bytes();
    require(completion.result > 0 && buffer.size() == static_cast<std::size_t>(completion.result),
            "selected lease does not match receive length");
    bytes.append(buffer.data(), buffer.size());
}

void fragmented_stream(ReceiveMode mode)
{
    Loopback sockets;
    ReceiveBackend backend(sockets.server->get(), mode);
    backend.receive();
    backend.owner().submit();
    std::string expected;
    std::string received;
    const std::array fragments{std::string("first fragment"), std::string(9001, 'x'),
                               std::string("\0last\xff", 6)};
    for (const auto &fragment : fragments)
    {
        // The next write is gated on consuming every byte from this write
        expected += fragment;
        transfer(sockets.client.get(), fragment);
        while (received.size() < expected.size())
        {
            if (!backend.receiving())
            {
                backend.receive();
            }
            turn(backend, [&](ReceiveCompletion &completion) {
                require(completion.event == ReceiveEvent::receive && completion.result > 0,
                        "fragment produced an unexpected completion");
                require(more(completion) == (mode == ReceiveMode::multishot),
                        "fragment receive lifetime flags");
                append(received, completion);
            });
        }
        require(received == expected, "fragment byte order changed");
        if (mode == ReceiveMode::multishot)
        {
            require(backend.receiving() && backend.pending() == 1 &&
                        backend.counts.submissions == 1 && backend.counts.terminals == 0,
                    "gated write replaced the live multishot SQE");
        }
    }
    require(::shutdown(sockets.client.get(), SHUT_WR) == 0, "half-close fragmented stream");
    if (!backend.receiving())
    {
        backend.receive();
    }
    bool eof = false;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.event == ReceiveEvent::receive && completion.result == 0 &&
                        !more(completion) && !eof,
                    "EOF did not terminate receive exactly once");
            eof = true;
        });
    }
    require(eof && !backend.receiving() && received == expected, "fragmented stream lost EOF");
    if (mode == ReceiveMode::multishot)
    {
        require(backend.counts.submissions == 1 && backend.counts.terminals == 1,
                "multishot stream needed more than one receive SQE");
    }
    else
    {
        require(backend.counts.submissions == backend.counts.data + 1 &&
                    backend.counts.terminals == backend.counts.submissions,
                "one-shot control did not rearm each completed receive");
    }
}

void independent_send_and_half_close()
{
    Loopback sockets;
    const std::string early = "response while receive is pending";
    const std::string final = "response after peer half-close";
    iovec vector{const_cast<char *>(early.data()), early.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    // Send storage outlives the backend, including exceptional teardown
    ReceiveBackend backend(sockets.server->get(), ReceiveMode::multishot);
    backend.receive();
    backend.owner().submit();
    backend.send(message);
    require(backend.receiving() && backend.sending() && backend.pending() == 2,
            "independent send did not overlap the pending receive");
    while (backend.sending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.event == ReceiveEvent::send &&
                        completion.result == static_cast<int>(early.size()) &&
                        backend.receiving() && backend.pending() == 1,
                    "send completion retired the pending receive");
        });
    }
    require(read_bytes(sockets.client.get(), early.size()) == early,
            "overlapping send response bytes");
    const std::string request = "request before FIN";
    transfer(sockets.client.get(), request);
    require(::shutdown(sockets.client.get(), SHUT_WR) == 0, "half-close request stream");
    std::string received;
    bool eof = false;
    while (backend.receiving())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.event == ReceiveEvent::receive, "unexpected half-close event");
            if (completion.result > 0)
            {
                require(more(completion), "request data unexpectedly terminated receive");
                append(received, completion);
            }
            else
            {
                require(completion.result == 0 && !more(completion) && !eof,
                        "half-close did not deliver terminal EOF");
                eof = true;
            }
        });
    }
    require(eof && received == request && backend.pending() == 0, "half-close request bytes");
    vector = {const_cast<char *>(final.data()), final.size()};
    backend.send(message);
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.event == ReceiveEvent::send &&
                        completion.result == static_cast<int>(final.size()),
                    "send after peer half-close failed");
        });
    }
    require(read_bytes(sockets.client.get(), final.size()) == final,
            "half-close prevented response delivery");
}

void cancellation_drains_both_completions()
{
    Loopback sockets;
    ReceiveBackend backend(sockets.server->get(), ReceiveMode::multishot);
    backend.receive();
    backend.owner().submit();
    const std::string payload = "selected before cancellation is consumed";
    transfer(sockets.client.get(), payload);
    // Leave a selected data CQE queued so cancellation must handle a late lease
    wait_ready(backend);
    backend.cancel_receive();
    require(backend.pending() == 2, "cancellation did not retain both operation owners");
    std::vector<ReceiveBufferLease> held;
    std::string received;
    bool acknowledged = false;
    bool terminated = false;
    unsigned selected_count = 0;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            if (completion.event == ReceiveEvent::cancel)
            {
                require(!acknowledged && completion.result == 0 && !more(completion),
                        "receive cancellation acknowledgement failed");
                acknowledged = true;
            }
            else
            {
                require(completion.event == ReceiveEvent::receive, "unexpected cancellation event");
                if (completion.result > 0)
                {
                    require(!terminated && more(completion), "late receive lifetime flags");
                    append(received, completion);
                }
                else
                {
                    require(!terminated && completion.result == -ECANCELED && !more(completion),
                            "original receive did not deliver cancellation terminal");
                    terminated = true;
                }
                if (selected(completion))
                {
                    ++selected_count;
                    held.push_back(std::move(completion.buffer));
                }
            }
            require(backend.pending() ==
                        static_cast<unsigned>(!acknowledged) + static_cast<unsigned>(!terminated),
                    "cancel ack and receive terminal share one pending owner");
        });
    }
    require(acknowledged && terminated && !backend.receiving() && !received.empty() &&
                payload.starts_with(received),
            "cancellation failed to drain late data and both terminals");
    auto &buffers = *backend.owner().receive_buffers;
    require(buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "held late buffers returned before lease release");
    held.clear();
    require(buffers.take_retry_credits(ReceiveBuffers::count) == selected_count &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "late selected buffers were not returned exactly once");
}

void cancellation_after_queued_eof()
{
    Loopback sockets;
    ReceiveBackend backend(sockets.server->get(), ReceiveMode::multishot);
    backend.receive();
    backend.owner().submit();
    require(::shutdown(sockets.client.get(), SHUT_WR) == 0, "half-close before cancellation");
    // Kernel EOF is queued while the userspace receive owner is still live
    wait_ready(backend);
    require(backend.receiving() && backend.pending() == 1,
            "queued EOF prematurely retired the receive owner");
    backend.cancel_receive();
    bool acknowledged = false;
    bool terminated = false;
    unsigned selected_count = 0;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            if (completion.event == ReceiveEvent::cancel)
            {
                require(!acknowledged && !more(completion) &&
                            (completion.result == -ENOENT || completion.result == 0 ||
                             completion.result == -EALREADY),
                        "queued EOF cancellation acknowledgement failed");
                acknowledged = true;
            }
            else
            {
                require(completion.event == ReceiveEvent::receive && !terminated &&
                            completion.result == 0 && !more(completion),
                        "queued EOF was lost or receive retired twice");
                terminated = true;
                selected_count += selected(completion);
            }
            require(backend.pending() ==
                        static_cast<unsigned>(!acknowledged) + static_cast<unsigned>(!terminated),
                    "queued EOF and cancel acknowledgement retired the same owner");
        });
    }
    require(acknowledged && terminated && !backend.receiving() && backend.counts.submissions == 1 &&
                backend.counts.terminals == 1,
            "queued EOF cancellation did not drain both independent completions");
    auto &buffers = *backend.owner().receive_buffers;
    require(buffers.take_retry_credits(ReceiveBuffers::count) == selected_count &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "queued EOF selected buffer was not returned exactly once");
}

void exhaustion_and_bounded_rearm()
{
    Loopback sockets;
    std::string payload(static_cast<std::size_t>(ReceiveBuffers::count + 2) * ReceiveBuffers::size,
                        '\0');
    for (std::size_t index = 0; index < payload.size(); ++index)
    {
        payload[index] = static_cast<char>(index % 251);
    }
    ReceiveBackend backend(sockets.server->get(), ReceiveMode::multishot);
    backend.receive();
    backend.owner().submit();
    std::exception_ptr writer_error;
    std::jthread writer([&](std::stop_token stop) {
        try
        {
            std::string_view remaining(payload);
            while (!remaining.empty() && !stop.stop_requested())
            {
                const auto sent = ::send(sockets.client.get(), remaining.data(), remaining.size(),
                                         MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent < 0 && errno == EINTR)
                {
                    continue;
                }
                if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    pollfd ready{sockets.client.get(), POLLOUT, 0};
                    const int result = ::poll(&ready, 1, 50);
                    require(result >= 0 || errno == EINTR, "wait for exhaustion writer");
                    continue;
                }
                require(sent > 0, "send exhaustion bytes");
                remaining.remove_prefix(static_cast<std::size_t>(sent));
            }
        }
        catch (...)
        {
            writer_error = std::current_exception();
        }
    });
    std::array<bool, ReceiveBuffers::count> seen{};
    std::vector<ReceiveBufferLease> held;
    held.reserve(ReceiveBuffers::count + 1);
    std::size_t consumed = 0;
    unsigned first_id = ReceiveBuffers::count;
    unsigned exhausted = 0;
    auto hold = [&](ReceiveCompletion &completion, bool rearmed) {
        require(completion.event == ReceiveEvent::receive, "unexpected exhaustion event");
        if (completion.result > 0)
        {
            require(more(completion), "exhaustion data lost the multishot lifetime");
            const auto id = buffer_id(completion);
            require(id < seen.size(), "exhaustion selected invalid id");
            if (rearmed)
            {
                require(id == first_id && held.size() == ReceiveBuffers::count,
                        "rearm did not consume the sole returned buffer exactly once");
            }
            else
            {
                require(!seen[id], "kernel selected a buffer whose lease is still held");
                seen[id] = true;
                if (held.empty())
                {
                    first_id = id;
                }
            }
            const auto bytes = completion.buffer.bytes();
            require(bytes.size() == static_cast<std::size_t>(completion.result) &&
                        bytes.size() <= payload.size() - consumed &&
                        std::equal(bytes.begin(), bytes.end(), payload.data() + consumed),
                    "exhaustion or rearm changed stream byte order");
            consumed += bytes.size();
            held.push_back(std::move(completion.buffer));
        }
        else
        {
            require(completion.result == -ENOBUFS && !more(completion) && !selected(completion),
                    "buffer exhaustion did not terminate with ENOBUFS");
            ++exhausted;
        }
    };
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            hold(completion, false);
        });
    }
    auto &buffers = *backend.owner().receive_buffers;
    require(held.size() == ReceiveBuffers::count &&
                std::all_of(seen.begin(), seen.end(),
                            [](bool value) {
                                return value;
                            }) &&
                exhausted == 1 && backend.counts.submissions == 1 &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "exhaustion did not retain every unique buffer with one receive SQE");
    held.front().release();
    require(buffers.take_retry_credits(ReceiveBuffers::count) == 1 &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "returning one buffer did not produce exactly one retry credit");
    // One observed return permits one retry, no speculative ENOBUFS spin loop
    backend.receive();
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            hold(completion, true);
        });
    }
    require(held.size() == ReceiveBuffers::count + 1 && exhausted == 2 &&
                backend.counts.submissions == 2 && backend.counts.enobufs == 2 &&
                backend.counts.terminals == 2 && !backend.receiving(),
            "bounded rearm did not consume one buffer and terminate again");
    held.clear();
    // With no live receives, credits count releases rather than imply kernel availability
    require(buffers.take_retry_credits(ReceiveBuffers::count) == ReceiveBuffers::count &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "exhaustion leases did not return every buffer exactly once");
    writer.request_stop();
    writer.join();
    if (writer_error)
    {
        std::rethrow_exception(writer_error);
    }
}

void isolated_socket_streams(ReceiveMode mode, ReceiveWait wait = ReceiveWait::production)
{
    std::array<Loopback, 3> sockets;
    const std::array descriptors{sockets[0].server->get(), sockets[1].server->get(),
                                 sockets[2].server->get()};
    ReceiveBackend backend(descriptors, mode, wait);
    std::array<std::string, 3> expected;
    std::array<std::string, 3> received;
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        backend.receive(slot);
    }
    backend.owner().submit();
    for (unsigned round = 0; round < 3; ++round)
    {
        for (unsigned slot = 0; slot < sockets.size(); ++slot)
        {
            const std::string fragment =
                std::to_string(slot) + ':' + std::to_string(round) +
                std::string(5003 + 997 * slot, static_cast<char>('a' + slot));
            expected[slot] += fragment;
            transfer(sockets[slot].client.get(), fragment);
        }
        while (received != expected)
        {
            for (unsigned slot = 0; slot < sockets.size(); ++slot)
            {
                if (!backend.receiving(slot) && received[slot].size() < expected[slot].size())
                {
                    backend.receive(slot);
                }
            }
            turn(backend, [&](ReceiveCompletion &completion) {
                require(completion.slot < sockets.size() &&
                            completion.event == ReceiveEvent::receive && completion.result > 0 &&
                            more(completion) == (mode == ReceiveMode::multishot),
                        "multi-socket stream completion identity or lifetime");
                const unsigned slot = completion.slot;
                append(received[slot], completion);
                require(expected[slot].starts_with(received[slot]),
                        "multi-socket data crossed a slot or changed byte order");
            });
        }
        if (mode == ReceiveMode::multishot)
        {
            require(backend.pending() == sockets.size() &&
                        backend.counts.submissions == sockets.size(),
                    "gated multi-socket writes replaced live multishot receives");
        }
    }
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        if (!backend.receiving(slot))
        {
            backend.receive(slot);
        }
    }
    backend.owner().submit();
    std::array<bool, 3> eof{};
    unsigned retired = 0;
    for (const unsigned closing : {2U, 0U, 1U})
    {
        require(::shutdown(sockets[closing].client.get(), SHUT_WR) == 0,
                "half-close isolated socket");
        while (!eof[closing])
        {
            turn(backend, [&](ReceiveCompletion &completion) {
                require(completion.event == ReceiveEvent::receive && completion.slot == closing &&
                            completion.result == 0 && !more(completion) && !eof[closing],
                        "EOF retired a different socket or retired one socket twice");
                eof[closing] = true;
                ++retired;
            });
        }
        for (unsigned slot = 0; slot < sockets.size(); ++slot)
        {
            require(backend.receiving(slot) == !eof[slot],
                    "one socket EOF changed a neighboring receive owner");
        }
        require(backend.pending() == sockets.size() - retired, "multi-socket EOF pending count");
    }
    require(received == expected && backend.pending() == 0,
            "multi-socket streams did not drain independently");
    require(mode == ReceiveMode::multishot
                ? backend.counts.submissions == sockets.size() &&
                      backend.counts.terminals == sockets.size()
                : backend.counts.submissions == backend.counts.data + sockets.size() &&
                      backend.counts.terminals == backend.counts.submissions,
            "multi-socket receive submission accounting");
}

void isolated_socket_send_cancel_and_eof(ReceiveMode mode,
                                         ReceiveWait wait = ReceiveWait::production)
{
    std::array<Loopback, 3> sockets;
    const std::array descriptors{sockets[0].server->get(), sockets[1].server->get(),
                                 sockets[2].server->get()};
    const std::array responses{std::string("slot zero response"), std::string("slot one reply"),
                               std::string("slot two independent response")};
    std::array<iovec, 3> vectors{};
    std::array<msghdr, 3> messages{};
    ReceiveBackend backend(descriptors, mode, wait);
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        vectors[slot] = {const_cast<char *>(responses[slot].data()), responses[slot].size()};
        messages[slot].msg_iov = &vectors[slot];
        messages[slot].msg_iovlen = 1;
        backend.receive(slot);
        backend.send(messages[slot], slot);
    }
    require(backend.pending() == 2 * sockets.size(), "multi-socket send/receive did not overlap");
    std::array<bool, 3> sent{};
    unsigned sends = 0;
    while (sends != sockets.size())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.slot < sockets.size() && completion.event == ReceiveEvent::send,
                    "multi-socket send completion crossed event tags");
            const unsigned slot = completion.slot;
            require(!sent[slot] && completion.result == static_cast<int>(responses[slot].size()) &&
                        !backend.sending(slot),
                    "multi-socket send completion crossed socket tags");
            sent[slot] = true;
            ++sends;
            for (unsigned live = 0; live < sockets.size(); ++live)
            {
                require(backend.receiving(live), "send retired a neighboring receive owner");
            }
        });
    }
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        require(read_bytes(sockets[slot].client.get(), responses[slot].size()) == responses[slot],
                "response delivered on the wrong socket");
    }
    backend.cancel_receive(1);
    require(::shutdown(sockets[0].client.get(), SHUT_WR) == 0, "half-close neighboring socket");
    const std::string survivor = "slot two survives cancellation and EOF";
    transfer(sockets[2].client.get(), survivor);
    bool acknowledged = false;
    bool cancelled = false;
    bool eof = false;
    std::string received;
    while (!acknowledged || !cancelled || !eof || received.size() < survivor.size())
    {
        if (!backend.receiving(2) && received.size() < survivor.size())
        {
            backend.receive(2);
        }
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.slot < sockets.size(), "mixed socket completion has invalid slot");
            if (completion.event == ReceiveEvent::cancel)
            {
                require(completion.slot == 1 && !acknowledged && completion.result == 0,
                        "cancel acknowledgement crossed socket tags");
                acknowledged = true;
            }
            else
            {
                require(completion.event == ReceiveEvent::receive,
                        "unexpected mixed socket completion event");
                switch (completion.slot)
                {
                case 0:
                    require(!eof && completion.result == 0 && !more(completion),
                            "neighboring EOF lost its socket identity");
                    eof = true;
                    break;
                case 1:
                    require(!cancelled && completion.result == -ECANCELED && !more(completion),
                            "cancellation retired a different receive");
                    cancelled = true;
                    break;
                case 2:
                    require(completion.result > 0 &&
                                more(completion) == (mode == ReceiveMode::multishot),
                            "surviving socket lost its receive lifetime");
                    append(received, completion);
                    require(survivor.starts_with(received),
                            "neighboring teardown changed stream bytes");
                    break;
                }
            }
            unsigned live = static_cast<unsigned>(!acknowledged);
            for (unsigned slot = 0; slot < sockets.size(); ++slot)
            {
                live += backend.receiving(slot);
            }
            require(backend.pending() == live, "mixed socket owners retired more than once");
        });
    }
    require(!backend.receiving(0) && !backend.receiving(1) && received == survivor,
            "neighboring teardown changed the wrong socket state");
    if (!backend.receiving(2))
    {
        backend.receive(2);
    }
    const std::string tail = " and remains usable";
    transfer(sockets[2].client.get(), tail);
    while (received.size() < survivor.size() + tail.size())
    {
        if (!backend.receiving(2))
        {
            backend.receive(2);
        }
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.slot == 2 && completion.event == ReceiveEvent::receive &&
                        completion.result > 0,
                    "retired neighbor consumed surviving socket data");
            append(received, completion);
        });
    }
    require(received == survivor + tail, "surviving socket byte order after teardown");
    if (!backend.receiving(2))
    {
        backend.receive(2);
    }
    require(::shutdown(sockets[2].client.get(), SHUT_WR) == 0, "half-close surviving socket");
    bool final_eof = false;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.slot == 2 && completion.event == ReceiveEvent::receive &&
                        completion.result == 0 && !more(completion) && !final_eof,
                    "surviving socket EOF retired the wrong owner");
            final_eof = true;
        });
    }
    require(final_eof && backend.counts.sends == sockets.size(),
            "multi-socket send/cancel/EOF contract did not drain");
}

void deferred_wait_without_completions(ReceiveMode mode, ReceiveWait wait = ReceiveWait::production)
{
    std::array<Loopback, 2> sockets;
    const std::array descriptors{sockets[0].server->get(), sockets[1].server->get()};
    ReceiveBackend backend(descriptors, mode, wait);
    require((backend.owner().queue.features & IORING_FEAT_EXT_ARG) != 0,
            "deferred wait fixture requires extended wait arguments");
    backend.receive(0);
    backend.receive(1);
    require(backend.owner().receive_buffers->take_retry_credits(ReceiveBuffers::count) == 0,
            "idle deferred wait unexpectedly has retry credits");
    const auto waits = backend.counts.waits;
    // No readiness helper here, the deferred wait itself must return
    backend.begin_turn(false, true);
    ReceiveCompletion completion;
    require(!backend.next(completion), "idle deferred wait invented a completion");
    backend.finish_turn();
    require(backend.counts.waits == waits + 1 && backend.counts.submissions == 2 &&
                backend.counts.terminals == 0 && backend.pending() == 2 && backend.receiving(0) &&
                backend.receiving(1),
            "deferred timeout lost pending receive ownership");
    transfer(sockets[0].client.get(), "x");
    bool received = false;
    while (!received)
    {
        turn(backend, [&](ReceiveCompletion &entry) {
            const auto bytes = entry.buffer.bytes();
            require(entry.slot == 0 && entry.event == ReceiveEvent::receive && entry.result == 1 &&
                        bytes.size() == 1 && bytes.front() == 'x' && !received &&
                        more(entry) == (mode == ReceiveMode::multishot),
                    "data did not progress after an empty deferred wait");
            received = true;
        });
    }
    const std::array cancelling{backend.receiving(0), backend.receiving(1)};
    std::array<bool, 2> acknowledged{};
    std::array<bool, 2> terminated{};
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        if (cancelling[slot])
        {
            backend.cancel_receive(slot);
        }
    }
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &entry) {
            require(entry.slot < sockets.size() && cancelling[entry.slot],
                    "deferred wait cancellation crossed socket owners");
            const unsigned slot = entry.slot;
            if (entry.event == ReceiveEvent::cancel)
            {
                require(!acknowledged[slot] && entry.result == 0 && !more(entry),
                        "deferred wait cancellation acknowledgement failed");
                acknowledged[slot] = true;
            }
            else
            {
                require(entry.event == ReceiveEvent::receive && !terminated[slot] &&
                            entry.result == -ECANCELED && !more(entry),
                        "deferred wait receive cancellation did not terminate");
                terminated[slot] = true;
            }
        });
    }
    require(acknowledged == cancelling && terminated == cancelling && !backend.receiving(0) &&
                !backend.receiving(1),
            "deferred wait prevented cancellation progress or duplicate-free drain");
}

void ready_completion_after_warm_batch(ReceiveMode mode, ReceiveWait wait)
{
    std::array<Loopback, 2> sockets;
    const std::array descriptors{sockets[0].server->get(), sockets[1].server->get()};
    ReceiveBackend backend(descriptors, mode, wait);
    backend.receive(0);
    backend.receive(1);
    transfer(sockets[0].client.get(), "a");
    transfer(sockets[1].client.get(), "b");
    wait_ready(backend, 2);
    require(io_uring_cq_ready(&backend.owner().queue) == 2,
            "warm fixture did not queue exactly two receive completions");
    const auto warm_drains = backend.counts.ready_drains;
    backend.begin_turn();
    std::array<std::string, 2> received;
    unsigned consumed = 0;
    ReceiveCompletion completion;
    while (backend.next(completion))
    {
        require(completion.slot < sockets.size() && completion.event == ReceiveEvent::receive &&
                    completion.result == 1 && more(completion) == (mode == ReceiveMode::multishot),
                "warm batch lost socket identity or receive lifetime");
        append(received[completion.slot], completion);
        ++consumed;
    }
    backend.finish_turn();
    require(consumed == 2 && received[0] == "a" && received[1] == "b" &&
                backend.counts.ready_drains == warm_drains + 1 &&
                backend.pending() == (mode == ReceiveMode::multishot ? 2U : 0U),
            "warm batch did not establish two completed receives");
    if (!backend.receiving(0))
    {
        backend.receive(0);
    }
    transfer(sockets[0].client.get(), "c");
    wait_ready(backend);
    require(io_uring_cq_ready(&backend.owner().queue) == 1,
            "single ready fixture queued an unexpected completion count");
    if (!backend.receiving(1))
    {
        // The ready path must also flush a neighboring receive SQE
        backend.receive(1);
    }
    const auto ready_drains = backend.counts.ready_drains;
    // The producer has stopped, so no later completion can satisfy a batch target
    backend.begin_turn();
    require(backend.counts.ready_drains == ready_drains + 1 &&
                io_uring_sq_ready(&backend.owner().queue) == 0,
            "already-ready completion did not select the ready drain or flush SQEs");
    require(backend.next(completion) && completion.slot == 0 &&
                completion.event == ReceiveEvent::receive && completion.result == 1 &&
                more(completion) == (mode == ReceiveMode::multishot),
            "single ready completion stalled after a warm batch");
    append(received[0], completion);
    require(!backend.next(completion), "single ready completion was consumed more than once");
    backend.finish_turn();
    require(received[0] == "ac" && received[1] == "b" && backend.receiving(1) &&
                backend.receiving(0) == (mode == ReceiveMode::multishot) &&
                backend.pending() == (mode == ReceiveMode::multishot ? 2U : 1U),
            "single ready drain changed stream bytes or neighboring owners");
    std::array<bool, 2> eof{};
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        if (!backend.receiving(slot))
        {
            backend.receive(slot);
        }
        require(::shutdown(sockets[slot].client.get(), SHUT_WR) == 0,
                "half-close ready-drain fixture");
    }
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &entry) {
            require(entry.slot < sockets.size() && !eof[entry.slot] &&
                        entry.event == ReceiveEvent::receive && entry.result == 0 && !more(entry),
                    "ready-drain EOF lost or duplicated a receive owner");
            eof[entry.slot] = true;
        });
    }
    require(eof[0] && eof[1], "ready-drain fixture lost EOF");
}

void ready_drain_is_bounded(ReceiveWait wait)
{
    Loopback sockets;
    const std::array descriptors{sockets.server->get()};
    ReceiveBackend backend(descriptors, ReceiveMode::multishot, wait);
    backend.receive();
    constexpr unsigned completions = 257;
    for (unsigned count = 1; count <= completions; ++count)
    {
        // Gate each byte on a new CQE so TCP coalescing cannot reduce this batch
        transfer(sockets.client.get(), "x");
        wait_ready(backend, count);
        require(io_uring_cq_ready(&backend.owner().queue) == count,
                "bounded ready fixture did not queue each gated receive");
    }
    std::string received;
    for (const unsigned expected : {256U, 1U})
    {
        const auto ready_drains = backend.counts.ready_drains;
        backend.begin_turn();
        unsigned consumed = 0;
        ReceiveCompletion completion;
        while (backend.next(completion))
        {
            require(completion.slot == 0 && completion.event == ReceiveEvent::receive &&
                        completion.result == 1 && more(completion),
                    "bounded ready drain changed the multishot receive lifetime");
            append(received, completion);
            ++consumed;
        }
        backend.finish_turn();
        require(consumed == expected && backend.counts.ready_drains == ready_drains + 1 &&
                    backend.pending() == 1 && backend.receiving(),
                "ready drain exceeded its 256-completion bound or lost its live owner");
    }
    require(received == std::string(completions, 'x') &&
                io_uring_cq_ready(&backend.owner().queue) == 0,
            "bounded ready drains lost or duplicated bytes");
    require(::shutdown(sockets.client.get(), SHUT_WR) == 0, "half-close bounded ready fixture");
    bool eof = false;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.event == ReceiveEvent::receive && completion.result == 0 &&
                        !more(completion) && !eof,
                    "bounded ready fixture did not terminate exactly once");
            eof = true;
        });
    }
    require(eof, "bounded ready fixture lost EOF");
}

void empty_cq_runs_deferred_work(ReceiveMode mode, ReceiveWait wait, bool deferred)
{
    Loopback sockets;
    const std::array descriptors{sockets.server->get()};
    ReceiveBackend backend(descriptors, mode, wait);
    constexpr unsigned flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    require((backend.owner().queue.flags & flags) == flags,
            "empty CQ fixture requires single-issuer deferred task work");
    backend.receive();
    const std::string expected = "queued work must enter the kernel";
    transfer(sockets.client.get(), expected);
    require(io_uring_cq_ready(&backend.owner().queue) == 0 &&
                io_uring_sq_ready(&backend.owner().queue) == 1,
            "empty CQ fixture submitted its queued receive prematurely");
    const auto waits = backend.counts.waits;
    const auto ready_drains = backend.counts.ready_drains;
    // No readiness helper may submit or run deferred work before this wait
    backend.begin_turn(false, deferred);
    std::string received;
    ReceiveCompletion completion;
    while (backend.next(completion))
    {
        require(completion.slot == 0 && completion.event == ReceiveEvent::receive &&
                    completion.result > 0 && more(completion) == (mode == ReceiveMode::multishot),
                "empty CQ deferred wait changed receive ownership");
        append(received, completion);
    }
    backend.finish_turn();
    require(received == expected && backend.counts.waits == waits + 1 &&
                backend.counts.ready_drains == ready_drains &&
                backend.pending() == static_cast<unsigned>(mode == ReceiveMode::multishot) &&
                backend.receiving() == (mode == ReceiveMode::multishot),
            "empty CQ did not submit and run deferred receive work");
    if (!backend.receiving())
    {
        backend.receive();
    }
    require(::shutdown(sockets.client.get(), SHUT_WR) == 0, "half-close empty CQ fixture");
    bool eof = false;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &entry) {
            require(entry.event == ReceiveEvent::receive && entry.result == 0 && !more(entry) &&
                        !eof,
                    "empty CQ fixture did not terminate exactly once");
            eof = true;
        });
    }
    require(eof, "empty CQ fixture lost EOF");
}

void shared_pool_exhaustion(ReceiveMode mode)
{
    std::array<Loopback, 2> sockets;
    const std::array descriptors{sockets[0].server->get(), sockets[1].server->get()};
    ReceiveBackend backend(descriptors, mode, ReceiveWait::production);
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        backend.receive(slot);
    }
    backend.owner().submit();
    std::array<bool, ReceiveBuffers::count> seen{};
    std::array<bool, 2> exhausted{};
    std::array<unsigned, 2> received{};
    std::vector<ReceiveBufferLease> held;
    held.reserve(ReceiveBuffers::count + 1);
    unsigned returned_id = ReceiveBuffers::count;
    auto consume = [&](ReceiveCompletion &completion) {
        require(completion.slot < sockets.size() && completion.event == ReceiveEvent::receive,
                "shared-pool completion crossed socket tags");
        const unsigned slot = completion.slot;
        if (completion.result > 0)
        {
            const auto id = buffer_id(completion);
            const auto bytes = completion.buffer.bytes();
            require(completion.result == 1 && bytes.size() == 1 &&
                        bytes.front() == static_cast<char>('a' + slot) && !seen[id] &&
                        more(completion) == (mode == ReceiveMode::multishot),
                    "shared-pool buffer is duplicated or belongs to another stream");
            seen[id] = true;
            if (held.empty())
            {
                require(slot == 0, "first shared-pool lease did not belong to slot zero");
                returned_id = id;
            }
            held.push_back(std::move(completion.buffer));
            ++received[slot];
        }
        else
        {
            require(held.size() == ReceiveBuffers::count && !exhausted[slot] &&
                        completion.result == -ENOBUFS && !more(completion) && !selected(completion),
                    "shared pool exhausted before all unique leases were held");
            exhausted[slot] = true;
        }
    };
    for (unsigned index = 0; index < ReceiveBuffers::count; ++index)
    {
        const unsigned slot = index % static_cast<unsigned>(sockets.size());
        if (!backend.receiving(slot))
        {
            backend.receive(slot);
        }
        const char byte = static_cast<char>('a' + slot);
        transfer(sockets[slot].client.get(), std::string_view(&byte, 1));
        const unsigned target = received[slot] + 1;
        while (received[slot] != target)
        {
            turn(backend, consume);
        }
    }
    require(std::all_of(seen.begin(), seen.end(),
                        [](bool value) {
                            return value;
                        }) &&
                received[0] == ReceiveBuffers::count / 2 &&
                received[1] == ReceiveBuffers::count / 2,
            "shared pool was not consumed by both sockets");
    for (unsigned slot = 0; slot < sockets.size(); ++slot)
    {
        if (!exhausted[slot] && !backend.receiving(slot))
        {
            backend.receive(slot);
        }
        const char byte = static_cast<char>('a' + slot);
        transfer(sockets[slot].client.get(), std::string_view(&byte, 1));
    }
    while (backend.pending())
    {
        turn(backend, consume);
    }
    auto &buffers = *backend.owner().receive_buffers;
    require(exhausted[0] && exhausted[1] && buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "shared pool did not independently terminate both exhausted receives");
    held.front().release();
    require(buffers.take_retry_credits(ReceiveBuffers::count) == 1 &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "shared-pool return did not grant exactly one bounded retry");
    // A buffer formerly held by slot zero must be usable by slot one
    const std::string retry_tail(ReceiveBuffers::size, 'r');
    transfer(sockets[1].client.get(), retry_tail);
    const std::string retry_expected = 'b' + retry_tail;
    const auto submissions = backend.counts.submissions;
    backend.receive(1);
    bool reused = false;
    bool retry_exhausted = false;
    while (backend.pending())
    {
        turn(backend, [&](ReceiveCompletion &completion) {
            require(completion.slot == 1 && completion.event == ReceiveEvent::receive,
                    "shared-pool retry woke the wrong socket");
            if (completion.result > 0)
            {
                require(!reused && buffer_id(completion) == returned_id &&
                            more(completion) == (mode == ReceiveMode::multishot),
                        "shared-pool retry did not use the sole returned lease");
                const auto bytes = completion.buffer.bytes();
                require(bytes.size() == static_cast<std::size_t>(completion.result) &&
                            bytes.size() <= retry_expected.size() &&
                            std::equal(bytes.begin(), bytes.end(), retry_expected.data()),
                        "shared-pool retry crossed stream bytes");
                held.push_back(std::move(completion.buffer));
                reused = true;
            }
            else
            {
                require(mode == ReceiveMode::multishot && reused && !retry_exhausted &&
                            completion.result == -ENOBUFS && !more(completion) &&
                            !selected(completion),
                        "shared-pool multishot retry did not terminate on renewed exhaustion");
                retry_exhausted = true;
            }
        });
    }
    require(reused && retry_exhausted == (mode == ReceiveMode::multishot) &&
                backend.counts.submissions == submissions + 1 &&
                held.size() == ReceiveBuffers::count + 1 &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "one shared-pool return caused an unbounded retry or extra lease return");
    held.clear();
    require(buffers.take_retry_credits(ReceiveBuffers::count) == ReceiveBuffers::count &&
                buffers.take_retry_credits(ReceiveBuffers::count) == 0,
            "shared-pool leases did not return exactly once while both sockets were quiet");
}

} // namespace

int main()
{
    ::alarm(30);
    try
    {
        fragmented_stream(ReceiveMode::one_shot);
        fragmented_stream(ReceiveMode::multishot);
        independent_send_and_half_close();
        cancellation_drains_both_completions();
        cancellation_after_queued_eof();
        exhaustion_and_bounded_rearm();
        for (const auto mode : {ReceiveMode::one_shot, ReceiveMode::multishot})
        {
            isolated_socket_streams(mode);
            isolated_socket_send_cancel_and_eof(mode);
            deferred_wait_without_completions(mode);
            shared_pool_exhaustion(mode);
        }
        for (const auto wait : {ReceiveWait::ready_first, ReceiveWait::ready_one})
        {
            for (const auto mode : {ReceiveMode::one_shot, ReceiveMode::multishot})
            {
                ready_completion_after_warm_batch(mode, wait);
                empty_cq_runs_deferred_work(mode, wait, false);
                empty_cq_runs_deferred_work(mode, wait, true);
                deferred_wait_without_completions(mode, wait);
                isolated_socket_streams(mode, wait);
                isolated_socket_send_cancel_and_eof(mode, wait);
            }
            ready_drain_is_bounded(wait);
        }
        std::puts("receive backend contracts passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "receive backend contract failed: %s\n", error.what());

        return 1;
    }
}
