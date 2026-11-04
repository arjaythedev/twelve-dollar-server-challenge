#include "inputs.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <netdb.h>
#include <random>
#include <stdexcept>
#include <utility>

namespace loadgen
{
Document json(std::string_view text)
{
    return {yyjson_read(text.data(), text.size(), 0), &yyjson_doc_free};
}

std::string json_string(std::string_view value)
{
    std::string output = "\"";
    for (const unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            output += '\\';
            output += static_cast<char>(c);
        }
        else if (c < 32)
        {
            output += "\\u00";
            output += "0123456789abcdef"[c >> 4];
            output += "0123456789abcdef"[c & 15];
        }
        else
        {
            output += static_cast<char>(c);
        }
    }
    return output + '"';
}

std::string environment(const char *name, std::string fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? value : std::move(fallback);
}

unsigned integer(std::string_view text, unsigned maximum, std::string_view name)
{
    unsigned result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || result == 0 || result > maximum)
    {
        throw std::runtime_error(std::string(name) + " must be between 1 and " +
                                 std::to_string(maximum));
    }
    return result;
}

std::uint64_t random_seed(std::string_view text)
{
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
    {
        throw std::runtime_error("--seed must be an unsigned 64-bit integer");
    }
    return result;
}

double duration(std::string_view text)
{
    if (text.empty())
    {
        throw std::runtime_error("duration cannot be empty");
    }
    double total = 0;
    while (!text.empty())
    {
        double number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{} || end == text.data() || !std::isfinite(number) || number < 0)
        {
            throw std::runtime_error("invalid duration, use units such as 500ms, 30s or 5m");
        }
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        double scale = 1;
        if (text.starts_with("ms"))
        {
            scale = 0.001;
            text.remove_prefix(2);
        }
        else if (text.starts_with('s'))
        {
            text.remove_prefix(1);
        }
        else if (text.starts_with('m'))
        {
            scale = 60;
            text.remove_prefix(1);
        }
        else if (text.starts_with('h'))
        {
            scale = 3600;
            text.remove_prefix(1);
        }
        else
        {
            throw std::runtime_error("duration requires ms, s, m or h units");
        }
        total += number * scale;
    }
    if (!std::isfinite(total) || total > 7 * 24 * 3600)
    {
        throw std::runtime_error("duration exceeds seven days");
    }
    return total;
}

Endpoint endpoint(std::string_view url)
{
    if (!url.starts_with("http://"))
    {
        throw std::runtime_error("BASE_URL supports plain http:// with IPv4 only");
    }
    url.remove_prefix(7);
    const auto slash = url.find('/');
    const auto authority = url.substr(0, slash);
    if (authority.empty() || authority.find_first_of("@[]?#\r\n ") != std::string_view::npos)
    {
        throw std::runtime_error("BASE_URL requires an IPv4 host without credentials or query");
    }
    const auto colon = authority.find(':');
    const std::string host(authority.substr(0, colon));
    const std::string port =
        colon == std::string_view::npos ? "80" : std::string(authority.substr(colon + 1));
    static_cast<void>(integer(port, 65535, "port"));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *raw = nullptr;
    const int result = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &raw);
    if (result != 0)
    {
        throw std::runtime_error("IPv4 lookup failed: " + std::string(gai_strerror(result)));
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, &freeaddrinfo);
    Endpoint output;
    std::memcpy(&output.address, raw->ai_addr, sizeof(output.address));
    output.authority = authority;
    if (slash != std::string_view::npos)
    {
        output.prefix = url.substr(slash);
        if (output.prefix.find_first_of("?#\r\n ") != std::string::npos)
        {
            throw std::runtime_error("BASE_URL path cannot contain query, fragment or whitespace");
        }
        while (output.prefix.ends_with('/'))
        {
            output.prefix.pop_back();
        }
    }
    return output;
}

