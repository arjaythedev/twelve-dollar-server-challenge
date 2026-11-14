#include "wal_vfs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace tds::storage
{
namespace
{

// unix seekAndWriteFd masks its count to 17 bits, so exactly 128 KiB would become zero
constexpr std::size_t buffer_capacity = 0x1ffff;
constexpr sqlite3_int64 wal_header_size = 32;
constexpr std::uint32_t frame_header_size = 24;

struct File
{
    sqlite3_file base{};
    sqlite3_file *parent = nullptr;
    unsigned char *buffer = nullptr;
    sqlite3_int64 offset = 0;
    sqlite3_int64 next_offset = 0;
    sqlite3_int64 end = 0;
    std::size_t used = 0;
    std::uint32_t page_size = 0;
    std::uint32_t remaining = 0;
    int error = SQLITE_OK;
    bool commit = false;
    bool passthrough = false;
};

constexpr auto parent_offset = (sizeof(File) + alignof(File) - 1) & ~(alignof(File) - 1);

File &file(sqlite3_file *handle) noexcept
{
    return *reinterpret_cast<File *>(handle);
}

sqlite3_vfs &parent(sqlite3_vfs *vfs) noexcept
{
    return *static_cast<sqlite3_vfs *>(vfs->pAppData);
}

std::uint32_t big_endian(const unsigned char *bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

std::uint32_t page_size(const unsigned char *header) noexcept
{
    const auto magic = big_endian(header);
    const auto size = big_endian(header + 8);
    return (magic == 0x377f0682 || magic == 0x377f0683) && size >= 512 && size <= 65536 &&
                   (size & (size - 1)) == 0
               ? size
               : 0;
}

int write_parent(File &owner, const void *bytes, int amount, sqlite3_int64 offset) noexcept
{
    const int result = owner.parent->pMethods->xWrite(owner.parent, bytes, amount, offset);
    if (result != SQLITE_OK)
    {
        owner.error = result;
    }
    else if (amount > 0 && offset >= 0 &&
             offset <= std::numeric_limits<sqlite3_int64>::max() - amount)
    {
        owner.end = std::max(owner.end, offset + amount);
    }
    return result;
}

int flush(File &owner) noexcept
{
    if (owner.error != SQLITE_OK || !owner.used)
    {
        return owner.error;
    }
    const auto amount = static_cast<int>(owner.used);
    // An error may follow a partial write, so this buffer must never be replayed
    owner.used = 0;
    return write_parent(owner, owner.buffer, amount, owner.offset);
}

int close(sqlite3_file *handle) noexcept
{
    auto &owner = file(handle);
    const int flushed = flush(owner);
    const int closed = owner.parent->pMethods->xClose(owner.parent);
    sqlite3_free(owner.buffer);
    owner.base.pMethods = nullptr;
    owner.~File();
    return flushed != SQLITE_OK ? flushed : closed;
}

int read(sqlite3_file *handle, void *bytes, int amount, sqlite3_int64 offset) noexcept
{
    auto &owner = file(handle);
    const int result = flush(owner);
    return result == SQLITE_OK ? owner.parent->pMethods->xRead(owner.parent, bytes, amount, offset)
                               : result;
}

int write(sqlite3_file *handle, const void *data, int amount, sqlite3_int64 offset) noexcept
{
    auto &owner = file(handle);
    if (owner.error != SQLITE_OK)
    {
        return owner.error;
    }
    if (owner.passthrough)
    {
        return write_parent(owner, data, amount, offset);
    }
    if (amount <= 0 || offset < 0 || offset > std::numeric_limits<sqlite3_int64>::max() - amount)
    {
        const int result = flush(owner);
        owner.remaining = 0;
        return result == SQLITE_OK ? write_parent(owner, data, amount, offset) : result;
    }
    const auto *bytes = static_cast<const unsigned char *>(data);
    if (offset == 0)
    {
        const int result = flush(owner);
        if (result != SQLITE_OK)
        {
            return result;
        }
        owner.remaining = 0;
        owner.page_size = 0;
        const int written = write_parent(owner, data, amount, offset);
        if (written == SQLITE_OK && amount == wal_header_size)
        {
            owner.page_size = page_size(bytes);
        }
        return written;
    }
    if (!owner.page_size)
    {
        std::array<unsigned char, wal_header_size> header{};
        if (owner.parent->pMethods->xRead(owner.parent, header.data(), wal_header_size, 0) == SQLITE_OK)
        {
            owner.page_size = page_size(header.data());
        }
    }
    if (offset != owner.next_offset)
    {
        const int result = flush(owner);
        if (result != SQLITE_OK)
        {
            return result;
        }
        owner.remaining = 0;
    }
    const auto frame_size = static_cast<sqlite3_int64>(owner.page_size) + frame_header_size;
    // Checksum repair rewrites headers without bodies, so only append headers can wait for data
    const bool header = owner.page_size && offset >= owner.end && offset >= wal_header_size &&
                        (offset - wal_header_size) % frame_size == 0 &&
                        amount == static_cast<int>(frame_header_size);
    const bool body = owner.remaining && offset == owner.next_offset && amount > 0 &&
                      static_cast<unsigned>(amount) <= owner.remaining;
    if (!header && !body)
    {
        const int result = flush(owner);
        owner.remaining = 0;
        return result == SQLITE_OK ? write_parent(owner, data, amount, offset) : result;
    }
    if (!owner.buffer)
    {
        owner.buffer = static_cast<unsigned char *>(sqlite3_malloc64(buffer_capacity));
        if (!owner.buffer)
        {
            owner.passthrough = true;
            owner.remaining = 0;
            return write_parent(owner, data, amount, offset);
        }
    }
    if (owner.used + static_cast<std::size_t>(amount) > buffer_capacity)
    {
        const int result = flush(owner);
        if (result != SQLITE_OK)
        {
            return result;
        }
    }
    if (!owner.used)
    {
        owner.offset = offset;
    }
    std::memcpy(owner.buffer + owner.used, data, static_cast<std::size_t>(amount));
    owner.used += static_cast<std::size_t>(amount);
    owner.next_offset = offset + amount;
    owner.end = std::max(owner.end, owner.next_offset);
    if (header)
    {
        owner.remaining = owner.page_size;
        owner.commit = big_endian(bytes + 4) != 0;
    }
    else
    {
        owner.remaining -= static_cast<unsigned>(amount);
    }
    // NORMAL does not call xSync at each commit, so the final frame publishes its own bytes
    return !owner.remaining && owner.commit ? flush(owner) : SQLITE_OK;
}

int truncate(sqlite3_file *handle, sqlite3_int64 size) noexcept
{
    auto &owner = file(handle);
    const int result = flush(owner);
    if (result != SQLITE_OK)
    {
        return result;
    }
    owner.remaining = 0;
    owner.page_size = 0;
    const int truncated = owner.parent->pMethods->xTruncate(owner.parent, size);
    if (truncated == SQLITE_OK)
    {
        owner.end = size;
    }
    return truncated;
}

int sync(sqlite3_file *handle, int flags) noexcept
{
    auto &owner = file(handle);
    const int result = flush(owner);
    return result == SQLITE_OK ? owner.parent->pMethods->xSync(owner.parent, flags) : result;
}

int file_size(sqlite3_file *handle, sqlite3_int64 *size) noexcept
{
    auto &owner = file(handle);
    const int result = flush(owner);
    return result == SQLITE_OK ? owner.parent->pMethods->xFileSize(owner.parent, size) : result;
}

int lock(sqlite3_file *handle, int level) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xLock(owner.parent, level);
}

int unlock(sqlite3_file *handle, int level) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xUnlock(owner.parent, level);
}

int check_reserved(sqlite3_file *handle, int *reserved) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xCheckReservedLock(owner.parent, reserved);
}

