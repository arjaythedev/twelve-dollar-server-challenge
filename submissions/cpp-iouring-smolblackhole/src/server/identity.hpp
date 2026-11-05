#pragma once

#include "model.hpp"

#include <optional>
#include <string>

namespace tds
{

struct Identity
{
    // A validated positive subject can exceed SQLite's signed integer range
    std::optional<UserId> user_id;
    std::string username;
};

} // namespace tds
