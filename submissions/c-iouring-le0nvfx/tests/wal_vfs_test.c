#define _GNU_SOURCE

#include "sqlite3.h"
#include "wal_vfs.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int rc = sqlite3_initialize();
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_initialize: %s\n", sqlite3_errstr(rc));
        return 1;
    }
    rc = challenge_wal_vfs_install();
    if (rc != SQLITE_OK) {
        fprintf(stderr, "challenge_wal_vfs_install: %s\n", sqlite3_errstr(rc));
        return 1;
    }

    sqlite3_vfs *vfs = sqlite3_vfs_find(CHALLENGE_WAL_VFS_NAME);
    if (!vfs) {
        fputs("custom WAL VFS was not registered\n", stderr);
        return 1;
    }

    /* A WAL name must be the database name plus "-wal". xOpen filenames
     * also have SQLite's required second trailing NUL. */
    char db_path[128] = {0};
    char path[132] = {0};
    snprintf(db_path, sizeof(db_path) - 1,
             "./wal-vfs-regression-%ld.db", (long)getpid());
    snprintf(path, sizeof(path) - 1, "%s-wal", db_path);
    unlink(path);
    unlink(db_path);
    int db_fd = open(db_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (db_fd < 0) {
        perror("create database stub");
        return 1;
    }
    close(db_fd);

    sqlite3_file *file = sqlite3_malloc64((sqlite3_uint64)vfs->szOsFile);
    if (!file) {
        fputs("could not allocate sqlite3_file\n", stderr);
        return 1;
    }
    memset(file, 0, (size_t)vfs->szOsFile);
    int output_flags = 0;
    rc = vfs->xOpen(vfs, path, file,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_WAL,
                    &output_flags);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "VFS xOpen: %s\n", sqlite3_errstr(rc));
        sqlite3_free(file);
        unlink(db_path);
        return 1;
    }

    unsigned char wal_header[32] = {0};
    unsigned char commit_header[24] = {0};
    unsigned char page[4096];
    memset(page, 0xA7, sizeof(page));
    commit_header[7] = 1; /* nonzero database-size field: this frame commits */

    /* Leave a gap so the commit frame header takes the direct-write path. */
    rc = file->pMethods->xWrite(file, wal_header, sizeof(wal_header), 0);
    if (rc == SQLITE_OK) {
        rc = file->pMethods->xWrite(file, commit_header,
                                    sizeof(commit_header), 64);
    }
    if (rc == SQLITE_OK) {
        rc = file->pMethods->xWrite(file, page, sizeof(page), 88);
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, "WAL frame writes: %s\n", sqlite3_errstr(rc));
        file->pMethods->xClose(file);
        sqlite3_free(file);
        unlink(path);
        unlink(db_path);
        return 1;
    }

    int raw_fd = open(path, O_RDONLY);
    unsigned char observed[sizeof(page)];
    ssize_t bytes_read = raw_fd < 0 ? -1 : pread(raw_fd, observed,
                                                 sizeof(observed), 88);
    if (raw_fd >= 0) close(raw_fd);
    int close_rc = file->pMethods->xClose(file);
    sqlite3_free(file);
    unlink(path);
    unlink(db_path);

    if (bytes_read != (ssize_t)sizeof(observed) ||
        memcmp(observed, page, sizeof(page)) != 0) {
        fputs("commit frame page was not visible before an I/O barrier\n",
              stderr);
        return 1;
    }
    if (close_rc != SQLITE_OK) {
        fprintf(stderr, "VFS xClose: %s\n", sqlite3_errstr(close_rc));
        return 1;
    }
    puts("WAL VFS discontinuous commit-frame regression: PASS");
    return 0;
}