int file_control(sqlite3_file *handle, int operation, void *argument) noexcept
{
    auto &owner = file(handle);
    const int result = flush(owner);
    return result == SQLITE_OK
               ? owner.parent->pMethods->xFileControl(owner.parent, operation, argument)
               : result;
}

int sector_size(sqlite3_file *handle) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xSectorSize(owner.parent);
}

int device_characteristics(sqlite3_file *handle) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xDeviceCharacteristics(owner.parent);
}

int shm_map(sqlite3_file *handle, int page, int size, int extend, void volatile **output) noexcept
{
    auto &owner = file(handle);
    if (!owner.parent->pMethods->xShmMap)
    {
        *output = nullptr;
        return SQLITE_IOERR_SHMMAP;
    }
    return owner.parent->pMethods->xShmMap(owner.parent, page, size, extend, output);
}

int shm_lock(sqlite3_file *handle, int offset, int count, int flags) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xShmLock
               ? owner.parent->pMethods->xShmLock(owner.parent, offset, count, flags)
               : SQLITE_IOERR_SHMLOCK;
}

void shm_barrier(sqlite3_file *handle) noexcept
{
    auto &owner = file(handle);
    if (owner.parent->pMethods->xShmBarrier)
    {
        owner.parent->pMethods->xShmBarrier(owner.parent);
    }
}

