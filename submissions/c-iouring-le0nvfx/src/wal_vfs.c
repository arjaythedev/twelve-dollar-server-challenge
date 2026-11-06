#include "vendor/sqlite-3.54.1/sqlite3.h"
#include "wal_vfs.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WAL_FRAME_HEADER_SIZE 24
#define WAL_WRITE_BUFFER_SIZE 0x1ffff

/*
 * SQLite appends each WAL frame as a 24-byte header followed by its page.
 * Keep adjacent writes together and flush after the page for a commit frame
 * (whose header has a nonzero database-size field). Other file operations
 * flush first, so the stock VFS observes the same ordering.
 */
typedef struct {
    sqlite3_file base;
    sqlite3_file *underlying;
    int is_wal;
    int commit_header_pending;
    int buffered_bytes;
    sqlite3_int64 buffered_offset;
    sqlite3_int64 last_write_end;
    unsigned char *buffer;
} BufferedFile;

static sqlite3_vfs *g_underlying_vfs;

#define FILE_ALIGNMENT ((size_t)_Alignof(max_align_t))
#define UNDERLYING_FILE_OFFSET \
    ((sizeof(BufferedFile) + FILE_ALIGNMENT - 1U) & ~(FILE_ALIGNMENT - 1U))
#define WRAPPED_FILE(p) ((BufferedFile *)(p))
#define UNDERLYING_FILE(p) (WRAPPED_FILE(p)->underlying)

static int flush_wal_buffer(BufferedFile *file) {
    if (file->buffered_bytes == 0) return SQLITE_OK;
    int bytes = file->buffered_bytes;
    file->buffered_bytes = 0;
    return file->underlying->pMethods->xWrite(file->underlying, file->buffer,
                                              bytes, file->buffered_offset);
}

static void remember_commit_header(BufferedFile *file, const void *buffer,
                                   int amount) {
    file->commit_header_pending = 0;
    if (amount != WAL_FRAME_HEADER_SIZE) return;
    const unsigned char *header = buffer;
    file->commit_header_pending =
        (header[4] | header[5] | header[6] | header[7]) != 0;
}

#define FLUSH_WAL_BUFFER(p)                                      \
    do {                                                         \
        int flush_rc_ = flush_wal_buffer(WRAPPED_FILE(p));       \
        if (flush_rc_ != SQLITE_OK) return flush_rc_;            \
    } while (0)

static int wrapped_close(sqlite3_file *p) {
    BufferedFile *file = WRAPPED_FILE(p);
    int flush_rc = flush_wal_buffer(file);
    int close_rc = SQLITE_OK;
    if (file->underlying->pMethods) {
        close_rc = file->underlying->pMethods->xClose(file->underlying);
    }
    sqlite3_free(file->buffer);
    file->buffer = NULL;
    return flush_rc != SQLITE_OK ? flush_rc : close_rc;
}

static int wrapped_read(sqlite3_file *p, void *buffer, int amount,
                        sqlite3_int64 offset) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xRead(UNDERLYING_FILE(p), buffer,
                                               amount, offset);
}

static int wrapped_write(sqlite3_file *p, const void *buffer, int amount,
                         sqlite3_int64 offset) {
    BufferedFile *file = WRAPPED_FILE(p);
    sqlite3_file *underlying = file->underlying;
    if (!file->is_wal) {
        return underlying->pMethods->xWrite(underlying, buffer, amount, offset);
    }
    if (amount < 0) return SQLITE_IOERR_WRITE;

    int contiguous = offset == file->last_write_end;
    file->last_write_end = offset + amount;

    if ((size_t)amount > WAL_WRITE_BUFFER_SIZE - (size_t)file->buffered_bytes) {
        FLUSH_WAL_BUFFER(p);
    }
    if (!contiguous || (size_t)amount > WAL_WRITE_BUFFER_SIZE) {
        FLUSH_WAL_BUFFER(p);
        remember_commit_header(file, buffer, amount);
        return underlying->pMethods->xWrite(underlying, buffer, amount, offset);
    }
    if (!file->buffer) {
        file->buffer = sqlite3_malloc(WAL_WRITE_BUFFER_SIZE);
        if (!file->buffer) {
            FLUSH_WAL_BUFFER(p);
            remember_commit_header(file, buffer, amount);
            return underlying->pMethods->xWrite(underlying, buffer, amount, offset);
        }
    }

    if (file->buffered_bytes == 0) file->buffered_offset = offset;
    memcpy(file->buffer + file->buffered_bytes, buffer, (size_t)amount);
    file->buffered_bytes += amount;

    if (amount == WAL_FRAME_HEADER_SIZE) {
        remember_commit_header(file, buffer, amount);
        return SQLITE_OK;
    }
    if (file->commit_header_pending) {
        file->commit_header_pending = 0;
        return flush_wal_buffer(file);
    }
    return SQLITE_OK;
}

static int wrapped_truncate(sqlite3_file *p, sqlite3_int64 size) {
    FLUSH_WAL_BUFFER(p);
    int rc = UNDERLYING_FILE(p)->pMethods->xTruncate(UNDERLYING_FILE(p), size);
    if (rc == SQLITE_OK && WRAPPED_FILE(p)->is_wal) {
        WRAPPED_FILE(p)->last_write_end = size;
        WRAPPED_FILE(p)->commit_header_pending = 0;
    }
    return rc;
}

static int wrapped_sync(sqlite3_file *p, int flags) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xSync(UNDERLYING_FILE(p), flags);
}

static int wrapped_file_size(sqlite3_file *p, sqlite3_int64 *size) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xFileSize(UNDERLYING_FILE(p), size);
}

static int wrapped_lock(sqlite3_file *p, int lock) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xLock(UNDERLYING_FILE(p), lock);
}

static int wrapped_unlock(sqlite3_file *p, int lock) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xUnlock(UNDERLYING_FILE(p), lock);
}

static int wrapped_check_reserved_lock(sqlite3_file *p, int *result) {
    return UNDERLYING_FILE(p)->pMethods->xCheckReservedLock(UNDERLYING_FILE(p),
                                                            result);
}

static int wrapped_file_control(sqlite3_file *p, int operation, void *argument) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xFileControl(UNDERLYING_FILE(p),
                                                       operation, argument);
}

static int wrapped_sector_size(sqlite3_file *p) {
    return UNDERLYING_FILE(p)->pMethods->xSectorSize(UNDERLYING_FILE(p));
}

static int wrapped_device_characteristics(sqlite3_file *p) {
    return UNDERLYING_FILE(p)->pMethods->xDeviceCharacteristics(
        UNDERLYING_FILE(p));
}

static int wrapped_shm_map(sqlite3_file *p, int page, int page_size, int extend,
                           void volatile **result) {
    return UNDERLYING_FILE(p)->pMethods->xShmMap(UNDERLYING_FILE(p), page,
                                                 page_size, extend, result);
}

static int wrapped_shm_lock(sqlite3_file *p, int offset, int count, int flags) {
    return UNDERLYING_FILE(p)->pMethods->xShmLock(UNDERLYING_FILE(p), offset,
                                                  count, flags);
}

static void wrapped_shm_barrier(sqlite3_file *p) {
    UNDERLYING_FILE(p)->pMethods->xShmBarrier(UNDERLYING_FILE(p));
}

static int wrapped_shm_unmap(sqlite3_file *p, int delete_flag) {
    return UNDERLYING_FILE(p)->pMethods->xShmUnmap(UNDERLYING_FILE(p),
                                                   delete_flag);
}

static int wrapped_fetch(sqlite3_file *p, sqlite3_int64 offset, int amount,
                         void **page) {
    FLUSH_WAL_BUFFER(p);
    return UNDERLYING_FILE(p)->pMethods->xFetch(UNDERLYING_FILE(p), offset,
                                                amount, page);
}

static int wrapped_unfetch(sqlite3_file *p, sqlite3_int64 offset, void *page) {
    return UNDERLYING_FILE(p)->pMethods->xUnfetch(UNDERLYING_FILE(p), offset,
                                                  page);
}

static const sqlite3_io_methods g_buffered_file_methods = {
    .iVersion = 3,
    .xClose = wrapped_close,
    .xRead = wrapped_read,
    .xWrite = wrapped_write,
    .xTruncate = wrapped_truncate,
    .xSync = wrapped_sync,
    .xFileSize = wrapped_file_size,
    .xLock = wrapped_lock,
    .xUnlock = wrapped_unlock,
    .xCheckReservedLock = wrapped_check_reserved_lock,
    .xFileControl = wrapped_file_control,
    .xSectorSize = wrapped_sector_size,
    .xDeviceCharacteristics = wrapped_device_characteristics,
    .xShmMap = wrapped_shm_map,
    .xShmLock = wrapped_shm_lock,
    .xShmBarrier = wrapped_shm_barrier,
    .xShmUnmap = wrapped_shm_unmap,
    .xFetch = wrapped_fetch,
    .xUnfetch = wrapped_unfetch,
};

