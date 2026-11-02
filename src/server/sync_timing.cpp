#include "database_timing.hpp"

#ifdef TDS_DIAGNOSTICS

#include <cstdio>
#include <mutex>

namespace tds::database_timing
{

void install_sync_hooks()
{
    static std::once_flag installed;
    std::call_once(installed, [] {
        std::fprintf(stderr,
                     "TDS_TIMING {\"source\":\"sync_hooks\",\"time_ns\":%llu,"
                     "\"method\":\"linker_wrap\",\"fsync\":\"enabled\","
                     "\"fdatasync\":\"enabled\"}\n",
                     static_cast<unsigned long long>(timing::now_ns()));
    });
}

} // namespace tds::database_timing

extern "C" int __wrap_fsync(int descriptor) noexcept
{
    return tds::database_timing::measured_fsync(descriptor);
}

extern "C" int __wrap_fdatasync(int descriptor) noexcept
{
    return tds::database_timing::measured_fdatasync(descriptor);
}

#endif
