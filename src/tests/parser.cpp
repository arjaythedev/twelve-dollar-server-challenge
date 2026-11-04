#include "http/parser.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool feed(tds::http::Parser &parser, std::string_view bytes)
{
    while (true)
    {
        const auto parsed = parser.parse(bytes);
        if (!parsed.valid)
        {
            return false;
        }
        bytes.remove_prefix(parsed.consumed);
        if (parser.headers_pending())
        {
            parser.accept_headers();
            continue;
        }

        return bytes.empty();
    }
}

void fragmented_headers()
{
    const std::string request =
        "POST /posts?ignored=true HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: first\r\nX-Discarded: a long irrelevant header value\r\n"
        "aUtHoRiZaTiOn: Bearer final-token\r\nContent-Length: 3\r\n"
        "Connection: close\r\n\r\nabc";
    for (std::size_t split = 1; split < request.size(); ++split)
    {
        tds::http::Parser parser;
        require(feed(parser, std::string_view(request).substr(0, split)) &&
                    feed(parser, std::string_view(request).substr(split)) && parser.complete(),
                "two-fragment header/body parsing changed");
        const auto &message = parser.message();
        require(message.authorization == "Bearer final-token" && message.body == "abc" &&
                    message.path == "/posts" && !message.keep_alive,
                "fragmented duplicate authorization or framing changed");
    }

    tds::http::Parser parser;
    for (const char &byte : request)
    {
        require(feed(parser, std::string_view(&byte, 1)), "one-byte header fragments failed");
    }
    require(parser.complete() && parser.message().authorization == "Bearer final-token",
            "one-byte fragmented header name was not recognized");

    parser.reset();
    require(feed(parser, "GET /feed HTTP/1.1\r\nAuthorization: prior\r\n"
                         "AuthorizatioN: \r\nAuthorizationX: ignored\r\n\r\n") &&
                parser.complete() && parser.message().authorization.empty(),
            "empty duplicate or near-matching header changed");
}

void header_limits_and_reset()
{
    for (const auto field : {std::string_view("X-Discarded"), std::string_view("Authorization")})
    {
        for (const std::size_t size : {16384U, 16385U})
        {
            tds::http::Parser parser;
            const std::string prefix = "GET /feed HTTP/1.1\r\n" + std::string(field) + ": ";
            const std::string value(size, 'x');
            require(feed(parser, prefix), "header prefix failed");
            const bool valid = feed(parser, std::string_view(value).substr(0, 8192)) &&
                               feed(parser, std::string_view(value).substr(8192)) &&
                               feed(parser, "\r\n\r\n");
            require(valid == (size == 16384), "captured/discarded value size limit changed");
            if (valid)
            {
                require(parser.complete(), "bounded header did not complete");
            }
            parser.reset();
            require(feed(parser, "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n") &&
                        parser.complete() && parser.message().authorization.empty(),
                    "reset retained header capture or size state");
        }
    }

    for (const std::size_t size : {16384U, 16385U})
    {
        tds::http::Parser parser;
        const std::string request =
            "GET /feed HTTP/1.1\r\n" + std::string(size, 'X') + ": v\r\n\r\n";
        require(feed(parser, request) == (size == 16384), "header field size limit changed");
    }
}

void completed_authorization_visibility()
{
    tds::http::Parser parser;
    require(feed(parser, "GET /feed HTTP/1.1\r\nAuthoRiz") && feed(parser, "ation: Bearer par") &&
                parser.message().authorization.empty() && feed(parser, "tial") &&
                parser.message().authorization.empty(),
            "partial first authorization became visible before value completion");
    require(feed(parser, "\r\nX-After: yes\r\n") &&
                parser.message().authorization == "Bearer partial",
            "completed first authorization was not published");
    require(feed(parser, "AUTHORIZATION: next") &&
                parser.message().authorization == "Bearer partial" && feed(parser, "-token") &&
                parser.message().authorization == "Bearer partial",
            "partial duplicate replaced the last completed authorization");
    require(feed(parser, "\r\n\r\n") && parser.complete() &&
                parser.message().authorization == "next-token",
            "completed duplicate did not replace authorization");

    for (const bool oversized : {false, true})
    {
        parser.reset();
        require(feed(parser, "GET /feed HTTP/1.1\r\nAuthorization: last-valid\r\n"
                             "Authorization: partial") &&
                    parser.message().authorization == "last-valid",
                "duplicate setup changed completed authorization");
        const std::string invalid = oversized ? std::string(16384, 'x') : "bad\x01value\r\n\r\n";
        require(!feed(parser, invalid) && parser.message().authorization == "last-valid",
                "invalid or oversized duplicate discarded the last completed authorization");
    }

    parser.reset();
    require(feed(parser, "GET /feed HTTP/1.1\r\nAuthorization: unfinished") &&
                parser.message().authorization.empty(),
            "partial value leaked after error reset");
    parser.reset();
    require(feed(parser, "GET /feed HTTP/1.1\r\nHost: local\r\n\r\n") && parser.complete() &&
                parser.message().authorization.empty(),
            "reset retained an unfinished value or field matcher");
}

void varied_authorization_and_near_matches()
{
    tds::http::Parser parser;
    for (const std::size_t size : {0U, 1U, 15U, 16U, 31U, 180U, 1024U, 16384U, 1U, 0U})
    {
        parser.reset();
        const std::string token(size, 'a');
        require(feed(parser, "GET /feed HTTP/1.1\r\nAuthorization: old\r\n") &&
                    feed(parser, "aUtHoRiZaTiOn: ") && feed(parser, token) &&
                    parser.message().authorization == "old" &&
                    feed(parser, "\r\nAuthorizationX: ignored\r\nXAuthorization: ignored\r\n"
                                 "Authorizatio: ignored\r\nAuthorizatioN-Header: ignored\r\n"
                                 "Authorizatiom: ignored\r\n\r\n") &&
                    parser.complete() && parser.message().authorization == token,
                "varied token sizes, empty duplicates or near-matching fields changed");
    }
}

void framing_and_early_headers()
{
    tds::http::Parser parser;
    const std::string wire =
        "POST /posts HTTP/1.1\r\nAuthorization: Bearer token\r\n"
        "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\nX-Trailer: ignored\r\n\r\n"
        "GET /feed HTTP/1.1\r\n\r\n";
    const auto headers = parser.parse(wire);
    require(headers.valid && parser.headers_pending() && parser.message().body.empty() &&
                parser.message().authorization == "Bearer token",
            "headers no longer pause before body/auth decision");
    parser.accept_headers();
    const auto remainder = std::string_view(wire).substr(headers.consumed);
    const auto body = parser.parse(remainder);
    require(body.valid && parser.complete() && parser.message().body == "abc",
            "discarding framing/trailer values changed chunked body");
    parser.reset();
    require(feed(parser, remainder.substr(body.consumed)) && parser.complete() &&
                parser.message().path == "/feed" && parser.message().authorization.empty(),
            "pipeline tail or authorization reset changed");

    parser.reset();
    require(feed(parser, "POST /posts HTTP/1.1\r\nAuthorization: head-token\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n"
                         "AUTHORIZATION: trailer-token\r\n\r\n") &&
                parser.complete() && parser.message().body == "x" &&
                parser.message().authorization == "trailer-token",
            "authorization trailers no longer retain the last completed value");
    parser.reset();
    require(feed(parser, "POST /posts HTTP/1.1\r\nAuthorization: head-token\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n0\r\nAUTHORIZATION:\r\n\r\n") &&
                parser.complete() && parser.message().authorization.empty(),
            "empty authorization trailer retained the prior value");

    for (const auto invalid :
         {"GET /feed HTTP/1.1\r\nX-Discarded: bad\x01value\r\n\r\n",
          "POST /posts HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
          "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\nx"})
    {
        parser.reset();
        require(!feed(parser, invalid), "llhttp no longer rejects malformed discarded headers");
    }
}
} // namespace

int main()
{
    try
    {
        fragmented_headers();
        header_limits_and_reset();
        completed_authorization_visibility();
        varied_authorization_and_near_matches();
        framing_and_early_headers();
        std::puts(
            "fragmented header capture, limits, framing, early pause and pipeline checks passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "parser checks failed: %s\n", error.what());

        return 1;
    }
}
