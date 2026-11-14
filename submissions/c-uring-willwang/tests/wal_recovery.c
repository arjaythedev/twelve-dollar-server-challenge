/* Run after build.sh, from the submission directory:
 * clang-18 -fuse-ld=gold -flto -O2 -Ivendor tests/wal_recovery.c wal.c build/{reader,buffer}.o -pthread -ldl -lm -o /tmp/wal-recovery
 * /tmp/wal-recovery
 * Each treatment exits without sqlite3_close, then a new process checks the
 * database with the original unix-excl VFS. The fixture interposes native
 * pread/pwrite through SQLite's public xSetSystemCall. Failures cover an
 * entire grouped write, a short write, the commit marker and its final page. */
#define _GNU_SOURCE
#include "sqlite3.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern const char *wal_vfs_register(void);
extern int wal_commit_step(sqlite3_stmt*);
enum Stage { SEED, COMMIT, VERIFY, FAIL_WRITE, SHORT_WRITE, FAIL_MARKER, FAIL_NEXT, CRASH_MARKER };
static enum Stage fault;
static int injected, paired, pending_fd = -1;
static off_t pending_off;
static ssize_t (*write_at)(int, const void *, size_t, off_t);
static ssize_t (*read_at)(int, void *, size_t, off_t);
static unsigned repair_reads;
static sqlite3 *db;

static ssize_t checked_read(int fd, void *data, size_t n, off_t off) {
    if (n == 4096 + 24) ++repair_reads;
    return read_at(fd, data, n, off);
}

static ssize_t checked_write(int fd, const void *data, size_t n, off_t off) {
    if (fd == pending_fd && off == pending_off) {
        injected = 1;
        pending_fd = -1;
        errno = EIO;
        return -1;
    }
    const unsigned char *bytes = data;
    int grouped = n >= 4096+24 && (n % 4120 == 0 || n % 4120 == 24);
    int marker = n % 4120 == 24 && (bytes[n-20]|bytes[n-19]|bytes[n-18]|bytes[n-17]);
    if (marker && fault == CRASH_MARKER) _exit(0);
    if (marker && fault == FAIL_MARKER) { injected = 1; errno = EIO; return -1; }
    if (marker && fault == FAIL_NEXT) { pending_fd = fd; pending_off = off + n; }
    if (grouped) {
        ++paired;
        if (fault == FAIL_WRITE) { injected = 1; errno = EIO; return -1; }
        if (fault == SHORT_WRITE) {
            ssize_t done = write_at(fd, data, n / 2, off);
            if (done > 0) { pending_fd = fd; pending_off = off + done; }
            return done;
        }
    }
    return write_at(fd, data, n, off);
}

static void must(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "%s failed (%s)\n", what, db ? sqlite3_errmsg(db) : "no db");
        _exit(1);
    }
}

static void sql(const char *q) {
    must(sqlite3_exec(db, q, NULL, NULL, NULL) == SQLITE_OK, q);
}

static int truth(const char *query) {
    sqlite3_stmt *s;
    must(sqlite3_prepare_v2(db, query, -1, &s, NULL) == SQLITE_OK, query);
    must(sqlite3_step(s) == SQLITE_ROW, query);
    int result = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return result;
}

