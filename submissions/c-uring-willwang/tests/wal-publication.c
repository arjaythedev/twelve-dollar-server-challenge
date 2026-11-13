/* Run after build.sh, from the submission directory:
 * clang-18 -fuse-ld=gold -flto -O2 -Ivendor tests/wal-publication.c wal.c build/reader.o build/buffer.o -pthread -ldl -lm -o /tmp/wal-publication
 * /tmp/wal-publication
 *
 * At SQLite's WAL hook, the writer has published its commit but has not
 * returned from wal_commit_step(). Verify the native write boundaries, recover
 * the WAL with stock SQLite, then checkpoint it through a second connection.
 */
#define _XOPEN_SOURCE 700
#include "sqlite3.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern const char *wal_vfs_register(void);
extern int wal_commit_step(sqlite3_stmt *);

enum { WAL_HEADER = 32, FRAME_HEADER = 24, GROUP_LIMIT = 131071, MAX_WRITES = 32 };
enum Start { FRESH, APPEND, RESTART };
enum Image { OLD_MAIN, MAIN_AND_WAL, CHECKPOINTED_MAIN };

struct Write {
    off_t offset;
    size_t size;
    uint32_t marker_db_pages, tail_page, tail_db_pages;
};

struct Counts {
    int writes, full_groups, noncommit_splits;
};

struct Trial {
    enum Start start;
    int page_size, changed, rows, db_pages, channel;
    int published, in_hook;
    off_t first_frame, marker;
    struct stat wal;
    sqlite3 *reader;
    struct Write writes[MAX_WRITES];
    struct Counts count;
};

static const char *const image_names[] = {"before.db", "recovered.db", "after.db"};
static const char *const start_names[] = {"fresh", "append", "restart"};
static struct Trial *observed;
static ssize_t (*native_write)(int, const void *, size_t, off_t);
static char temp_dir[PATH_MAX];
static int original_cwd;
static pid_t owner;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "WAL publication: %s\n", message);
        exit(1);
    }
}

static int remove_files(void) {
    const char *const databases[] = {"data.db", "before.db", "recovered.db", "after.db"};
    const char *const suffixes[] = {"", "-wal", "-shm", "-journal"};
    int ok = 1;
    for (size_t i = 0; i < sizeof(databases) / sizeof(*databases); ++i) {
        for (size_t j = 0; j < sizeof(suffixes) / sizeof(*suffixes); ++j) {
            char name[64];
            snprintf(name, sizeof(name), "%s%s", databases[i], suffixes[j]);
            if (unlink(name) < 0 && errno != ENOENT) ok = 0;
        }
    }
    return ok;
}

/* Only the original supervisor owns the directory. Children may exit on a failed assertion. */
static void cleanup(void) {
    if (getpid() != owner) return;
    int ok = remove_files();
    if (fchdir(original_cwd) < 0 || rmdir(temp_dir) < 0) ok = 0;
    if (close(original_cwd) < 0) ok = 0;
    if (!ok) {
        perror("clean up WAL publication test");
        _exit(1);
    }
}

static void transfer(int fd, void *buffer, size_t length, int writing) {
    unsigned char *next = buffer;
    while (length) {
        ssize_t done = writing ? write(fd, next, length) : read(fd, next, length);
        if (done < 0 && errno == EINTR) continue;
        check(done > 0, "child communication failed");
        next += done;
        length -= (size_t)done;
    }
}

static void wait_for(pid_t child) {
    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    check(result == child && WIFEXITED(status) && !WEXITSTATUS(status), "child failed");
}

/* Each row occupies one leaf. Compare the entire blob, including a different
 * pattern for every row and its committed revision. */
