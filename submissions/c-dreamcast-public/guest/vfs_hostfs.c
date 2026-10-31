/* vfs_hostfs.c - a SQLite VFS whose files live on the host, reached through the hostfs syscall.
 *
 * SQLite is built with SQLITE_OS_OTHER, so this is the only VFS. The database is "main" ->
 * $SQLITE_PATH on the host, and its -wal / -journal siblings map to the same host names, so the
 * real file at $SQLITE_PATH is what we read and write. There is no shared-memory support (no
 * xShmMap), which SQLite allows for WAL only in EXCLUSIVE locking mode: the index lives in the
 * heap and nobody else may open the file while we run. Locks are no-ops for the same reason.
 * xSync is a real fsync on the host, so synchronous=NORMAL keeps its usual meaning. */
#include "hostfs.h"
#include "sqlite3.h"
#include <string.h>
#include <kos/thread.h>

typedef struct {
    sqlite3_file base;
    int fd;
} HFile;

static const char *suffix_of(const char *name) {
    if (name == 0) return 0;
    const char *dash = strrchr(name, '-');
    if (strcmp(name, "main") == 0) return "";
    if (dash && strncmp(name, "main", 4) == 0 && dash == name + 4 &&
        (strcmp(dash, "-wal") == 0 || strcmp(dash, "-journal") == 0))
        return dash;
    return 0;
}

static int hClose(sqlite3_file *f) {
    volatile uint32_t b[4] = {(uint32_t)((HFile *)f)->fd};
    hostfs(HFS_CLOSE, b);
    return SQLITE_OK;
}

static int hRead(sqlite3_file *f, void *buf, int n, sqlite3_int64 off) {
    volatile uint32_t b[4] = {(uint32_t)((HFile *)f)->fd, (uint32_t)buf, (uint32_t)n, (uint32_t)off};
    int32_t got = hostfs(HFS_PREAD, b);
    if (got < 0) return SQLITE_IOERR_READ;
    if (got < n) {
        memset((char *)buf + got, 0, n - got);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int hWrite(sqlite3_file *f, const void *buf, int n, sqlite3_int64 off) {
    while (n > 0) {
        volatile uint32_t b[4] = {(uint32_t)((HFile *)f)->fd, (uint32_t)buf, (uint32_t)n, (uint32_t)off};
        int32_t put = hostfs(HFS_PWRITE, b);
        if (put <= 0) return SQLITE_IOERR_WRITE;
        buf = (const char *)buf + put;
        n -= put;
        off += put;
    }
    return SQLITE_OK;
}

static int hTruncate(sqlite3_file *f, sqlite3_int64 size) {
    volatile uint32_t b[4] = {(uint32_t)((HFile *)f)->fd, (uint32_t)size};
    return hostfs(HFS_TRUNCATE, b) < 0 ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int hSync(sqlite3_file *f, int flags) {
    (void)flags;
    volatile uint32_t b[4] = {(uint32_t)((HFile *)f)->fd};
    return hostfs(HFS_SYNC, b) < 0 ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int hFileSize(sqlite3_file *f, sqlite3_int64 *size) {
    volatile uint32_t b[4] = {(uint32_t)((HFile *)f)->fd};
    int32_t s = hostfs(HFS_SIZE, b);
    if (s < 0) return SQLITE_IOERR_FSTAT;
    *size = s;
    return SQLITE_OK;
}

static int hLock(sqlite3_file *f, int l) { (void)f; (void)l; return SQLITE_OK; }
static int hCheckReservedLock(sqlite3_file *f, int *out) { (void)f; *out = 0; return SQLITE_OK; }
static int hFileControl(sqlite3_file *f, int op, void *arg) { (void)f; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int hSectorSize(sqlite3_file *f) { (void)f; return 4096; }
static int hDeviceCharacteristics(sqlite3_file *f) { (void)f; return SQLITE_IOCAP_SAFE_APPEND | SQLITE_IOCAP_POWERSAFE_OVERWRITE; }

static const sqlite3_io_methods hMethods = {
    1, hClose, hRead, hWrite, hTruncate, hSync, hFileSize, hLock, hLock,
    hCheckReservedLock, hFileControl, hSectorSize, hDeviceCharacteristics,
};

static int hOpen(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *f, int flags, int *outFlags) {
    (void)vfs;
    const char *suffix = suffix_of(name);
    if (suffix == 0) return SQLITE_CANTOPEN; /* temp files: SQLITE_TEMP_STORE=3 keeps them in RAM */
    volatile uint32_t b[4] = {(uint32_t)suffix, (flags & SQLITE_OPEN_CREATE) ? 1u : 0u};
    int32_t fd = hostfs(HFS_OPEN, b);
    if (fd < 0) return SQLITE_CANTOPEN;
    ((HFile *)f)->base.pMethods = &hMethods;
    ((HFile *)f)->fd = fd;
    if (outFlags) *outFlags = flags;
    return SQLITE_OK;
}

static int hDelete(sqlite3_vfs *vfs, const char *name, int syncDir) {
    (void)vfs; (void)syncDir;
    const char *suffix = suffix_of(name);
    if (suffix == 0) return SQLITE_IOERR_DELETE;
    volatile uint32_t b[4] = {(uint32_t)suffix};
    return hostfs(HFS_DELETE, b) < 0 ? SQLITE_IOERR_DELETE : SQLITE_OK;
}

static int hAccess(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    (void)vfs; (void)flags;
    const char *suffix = suffix_of(name);
    volatile uint32_t b[4] = {(uint32_t)suffix};
    *out = suffix != 0 && hostfs(HFS_ACCESS, b) == 1;
    return SQLITE_OK;
}

static int hFullPathname(sqlite3_vfs *vfs, const char *name, int n, char *out) {
    (void)vfs;
    sqlite3_snprintf(n, out, "%s", name);
    return SQLITE_OK;
}

static int hRandomness(sqlite3_vfs *vfs, int n, char *out) {
    (void)vfs;
    uint64_t x = hostfs_time_ms() * 6364136223846793005ull + 1442695040888963407ull;
    for (int i = 0; i < n; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        out[i] = (char)(x >> 56);
    }
    return n;
}

static int hSleep(sqlite3_vfs *vfs, int us) {
    (void)vfs;
    thd_sleep(us / 1000 + 1);
    return us;
}

static int hCurrentTimeInt64(sqlite3_vfs *vfs, sqlite3_int64 *out) {
    (void)vfs;
    *out = (sqlite3_int64)hostfs_time_ms() + 210866760000000LL; /* Julian day 0 in ms */
    return SQLITE_OK;
}

static int hCurrentTime(sqlite3_vfs *vfs, double *out) {
    sqlite3_int64 ms;
    hCurrentTimeInt64(vfs, &ms);
    *out = ms / 86400000.0;
    return SQLITE_OK;
}

static int hGetLastError(sqlite3_vfs *vfs, int n, char *out) { (void)vfs; (void)n; (void)out; return 0; }

static sqlite3_vfs hVfs = {
    2, sizeof(HFile), 64, 0, "hostfs", 0,
    hOpen, hDelete, hAccess, hFullPathname, 0, 0, 0, 0,
    hRandomness, hSleep, hCurrentTime, hGetLastError, hCurrentTimeInt64,
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&hVfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }
