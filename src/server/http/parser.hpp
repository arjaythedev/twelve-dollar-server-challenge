#pragma once

#include "../http.hpp"

#include <llhttp.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tds::http
{

struct ParseResult
{
    std::size_t consumed;
    bool valid;
};

class Parser
{
  public:
    Parser();
    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;

    ParseResult parse(std::string_view input);

    Request &request() noexcept
    {
        return request_;
    }

    const Request &request() const noexcept
    {
        return request_;
    }

    bool complete() const noexcept
    {
        return phase_ == Phase::complete;
    }

    bool headers_pending() const noexcept
    {
        return phase_ == Phase::awaiting_headers;
    }

    void accept_headers() noexcept
    {
        phase_ = Phase::body;
        llhttp_resume(&parser_);
    }

    Status error() const noexcept
    {
        return error_;
    }

    std::size_t scratch_capacity() const noexcept
    {
        return url_.capacity() + field_.capacity() + value_.capacity() +
               request_.method.capacity() + request_.path.capacity() +
               request_.authorization.capacity() + request_.body.capacity();
    }

    void reset();

  private:
    enum class Phase : std::uint8_t
    {
        headers,
        awaiting_headers,
        body,
        complete
    };

    static int on_url(llhttp_t *parser, const char *data, std::size_t length);
    static int on_header_field(llhttp_t *parser, const char *data, std::size_t length);
    static int on_header_value(llhttp_t *parser, const char *data, std::size_t length);
    static int on_header_value_complete(llhttp_t *parser);
    static int on_headers_complete(llhttp_t *parser);
    static int on_body(llhttp_t *parser, const char *data, std::size_t length);
    static int on_message_complete(llhttp_t *parser);

    static const llhttp_settings_t settings_;

    llhttp_t parser_{};
    Request request_;
    std::string url_;
    std::string field_;
    std::string value_;
    Phase phase_ = Phase::headers;
    Status error_ = Status::bad_request;
};

} // namespace tds::http
