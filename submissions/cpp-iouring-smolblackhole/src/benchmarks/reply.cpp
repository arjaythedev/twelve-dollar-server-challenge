#include "http/reply.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>

namespace
{

void require(bool valid, const char *message)
{
    if (!valid)
    {
        throw std::runtime_error(message);
    }
}

std::uint64_t now(clockid_t clock)
{
    timespec value{};
    require(::clock_gettime(clock, &value) == 0, "reply probe clock");
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

struct Case
{
    tds::Status status;
    std::string body;
    std::string expected;
};

std::array<Case, 16> cases()
{
    std::array<Case, 16> result;
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        auto &entry = result[index];
        entry.status = index < 14 ? tds::Status::ok : tds::Status::created;
        const std::size_t bytes = index < 7 ? 5000 : index < 14 ? 384 : index == 14 ? 14 : 400;
        entry.body.assign(bytes, static_cast<char>('a' + index));
        entry.expected =
            entry.status == tds::Status::ok ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 201 Created\r\n";
        entry.expected += "Content-Type: application/json\r\nContent-Length: ";
        entry.expected += std::to_string(bytes);
        entry.expected += "\r\n\r\n";
        entry.expected += entry.body;
    }
    return result;
}

struct Work
{
    std::uint64_t bytes = 0;
    std::uint64_t body_bytes = 0;
    std::uint64_t statuses = 0;
    std::uint64_t header_sample = 0;

    bool operator==(const Work &) const = default;
};

void prepare(tds::http::Reply &reply, const Case &entry)
{
    auto output = reply.take_body();
    output.assign(entry.body);
    reply.prepare({entry.status, std::move(output)}, false);
}

Work consume(tds::http::Reply &reply, const Case &entry)
{
    const auto &message = reply.message();
    std::size_t sent = 0;
    for (std::size_t index = 0; index < message.msg_iovlen; ++index)
    {
        sent += message.msg_iov[index].iov_len;
    }
    const auto *header = static_cast<const unsigned char *>(message.msg_iov[0].iov_base);
    const auto header_size = message.msg_iov[0].iov_len;
    const Work work{sent, entry.body.size(), static_cast<std::uint64_t>(entry.status),
                    static_cast<std::uint64_t>(header[9]) + header[header_size - 5]};
    require(reply.advance(sent), "reply probe response did not complete");
    reply.reset();
    return work;
}

void add(Work &sum, const Work &value)
{
    sum.bytes += value.bytes;
    sum.body_bytes += value.body_bytes;
    sum.statuses += value.statuses;
    sum.header_sample += value.header_sample;
}

Work validate(const std::array<Case, 16> &input, std::size_t iterations)
{
    std::array<Work, 16> work;
    tds::http::Reply reply;
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto &entry = input[index];
        prepare(reply, entry);
        const auto &message = reply.message();
        std::string actual;
        for (std::size_t part = 0; part < message.msg_iovlen; ++part)
        {
            actual.append(static_cast<const char *>(message.msg_iov[part].iov_base),
                          message.msg_iov[part].iov_len);
        }
        require(actual == entry.expected, "reply probe wire output differs from oracle");
        work[index] = consume(reply, entry);
    }
    Work expected;
    for (std::size_t index = 0; index < iterations; ++index)
    {
        add(expected, work[index % input.size()]);
    }
    return expected;
}

void measure(const std::array<Case, 16> &input, std::size_t iterations, bool fresh)
{
    const auto expected = validate(input, iterations);
    tds::http::Reply recycled;
    for (std::size_t index = 0; index < 256; ++index)
    {
        const auto &entry = input[index % input.size()];
        prepare(recycled, entry);
        static_cast<void>(consume(recycled, entry));
    }
    Work observed;
    const auto started_wall = now(CLOCK_MONOTONIC);
    const auto started_cpu = now(CLOCK_THREAD_CPUTIME_ID);
    for (std::size_t index = 0; index < iterations; ++index)
    {
        const auto &entry = input[index % input.size()];
        if (fresh)
        {
            tds::http::Reply reply;
            prepare(reply, entry);
            add(observed, consume(reply, entry));
        }
        else
        {
            prepare(recycled, entry);
            add(observed, consume(recycled, entry));
        }
    }
    const auto cpu = now(CLOCK_THREAD_CPUTIME_ID) - started_cpu;
    const auto wall = now(CLOCK_MONOTONIC) - started_wall;
    require(observed == expected, "reply probe changed useful work");
    std::printf("{\"case\":\"%s\",\"responses\":%zu,\"wire_bytes\":%llu,\"body_bytes\":%llu,"
                "\"status_sum\":%llu,\"header_sample\":%llu,\"cpu_ns\":%llu,\"wall_ns\":%llu,"
                "\"scope\":\"body copy, prepare, message, complete send, reset%s\"}\n",
                fresh ? "fresh" : "recycled", iterations,
                static_cast<unsigned long long>(observed.bytes),
                static_cast<unsigned long long>(observed.body_bytes),
                static_cast<unsigned long long>(observed.statuses),
                static_cast<unsigned long long>(observed.header_sample),
                static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(wall),
                fresh ? ", construction and destruction" : "");
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        std::size_t iterations = 1'000'000;
        if (argc != 1)
        {
            require(argc == 3 && std::string_view(argv[1]) == "--responses",
                    "usage: reply_probe [--responses N]");
            const std::string_view value(argv[2]);
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), iterations);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                        iterations > 0 && iterations <= 20'000'000,
                    "invalid reply probe response count");
        }
        const auto input = cases();
        measure(input, iterations, false);
        measure(input, iterations, true);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "Reply probe failed: %s\n", error.what());
        return 1;
    }
}
