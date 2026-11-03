/* Test-only fault injection, linked into build/commit-server with -Wl,--wrap=sqlite3_open_v2 and never
 * into bin/server. A commit hook on every connection the server opens reads control files in $TEST_DIR:
 *   fail     the next commit fails (the file is removed)
 *   hold     commits wait while it exists; "entered" is created when one starts waiting */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sqlite3.h"

static int commit_hook(void *unused)
{
    (void)unused;
    const char *dir = getenv("TEST_DIR");
    if (!dir) return 0;
    char hold[4096], entered[4096], fail[4096];
    snprintf(hold, sizeof hold, "%s/hold", dir);
    snprintf(entered, sizeof entered, "%s/entered", dir);
    snprintf(fail, sizeof fail, "%s/fail", dir);
    if (!access(hold, F_OK)) {
        int fd = open(entered, O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) close(fd);
        for (int i = 0; i < 1000 && !access(hold, F_OK); i++) usleep(10000);
    }
    return unlink(fail) == 0; /* nonzero turns the commit into a rollback */
}

int __real_sqlite3_open_v2(const char *path, sqlite3 **db, int flags, const char *vfs);
int __wrap_sqlite3_open_v2(const char *path, sqlite3 **db, int flags, const char *vfs)
{
    int rc = __real_sqlite3_open_v2(path, db, flags, vfs);
    if (rc == SQLITE_OK) sqlite3_commit_hook(*db, commit_hook, NULL);
    return rc;
}