static void fixture_blob(sqlite3_context *ctx, int argc, sqlite3_value **values) {
    (void)argc;
    unsigned id = (unsigned)sqlite3_value_int(values[0]);
    int size = sqlite3_value_int(values[1]) * 3 / 5;
    unsigned revision = (unsigned)sqlite3_value_int(values[2]);
    unsigned char blob[16384];
    check(size > 0 && (size_t)size <= sizeof(blob), "fixture blob size");
    for (int i = 0; i < size; ++i) {
        blob[i] = (unsigned char)(id * 73 + i * 29 + (i >> 3) * 7 + revision * 31);
    }
    sqlite3_result_blob(ctx, blob, size, SQLITE_TRANSIENT);
}

static sqlite3 *open_db(const char *path, const char *vfs, int flags) {
    sqlite3 *db = NULL;
    check(sqlite3_open_v2(path, &db, flags, vfs) == SQLITE_OK, "open database");
    check(sqlite3_create_function(db, "fixture_blob", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                  NULL, fixture_blob, NULL, NULL) == SQLITE_OK, "register fixture blob");
    return db;
}

static int read_int(void *result, int columns, char **values, char **names) {
    (void)columns;
    (void)names;
    if (values[0]) *(int *)result = atoi(values[0]);
    return 0;
}

static int sql(sqlite3 *db, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *statement = sqlite3_vmprintf(format, args);
    va_end(args);
    check(statement != NULL, "format SQL");
    int result = 0;
    if (sqlite3_exec(db, statement, read_int, &result, NULL) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", statement, sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_free(statement);
    return result;
}

static void verify_rows(sqlite3 *db, const struct Trial *trial, int changed) {
    check(sql(db, "SELECT count(*)=%d AND min(id)=1 AND max(id)=%d AND "
                  "min(typeof(value)='blob' AND value=fixture_blob(id,%d,id<=%d)) FROM items",
              trial->rows, trial->rows, trial->page_size, changed), "all row ids and payload bytes");
    check(sql(db, "SELECT count(*)=1 AND min(integrity_check)='ok' FROM pragma_integrity_check"),
          "SQLite integrity check");
}

static void copy_file(const char *from, const char *to) {
    FILE *source = fopen(from, "rb");
    FILE *dest = fopen(to, "wbx");
    check(source && dest, "open image files");
    char buffer[32768];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        check(fwrite(buffer, 1, n, dest) == n, "write image");
    }
    check(!ferror(source) && fclose(source) == 0 && fclose(dest) == 0, "finish image");
}

/* This process forks from the supervisor, which never opens a SQLite connection.
 * Its raw file copies cannot release any of the writer's POSIX file locks. */
static void verify_image(const struct Trial *trial, enum Image image) {
    copy_file("data.db", image_names[image]);
    if (image == MAIN_AND_WAL) copy_file("data.db-wal", "recovered.db-wal");
    sqlite3 *stock = open_db(image_names[image], "unix-excl", SQLITE_OPEN_READWRITE);
    verify_rows(stock, trial, image == OLD_MAIN ? 0 : trial->changed);
    check(sqlite3_close(stock) == SQLITE_OK, "close stock image");
}

