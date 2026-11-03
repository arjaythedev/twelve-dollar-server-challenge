#pragma once

namespace tds::telemetry
{

struct IoReport;
struct DatabaseReport;

// Export only after the owner has stopped collecting, never from an individual hook
void write_report(const IoReport &report) noexcept;
void write_report(const DatabaseReport &report) noexcept;

} // namespace tds::telemetry
