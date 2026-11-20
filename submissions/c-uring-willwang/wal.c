/* A public VFS wrapper over SQLite's stock unix-excl VFS. Only WAL handles
 * receive custom methods. Directory operations, the database, locking and the
 * wal-index all use the original VFS. SQLite still determines the bytes, write
 * order, durability policy and checkpoint behavior. */
#include "sqlite3.h"
#include <string.h>

enum { HEADER_BYTES = 24, MAX_PAGE = 65536, FRAME_BUFFER = 131071 };
typedef struct {
    sqlite3_file file;
    sqlite3_int64 next_header, base_offset;
    int buffered, awaiting_page;
    unsigned char *frame;
    /* The original sqlite3_file follows this structure at an aligned offset. */
} WalFile;

static sqlite3_vfs frame_vfs, *unix_vfs;

static sqlite3_file *original(WalFile *f) {
    return (sqlite3_file *)(f + 1);
}

/* Only COMMIT may accumulate complete frames. During ordinary DML a pager
 * spill still writes each page immediately: a later rollback cannot strand
 * a buffered frame after the write lock has been released. The checkpoint
 * connection uses another thread and never participates in this scope. */
static _Thread_local int committing;
static _Thread_local WalFile *group_owner;

static int flush_frames(WalFile *f) {
    /* Clear first even on short writes/IOERR. SQLite receives the error; a
     * later transaction must never retry this transaction's bytes. */
    int n = f->buffered;
    if (group_owner == f) group_owner = NULL;
    f->next_header = -1;
    f->buffered = f->awaiting_page = 0;
    if (!n) return SQLITE_OK;
    sqlite3_file *orig = original(f);
    return orig->pMethods->xWrite(orig, f->frame, n, f->base_offset);
}

int wal_commit_step(sqlite3_stmt *s) {
    committing = 1;
    int rc = sqlite3_step(s);
    committing = 0;
    if (group_owner) {
        /* A successful WAL commit normally flushed its group inside xWrite.
         * Also drain an unfinished group before returning through an error or
         * busy path. Nothing survives this call. */
        int frc = flush_frames(group_owner);
        if (rc == SQLITE_DONE && frc != SQLITE_OK) return frc;
    }
    return rc;
}

static int frame_write(sqlite3_file *file, const void *p, int n, sqlite3_int64 off) {
    WalFile *f = (WalFile *)file;
    sqlite3_file *orig = original(f);
    int full_page = n >= 512 && n <= MAX_PAGE && (n & (n - 1)) == 0;
    /* Only combine adjacent writes. Splits (including sync boundaries),
     * overwrites, and the buffer bound preserve the original write order. */
    if (f->buffered && (off != f->base_offset + f->buffered || n > FRAME_BUFFER - f->buffered)) {
        int rc = flush_frames(f);
        if (rc) return rc;
    }
    if (f->awaiting_page && full_page) {
        memcpy(f->frame + f->buffered, p, (size_t)n);
        f->buffered += n;
        f->awaiting_page = 0;
        int rc = committing && group_owner == f ? SQLITE_OK : flush_frames(f);
        if (!rc) f->next_header = off + n;
        return rc;
    }
    /* A sequential non-commit header is followed by its page. A commit
     * header has a nonzero database-size field. Submit it and every preceding
     * buffered byte immediately, before returning even from this xWrite.
     * Its following page is a separate synchronous write. SQLite only
     * publishes the wal-index commit afterwards. In walRewriteChecksums each
     * standalone header is preceded by a full-frame xRead, which clears
     * next_header; the final repaired marker thus also goes straight through. */
    if (f->frame && n == HEADER_BYTES && off == f->next_header) {
        const unsigned char *h = p;
        int marker = h[4] | h[5] | h[6] | h[7];
        if (f->buffered || !marker) {
            if (committing && group_owner && group_owner != f) {
                int rc = flush_frames(group_owner);
                if (rc) return rc;
            }
            if (!f->buffered) f->base_offset = off;
            memcpy(f->frame + f->buffered, p, HEADER_BYTES);
            f->buffered += HEADER_BYTES;
            f->next_header = -1;
            if (marker) return flush_frames(f);
            f->awaiting_page = 1;
            if (committing) group_owner = f;
            return SQLITE_OK;
        }
    }
    if (f->buffered) {
        int rc = flush_frames(f);
        if (rc) return rc;
    }
    f->next_header = -1;
    int rc = orig->pMethods->xWrite(orig, p, n, off);
    if (!rc && (full_page || (off == 0 && n == 32))) f->next_header = off + n;
    return rc;
}

#define FORWARD_ORDERED(method, signature, args)          \
    static int method signature {                         \
        WalFile *f = (WalFile *)file;                     \
        int rc = flush_frames(f);                         \
        if (rc != SQLITE_OK) return rc;                   \
        sqlite3_file *orig = original(f);                 \
        return orig->pMethods->method args;               \
    }
FORWARD_ORDERED(xRead, (sqlite3_file *file, void *p, int n, sqlite3_int64 off), (orig,p,n,off))
FORWARD_ORDERED(xTruncate, (sqlite3_file *file, sqlite3_int64 n), (orig,n))
FORWARD_ORDERED(xSync, (sqlite3_file *file, int flags), (orig,flags))
FORWARD_ORDERED(xFileSize, (sqlite3_file *file, sqlite3_int64 *size), (orig,size))
FORWARD_ORDERED(xLock, (sqlite3_file *file, int type), (orig,type))
FORWARD_ORDERED(xUnlock, (sqlite3_file *file, int type), (orig,type))
FORWARD_ORDERED(xCheckReservedLock, (sqlite3_file *file, int *out), (orig,out))
FORWARD_ORDERED(xFileControl, (sqlite3_file *file, int op, void *arg), (orig,op,arg))

/* These methods report immutable capabilities; neither observes file data. */
static int sector_size(sqlite3_file *file) {
    sqlite3_file *orig = original((WalFile *)file);
    return orig->pMethods->xSectorSize(orig);
}

static int characteristics(sqlite3_file *file) {
    sqlite3_file *orig = original((WalFile *)file);
    return orig->pMethods->xDeviceCharacteristics(orig);
}

static int frame_close(sqlite3_file *file) {
    WalFile *f = (WalFile *)file;
    int rc = flush_frames(f);
    sqlite3_file *orig = original(f);
    int close_rc = orig->pMethods->xClose(orig);
    sqlite3_free(f->frame);
    file->pMethods = NULL;
    return rc != SQLITE_OK ? rc : close_rc;
}

static const sqlite3_io_methods frame_methods = {
    .iVersion = 1, .xClose = frame_close, .xRead = xRead, .xWrite = frame_write,
    .xTruncate = xTruncate, .xSync = xSync, .xFileSize = xFileSize, .xLock = xLock,
    .xUnlock = xUnlock, .xCheckReservedLock = xCheckReservedLock, .xFileControl = xFileControl,
    .xSectorSize = sector_size, .xDeviceCharacteristics = characteristics
};

static int frame_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file,
                      int flags, int *out_flags) {
    (void)vfs;
    if (!(flags & SQLITE_OPEN_WAL))
        return unix_vfs->xOpen(unix_vfs, name, file, flags, out_flags);
    WalFile *f = (WalFile *)file;
    file->pMethods = NULL;
    sqlite3_file *orig = original(f);
    orig->pMethods = NULL;
    int rc = unix_vfs->xOpen(unix_vfs, name, orig, flags, out_flags);
    if (rc != SQLITE_OK) {
        if (orig->pMethods) orig->pMethods->xClose(orig);
        return rc;
    }
    /* SQLite's unix VFS accepts at most 17 bits per xWrite length. A single
     * allocation failure simply leaves this handle on the ungrouped path. */
    f->frame = sqlite3_malloc(FRAME_BUFFER);
    f->next_header = -1;
    f->buffered = f->awaiting_page = 0;
    file->pMethods = &frame_methods;
    return SQLITE_OK;
}

const char *wal_vfs_register(void) {
    if (frame_vfs.zName) return frame_vfs.zName;
    unix_vfs = sqlite3_vfs_find("unix-excl");
    if (!unix_vfs) return NULL;
    frame_vfs = *unix_vfs;
    frame_vfs.szOsFile += (int)sizeof(WalFile);
    frame_vfs.zName = "unix-excl-frame";
    frame_vfs.xOpen = frame_open;
    if (sqlite3_vfs_register(&frame_vfs, 0) == SQLITE_OK) return frame_vfs.zName;
    frame_vfs.zName = NULL;
    return NULL;
}
