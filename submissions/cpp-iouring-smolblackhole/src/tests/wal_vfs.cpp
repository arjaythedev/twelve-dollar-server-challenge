#include "storage/wal_vfs.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

using Bytes = std::vector<unsigned char>;

enum class Kind
{
    write,
    read,
    size,
    sync,
    truncate,
    control,
    fetch,
    close
};

struct Event
{
    Kind kind;
    sqlite3_int64 offset = 0;
    Bytes bytes;
};

struct FakeState
{
    Bytes bytes;
    std::vector<Event> events;
    int write_result = SQLITE_OK;
    std::size_t partial_bytes = 0;
    int close_result = SQLITE_OK;
    int open_result = SQLITE_OK;
    bool optional_fetch = true;
};

struct FakeFile
{
    sqlite3_file base;
    FakeState *state;
};

FakeState &state(sqlite3_file *file)
{
    return *reinterpret_cast<FakeFile *>(file)->state;
}

int fake_close(sqlite3_file *file)
{
    auto &owner = state(file);
    owner.events.push_back({Kind::close, 0, {}});
    file->pMethods = nullptr;
    return owner.close_result;
}

int fake_read(sqlite3_file *file, void *output, int length, sqlite3_int64 offset)
{
    auto &owner = state(file);
    owner.events.push_back({Kind::read, offset, {}});
    const auto start = static_cast<std::size_t>(offset);
    const auto size = static_cast<std::size_t>(length);
    const auto available = start < owner.bytes.size() ? owner.bytes.size() - start : 0;
    const auto copied = std::min(size, available);
    std::memset(output, 0, size);
    if (copied)
    {
        std::memcpy(output, owner.bytes.data() + start, copied);
    }
    return copied == size ? SQLITE_OK : SQLITE_IOERR_SHORT_READ;
}

int fake_write(sqlite3_file *file, const void *input, int length, sqlite3_int64 offset)
{
    auto &owner = state(file);
    const auto *bytes = static_cast<const unsigned char *>(input);
    const auto size = static_cast<std::size_t>(length);
    owner.events.push_back({Kind::write, offset, Bytes(bytes, bytes + size)});
    const auto copied = owner.write_result == SQLITE_OK ? size : std::min(size, owner.partial_bytes);
    const auto start = static_cast<std::size_t>(offset);
    if (copied)
    {
        owner.bytes.resize(std::max(owner.bytes.size(), start + copied));
        std::copy_n(bytes, copied, owner.bytes.begin() + static_cast<std::ptrdiff_t>(start));
    }
    return owner.write_result;
}

int fake_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    auto &owner = state(file);
    owner.events.push_back({Kind::truncate, size, {}});
    owner.bytes.resize(static_cast<std::size_t>(size));
    return SQLITE_OK;
}

int fake_sync(sqlite3_file *file, int)
{
    state(file).events.push_back({Kind::sync, 0, {}});
    return SQLITE_OK;
}

int fake_size(sqlite3_file *file, sqlite3_int64 *size)
{
    auto &owner = state(file);
    owner.events.push_back({Kind::size, 0, {}});
    *size = static_cast<sqlite3_int64>(owner.bytes.size());
    return SQLITE_OK;
}

int fake_lock(sqlite3_file *, int)
{
    return SQLITE_OK;
}

int fake_reserved(sqlite3_file *, int *reserved)
{
    *reserved = 0;
    return SQLITE_OK;
}

int fake_control(sqlite3_file *file, int, void *)
{
    state(file).events.push_back({Kind::control, 0, {}});
    return SQLITE_NOTFOUND;
}

int fake_sector(sqlite3_file *)
{
    return 4096;
}

int fake_characteristics(sqlite3_file *)
{
    return 0;
}

int fake_map(sqlite3_file *, int, int, int, void volatile **output)
{
    *output = nullptr;
    return SQLITE_OK;
}

int fake_shm_lock(sqlite3_file *, int, int, int)
{
    return SQLITE_OK;
}

