#include "http/session.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void headers(tds::http::HttpSession &session)
{
    require(session.consume().valid && session.headers_pending(), "expected header pause");
}

void complete(tds::http::HttpSession &session)
{
    session.accept_headers(tds::MissingHead{});
    require(session.consume().valid && session.complete(), "expected completed message");
}

void fragmented_body_and_pipeline()
{
    tds::http::HttpSession session;
    session.append("POST /posts/123/like HTTP/1.1\r\nContent-Length: 4\r\n\r\nab");
    headers(session);
    require(session.has_input() && session.message().body.empty(),
            "header pause consumed the body before acceptance");
    const std::string_view id(session.message().path.data() + 7, 3);
    session.accept_headers(tds::PostHead{id});
    require(session.consume().valid && !session.complete() && !session.has_input() &&
                session.message().body == "ab",
            "partially consumed body left phantom input or completed early");
    session.append("cdGET /feed HTTP/1.1\r\n\r\n");
    require(session.consume().valid && session.complete() && session.has_input() &&
                session.message().body == "abcd" &&
                std::get<tds::PostHead>(session.accepted_head()).id == "123" &&
                std::get<tds::PostHead>(session.accepted_head()).id.data() == id.data(),
            "appended body or pipeline invalidated borrowed route storage");
    session.reset();
    require(session.has_input() && session.message().path.empty() && session.message().body.empty(),
            "reset discarded pipeline input or retained message fields");
    headers(session);
    require(!session.has_input() && session.message().path == "/feed",
            "pipeline request was lost or retained consumed prefix");
    session.accept_headers(tds::FeedHead{});
    require(session.consume().valid && session.complete() && !session.has_input() &&
                std::holds_alternative<tds::FeedHead>(session.accepted_head()),
            "new accepted context inherited the previous borrowed route");
}

void append_with_retained_tail()
{
    for (const bool warm : {false, true})
    {
        tds::http::HttpSession session;
        if (warm)
        {
            // Leave room at the end so this pass keeps the consumed prefix during append
            session.append("GET /feed HTTP/1.1\r\nX-Discarded: " + std::string(8192, 'x') +
                           "\r\n\r\n");
            headers(session);
            complete(session);
            session.reset();
        }
        session.append("POST /posts HTTP/1.1\r\nContent-Length: 4\r\n\r\nabcdGET /po");
        headers(session);
        session.append("sts/42 HTTP/1.1\r\nX-Discarded: " + std::string(4096, 'x') + "\r\n\r\n");
        complete(session);
        require(session.message().body == "abcd" && session.has_input(),
                "append after header pause corrupted retained body/pipeline bytes");
        session.reset();
        headers(session);
        complete(session);
        require(session.message().path == "/posts/42" && !session.has_input(),
                "append lost a fragmented pipeline request");
    }
}

void chunked_fragments()
{
    const std::string wire = "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "3\r\nabc\r\n2\r\nde\r\n0\r\nX-Trailer: ignored\r\n\r\n";
    tds::http::HttpSession session;
    for (const char &byte : wire)
    {
        session.append(std::string_view(&byte, 1));
        require(session.consume().valid, "chunked byte fragment failed");
        if (session.headers_pending())
        {
            require(session.message().body.empty(), "chunked body preceded header acceptance");
            session.accept_headers(tds::MissingHead{});
            require(session.consume().valid, "resume after fragmented chunk headers failed");
        }
        require(!session.has_input(), "consumed chunk fragment remained logically buffered");
    }
    require(session.complete() && session.message().body == "abcde",
            "chunk sizes/trailers leaked into assembled body");
}

void bounded_stream_and_invalid_input()
{
    constexpr std::string_view request = "GET /feed HTTP/1.1\r\nHost: local\r\n\r\n";
    tds::http::HttpSession session;
    session.append(std::string(request) + std::string(request));
    std::size_t warmed_capacity = 0;
    for (unsigned i = 0; i < 10000; ++i)
    {
        headers(session);
        complete(session);
        require(session.message().path == "/feed" && session.has_input(),
                "stream lost its single pending pipeline request");
        session.reset();
        session.append(request);
        if (i == 63)
        {
            warmed_capacity = session.retained_capacity();
        }
        if (i > 63)
        {
            require(session.retained_capacity() <= warmed_capacity,
                    "consumed prefixes caused unbounded retained-capacity growth");
        }
    }

    tds::http::HttpSession invalid;
    invalid.append("GET /feed HTTP/1.1\r\nContent-Length: no\r\n\r\n");
    require(!invalid.consume().valid && invalid.has_input() && !invalid.headers_pending() &&
                !invalid.complete() && invalid.error() == tds::Status::bad_request,
            "invalid parse consumed buffered bytes or changed its error contract");
}
} // namespace

int main()
{
    try
    {
        fragmented_body_and_pipeline();
        append_with_retained_tail();
        chunked_fragments();
        bounded_stream_and_invalid_input();
        std::puts(
            "session body/pipeline tails, append compaction, borrowed routes and bounds passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "session checks failed: %s\n", error.what());

        return 1;
    }
}
