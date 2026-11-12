/* Test-only preload for the private server. At the first gated write to its
 * own SQLite WAL, let the client observe the blocked socket, then fail with
 * ENOSPC. Drop the barrier after five seconds even if the test client exits. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int gated_wal(int fd) {
    const char *gate = getenv("WIRE_FAIL_WAL");
    const char *db = getenv("SQLITE_PATH");
    if (!gate || !db || access(gate, F_OK)) return 0;

    char proc[48], actual[PATH_MAX], entered[PATH_MAX], release[PATH_MAX];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(proc, actual, sizeof(actual));
    size_t db_len = strlen(db);
    if (len < 4 || (size_t)len != db_len + 4 || memcmp(actual, db, db_len) ||
        memcmp(actual + db_len, "-wal", 4)) return 0;
    if (snprintf(entered, sizeof(entered), "%s.entered", gate) >= (int)sizeof(entered) ||
        snprintf(release, sizeof(release), "%s.release", gate) >= (int)sizeof(release)) return 1;

    int marker = open(entered, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (marker >= 0) {
        close(marker);
        const struct timespec pause = {.tv_nsec = 2000000};
        for (unsigned i = 0; i < 2500 && !access(gate, F_OK) && access(release, F_OK); ++i)
            nanosleep(&pause, NULL);
    }
    return 1;
}

ssize_t pwrite(int fd, const void *data, size_t n, off_t offset) {
    if (gated_wal(fd)) { errno = ENOSPC; return -1; }
    ssize_t (*native)(int, const void *, size_t, off_t) = dlsym(RTLD_NEXT, "pwrite");
    if (!native) { errno = ENOSYS; return -1; }
    return native(fd, data, n, offset);
}

ssize_t pwrite64(int fd, const void *data, size_t n, off64_t offset) {
    if (gated_wal(fd)) { errno = ENOSPC; return -1; }
    ssize_t (*native)(int, const void *, size_t, off64_t) = dlsym(RTLD_NEXT, "pwrite64");
    if (!native) { errno = ENOSYS; return -1; }
    return native(fd, data, n, offset);
}