void fake_barrier(sqlite3_file *)
{
}

int fake_unmap(sqlite3_file *, int)
{
    return SQLITE_OK;
}

int fake_fetch(sqlite3_file *file, sqlite3_int64 offset, int, void **output)
{
    state(file).events.push_back({Kind::fetch, offset, {}});
    *output = nullptr;
    return SQLITE_OK;
}

int fake_unfetch(sqlite3_file *, sqlite3_int64, void *)
{
    return SQLITE_OK;
}

const sqlite3_io_methods fake_methods{3,
                                     fake_close,
                                     fake_read,
                                     fake_write,
                                     fake_truncate,
                                     fake_sync,
                                     fake_size,
                                     fake_lock,
                                     fake_lock,
                                     fake_reserved,
                                     fake_control,
                                     fake_sector,
                                     fake_characteristics,
                                     fake_map,
                                     fake_shm_lock,
                                     fake_barrier,
                                     fake_unmap,
                                     fake_fetch,
                                     fake_unfetch};

const sqlite3_io_methods fake_nullable_methods = [] {
    auto methods = fake_methods;
    methods.xFetch = nullptr;
    methods.xUnfetch = nullptr;
    return methods;
}();

int fake_open(sqlite3_vfs *vfs, const char *, sqlite3_file *file, int flags, int *output_flags)
{
    auto &opened = *reinterpret_cast<FakeFile *>(file);
    opened.state = static_cast<FakeState *>(vfs->pAppData);
    opened.base.pMethods = opened.state->optional_fetch ? &fake_methods : &fake_nullable_methods;
    if (output_flags)
    {
        *output_flags = flags;
    }
    return opened.state->open_result;
}

class File
{
  public:
    explicit File(sqlite3_vfs &vfs, int kind = SQLITE_OPEN_WAL)
        : storage_((static_cast<std::size_t>(vfs.szOsFile) + sizeof(std::max_align_t) - 1) /
                   sizeof(std::max_align_t))
    {
        require(vfs.xOpen(&vfs, "fixture-wal", get(),
                          kind | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK,
                "open wrapped fake file");
    }

    ~File()
    {
        if (opened_)
        {
            close();
        }
    }

    sqlite3_file *get()
    {
        return reinterpret_cast<sqlite3_file *>(storage_.data());
    }

    int write(const Bytes &bytes, sqlite3_int64 offset)
    {
        return get()->pMethods->xWrite(get(), bytes.data(), static_cast<int>(bytes.size()), offset);
    }

    int close()
    {
        opened_ = false;
        return get()->pMethods->xClose(get());
    }

  private:
    std::vector<std::max_align_t> storage_;
    bool opened_ = true;
};

struct Fake
{
    FakeState data;
    sqlite3_vfs parent{};
    std::unique_ptr<tds::storage::WalVfs> adapter;