static int wrapper_open(sqlite3_vfs *vfs, sqlite3_filename name,
                        sqlite3_file *p, int flags, int *output_flags) {
    (void)vfs;
    BufferedFile *file = WRAPPED_FILE(p);
    memset(file, 0, sizeof(*file));
    file->last_write_end = -1;
    file->underlying =
        (sqlite3_file *)((unsigned char *)p + UNDERLYING_FILE_OFFSET);
    memset(file->underlying, 0, (size_t)g_underlying_vfs->szOsFile);

    int rc = g_underlying_vfs->xOpen(g_underlying_vfs, name, file->underlying,
                                     flags, output_flags);
    if (!file->underlying->pMethods) return rc;
    if (file->underlying->pMethods->iVersion < 3) {
        file->underlying->pMethods->xClose(file->underlying);
        file->underlying->pMethods = NULL;
        return SQLITE_CANTOPEN;
    }
    file->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
    p->pMethods = &g_buffered_file_methods;
    return rc;
}

static int wrapper_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    (void)vfs;
    return g_underlying_vfs->xDelete(g_underlying_vfs, name, sync_dir);
}

static int wrapper_access(sqlite3_vfs *vfs, const char *name, int flags,
                          int *result) {
    (void)vfs;
    return g_underlying_vfs->xAccess(g_underlying_vfs, name, flags, result);
}

static int wrapper_full_pathname(sqlite3_vfs *vfs, const char *name, int size,
                                 char *result) {
    (void)vfs;
    return g_underlying_vfs->xFullPathname(g_underlying_vfs, name, size,
                                           result);
}

static void *wrapper_dl_open(sqlite3_vfs *vfs, const char *name) {
    (void)vfs;
    return g_underlying_vfs->xDlOpen(g_underlying_vfs, name);
}

static void wrapper_dl_error(sqlite3_vfs *vfs, int size, char *message) {
    (void)vfs;
    g_underlying_vfs->xDlError(g_underlying_vfs, size, message);
}

static void (*wrapper_dl_sym(sqlite3_vfs *vfs, void *handle,
                             const char *symbol))(void) {
    (void)vfs;
    return g_underlying_vfs->xDlSym(g_underlying_vfs, handle, symbol);
}

static void wrapper_dl_close(sqlite3_vfs *vfs, void *handle) {
    (void)vfs;
    g_underlying_vfs->xDlClose(g_underlying_vfs, handle);
}

static int wrapper_randomness(sqlite3_vfs *vfs, int amount, char *buffer) {
    (void)vfs;
    return g_underlying_vfs->xRandomness(g_underlying_vfs, amount, buffer);
}

static int wrapper_sleep(sqlite3_vfs *vfs, int microseconds) {
    (void)vfs;
    return g_underlying_vfs->xSleep(g_underlying_vfs, microseconds);
}

static int wrapper_current_time(sqlite3_vfs *vfs, double *time_value) {
    (void)vfs;
    return g_underlying_vfs->xCurrentTime(g_underlying_vfs, time_value);
}

static int wrapper_last_error(sqlite3_vfs *vfs, int size, char *message) {
    (void)vfs;
    return g_underlying_vfs->xGetLastError(g_underlying_vfs, size, message);
}

static int wrapper_current_time64(sqlite3_vfs *vfs, sqlite3_int64 *time_value) {
    (void)vfs;
    return g_underlying_vfs->xCurrentTimeInt64(g_underlying_vfs, time_value);
}

int challenge_wal_vfs_install(void) {
    if (sqlite3_vfs_find(CHALLENGE_WAL_VFS_NAME)) return SQLITE_OK;
    /* The server and its checkpoint thread are the only database processes. */
    g_underlying_vfs = sqlite3_vfs_find("unix-excl");
    if (!g_underlying_vfs || g_underlying_vfs->iVersion < 2) {
        return SQLITE_ERROR;
    }
    size_t file_size = UNDERLYING_FILE_OFFSET +
                       (size_t)g_underlying_vfs->szOsFile;
    if (file_size > INT_MAX) return SQLITE_TOOBIG;

    static sqlite3_vfs vfs;
    memset(&vfs, 0, sizeof(vfs));
    vfs.iVersion = 2;
    vfs.szOsFile = (int)file_size;
    vfs.mxPathname = g_underlying_vfs->mxPathname;
    vfs.zName = CHALLENGE_WAL_VFS_NAME;
    vfs.xOpen = wrapper_open;
    vfs.xDelete = wrapper_delete;
    vfs.xAccess = wrapper_access;
    vfs.xFullPathname = wrapper_full_pathname;
    vfs.xDlOpen = wrapper_dl_open;
    vfs.xDlError = wrapper_dl_error;
    vfs.xDlSym = wrapper_dl_sym;
    vfs.xDlClose = wrapper_dl_close;
    vfs.xRandomness = wrapper_randomness;
    vfs.xSleep = wrapper_sleep;
    vfs.xCurrentTime = wrapper_current_time;
    vfs.xGetLastError = wrapper_last_error;
    vfs.xCurrentTimeInt64 = wrapper_current_time64;
    return sqlite3_vfs_register(&vfs, 0);
}
