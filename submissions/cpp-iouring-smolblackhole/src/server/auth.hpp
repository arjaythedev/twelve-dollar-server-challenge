#pragma once

#include "http.hpp"
#include "identity.hpp"

#include <expected>
#include <string_view>

namespace tds
{

std::expected<Identity, ApiError> authenticate(std::string_view header, std::string_view secret);

} // namespace tds