    Fake()
    {
        parent.iVersion = 3;
        parent.szOsFile = static_cast<int>(sizeof(FakeFile));
        parent.mxPathname = 1024;
        parent.zName = "fake-wal-parent";
        parent.pAppData = &data;
        parent.xOpen = fake_open;
        adapter = std::make_unique<tds::storage::WalVfs>(parent, "fake-wal-adapter");
    }
};

void big_endian(Bytes &bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
    {
        bytes[offset + index] = static_cast<unsigned char>(value >> (24U - index * 8U));
    }
}

Bytes wal_header(std::uint32_t page_size)
{
    Bytes bytes(32);
    big_endian(bytes, 0, 0x377f0682);
    big_endian(bytes, 4, 3007000);
    big_endian(bytes, 8, page_size);
    return bytes;
}

Bytes frame_header(std::uint32_t page, std::uint32_t committed_pages)
{
    Bytes bytes(24);
    big_endian(bytes, 0, page);
    big_endian(bytes, 4, committed_pages);
    return bytes;
}

Bytes joined(const Bytes &first, const Bytes &second)
{
    auto result = first;
    result.insert(result.end(), second.begin(), second.end());
    return result;
}

void start_wal(Fake &fixture, File &file, std::uint32_t page_size = 4096)
{
    require(file.write(wal_header(page_size), 0) == SQLITE_OK &&
                file.get()->pMethods->xSync(file.get(), SQLITE_SYNC_NORMAL) == SQLITE_OK,
            "start fake WAL");
    fixture.data.events.clear();
}

void expect_write(const Event &event, sqlite3_int64 offset, const Bytes &bytes)
{
    require(event.kind == Kind::write && event.offset == offset && event.bytes == bytes,
            "coalescing changed parent write bytes, offset or ordering");
}

void frame_commit_visibility()
{
    Fake fixture;
    File file(fixture.adapter->native());
    start_wal(fixture, file);
    const Bytes first_page(4096, 0x31);
    const Bytes second_page(4096, 0x72);
    const auto first = frame_header(1, 0);
    const auto commit = frame_header(2, 2);
    require(file.write(first, 32) == SQLITE_OK && file.write(first_page, 56) == SQLITE_OK &&
                file.write(commit, 4152) == SQLITE_OK,
            "append fake transaction frames");
    require(fixture.data.events.empty(), "noncommitting frame escaped the coalescing buffer");
    const Bytes prefix(second_page.begin(), second_page.begin() + 1000);
    const Bytes suffix(second_page.begin() + 1000, second_page.end());
    require(file.write(prefix, 4176) == SQLITE_OK && fixture.data.events.empty(),
            "commit header flushed before its complete page body");
    require(file.write(suffix, 5176) == SQLITE_OK && fixture.data.events.size() == 1,
            "NORMAL commit did not flush before the final page-piece write returned");
    expect_write(fixture.data.events[0], 32,
                 joined(joined(first, first_page), joined(commit, second_page)));
    require(std::equal(second_page.begin(), second_page.end(), fixture.data.bytes.begin() + 4176),
            "parent cannot read the complete committed frame without xSync");
    require(tds_wal_unwrap(file.get()) != file.get(), "WAL wrapper did not expose its parent");
    require(tds_wal_unwrap(tds_wal_unwrap(file.get())) == tds_wal_unwrap(file.get()),
            "unwrapping a native parent changed its identity");
}

void noncontiguous_commit_frame()
{
    for (const bool preceding_frame : {false, true})
    {
        Fake fixture;
        File file(fixture.adapter->native());
        start_wal(fixture, file);
        const Bytes page(4096, 0x5a);
        if (preceding_frame)
        {
            require(file.write(frame_header(1, 0), 32) == SQLITE_OK &&
                        file.write(page, 56) == SQLITE_OK,
                    "queue frame before offset discontinuity");
        }
        constexpr sqlite3_int64 offset = 32 + 5 * (24 + 4096);
        const auto commit = frame_header(6, 6);
        require(file.write(commit, offset) == SQLITE_OK && file.write(page, offset + 24) == SQLITE_OK,
                "append noncontiguous commit frame");
        require(fixture.data.events.size() == (preceding_frame ? 2U : 1U),
                "first noncontiguous frame failed to recognize its commit marker");
        if (preceding_frame)
        {
            expect_write(fixture.data.events.front(), 32, joined(frame_header(1, 0), page));
        }
        expect_write(fixture.data.events.back(), offset, joined(commit, page));
    }
}

void existing_header_rewrite()
{
    Fake fixture;
    const Bytes page(4096, 0x63);
    fixture.data.bytes = joined(wal_header(4096), joined(frame_header(1, 0), page));
    File file(fixture.adapter->native());
    fixture.data.events.clear();
    const auto rewritten = frame_header(1, 1);
    require(file.write(rewritten, 32) == SQLITE_OK && !fixture.data.events.empty() &&
                std::count_if(fixture.data.events.begin(), fixture.data.events.end(),
                              [](const Event &event) { return event.kind == Kind::write; }) == 1,
            "existing commit header rewrite waited for a page body that SQLite never writes");
    expect_write(fixture.data.events.back(), 32, rewritten);
    require(std::equal(rewritten.begin(), rewritten.end(), fixture.data.bytes.begin() + 32) &&
                std::equal(page.begin(), page.end(), fixture.data.bytes.begin() + 56),
            "existing header-only rewrite lost its commit marker or changed the page");
}

void open_failure_and_optional_methods()
{
    {
        Fake fixture;
        fixture.data.open_result = SQLITE_CANTOPEN;
        auto &vfs = fixture.adapter->native();
        std::vector<std::max_align_t> storage(
            (static_cast<std::size_t>(vfs.szOsFile) + sizeof(std::max_align_t) - 1) /
            sizeof(std::max_align_t));
        auto *file = reinterpret_cast<sqlite3_file *>(storage.data());
        require(vfs.xOpen(&vfs, "failed-wal", file, SQLITE_OPEN_WAL | SQLITE_OPEN_READWRITE,
                          nullptr) == SQLITE_CANTOPEN && file->pMethods == nullptr,
                "failed parent open exposed a usable outer file");
        require(fixture.data.events.size() == 1 && fixture.data.events.front().kind == Kind::close,
                "failed parent open retained a partially opened native file");
    }
    {
        Fake fixture;
        fixture.data.optional_fetch = false;
        File file(fixture.adapter->native());
        start_wal(fixture, file);
        auto *raw = file.get();
        if (raw->pMethods->xFetch)
        {
            void *mapping = reinterpret_cast<void *>(1);
            require(raw->pMethods->xFetch(raw, 0, 1, &mapping) == SQLITE_OK && mapping == nullptr,
                    "absent optional parent fetch did not return an unmapped result");
        }
        if (raw->pMethods->xUnfetch)
        {
            require(raw->pMethods->xUnfetch(raw, 0, nullptr) == SQLITE_OK,
                    "absent optional parent unfetch was not a no-op");
        }
        require(file.close() == SQLITE_OK, "nullable version-3 methods prevented parent close");
    }
}

void flush_boundaries()
{
    constexpr std::array boundaries{Kind::read, Kind::size, Kind::sync, Kind::truncate,
                                     Kind::control, Kind::fetch, Kind::close};
    for (const auto boundary : boundaries)
    {
        Fake fixture;
        File file(fixture.adapter->native());
        start_wal(fixture, file);
        const auto frame = joined(frame_header(1, 0), Bytes(4096, 0x49));
        require(file.write(frame_header(1, 0), 32) == SQLITE_OK &&
                    file.write(Bytes(4096, 0x49), 56) == SQLITE_OK && fixture.data.events.empty(),
                "boundary fixture did not retain an uncommitted frame");
        auto *raw = file.get();
        int result = SQLITE_ERROR;
        switch (boundary)
        {
        case Kind::read:
        {
            Bytes output(frame.size());
            result = raw->pMethods->xRead(raw, output.data(), static_cast<int>(output.size()), 32);
            require(output == frame, "read boundary returned stale buffered bytes");
            break;
        }
        case Kind::size:
        {
            sqlite3_int64 size = 0;
            result = raw->pMethods->xFileSize(raw, &size);
            require(size == 32 + static_cast<sqlite3_int64>(frame.size()),
                    "file size boundary missed buffered bytes");
            break;
        }
        case Kind::sync:
            result = raw->pMethods->xSync(raw, SQLITE_SYNC_NORMAL);
            break;
        case Kind::truncate:
            result = raw->pMethods->xTruncate(raw, 32);
            break;
        case Kind::control:
            result = raw->pMethods->xFileControl(raw, 10000, nullptr);
            require(result == SQLITE_NOTFOUND, "file control changed its parent's result");
            result = SQLITE_OK;
            break;
        case Kind::fetch:
        {
            void *output = reinterpret_cast<void *>(1);
            result = raw->pMethods->xFetch(raw, 32, 1, &output);
            require(output == nullptr, "fetch changed its parent's mapping result");
            break;
        }
        case Kind::close:
            result = file.close();
            break;
        case Kind::write:
            break;
        }
        require(result == SQLITE_OK && fixture.data.events.size() == 2 &&
                    fixture.data.events.back().kind == boundary,
                "flush boundary did not run after exactly one parent write");
        expect_write(fixture.data.events.front(), 32, frame);
    }
}

void capacity_and_passthrough()
{
    Fake fixture;
    File file(fixture.adapter->native());
    start_wal(fixture, file, 65536);
    const Bytes page(65536, 0x92);
    const auto first = joined(frame_header(1, 0), page);
    const auto second = joined(frame_header(2, 2), page);
    const Bytes prefix(page.begin(), page.begin() + 65488);
    const Bytes suffix(page.begin() + 65488, page.end());
    require(file.write(frame_header(1, 0), 32) == SQLITE_OK && file.write(page, 56) == SQLITE_OK &&
                file.write(frame_header(2, 2), 65592) == SQLITE_OK &&
                file.write(prefix, 65616) == SQLITE_OK && file.write(suffix, 131104) == SQLITE_OK,
            "large-page transaction crossed the bounded buffer");
    const auto expected = joined(first, second);
    Bytes written;
    sqlite3_int64 offset = 32;
    require(fixture.data.events.size() >= 2, "safe unix write capacity did not bound large-page writes");
    for (const auto &event : fixture.data.events)
    {
        require(event.kind == Kind::write && event.offset == offset &&
                    event.bytes.size() <= 0x1ffff,
                "capacity flush broke contiguity or exceeded its buffer");
        written.insert(written.end(), event.bytes.begin(), event.bytes.end());
        offset += static_cast<sqlite3_int64>(event.bytes.size());
    }
    require(written == expected, "capacity flush lost or repeated a large-page byte");
    fixture.data.events.clear();
    const Bytes oversized(200000, 0xb4);
    require(file.write(oversized, 300001) == SQLITE_OK && fixture.data.events.size() == 1,
            "oversized unknown write did not pass through directly");
    expect_write(fixture.data.events.front(), 300001, oversized);

    Fake ordinary;
    File database(ordinary.adapter->native(), SQLITE_OPEN_MAIN_DB);
    require(database.write(page, 7) == SQLITE_OK && ordinary.data.events.size() == 1,
            "non-WAL database file was buffered");
    expect_write(ordinary.data.events.front(), 7, page);
}

void errors_never_replay()
{
    for (const std::size_t partial : {0U, 17U})
    {
        Fake fixture;
        File file(fixture.adapter->native());
        start_wal(fixture, file);
        fixture.data.write_result = SQLITE_IOERR_WRITE;
        fixture.data.partial_bytes = partial;
        require(file.write(frame_header(1, 1), 32) == SQLITE_OK &&
                    file.write(Bytes(4096, 0xe1), 56) == SQLITE_IOERR_WRITE,
                "commit hid an underlying failed or partial write");
        require(fixture.data.events.size() == 1, "failed write did not reach its parent once");
        fixture.data.write_result = SQLITE_OK;
        auto *raw = file.get();
        require(file.write(Bytes(4, 0xff), 0) == SQLITE_IOERR_WRITE &&
                    raw->pMethods->xSync(raw, SQLITE_SYNC_NORMAL) == SQLITE_IOERR_WRITE &&
                    fixture.data.events.size() == 1,
                "later write or flush retried a partially failed buffered run");
        fixture.data.close_result = SQLITE_IOERR_CLOSE;
        require(file.close() == SQLITE_IOERR_WRITE && fixture.data.events.size() == 2 &&
                    fixture.data.events.back().kind == Kind::close,
                "sticky write error skipped parent close or lost the original error");
    }
    Fake fixture;
    File file(fixture.adapter->native());
    fixture.data.events.clear();
    fixture.data.close_result = SQLITE_IOERR_CLOSE;
    require(file.close() == SQLITE_IOERR_CLOSE && fixture.data.events.size() == 1 &&
                fixture.data.events.front().kind == Kind::close,
            "parent close error was hidden or close ran more than once");
}

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

using Database = std::unique_ptr<sqlite3, Close>;

void sql(sqlite3 *db, const char *text)
{
    if (sqlite3_exec(db, text, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

Database open(const std::string &path, const char *vfs)
{
    sqlite3 *raw = nullptr;
    const int result = sqlite3_open_v2(path.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                       vfs);
    Database db(raw);
    require(result == SQLITE_OK, "open real WAL database");
    sql(raw, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;");
    return db;
}

std::int64_t scalar(sqlite3 *db, const char *text)
{
    sqlite3_stmt *raw = nullptr;
    require(sqlite3_prepare_v2(db, text, -1, &raw, nullptr) == SQLITE_OK, "prepare real WAL scalar");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    require(sqlite3_step(raw) == SQLITE_ROW, "real WAL scalar has no row");
    return sqlite3_column_int64(raw, 0);
}

class Directory
{
  public:
    Directory()
    {
        char pattern[] = "/tmp/td-wal-vfs-XXXXXX";
        const auto *created = ::mkdtemp(pattern);
        require(created != nullptr, "create WAL fixture directory");
        path = created;
    }

    ~Directory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::string path;
};

struct NativeFile
{
    const sqlite3_io_methods *original;
    sqlite3_io_methods methods;
};

class SyncProbe
{
  public:
    explicit SyncProbe(sqlite3_vfs &parent) : parent_(parent), native_(parent)
    {
        require(active_ == nullptr, "overlapping native WAL sync probes");
        active_ = this;
        native_.zName = "wal-sync-probe-parent";
        native_.pNext = nullptr;
        native_.xOpen = opened;
    }

    ~SyncProbe()
    {
        active_ = nullptr;
    }

    sqlite3_vfs &native()
    {
        return native_;
    }

    unsigned syncs = 0;

  private:
    static int opened(sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *output)
    {
        auto &owner = *active_;
        const int result = owner.parent_.xOpen(&owner.parent_, name, file, flags, output);
        if (result == SQLITE_OK && (flags & SQLITE_OPEN_WAL))
        {
            auto [entry, inserted] = owner.files_.emplace(
                file, NativeFile{file->pMethods, *file->pMethods});
            require(inserted, "duplicate native WAL sync probe file");
            entry->second.methods.xSync = synced;
            entry->second.methods.xClose = closed;
            file->pMethods = &entry->second.methods;
        }
        return result;
    }

    static int synced(sqlite3_file *file, int flags)
    {
        auto &owner = *active_;
        ++owner.syncs;
        return owner.files_.at(file).original->xSync(file, flags);
    }

    static int closed(sqlite3_file *file)
    {
        auto &owner = *active_;
        const auto *methods = owner.files_.at(file).original;
        const int result = methods->xClose(file);
        owner.files_.erase(file);
        return result;
    }

    static inline SyncProbe *active_ = nullptr;
    sqlite3_vfs &parent_;
    sqlite3_vfs native_;
    std::unordered_map<sqlite3_file *, NativeFile> files_;
};

void checkpoint(sqlite3 *db, int mode)
{
    int frames = -1;
    int completed = -1;
    require(sqlite3_wal_checkpoint_v2(db, "main", mode, &frames, &completed) == SQLITE_OK &&
                frames == completed,
            "real WAL checkpoint did not complete");
}

void real_sqlite_contract()
{
    auto *unix_vfs = sqlite3_vfs_find("unix-excl");
    require(unix_vfs != nullptr, "unix-excl VFS unavailable");
    SyncProbe probe(*unix_vfs);
    tds::storage::WalVfs adapter(probe.native(), "real-wal-contract");
    Directory directory;
    const auto path = directory.path + "/feed.db";
    {
        auto writer = open(path, adapter.name());
        sql(writer.get(), "CREATE TABLE posts(id INTEGER PRIMARY KEY, body BLOB NOT NULL);"
                          "INSERT INTO posts VALUES(1,'seed');");
        auto reader = open(path, adapter.name());
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 1, "seed visibility");
        const auto syncs_before = probe.syncs;
        sql(writer.get(), "INSERT INTO posts VALUES(2,'normal-visible');");
        require(probe.syncs == syncs_before, "NORMAL existing-WAL commit unexpectedly called xSync");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 2,
                "second handle missed NORMAL commit without xSync");

        sql(reader.get(), "BEGIN;");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 2, "reader snapshot setup");
        sql(writer.get(), "BEGIN; INSERT INTO posts VALUES(3,'kept'); SAVEPOINT nested;"
                          "INSERT INTO posts VALUES(4,'discarded'); ROLLBACK TO nested;"
                          "RELEASE nested; COMMIT;");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 2,
                "writer commit changed an existing reader snapshot");
        sql(reader.get(), "COMMIT;");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 3 &&
                    scalar(reader.get(), "SELECT count(*) FROM posts WHERE id=4") == 0,
                "savepoint rollback leaked a discarded write");
        sql(writer.get(), "BEGIN; INSERT INTO posts VALUES(4,'rolled-back'); ROLLBACK;");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 3,
                "full rollback leaked a buffered transaction");

