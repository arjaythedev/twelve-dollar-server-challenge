#include "http/parser.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>

namespace
{
bool counting = false;
std::uint64_t allocations = 0;
std::uint64_t allocated_bytes = 0;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint64_t cpu_ns()
{
    timespec now{};
    require(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0, "thread CPU clock");

    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

void parse(tds::http::Parser &parser, std::string_view bytes)
{
    while (true)
    {
        const auto parsed = parser.parse(bytes);
        require(parsed.valid, "invalid probe request");
        bytes.remove_prefix(parsed.consumed);
        if (parser.headers_pending())
        {
            parser.accept_headers();
            continue;
        }
        require(bytes.empty(), "probe parser left unexpected bytes");

        return;
    }
}

void measure(std::string_view name, const std::string &request, std::string_view authorization,
             std::size_t iterations, std::size_t fragment_size,
             std::string_view alternate_request = {}, std::string_view alternate_authorization = {})
{
    tds::http::Parser parser;
    std::uint64_t observed = 0;
    std::uint64_t ordinal = 0;
    const auto one = [&] {
        const bool alternate = !alternate_request.empty() && (ordinal++ & 1U) != 0;
        const std::string_view wire = alternate ? alternate_request : request;
        const auto expected_authorization = alternate ? alternate_authorization : authorization;
        parser.reset();
        for (std::size_t offset = 0; offset < wire.size(); offset += fragment_size)
        {
            parse(parser, wire.substr(offset, fragment_size));
        }
        require(parser.complete() && parser.message().method == "GET" &&
                    parser.message().path == "/posts/42" && parser.message().body.empty() &&
                    parser.message().authorization == expected_authorization &&
                    parser.message().keep_alive,
                "probe request fields changed");
        observed += parser.message().path.size() + parser.message().authorization.size();
    };
    counting = true;
    allocations = 0;
    allocated_bytes = 0;
    one();
    counting = false;
    const auto cold_allocations = allocations;
    const auto cold_bytes = allocated_bytes;
    for (unsigned i = 0; i < 1000; ++i)
    {
        one();
    }
    counting = true;
    allocations = 0;
    allocated_bytes = 0;
    one();
    one();
    counting = false;
    const auto warm_allocations = allocations;
    const auto warm_bytes = allocated_bytes;
    observed = 0;
    ordinal = 0;
    const auto started = cpu_ns();
    for (std::size_t i = 0; i < iterations; ++i)
    {
        one();
    }
    const auto elapsed = cpu_ns() - started;
    std::printf(
        "{\"case\":\"%.*s\",\"requests\":%zu,\"request_bytes\":%zu,"
        "\"alternate_request_bytes\":%zu,\"fragment_bytes\":%zu,\"cpu_ns\":%llu,\"observed_bytes\":"
        "%llu,"
        "\"cold_cpp_allocations\":%llu,\"cold_allocated_bytes\":%llu,"
        "\"warm_two_request_cpp_allocations\":%llu,\"warm_two_request_allocated_bytes\":%llu,"
        "\"scratch_capacity\":%zu,\"parser_bytes\":%zu}\n",
        static_cast<int>(name.size()), name.data(), iterations, request.size(),
        alternate_request.size(), fragment_size, static_cast<unsigned long long>(elapsed),
        static_cast<unsigned long long>(observed),
        static_cast<unsigned long long>(cold_allocations),
        static_cast<unsigned long long>(cold_bytes),
        static_cast<unsigned long long>(warm_allocations),
        static_cast<unsigned long long>(warm_bytes), parser.scratch_capacity(), sizeof(parser));
}
} // namespace

void *operator new(std::size_t bytes)
{
    if (void *value = std::malloc(bytes ? bytes : 1))
    {
        if (counting)
        {
            ++allocations;
            allocated_bytes += bytes;
        }

        return value;
    }

    throw std::bad_alloc();
}

void operator delete(void *value) noexcept
{
    std::free(value);
}

void operator delete(void *value, std::size_t) noexcept
{
    std::free(value);
}

int main(int argc, char **argv)
{
    try
    {
        std::size_t iterations = 500000;
        if (argc > 1)
        {
            const std::string_view text(argv[1]);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), iterations);
            require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                        iterations,
                    "positive iteration count required");
        }
        const std::string common =
            "GET /posts/42 HTTP/1.1\r\nHost: server:8080\r\nUser-Agent: native-load/0.1.0\r\n"
            "Accept: */*\r\nConnection: keep-alive\r\n";
        const std::string token = "Bearer " + std::string(180, 'a');
        const std::string ordinary = common + "\r\n";
        const std::string authenticated = common + "Authorization: " + token + "\r\n\r\n";
        const std::string long_header =
            common + "X-Discarded: " + std::string(1024, 'x') + "\r\n\r\n";
        const std::string duplicate = common + "Authorization: " + std::string(1024, 'b') +
                                      "\r\nAuthorization: " + token + "\r\n\r\n";
        const std::string long_name = common + std::string(1024, 'X') + ": ignored\r\n\r\n";
        const std::string short_auth = common + "Authorization: x\r\n\r\n";
        measure("native_get", ordinary, "", iterations, ordinary.size());
        measure("authenticated_get", authenticated, token, iterations, authenticated.size());
        measure("fragmented_auth", authenticated, token, iterations / 5 + 1, 7);
        measure("long_ignored_header", long_header, "", iterations / 5 + 1, long_header.size());
        measure("duplicate_auth", duplicate, token, iterations, duplicate.size());
        measure("long_ignored_name", long_name, "", iterations / 5 + 1, long_name.size());
        measure("alternating_auth_reuse", authenticated, token, iterations, authenticated.size(),
                short_auth, "x");

        return 0;
    }
    catch (const std::exception &error)
    {
        counting = false;
        std::fprintf(stderr, "parser probe failed: %s\n", error.what());

        return 1;
    }
}