int shm_unmap(sqlite3_file *handle, int remove) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xShmUnmap ? owner.parent->pMethods->xShmUnmap(owner.parent, remove)
                                           : SQLITE_OK;
}

int fetch(sqlite3_file *handle, sqlite3_int64 offset, int amount, void **output) noexcept
{
    auto &owner = file(handle);
    const int result = flush(owner);
    if (result == SQLITE_OK && !owner.parent->pMethods->xFetch)
    {
        *output = nullptr;
        return SQLITE_OK;
    }
    return result == SQLITE_OK ? owner.parent->pMethods->xFetch(owner.parent, offset, amount, output)
                               : result;
}

int unfetch(sqlite3_file *handle, sqlite3_int64 offset, void *data) noexcept
{
    auto &owner = file(handle);
    return owner.parent->pMethods->xUnfetch
               ? owner.parent->pMethods->xUnfetch(owner.parent, offset, data)
               : SQLITE_OK;
}

constexpr sqlite3_io_methods methods(int version) noexcept
{
    return {version, close, read, write, truncate, sync, file_size, lock, unlock, check_reserved,
            file_control, sector_size, device_characteristics,
            version >= 2 ? shm_map : nullptr, version >= 2 ? shm_lock : nullptr,
            version >= 2 ? shm_barrier : nullptr, version >= 2 ? shm_unmap : nullptr,
            version >= 3 ? fetch : nullptr, version >= 3 ? unfetch : nullptr};
}

constexpr std::array io_methods{methods(1), methods(2), methods(3)};

int open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *handle, int flags,
         int *output_flags) noexcept
{
    if (!(flags & SQLITE_OPEN_WAL))
    {
        return parent(vfs).xOpen(&parent(vfs), name, handle, flags, output_flags);
    }
    auto &owner = *new (handle) File{};
    owner.parent = reinterpret_cast<sqlite3_file *>(reinterpret_cast<unsigned char *>(handle) +
                                                   parent_offset);
    owner.parent->pMethods = nullptr;
    const int result = parent(vfs).xOpen(&parent(vfs), name, owner.parent, flags, output_flags);
    if (result != SQLITE_OK)
    {
        if (owner.parent->pMethods)
        {
            owner.parent->pMethods->xClose(owner.parent);
        }
        owner.~File();
        return result;
    }
    if (owner.parent->pMethods)
    {
        const auto version = std::clamp(owner.parent->pMethods->iVersion, 1, 3);
        owner.base.pMethods = &io_methods[static_cast<std::size_t>(version - 1)];
        if (owner.parent->pMethods->xFileSize(owner.parent, &owner.end) != SQLITE_OK)
        {
            owner.passthrough = true;
        }
    }
    return result;
}

int remove(sqlite3_vfs *vfs, const char *name, int sync_directory) noexcept
{
    return parent(vfs).xDelete(&parent(vfs), name, sync_directory);
}

int access(sqlite3_vfs *vfs, const char *name, int flags, int *result) noexcept
{
    return parent(vfs).xAccess(&parent(vfs), name, flags, result);
}

int full_path(sqlite3_vfs *vfs, const char *name, int size, char *output) noexcept
{
    return parent(vfs).xFullPathname(&parent(vfs), name, size, output);
}

void *dl_open(sqlite3_vfs *vfs, const char *name) noexcept
{
    return parent(vfs).xDlOpen(&parent(vfs), name);
}

void dl_error(sqlite3_vfs *vfs, int size, char *output) noexcept
{
    parent(vfs).xDlError(&parent(vfs), size, output);
}

auto dl_symbol(sqlite3_vfs *vfs, void *library, const char *name) noexcept -> void (*)(void)
{
    return parent(vfs).xDlSym(&parent(vfs), library, name);
}

void dl_close(sqlite3_vfs *vfs, void *library) noexcept
{
    parent(vfs).xDlClose(&parent(vfs), library);
}

