#pragma once

#include <utility>

#ifndef TDS_TELEMETRY_LEVEL
#define TDS_TELEMETRY_LEVEL 0
#endif

static_assert(TDS_TELEMETRY_LEVEL >= 0 && TDS_TELEMETRY_LEVEL <= 2);

namespace tds::telemetry
{

enum class Level
{
    off,
    basic,
    profile
};

constexpr auto level = static_cast<Level>(TDS_TELEMETRY_LEVEL);
constexpr bool enabled = level != Level::off;
constexpr bool profiling = level == Level::profile;

[[nodiscard]] constexpr const char *name(Level value) noexcept
{
    switch (value)
    {
    case Level::off:
        return "off";
    case Level::basic:
        return "basic";
    case Level::profile:
        return "profile";
    }
    std::unreachable();
}

} // namespace tds::telemetry
