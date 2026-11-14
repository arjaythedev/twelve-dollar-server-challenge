#include "http/session.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>

namespace
{
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

void headers(tds::http::HttpSession &session)
{
    require(session.consume().valid && session.headers_pending(), "probe header pause");
}

std::uint64_t finish(tds::http::HttpSession &session, std::string_view body)
{
    session.accept_headers(tds::MissingHead{});
    require(session.consume().valid && session.complete() &&
                session.message().path == "/posts/42" && session.message().body == body,
            "probe message fields changed");
    const auto observed = session.message().path.size() + session.message().body.size();
    session.reset();

    return observed;
}

void measure(std::string_view name, const std::string &request, std::string_view body,
             std::size_t iterations, std::size_t batch, bool fragmented)
{
    tds::http::HttpSession session;
    std::string input;
    std::string prefix;
    if (fragmented)
    {
        prefix = request.substr(0, request.size() - body.size() + 64);
        input = std::string(body.substr(64)) + prefix;
        session.append(prefix);
    }
    else
    {
        for (std::size_t i = 0; i < batch; ++i)
        {
            input += request;
        }
    }
    std::uint64_t observed = 0;
    const auto one = [&] {
        if (fragmented)
        {
            headers(session);
            // Keep the next incomplete message queued so append must eventually reclaim its prefix
            session.append(input);
            observed += finish(session, body);
            require(session.has_input(), "fragmented probe lost its next message");

            return;
        }
        session.append(input);
        for (std::size_t i = 0; i < batch; ++i)
        {
            headers(session);
            observed += finish(session, body);
        }
        require(!session.has_input(), "fully drained probe retained logical input");
    };
    for (unsigned i = 0; i < 1000; ++i)
    {
        one();
    }
    observed = 0;
    const auto rounds = (iterations + batch - 1) / batch;
    const auto started = cpu_ns();
    for (std::size_t i = 0; i < rounds; ++i)
    {
        one();
    }
    const auto elapsed = cpu_ns() - started;
    std::printf("{\"case\":\"%.*s\",\"requests\":%zu,\"request_bytes\":%zu,"
                "\"pipeline_batch\":%zu,\"cpu_ns\":%llu,\"observed_bytes\":%llu,"
                "\"retained_capacity\":%zu,\"session_bytes\":%zu}\n",
                static_cast<int>(name.size()), name.data(), rounds * batch, request.size(), batch,
                static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(observed),
                session.retained_capacity(), sizeof(session));
}
} // namespace

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
        const std::string get =
            "GET /posts/42 HTTP/1.1\r\nHost: server:8080\r\nUser-Agent: native-load/0.1.0\r\n"
            "Accept: */*\r\nConnection: keep-alive\r\n\r\n";
        const std::string body(500, 'x');
        const std::string post =
            "POST /posts/42 HTTP/1.1\r\nHost: server:8080\r\nContent-Length: 500\r\n\r\n" + body;
        measure("ordinary_get", get, "", iterations, 1, false);
        measure("contiguous_body", post, body, iterations, 1, false);
        measure("pipeline8", get, "", iterations, 8, false);
        measure("pipeline32", get, "", iterations, 32, false);
        measure("fragmented_body_tail", post, body, iterations, 1, true);

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "session probe failed: %s\n", error.what());

        return 1;
    }
}
