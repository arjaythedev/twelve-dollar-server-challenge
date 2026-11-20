/* The same sequential, deterministic moving-feed replay links to either
 * production read path. Time only database lookup and JSON construction.
 * Hash every rendered byte outside those timing intervals. */
#include "sqlite3.h"
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { const void *data; size_t len; } Bytes;
void *adapter_open(const char *path);
int adapter_commit(void *ctx);
int adapter_read(void *ctx, int point, int64_t id, Bytes *body);
int adapter_create(void *ctx, int64_t uid, const char *body, int64_t *id);
int adapter_like(void *ctx, int64_t uid, int64_t id);

#ifndef BENCH_KENSAC
#include "challenge.h"
void *adapter_open(const char *path) {
    static Buf out;
    return db_open(path) ? NULL : &out;
}
int adapter_commit(void *ctx) {
    (void)ctx;
    if (db_commit_pass()) return 0;
    /* This replay fixes the commit group and operation order itself. The
     * HTTP reactor's write-admission timer is outside the measured reader. */
    db_begin_pass();
    return 1;
}
int adapter_read(void *ctx, int point, int64_t id, Bytes *body) {
    Buf *out = ctx;
    out->len = 0;
    int status = point ? db_post(id, out) : db_feed(out);
    *body = (Bytes){out->data, out->len};
    return status == 200;
}
int adapter_create(void *ctx, int64_t uid, const char *text, int64_t *id) {
    Buf *out = ctx;
    out->len = 0;
    if (db_create(uid, text, strlen(text), "bench", 5, out) != 201) return 0;
    *id = strtoll(out->data + sizeof("{\"post\":{\"id\":") - 1, NULL, 10);
    return *id > 0;
}
int adapter_like(void *ctx, int64_t uid, int64_t id) {
    (void)ctx;
    int status = db_like(uid, id);
    return status == 200 || status == 201;
}
#endif

static void require(int ok, const char *where) {
    if (!ok) { fprintf(stderr, "failed at %s\n", where); exit(1); }
}

/* Deterministic SQL 'now' makes the bytes comparable across separate runs.
 * The test registers its own clock before either server opens its private DB.
 * Each production VFS, including its WAL methods, operates normally. */
static atomic_llong logical_time = 2461400LL * 86400000LL;
static int fixed_now(sqlite3_vfs *vfs, sqlite3_int64 *time) {
    (void)vfs;
    *time = atomic_load_explicit(&logical_time, memory_order_relaxed);
    return SQLITE_OK;
}
static void install_clock(void) {
    static sqlite3_vfs clock_vfs;
    require(sqlite3_initialize() == SQLITE_OK, "sqlite initialization");
    sqlite3_vfs *base = sqlite3_vfs_find("unix-excl");
    require(base && base->iVersion >= 2, "unix-excl clock");
    clock_vfs = *base;
    clock_vfs.xCurrentTimeInt64 = fixed_now;
    require(sqlite3_vfs_unregister(base) == SQLITE_OK &&
            sqlite3_vfs_register(&clock_vfs, 0) == SQLITE_OK, "test clock registration");
}

static uint32_t state = 3457239481u;
static uint32_t random_u32(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static uint64_t timed_read(void *ctx, int point, int64_t id, Bytes *bytes) {
    uint64_t t = now_ns();
    int ok = adapter_read(ctx, point, id, bytes);
    uint64_t elapsed = now_ns() - t;
    require(ok, "read");
    return elapsed;
}
static void publish(void *ctx, int uid, int sequence, int64_t *latest) {
    char body[128];
    snprintf(body, sizeof body, "User %d published a new post in the moving feed, sequence %d", uid, sequence);
    atomic_fetch_add_explicit(&logical_time, 3, memory_order_relaxed);
    require(adapter_create(ctx, uid, body, latest), "create");
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) return 2;
    int stable = argc == 4 && !strcmp(argv[3], "stable");
    int requests = atoi(argv[2]);
    require(requests >= 100, "request count");
    install_clock();
    void *ctx = adapter_open(argv[1]);
    require(ctx != NULL, "open");
    /* The measured point reads trail publication by 900–2100 posts. Likes
     * trail by 1800–4500 posts. Prefill so both sample live-created rows. */
    int64_t latest = 0;
    for (int i = 0; i < 4500; ++i) {
        publish(ctx, 1 + random_u32() % 20000, -i, &latest);
        if (!(i & 31)) require(adapter_commit(ctx), "prefill commit");
    }
    require(adapter_commit(ctx), "prefill final commit");
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    require(hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL), "SHA-256 initialization");
    uint64_t calls[2] = {0}, elapsed[2] = {0};
#ifdef BENCH_RANK
    extern int bench_all_keys;
    unsigned sequence = 0;
    uint64_t pair_time[2] = {0};
    Buf saved = {0};
#endif
    for (int i = -4096; i < requests; ++i) {
        if (!(i & 31)) require(adapter_commit(ctx), "commit");
        unsigned route = random_u32() % 217;
        unsigned age = random_u32();
        int uid = 1 + random_u32() % 20000;
        if (route >= 215) { if (!stable) publish(ctx, uid, i, &latest); }
        else if (route >= 200) {
            if (!stable) require(adapter_like(ctx, uid, latest - 1800 - age % 2701), "like");
        }
        else {
            int point = route >= 100;
            Bytes bytes;
#ifdef BENCH_RANK
            int first = point ? 0 : (sequence++ & 1);
            bench_all_keys = first;
#endif
            uint64_t delta = timed_read(ctx, point, latest - 900 - age % 1201, &bytes);
#ifdef BENCH_RANK
            if (!point) {
                saved.len = 0;
                require(!buf_append(&saved, bytes.data, bytes.len), "save first feed");
                bench_all_keys = 1 - first;
                uint64_t second = timed_read(ctx, 0, 0, &bytes);
                require(saved.len == bytes.len && !memcmp(saved.data, bytes.data, bytes.len), "paired feeds");
                if (i >= 0) { pair_time[first] += delta; pair_time[1 - first] += second; }
            }
#endif
            if (i >= 0) {
                ++calls[point];
                elapsed[point] += delta;
                require(EVP_DigestUpdate(hash, bytes.data, bytes.len), "SHA-256 update");
            }
        }
    }
    require(adapter_commit(ctx), "final commit");
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size;
    require(EVP_DigestFinal_ex(hash, digest, &size), "SHA-256 finalization");
    EVP_MD_CTX_free(hash);
    printf("{\"feeds\":%" PRIu64 ",\"posts\":%" PRIu64 ",\"feed_ns\":%.1f,\"post_ns\":%.1f,",
           calls[0], calls[1], (double)elapsed[0] / calls[0], (double)elapsed[1] / calls[1]);
#ifdef BENCH_RANK
    printf("\"rank_ns\":%.1f,\"all_ns\":%.1f,", (double)pair_time[0] / calls[0], (double)pair_time[1] / calls[0]);
    free(saved.data);
#endif
    printf("\"sha256\":\"");
    for (unsigned i = 0; i < size; ++i) printf("%02x", digest[i]);
    puts("\"}");
}