static uint32_t read_u32(const unsigned char *v) {
    return (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
}

static int frames_per_group(int page_size) {
    return GROUP_LIMIT / (page_size + FRAME_HEADER) + 1;
}

static size_t full_write_size(int page_size) {
    return (size_t)(frames_per_group(page_size) - 1) * (page_size + FRAME_HEADER) + FRAME_HEADER;
}

/* Always perform the original syscall first. Keep only successful writes to
 * the foreground WAL inode. Ignore the separate reader's checkpoint. */
static ssize_t observe_write(int fd, const void *data, size_t size, off_t offset) {
    ssize_t done = native_write(fd, data, size, offset);
    int saved_errno = errno;
    struct Trial *trial = observed;
    struct stat file;
    if (done > 0 && trial && !trial->in_hook && fstat(fd, &file) == 0 &&
        file.st_dev == trial->wal.st_dev && file.st_ino == trial->wal.st_ino) {
        check(!trial->published, "foreground WAL write after the publication hook");
        check(trial->count.writes < MAX_WRITES, "native trace capacity");
        struct Write *entry = &trial->writes[trial->count.writes++];
        entry->offset = offset;
        entry->size = (size_t)done;
        const unsigned char *bytes = data;
        if (offset <= trial->marker && offset + done >= trial->marker + FRAME_HEADER) {
            entry->marker_db_pages = read_u32(bytes + (trial->marker - offset) + 4);
        }
        if ((size_t)done == full_write_size(trial->page_size)) {
            const unsigned char *header = bytes + done - FRAME_HEADER;
            entry->tail_page = read_u32(header);
            entry->tail_db_pages = read_u32(header + 4);
        }
    }
    errno = saved_errno;
    return done;
}

static void verify_writes(struct Trial *trial) {
    int stride = trial->page_size + FRAME_HEADER;
    int group = frames_per_group(trial->page_size);
    int markers = 0, first_direct = 0, file_headers = 0;
    off_t next = trial->first_frame;
    for (int i = 0; i < trial->count.writes; ++i) {
        const struct Write *entry = &trial->writes[i];
        if (entry->offset == 0 && entry->size == WAL_HEADER) {
            check(i == 0 && trial->start != APPEND, "new WAL header precedes the frames");
            ++file_headers;
            continue;
        }
        check(entry->offset == next, "contiguous WAL frame writes");
        next += (off_t)entry->size;
        int direct_header = entry->offset == trial->first_frame && entry->size == FRAME_HEADER;
        int full_group = entry->size == full_write_size(trial->page_size);
        if (direct_header || full_group) {
            check(i + 1 < trial->count.writes && trial->writes[i + 1].offset == next &&
                  trial->writes[i + 1].size == (size_t)trial->page_size, "standalone full page follows header");
        }
        if (direct_header) first_direct = 1;
        if (full_group) {
            int previous_frames = first_direct + trial->count.full_groups * group;
            int ends_at_marker = next == trial->marker + FRAME_HEADER;
            check(entry->offset == trial->first_frame + (off_t)previous_frames * stride, "group start offset");
            check(entry->tail_page > 0 && entry->tail_page <= (uint32_t)trial->db_pages &&
                  entry->tail_db_pages == (ends_at_marker ? (uint32_t)trial->db_pages : 0), "group's frame header");
            ++trial->count.full_groups;
            if (!ends_at_marker) ++trial->count.noncommit_splits;
        }
        if (entry->marker_db_pages) {
            check(i == trial->count.writes - 2 && next == trial->marker + FRAME_HEADER &&
                  entry->marker_db_pages == (uint32_t)trial->db_pages, "last frame contains the real commit marker");
            ++markers;
        }
    }
    check(file_headers == (trial->start != APPEND) && first_direct == file_headers,
          "first frame after a fresh or restarted WAL header");
    check(markers == 1 && next == trial->marker + stride, "published commit and page extent");
    const struct Write *last = &trial->writes[trial->count.writes - 1];
    check(last->size == (size_t)trial->page_size && last->offset == trial->marker + FRAME_HEADER,
          "final page already written at publication");
    int grouped_frames = trial->changed - first_direct;
    check(trial->count.full_groups == grouped_frames / group &&
          trial->count.noncommit_splits == (grouped_frames - 1) / group, "actual physical boundary count");
}

static void request_image(const struct Trial *trial, enum Image image) {
    unsigned char request = (unsigned char)image, reply;
    transfer(trial->channel, &request, sizeof(request), 1);
    transfer(trial->channel, &reply, sizeof(reply), 0);
    check(reply == request, "image verification acknowledged");
}

static void checkpoint(sqlite3 *reader, int mode, int frames) {
    int published = -1, copied = -1;
    check(sqlite3_wal_checkpoint_v2(reader, "main", mode, &published, &copied) == SQLITE_OK &&
          published == frames && copied == frames, "stock checkpoint backfilled every published frame");
}

static int published(void *arg, sqlite3 *db, const char *name, int frames) {
    (void)db;
    struct Trial *trial = arg;
    check(trial == observed && !trial->published && !strcmp(name, "main"), "foreground publication hook");
    check(frames == trial->changed + (trial->start == APPEND), "published frame count");
    trial->in_hook = 1;
    verify_writes(trial);
    request_image(trial, OLD_MAIN);
    request_image(trial, MAIN_AND_WAL);
    verify_rows(trial->reader, trial, trial->changed);
    checkpoint(trial->reader, SQLITE_CHECKPOINT_FULL, frames);
    request_image(trial, CHECKPOINTED_MAIN);
    trial->in_hook = 0;
    trial->published = 1;
    return SQLITE_OK;
}

static int primed(void *arg, sqlite3 *db, const char *name, int frames) {
    (void)db;
    check(!strcmp(name, "main") && frames == 1, "one metadata frame primes the WAL");
    *(int *)arg = 1;
    return SQLITE_OK;
}

static void commit(sqlite3 *db) {
    sqlite3_stmt *statement;
    check(sqlite3_prepare_v2(db, "COMMIT", -1, &statement, NULL) == SQLITE_OK, "prepare COMMIT");
    check(wal_commit_step(statement) == SQLITE_DONE, "application COMMIT");
    check(sqlite3_finalize(statement) == SQLITE_OK, "finalize COMMIT");
}

static void run_writer(struct Trial *trial) {
    sqlite3 *seed = open_db("data.db", "unix-excl", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    sql(seed, "PRAGMA page_size=%d", trial->page_size);
    sql(seed, "CREATE TABLE items(id INTEGER PRIMARY KEY, value BLOB NOT NULL);"
              "WITH RECURSIVE ids(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM ids WHERE id<%d)"
              " INSERT INTO items SELECT id,fixture_blob(id,%d,0) FROM ids", trial->rows, trial->page_size);
    check(sql(seed, "PRAGMA page_size") == trial->page_size, "seed page size");
    trial->db_pages = sql(seed, "PRAGMA page_count");
    check(sqlite3_close(seed) == SQLITE_OK, "close seed");

    sqlite3_vfs *unix_vfs = sqlite3_vfs_find("unix-excl");
    check(unix_vfs && unix_vfs->iVersion >= 3, "stock VFS supports syscall observation");
    const char *syscall = unix_vfs->xGetSystemCall(unix_vfs, "pwrite") ? "pwrite" : "pwrite64";
    native_write = (ssize_t (*)(int, const void *, size_t, off_t))unix_vfs->xGetSystemCall(unix_vfs, syscall);
    check(native_write && unix_vfs->xSetSystemCall(unix_vfs, syscall, (sqlite3_syscall_ptr)observe_write) == SQLITE_OK,
          "install passive pwrite observer");
    const char *vfs = wal_vfs_register();
    check(vfs != NULL, "register submission WAL VFS");
    sqlite3 *writer = open_db("data.db", vfs, SQLITE_OPEN_READWRITE);
    sql(writer, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
                "PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=-4096");
    trial->reader = open_db("data.db", "unix-excl", SQLITE_OPEN_READWRITE);
    verify_rows(trial->reader, trial, 0);

    struct stat original_wal;
    if (trial->start != FRESH) {
        int seen = 0;
        sqlite3_wal_hook(writer, primed, &seen);
        sql(writer, "BEGIN IMMEDIATE; PRAGMA user_version=7");
        commit(writer);
        check(seen, "metadata transaction published");
        sqlite3_wal_hook(writer, NULL, NULL);
        if (trial->start == RESTART) checkpoint(trial->reader, SQLITE_CHECKPOINT_RESTART, 1);
        check(stat("data.db-wal", &original_wal) == 0, "primed WAL exists");
    }
    sql(writer, "BEGIN IMMEDIATE; UPDATE items SET value=fixture_blob(id,%d,1) WHERE id<=%d",
        trial->page_size, trial->changed);
    check(sqlite3_changes(writer) == trial->changed, "all intended rows changed");
    check(stat("data.db-wal", &trial->wal) == 0, "find foreground WAL");
    if (trial->start != FRESH) {
        check(original_wal.st_dev == trial->wal.st_dev && original_wal.st_ino == trial->wal.st_ino,
              "same WAL inode after priming");
    }
    sqlite3_wal_hook(writer, published, trial);
    observed = trial;
    commit(writer);
    observed = NULL;
    check(trial->published, "publication completed before COMMIT returned");
    check(sqlite3_close(trial->reader) == SQLITE_OK && sqlite3_close(writer) == SQLITE_OK, "close both connections");
}

static struct Counts run_case(int page_size, int changed, enum Start start) {
    int channel[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, channel) == 0, "create observer channel");
    int stride = page_size + FRAME_HEADER;
    struct Trial trial = {
        .start = start, .page_size = page_size, .changed = changed,
        .rows = 2 * frames_per_group(page_size) + 4, .channel = channel[1],
        .first_frame = WAL_HEADER + (start == APPEND ? stride : 0),
    };
    trial.marker = trial.first_frame + (off_t)(changed - 1) * stride;
    pid_t writer = fork();
    check(writer >= 0, "fork writer");
    if (writer == 0) {
        close(channel[0]);
        run_writer(&trial);
        transfer(channel[1], &trial.count, sizeof(trial.count), 1);
        _exit(0);
    }
    close(channel[1]);
    for (int image = OLD_MAIN; image <= CHECKPOINTED_MAIN; ++image) {
        unsigned char request;
        transfer(channel[0], &request, sizeof(request), 0);
        check(request == image, "image verification order");
        pid_t stock = fork();
        check(stock >= 0, "fork clean stock reader");
        if (stock == 0) {
            close(channel[0]);
            verify_image(&trial, (enum Image)image);
            _exit(0);
        }
        wait_for(stock);
        transfer(channel[0], &request, sizeof(request), 1);
    }
    transfer(channel[0], &trial.count, sizeof(trial.count), 0);
    close(channel[0]);
    wait_for(writer);
    check(remove_files(), "remove completed case files");
    printf("%5d-byte pages, %2d changed frames, %-7s: %d native writes, %d full groups\n",
           page_size, changed, start_names[start], trial.count.writes, trial.count.full_groups);
    return trial.count;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    check(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "report closed child channels");
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    int n = snprintf(temp_dir, sizeof(temp_dir), "%s/wal-publication-XXXXXX", tmp);
    check(n > 0 && (size_t)n < sizeof(temp_dir), "temporary path fits");
    original_cwd = open(".", O_RDONLY | O_DIRECTORY);
    check(original_cwd >= 0 && mkdtemp(temp_dir) != NULL, "create temporary directory");
    if (chdir(temp_dir) < 0) {
        rmdir(temp_dir);
        check(0, "enter temporary directory");
    }
    owner = getpid();
    if (atexit(cleanup) != 0) {
        cleanup();
        check(0, "register temporary cleanup");
    }
    check(!strcmp(sqlite3_sourceid(), SQLITE_SOURCE_ID), "SQLite library matches pinned headers");

    struct Counts totals = {0};
    int cases = 0;
    const int page_sizes[] = {4096, 16384};
    for (size_t p = 0; p < sizeof(page_sizes) / sizeof(*page_sizes); ++p) {
        int page_size = page_sizes[p];
        int group = frames_per_group(page_size);
        for (int delta = -1; delta <= 2; ++delta) {
            for (enum Start start = FRESH; start <= RESTART; ++start) {
                int changed = (start == RESTART ? 2 * group : group) + delta;
                struct Counts count = run_case(page_size, changed, start);
                totals.writes += count.writes;
                totals.full_groups += count.full_groups;
                totals.noncommit_splits += count.noncommit_splits;
                ++cases;
            }
        }
    }
    printf("SQLite %s: %d publication cases passed, %d native WAL writes, %d full groups "
           "(%d non-commit splits); all stock readers and checkpoints verified.\n",
           sqlite3_libversion(), cases, totals.writes, totals.full_groups, totals.noncommit_splits);
    return 0;
}