static void child(enum Stage stage, const char *path) {
    int writing = stage != SEED && stage != VERIFY;
    const char *vfs = writing ? wal_vfs_register() : "unix-excl";
    must(vfs != NULL, "register");
    must(!sqlite3_open_v2(path, &db, SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, vfs), "open");
    sql("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0");
    if (stage == SEED) {
        sql("CREATE TABLE blobs(k INTEGER PRIMARY KEY, v BLOB NOT NULL, gen INT NOT NULL);"
            "WITH RECURSIVE seq(k) AS (VALUES(1) UNION ALL SELECT k+1 FROM seq WHERE k<2400)"
            " INSERT INTO blobs SELECT k,zeroblob(1800),0 FROM seq");
    } else if (stage == VERIFY) {
        must(truth("SELECT count(*)=2400 AND min(gen)=2 AND max(gen)=2 AND min(length(v))=1800 FROM blobs"),
             "committed rows survive, failed transaction absent");
        must(truth("SELECT integrity_check='ok' FROM pragma_integrity_check"), "integrity_check");
    } else {
        sqlite3_vfs *unix_vfs = sqlite3_vfs_find("unix-excl");
        const char *name = unix_vfs->xGetSystemCall(unix_vfs, "pwrite") ? "pwrite" : "pwrite64";
        write_at = (ssize_t (*)(int,const void*,size_t,off_t))unix_vfs->xGetSystemCall(unix_vfs,name);
        must(write_at != NULL, "find pwrite");
        must(!unix_vfs->xSetSystemCall(unix_vfs,name,(sqlite3_syscall_ptr)checked_write), "interpose pwrite");
        name = unix_vfs->xGetSystemCall(unix_vfs, "pread") ? "pread" : "pread64";
        read_at = (ssize_t (*)(int,void*,size_t,off_t))unix_vfs->xGetSystemCall(unix_vfs,name);
        must(read_at != NULL, "find pread");
        must(!unix_vfs->xSetSystemCall(unix_vfs,name,(sqlite3_syscall_ptr)checked_read), "interpose pread");
        if (stage == COMMIT) sql("PRAGMA cache_size=8; PRAGMA cache_spill=ON");
        else sql("PRAGMA cache_size=4096; PRAGMA cache_spill=OFF");
        sql("BEGIN; UPDATE blobs SET gen=gen+1; UPDATE blobs SET v=zeroblob(1800);"
            "UPDATE blobs SET gen=gen+1");
        fault = stage;
        sqlite3_stmt *commit;
        must(sqlite3_prepare_v2(db,"COMMIT",-1,&commit,NULL)==SQLITE_OK,"prepare");
        repair_reads = 0;
        int raw = wal_commit_step(commit);
        must(fault != CRASH_MARKER, "crash marker reached");
        int rc = raw==SQLITE_DONE ? SQLITE_OK : raw;
        must(paired > 0, "paired WAL writes occurred");
        if (stage == COMMIT) must(rc == SQLITE_OK && repair_reads, "forced-spill checksum repair and COMMIT");
        else must(injected && (rc & 255) == SQLITE_IOERR, "failed COMMIT returned IOERR");
        _exit(0);
    }
    must(sqlite3_close(db) == SQLITE_OK, "sqlite3_close");
    _exit(0);
}

int main(void) {
    char dir[256], path[320];
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    int n = snprintf(dir,sizeof(dir),"%s/wal-recovery-XXXXXX",tmp);
    if (n < 0 || (size_t)n >= sizeof(dir) || !mkdtemp(dir)) return 1;
    snprintf(path,sizeof(path),"%s/data",dir);
    const enum Stage stages[] = { SEED,COMMIT,VERIFY,FAIL_WRITE,VERIFY,SHORT_WRITE,VERIFY,FAIL_MARKER,VERIFY,FAIL_NEXT,VERIFY,CRASH_MARKER,VERIFY };
    int result = 0;
    for (unsigned i = 0; i < sizeof(stages)/sizeof(*stages); ++i) {
        pid_t pid = fork();
        if (pid == 0) child(stages[i], path);
        int status;
        if (pid < 0 || waitpid(pid,&status,0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
            result = 1;
            break;
        }
    }
    const char *files[] = {"data","data-wal","data-shm"};
    for (unsigned i = 0; i < sizeof(files)/sizeof(*files); ++i) {
        snprintf(path,sizeof(path),"%s/%s",dir,files[i]);
        unlink(path);
    }
    rmdir(dir);
    if (!result) puts("WAL crash recovery, checksum repair, torn writes and commit marker/page failures passed");
    return result;
}
