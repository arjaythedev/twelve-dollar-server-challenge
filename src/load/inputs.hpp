#pragma once

#include "config.hpp"

#include <yyjson.h>

#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <vector>

namespace loadgen
{
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

struct Endpoint
{
    sockaddr_in address{};
    std::string authority;
    std::string prefix;
};

struct Token
{
    std::string username;
    std::string authorization;
};

struct Options
{
    RuntimeConfig runtime;
    std::string base_url;
    std::string tokens;
    std::string summary;
    std::string ready_file;
    std::string start_gate;
    std::string local_ips;
};

Document json(std::string_view text);
std::string json_string(std::string_view value);
std::string environment(const char *name, std::string fallback);
unsigned integer(std::string_view text, unsigned maximum, std::string_view name);
std::uint64_t random_seed(std::string_view text);
double duration(std::string_view text);
Endpoint endpoint(std::string_view url);
std::vector<in_addr> local_addresses(std::string_view input);
std::vector<Token> read_tokens(const std::string &path);
Options parse_options(int argc, char **argv);
std::string_view help();
void write_file(const std::string &path, std::string_view value);
} // namespace loadgen
