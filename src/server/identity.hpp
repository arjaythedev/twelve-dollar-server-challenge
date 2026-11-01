#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace tds
{

struct Identity
{
    // A validated positive subject can exceed SQLite's signed integer range.
    std::optional<std::int64_t> user_id;
    std::string username;
};

} // namespace tds