std::vector<in_addr> local_addresses(std::string_view input)
{
    std::vector<in_addr> result;
    while (!input.empty())
    {
        const auto comma = input.find(',');
        const auto item = input.substr(0, comma);
        const auto dash = item.find('-');
        in_addr first{};
        in_addr last{};
        if (::inet_pton(AF_INET, std::string(item.substr(0, dash)).c_str(), &first) != 1 ||
            (dash != std::string_view::npos &&
             ::inet_pton(AF_INET, std::string(item.substr(dash + 1)).c_str(), &last) != 1))
        {
            throw std::runtime_error("--local-ips requires IPv4 addresses or IPv4-IPv4 ranges");
        }
        const auto begin = ntohl(first.s_addr);
        const auto end = dash == std::string_view::npos ? begin : ntohl(last.s_addr);
        if (end < begin || static_cast<std::uint64_t>(end) - begin + result.size() >= 65536)
        {
            throw std::runtime_error("--local-ips range is reversed or exceeds 65536 addresses");
        }
        for (std::uint64_t address = begin; address <= end; ++address)
        {
            result.push_back(in_addr{htonl(static_cast<std::uint32_t>(address))});
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        input.remove_prefix(comma + 1);
        if (input.empty())
        {
            throw std::runtime_error("--local-ips has an empty address");
        }
    }
    return result;
}

std::vector<Token> read_tokens(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open token file: " + path);
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const auto document = json(text);
    auto *root = document ? yyjson_doc_get_root(document.get()) : nullptr;
    if (!yyjson_is_arr(root) || yyjson_arr_size(root) == 0)
    {
        throw std::runtime_error("token file must contain a nonempty JSON array");
    }
    std::vector<Token> tokens;
    tokens.reserve(yyjson_arr_size(root));
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val *entry = nullptr;
    yyjson_arr_foreach(root, index, count, entry)
    {
        auto *username = yyjson_obj_get(entry, "username");
        auto *token = yyjson_obj_get(entry, "token");
        if (!yyjson_is_str(username) || !yyjson_is_str(token) || yyjson_get_len(token) == 0)
        {
            throw std::runtime_error("each token needs string username and nonempty token");
        }
        const std::string_view value(yyjson_get_str(token), yyjson_get_len(token));
        if (value.find_first_of("\r\n") != std::string_view::npos)
        {
            throw std::runtime_error("token cannot contain HTTP header line breaks");
        }
        tokens.push_back({std::string(yyjson_get_str(username), yyjson_get_len(username)),
                          "Authorization: Bearer " + std::string(value) + "\r\n"});
    }
    return tokens;
}

Options parse_options(int argc, char **argv)
{
    Options options;
    options.base_url = environment("BASE_URL", "http://127.0.0.1:3000");
    options.tokens = environment("TOKENS", "seed/tokens.json");
    auto &config = options.runtime;
    config.vus = integer(environment("VUS", "100"), 1'000'000, "VUS");
    config.hold = duration(environment("DURATION", "5m"));
    config.ramp_up = duration(environment("RAMP_UP", "60s"));
    config.seed = std::random_device{}();
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--diagnostics")
        {
            config.diagnostics = true;
            continue;
        }
        const auto equals = argument.find('=');
        const auto name = argument.substr(0, equals);
        std::string_view value;
        if (equals != std::string_view::npos)
        {
            value = argument.substr(equals + 1);
        }
        else
        {
            if (index + 1 == argc)
            {
                throw std::runtime_error("missing value for " + std::string(name));
            }
            value = argv[++index];
        }
        if (name == "--base-url")
        {
            options.base_url = value;
        }
        else if (name == "--seed")
        {
            config.seed = random_seed(value);
        }
        else if (name == "--vus")
        {
            config.vus = integer(value, 1'000'000, "VUS");
        }
        else if (name == "--duration")
        {
            config.hold = duration(value);
        }
        else if (name == "--ramp-up")
        {
            config.ramp_up = duration(value);
        }
        else if (name == "--ramp-down")
        {
            config.ramp_down = duration(value);
        }
        else if (name == "--graceful-ramp-down")
        {
            config.grace = duration(value);
        }
        else if (name == "--request-timeout")
        {
            config.timeout = duration(value);
        }
        else if (name == "--tokens")
        {
            options.tokens = value;
        }
        else if (name == "--summary-export")
        {
            options.summary = value;
        }
        else if (name == "--local-ips")
        {
            options.local_ips = value;
        }
        else if (name == "--ready-file")
        {
            options.ready_file = value;
        }
        else if (name == "--start-gate")
        {
            options.start_gate = value;
        }
        else
        {
            throw std::runtime_error("unsupported option: " + std::string(name));
        }
    }
    if (config.hold <= 0 || config.timeout <= 0)
    {
        throw std::runtime_error("DURATION and request timeout must be greater than zero");
    }
    return options;
}

std::string_view help()
{
    return "load_generator [--base-url http://HOST:PORT] [--vus N] [--duration 5m]\n"
           "  [--seed UINT64]\n"
           "  [--ramp-up 60s] [--ramp-down 30s] [--graceful-ramp-down 10s]\n"
           "  [--request-timeout 60s] [--tokens seed/tokens.json] [--summary-export PATH]\n"
           "  [--local-ips IPv4,IPv4-IPv4] [--ready-file PATH] [--start-gate PATH]\n"
           "  [--diagnostics]\n"
           "Environment: BASE_URL, VUS, DURATION, RAMP_UP, TOKENS\n"
           "Linux, plain HTTP/1.1, IPv4; one persistent socket per VU\n"
           "Exact quantiles retain 16 bytes/request in chunks (20 million request limit)\n";
}

void write_file(const std::string &path, std::string_view value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    output.close();
    if (!output)
    {
        throw std::runtime_error("cannot write file: " + path);
    }
}

} // namespace loadgen