int randomness(sqlite3_vfs *vfs, int size, char *output) noexcept
{
    return parent(vfs).xRandomness(&parent(vfs), size, output);
}

int sleep(sqlite3_vfs *vfs, int microseconds) noexcept
{
    return parent(vfs).xSleep(&parent(vfs), microseconds);
}

int current_time(sqlite3_vfs *vfs, double *output) noexcept
{
    return parent(vfs).xCurrentTime(&parent(vfs), output);
}

int last_error(sqlite3_vfs *vfs, int size, char *output) noexcept
{
    return parent(vfs).xGetLastError(&parent(vfs), size, output);
}

int current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *output) noexcept
{
    return parent(vfs).xCurrentTimeInt64(&parent(vfs), output);
}

int set_system_call(sqlite3_vfs *vfs, const char *name, sqlite3_syscall_ptr callback) noexcept
{
    return parent(vfs).xSetSystemCall(&parent(vfs), name, callback);
}

sqlite3_syscall_ptr get_system_call(sqlite3_vfs *vfs, const char *name) noexcept
{
    return parent(vfs).xGetSystemCall(&parent(vfs), name);
}

const char *next_system_call(sqlite3_vfs *vfs, const char *name) noexcept
{
    return parent(vfs).xNextSystemCall(&parent(vfs), name);
}

} // namespace

WalVfs::WalVfs(sqlite3_vfs &underlying, const char *stable_name) : native_(underlying)
{
    if (underlying.szOsFile < 0 ||
        static_cast<std::size_t>(underlying.szOsFile) >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) - parent_offset)
    {
        throw std::invalid_argument("parent VFS file size is out of range");
    }
    native_.iVersion = std::min(underlying.iVersion, 3);
    native_.szOsFile = static_cast<int>(parent_offset) + underlying.szOsFile;
    native_.pNext = nullptr;
    native_.zName = stable_name;
    native_.pAppData = &underlying;
    native_.xOpen = open;
    native_.xDelete = remove;
    native_.xAccess = access;
    native_.xFullPathname = full_path;
    native_.xDlOpen = underlying.xDlOpen ? dl_open : nullptr;
    native_.xDlError = underlying.xDlError ? dl_error : nullptr;
    native_.xDlSym = underlying.xDlSym ? dl_symbol : nullptr;
    native_.xDlClose = underlying.xDlClose ? dl_close : nullptr;
    native_.xRandomness = randomness;
    native_.xSleep = sleep;
    native_.xCurrentTime = current_time;
    native_.xGetLastError = underlying.xGetLastError ? last_error : nullptr;
    native_.xCurrentTimeInt64 = underlying.iVersion >= 2 && underlying.xCurrentTimeInt64
                                  ? current_time_int64
                                  : nullptr;
    native_.xSetSystemCall = underlying.iVersion >= 3 && underlying.xSetSystemCall
                                ? set_system_call
                                : nullptr;
    native_.xGetSystemCall = underlying.iVersion >= 3 && underlying.xGetSystemCall
                                ? get_system_call
                                : nullptr;
    native_.xNextSystemCall = underlying.iVersion >= 3 && underlying.xNextSystemCall
                                 ? next_system_call
                                 : nullptr;
    if (sqlite3_vfs_register(&native_, 0) != SQLITE_OK)
    {
        throw std::runtime_error("register WAL VFS failed");
    }
}

WalVfs::~WalVfs()
{
    sqlite3_vfs_unregister(&native_);
}

sqlite3_vfs &WalVfs::native() noexcept
{
    return native_;
}

const char *WalVfs::name() const noexcept
{
    return native_.zName;
}

const char *wal_vfs_name()
{
    static WalVfs adapter([]() -> sqlite3_vfs & {
        auto *underlying = sqlite3_vfs_find("unix-excl");
        if (!underlying)
        {
            throw std::runtime_error("unix-excl VFS is unavailable");
        }
        return *underlying;
    }(), "tds-wal");
    return adapter.name();
}

sqlite3_file *unwrap(sqlite3_file *handle) noexcept
{
    if (handle)
    {
        for (const auto &entry : io_methods)
        {
            if (handle->pMethods == &entry)
            {
                return file(handle).parent;
            }
        }
    }
    return handle;
}

} // namespace tds::storage

extern "C" sqlite3_file *tds_wal_unwrap(sqlite3_file *handle)
{
    return tds::storage::unwrap(handle);
}
