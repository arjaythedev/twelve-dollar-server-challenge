#include "parser.hpp"
#include "../parsing.hpp"

#include <climits>

namespace tds::http
{
namespace
{

int append_fragment(std::string &target, const char *data, std::size_t length)
{
    if (length > 16384 - target.size())
    {
        return -1;
    }
    target.append(data, length);

    return 0;
}

} // namespace

constinit const llhttp_settings_t Parser::settings_ = []() constexpr {
    llhttp_settings_t settings{};
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_field_complete = on_header_field_complete;
    settings.on_header_value = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;
    return settings;
}();

Parser::Parser()
{
    reset();
}

ParseResult Parser::parse(std::string_view input)
{
    const auto result = llhttp_execute(&parser_, input.data(), input.size());
    std::size_t consumed = input.size();
    if (result == HPE_PAUSED)
    {
        consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - input.data());
    }
    return {consumed, result == HPE_OK || result == HPE_PAUSED};
}

void Parser::reset()
{
    request_.method.clear();
    request_.path.clear();
    request_.authorization.clear();
    request_.body.clear();
    request_.keep_alive = true;
    url_.clear();
    field_.clear();
    value_.clear();
    value_size_ = 0;
    capture_value_ = false;
    phase_ = Phase::headers;
    error_ = Status::bad_request;
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

int Parser::on_url(llhttp_t *parser, const char *data, std::size_t length)
{
    auto &self = *static_cast<Parser *>(parser->data);
    return append_fragment(self.url_, data, length);
}

int Parser::on_header_field(llhttp_t *parser, const char *data, std::size_t length)
{
    auto &self = *static_cast<Parser *>(parser->data);
    return append_fragment(self.field_, data, length);
}

int Parser::on_header_value(llhttp_t *parser, const char *data, std::size_t length)
{
    auto &self = *static_cast<Parser *>(parser->data);
    // Discarded values still count toward the same bound, and llhttp validates their bytes
    if (length > 16384U - self.value_size_)
    {
        return -1;
    }
    self.value_size_ = static_cast<std::uint16_t>(self.value_size_ + length);
    if (self.capture_value_)
    {
        self.value_.append(data, length);
    }

    return 0;
}

int Parser::on_header_field_complete(llhttp_t *parser)
{
    auto &self = *static_cast<Parser *>(parser->data);
    constexpr std::string_view authorization = "authorization";
    self.capture_value_ = self.field_.size() == authorization.size();
    if (!self.capture_value_)
    {
        return 0;
    }
    for (std::size_t i = 0; i < authorization.size(); ++i)
    {
        char c = self.field_[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c != authorization[i])
        {
            self.capture_value_ = false;

            return 0;
        }
    }

    return 0;
}

int Parser::on_header_value_complete(llhttp_t *parser)
{
    auto &self = *static_cast<Parser *>(parser->data);
    if (self.capture_value_)
    {
        self.request_.authorization = self.value_;
    }
    self.field_.clear();
    self.value_.clear();
    self.value_size_ = 0;
    self.capture_value_ = false;
    return 0;
}

int Parser::on_headers_complete(llhttp_t *parser)
{
    auto &self = *static_cast<Parser *>(parser->data);
    self.phase_ = Phase::awaiting_headers;
    self.request_.method = llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
    self.request_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    const auto end = self.url_.find('?');
    const std::string_view path(self.url_.data(),
                                end == std::string::npos ? self.url_.size() : end);
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '%' && i + 2 < path.size())
        {
            const int high = parsing::hex_digit(path[i + 1]);
            const int low = parsing::hex_digit(path[i + 2]);
            if (high < 0 || low < 0)
            {
                return -1;
            }
            self.request_.path.push_back(static_cast<char>(high * 16 + low));
            i += 2;
        }
        else
        {
            self.request_.path.push_back(path[i]);
        }
    }
    if (parser->content_length != ULLONG_MAX && parser->content_length > 16384)
    {
        self.error_ = Status::too_large;
        return -1;
    }
    return static_cast<int>(HPE_PAUSED);
}

int Parser::on_body(llhttp_t *parser, const char *data, std::size_t length)
{
    auto &self = *static_cast<Parser *>(parser->data);
    if (self.request_.body.size() + length > 16384)
    {
        self.error_ = Status::too_large;
        return -1;
    }
    self.request_.body.append(data, length);
    return 0;
}

int Parser::on_message_complete(llhttp_t *parser)
{
    static_cast<Parser *>(parser->data)->phase_ = Phase::complete;
    return static_cast<int>(HPE_PAUSED);
}

} // namespace tds::http
