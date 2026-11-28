#pragma once

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

sqlite3_file *tds_wal_unwrap(sqlite3_file *file);

#ifdef __cplusplus
}

namespace tds::storage
{

class WalVfs
{
  public:
    // Parent, name and adapter must outlive every file opened through this registration
    WalVfs(sqlite3_vfs &parent, const char *stable_name);
    ~WalVfs();
    WalVfs(const WalVfs &) = delete;
    WalVfs &operator=(const WalVfs &) = delete;

    sqlite3_vfs &native() noexcept;
    const char *name() const noexcept;

  private:
    sqlite3_vfs native_{};
};

const char *wal_vfs_name();

} // namespace tds::storage
#endif