        sql(writer.get(), "PRAGMA cache_size=8; BEGIN;"
                          "WITH RECURSIVE ids(n) AS (VALUES(100) UNION ALL SELECT n+1 FROM ids"
                          " WHERE n<163) INSERT INTO posts SELECT n,zeroblob(8192) FROM ids; COMMIT;");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 67 &&
                    scalar(reader.get(), "SELECT sum(length(body)) FROM posts WHERE id>=100") ==
                        64 * 8192,
                "transaction larger than the coalescing buffer lost page data");
        checkpoint(writer.get(), SQLITE_CHECKPOINT_PASSIVE);
        checkpoint(writer.get(), SQLITE_CHECKPOINT_RESTART);
        sql(writer.get(), "INSERT INTO posts VALUES(4,'after-restart');");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 68,
                "WAL restart lost the next commit");
        checkpoint(writer.get(), SQLITE_CHECKPOINT_TRUNCATE);
        require(std::filesystem::file_size(path + "-wal") == 0, "truncate retained WAL bytes");
        sql(writer.get(), "INSERT INTO posts VALUES(5,'after-truncate');");
        require(scalar(reader.get(), "SELECT count(*) FROM posts") == 69,
                "WAL truncate lost the next commit");
    }
    {
        auto reopened = open(path, adapter.name());
        require(scalar(reopened.get(), "SELECT count(*) FROM posts") == 69 &&
                    scalar(reopened.get(), "SELECT sum(length(body)) FROM posts WHERE id>=100") ==
                        64 * 8192,
                "reopen lost committed rows or large-page data");
        require(scalar(reopened.get(), "SELECT count(*) FROM pragma_integrity_check WHERE"
                                       " integrity_check<>'ok'") == 0,
                "reopened database failed integrity_check");
    }
}
} // namespace

int main()
{
    try
    {
        frame_commit_visibility();
        noncontiguous_commit_frame();
        existing_header_rewrite();
        open_failure_and_optional_methods();
        flush_boundaries();
        capacity_and_passthrough();
        errors_never_replay();
        real_sqlite_contract();
        std::puts("WAL coalescing bytes/order, commit visibility, boundaries, capacity, sticky errors, "
                  "two-handle NORMAL commits, rollback, checkpoint/restart/truncate and reopen passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "WAL VFS checks failed: %s\n", error.what());
        return 1;
    }
}
