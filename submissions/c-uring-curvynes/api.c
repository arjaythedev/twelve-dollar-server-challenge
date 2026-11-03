#define _GNU_SOURCE
#include "api.h"
#include "sqlite3.h"
#include "native_db.h"
#include "yyjson.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <unistd.h>

static sqlite3 *db;
static NativePlan *native_plan;
static uint64_t native_reads, native_fallbacks;
static sqlite3_stmt *feed_stmt, *post_stmt, *create_stmt, *like_stmt, *exists_stmt, *health_stmt;
static sqlite3_stmt *feed_ids_stmt, *feed_range_stmt, *feed_counts_stmt;
static sqlite3_stmt *begin_stmt, *commit_stmt;
static bool transaction_open, transaction_failed;
static pid_t checkpoint_pid = -1;
static int checkpoint_event = -1, wal_frames;
static unsigned checkpoint_fallbacks;
static volatile sig_atomic_t checkpoint_running = 1;
enum { CHECKPOINT_FRAMES = 16384, CHECKPOINT_FALLBACK = 65536 };
typedef struct { atomic_int finish; } CheckpointState;
static CheckpointState *checkpoint_state;
static SHA256_CTX hmac_inner, hmac_outer;
static struct timespec started;

static void checkpoint_stop(int signal_number) { (void)signal_number; checkpoint_running = 0; }
static void checkpoint_child(const char *path, int ready_fd, pid_t parent) {
    signal(SIGTERM, checkpoint_stop); signal(SIGINT, checkpoint_stop);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) _exit(1);
    sqlite3 *connection = NULL;
    int rc = sqlite3_open_v2(path, &connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_busy_timeout(connection, 5000);
        rc = sqlite3_exec(connection, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
                                     "PRAGMA cache_size=-2048; SELECT count(*) FROM sqlite_schema;", NULL, NULL, NULL);
    }
    if (write(ready_fd, &rc, sizeof(rc)) != (ssize_t)sizeof(rc)) rc = SQLITE_ERROR;
    close(ready_fd);
    unsigned passes = 0, failures = 0;
    while (rc == SQLITE_OK && checkpoint_running) {
        struct pollfd event = {.fd = checkpoint_event, .events = POLLIN};
        int ready = poll(&event, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        uint64_t notifications;
        if (read(checkpoint_event, &notifications, sizeof(notifications)) < 0 && errno != EAGAIN) break;
        if (atomic_load_explicit(&checkpoint_state->finish, memory_order_acquire)) continue;
        int total = 0, copied = 0;
        int result = sqlite3_wal_checkpoint_v2(connection, NULL, SQLITE_CHECKPOINT_PASSIVE, &total, &copied);
        if (result != SQLITE_OK || total < 0 || copied < 0) {
            if (!failures++) fprintf(stderr, "checkpoint helper: rc=%d total=%d copied=%d\n", result, total, copied);
        } else {
            passes++;
            if (total >= CHECKPOINT_FRAMES) atomic_store_explicit(&checkpoint_state->finish, 1, memory_order_release);
        }
    }
    fprintf(stderr, "checkpoint_helper_passes=%u errors=%u\n", passes, failures);
    if (connection) sqlite3_close(connection);
    close(checkpoint_event);
    munmap(checkpoint_state, sizeof(*checkpoint_state));
    exit(rc == SQLITE_OK ? 0 : 1);
}
static void stop_checkpoint(void) {
    if (checkpoint_pid > 0) {
        kill(checkpoint_pid, SIGTERM);
        while (waitpid(checkpoint_pid, NULL, 0) < 0 && errno == EINTR) {}
        checkpoint_pid = -1;
    }
    if (checkpoint_event >= 0) { close(checkpoint_event); checkpoint_event = -1; }
    if (checkpoint_state) { munmap(checkpoint_state, sizeof(*checkpoint_state)); checkpoint_state = NULL; }
}
static int start_checkpoint(const char *path) {
    int ready[2];
    if (pipe2(ready, O_CLOEXEC)) return -1;
    checkpoint_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (checkpoint_event < 0) { close(ready[0]); close(ready[1]); return -1; }
    checkpoint_state = mmap(NULL, sizeof(*checkpoint_state), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (checkpoint_state == MAP_FAILED) {
        checkpoint_state = NULL; close(ready[0]); close(ready[1]); stop_checkpoint(); return -1;
    }
    atomic_init(&checkpoint_state->finish, 0);
    pid_t parent = getpid();
    checkpoint_pid = fork();
    if (checkpoint_pid == 0) { close(ready[0]); checkpoint_child(path, ready[1], parent); _exit(1); }
    close(ready[1]);
    if (checkpoint_pid < 0) { close(ready[0]); stop_checkpoint(); return -1; }
    struct pollfd event = {.fd = ready[0], .events = POLLIN};
    int status = SQLITE_ERROR;
    bool success = poll(&event, 1, 5000) > 0 && read(ready[0], &status, sizeof(status)) == (ssize_t)sizeof(status) && status == SQLITE_OK;
    close(ready[0]);
    if (!success) { stop_checkpoint(); return -1; }
    return 0;
}
static int checkpoint_notify(void *unused, sqlite3 *connection, const char *schema, int pages) {
    (void)unused; (void)schema;
    wal_frames = pages;
    if (pages >= CHECKPOINT_FRAMES) {
        uint64_t notification = 1;
        ssize_t notified = write(checkpoint_event, &notification, sizeof(notification));
        (void)notified;
    }
    if (pages >= CHECKPOINT_FALLBACK) {
        /* A bounded synchronous fallback attempts backfill if the helper falls
           behind. Normal disk sync work belongs to the independent process. */
        checkpoint_fallbacks++;
        sqlite3_wal_checkpoint_v2(connection, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    }
    return SQLITE_OK;
}
void api_checkpoint_maintenance(void) {
    if (!checkpoint_state || transaction_open ||
        !atomic_load_explicit(&checkpoint_state->finish, memory_order_acquire)) return;
    /* The helper has released the checkpoint lock after bulk backfill. Finish
       at a boundary without a read snapshot; do not wait for external readers. */
    sqlite3_busy_timeout(db, 0);
    int total = 0, copied = 0;
    int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_RESTART, &total, &copied);
    sqlite3_busy_timeout(db, 1000);
    if (rc == SQLITE_OK || (rc != SQLITE_BUSY && rc != SQLITE_LOCKED))
        atomic_store_explicit(&checkpoint_state->finish, 0, memory_order_release);
}

static bool equal(Slice s, const char *literal) {
    size_t n = strlen(literal);
    return s.n == n && memcmp(s.p, literal, n) == 0;
}
static void reserve(Buffer *b, size_t extra) {
    if (b->failed || extra > SIZE_MAX - b->len) { b->failed = true; return; }
    if (b->len + extra <= b->cap) return;
    size_t capacity = b->cap ? b->cap : 8192;
    while (capacity < b->len + extra) {
        if (capacity > SIZE_MAX / 2) { b->failed = true; return; }
        capacity *= 2;
    }
    char *p = realloc(b->p, capacity);
    if (!p) { b->failed = true; return; }
    b->p = p; b->cap = capacity;
}
static void append(Buffer *b, const char *s, size_t n) {
    reserve(b, n);
    if (!b->failed) { memcpy(b->p + b->len, s, n); b->len += n; }
}
#define TEXT(b, s) append((b), (s), sizeof(s) - 1)
static void number(Buffer *b, uint64_t value) {
    char digits[24]; size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    reserve(b, n);
    if (!b->failed) while (n) b->p[b->len++] = digits[--n];
}
static void string(Buffer *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    TEXT(b, "\"");
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c != '"' && c != '\\') continue;
        append(b, s + start, i - start);
        if (c == '"') TEXT(b, "\\\"");
        else if (c == '\\') TEXT(b, "\\\\");
        else { char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]}; append(b, escaped, 6); }
        start = i + 1;
    }
    append(b, s + start, n - start);
    TEXT(b, "\"");
}
void api_error(Reply *r, int status, const char *message) {
    r->b.len = 256; r->status = status; r->pending_commit = false;
    TEXT(&r->b, "{\"error\":"); string(&r->b, message, strlen(message)); TEXT(&r->b, "}");
}
static void internal_error(Reply *r) { api_error(r, 500, "internal server error"); }
static bool begin_batch(void) {
    if (transaction_failed) return false;
    if (transaction_open) return sqlite3_get_autocommit(db) == 0;
    int rc = sqlite3_step(begin_stmt); sqlite3_reset(begin_stmt);
    if (rc != SQLITE_DONE) return false;
    transaction_open = true; return true;
}
int api_commit_batch(void) {
    if (!transaction_open) return transaction_failed ? -1 : 0;
    int rc = transaction_failed ? SQLITE_ERROR : sqlite3_step(commit_stmt);
    sqlite3_reset(commit_stmt);
    if (rc != SQLITE_DONE || !sqlite3_get_autocommit(db)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        transaction_open = transaction_failed = false;
        return -1;
    }
    transaction_open = transaction_failed = false; return 0;
}
static bool positive_integer(Slice s, sqlite3_int64 *value) {
    if (!s.n || s.n > 18) return false;
    uint64_t number_value = 0;
    for (size_t i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c < '0' || c > '9') return false;
        number_value = number_value * 10 + (uint64_t)(c - '0');
    }
    if (!number_value || number_value > INT64_MAX) return false;
    *value = (sqlite3_int64)number_value; return true;
}
static int b64digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
static bool decode64(Slice source, unsigned char *out, size_t capacity, size_t *length) {
    if (source.n % 4 == 1) return false;
    uint32_t bits = 0; unsigned count = 0; size_t written = 0;
    for (size_t i = 0; i < source.n; i++) {
        int digit = b64digit((unsigned char)source.p[i]);
        if (digit < 0) return false;
        bits = (bits << 6) | (unsigned)digit; count += 6;
        if (count >= 8) {
            count -= 8;
            if (written >= capacity) return false;
            out[written++] = (unsigned char)(bits >> count);
        }
    }
    if (count && (bits & ((1U << count) - 1U))) return false;
    *length = written; return true;
}
typedef struct { sqlite3_int64 id; const char *username; size_t username_len; yyjson_doc *doc; } User;
static int authenticate(Slice authorization, User *user) {
    if (authorization.n < 7 || memcmp(authorization.p, "Bearer ", 7)) return 1;
    Slice token = {authorization.p + 7, authorization.n - 7};
    const char *first = memchr(token.p, '.', token.n);
    if (!first) return 2;
    const char *second = memchr(first + 1, '.', token.n - (size_t)(first + 1 - token.p));
    if (!second) return 2;
    Slice h = {token.p, (size_t)(first - token.p)};
    Slice p = {first + 1, (size_t)(second - first - 1)};
    Slice s = {second + 1, token.n - (size_t)(second + 1 - token.p)};
    if (h.n > 2048 || p.n > 24576 || s.n != 43) return 2;
    unsigned char header[1536], payload[18432], signature[32], calculated[32], inner[32];
    size_t hn, pn, sn;
    if (!decode64(h, header, sizeof(header), &hn) || !decode64(p, payload, sizeof(payload), &pn) ||
        !decode64(s, signature, sizeof(signature), &sn) || sn != 32) return 2;
    yyjson_doc *header_doc = yyjson_read((const char *)header, hn, 0);
    if (!header_doc) return 2;
    yyjson_val *algorithm = yyjson_obj_get(yyjson_doc_get_root(header_doc), "alg");
    bool allowed = yyjson_is_str(algorithm) && yyjson_get_len(algorithm) == 5 &&
                   memcmp(yyjson_get_str(algorithm), "HS256", 5) == 0;
    yyjson_doc_free(header_doc);
    if (!allowed) return 2;
    SHA256_CTX sha = hmac_inner;
    SHA256_Update(&sha, token.p, (size_t)(second - token.p)); SHA256_Final(inner, &sha);
    sha = hmac_outer; SHA256_Update(&sha, inner, sizeof(inner)); SHA256_Final(calculated, &sha);
    if (CRYPTO_memcmp(signature, calculated, 32)) return 2;
    yyjson_doc *doc = yyjson_read((const char *)payload, pn, 0);
    if (!doc) return 2;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *expiry = yyjson_obj_get(root, "exp");
    double expiration = yyjson_get_num(expiry);
    if (!yyjson_is_num(expiry) || !isfinite(expiration) || expiration <= (double)time(NULL)) {
        yyjson_doc_free(doc); return 2;
    }
    yyjson_val *sub = yyjson_obj_get(root, "sub"), *name = yyjson_obj_get(root, "username");
    Slice id = {yyjson_get_str(sub), yyjson_get_len(sub)};
    if (!yyjson_is_str(sub) || !positive_integer(id, &user->id) || !yyjson_is_str(name)) {
        yyjson_doc_free(doc); return 3;
    }
    user->username = yyjson_get_str(name); user->username_len = yyjson_get_len(name); user->doc = doc;
    return 0;
}
static bool whitespace(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
           c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
static Slice trim_body(const char *s, size_t len, size_t *characters) {
    size_t start = len, end = 0, count = 0, kept_count = 0;
    for (size_t i = 0; i < len;) {
        size_t at = i; uint32_t c = (unsigned char)s[i++];
        if (c >= 0xc0) {
            unsigned remaining;
            if (c < 0xe0) { remaining = 1; c &= 31; }
            else if (c < 0xf0) { remaining = 2; c &= 15; }
            else { remaining = 3; c &= 7; }
            while (remaining--) c = (c << 6) | ((unsigned char)s[i++] & 63U);
        }
        if (!whitespace(c)) {
            if (start == len) start = at;
            end = i; kept_count = ++count;
        } else if (start != len) count++;
    }
    *characters = kept_count;
    return (Slice){s + start, end >= start ? end - start : 0};
}
static void post_object_count(Buffer *b, sqlite3_stmt *stmt, uint64_t count) {
    TEXT(b, "{\"id\":"); number(b, (uint64_t)sqlite3_column_int64(stmt, 0));
    TEXT(b, ",\"body\":"); string(b, (const char *)sqlite3_column_text(stmt, 1), (size_t)sqlite3_column_bytes(stmt, 1));
    TEXT(b, ",\"created_at\":"); string(b, (const char *)sqlite3_column_text(stmt, 2), (size_t)sqlite3_column_bytes(stmt, 2));
    TEXT(b, ",\"author\":"); string(b, (const char *)sqlite3_column_text(stmt, 3), (size_t)sqlite3_column_bytes(stmt, 3));
    TEXT(b, ",\"like_count\":"); number(b, count); TEXT(b, "}");
}
static void post_object(Buffer *b, sqlite3_stmt *stmt) {
    post_object_count(b, stmt, (uint64_t)sqlite3_column_int64(stmt, 4));
}
typedef struct { Buffer *buffer; bool first; } NativeOutput;
static void native_post_output(void *context, const NativePost *post) {
    NativeOutput *output = context; Buffer *b = output->buffer;
    if (!output->first) TEXT(b, ",");
    output->first = false;
    TEXT(b, "{\"id\":"); number(b, (uint64_t)post->id);
    TEXT(b, ",\"body\":"); string(b, post->body, post->body_len);
    TEXT(b, ",\"created_at\":"); string(b, post->created_at, post->created_at_len);
    TEXT(b, ",\"author\":"); string(b, post->author, post->author_len);
    TEXT(b, ",\"like_count\":"); number(b, post->like_count); TEXT(b, "}");
}
static void read_feed_reference(Reply *r) {
    if (!begin_batch()) { internal_error(r); return; }
    TEXT(&r->b, "{\"posts\":["); bool first = true; int rc;
    while ((rc = sqlite3_step(feed_stmt)) == SQLITE_ROW) {
        if (!first) TEXT(&r->b, ",");
        first = false; post_object(&r->b, feed_stmt);
    }
    sqlite3_reset(feed_stmt);
    if (rc != SQLITE_DONE) { internal_error(r); return; }
    r->pending_commit = true;
    TEXT(&r->b, "]}");
}
static void read_feed(Reply *r) {
    if (!begin_batch()) { internal_error(r); return; }
    sqlite3_int64 ids[20], low = INT64_MAX, high = 0;
    unsigned count = 0; int rc;
    while ((rc = sqlite3_step(feed_ids_stmt)) == SQLITE_ROW && count < 20) {
        sqlite3_int64 id = sqlite3_column_int64(feed_ids_stmt, 0);
        ids[count++] = id;
        if (id < low) low = id;
        if (id > high) high = id;
    }
    sqlite3_reset(feed_ids_stmt);
    if (rc != SQLITE_DONE) { internal_error(r); return; }
    if (!count) { TEXT(&r->b, "{\"posts\":[]}"); r->pending_commit = true; return; }
    if (native_plan) {
        size_t restore = r->b.len;
        TEXT(&r->b, "{\"posts\":[");
        NativeOutput output = {&r->b, true};
        rc = native_read_posts(native_plan, ids, count, native_post_output, &output);
        if (rc == SQLITE_OK) { native_reads++; TEXT(&r->b, "]}"); r->pending_commit = true; return; }
        r->b.len = restore;
        native_fallbacks++;
        read_feed_reference(r); return;
    }
    /* Select the actual newest ids every time. A bounded range is an optional
       query plan, not an assumption about rowid density or timestamp order. */
    if (low < 1 || high - low > 64) { read_feed_reference(r); return; }
    int positions[65]; for (unsigned i = 0; i < 65; i++) positions[i] = -1;
    uint64_t counts[20] = {0};
    size_t offsets[20] = {0}, lengths[20] = {0};
    for (unsigned i = 0; i < count; i++) positions[ids[i] - low] = (int)i;
    sqlite3_bind_int64(feed_counts_stmt, 1, low); sqlite3_bind_int64(feed_counts_stmt, 2, high);
    while ((rc = sqlite3_step(feed_counts_stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(feed_counts_stmt, 0);
        int position = positions[id - low];
        if (position >= 0) counts[position] = (uint64_t)sqlite3_column_int64(feed_counts_stmt, 1);
    }
    sqlite3_reset(feed_counts_stmt);
    if (rc != SQLITE_DONE) { internal_error(r); return; }
    Buffer arena = {0}; uint32_t seen = 0;
    sqlite3_bind_int64(feed_range_stmt, 1, low); sqlite3_bind_int64(feed_range_stmt, 2, high);
    while ((rc = sqlite3_step(feed_range_stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(feed_range_stmt, 0);
        int position = positions[id - low];
        if (position >= 0) {
            offsets[position] = arena.len;
            post_object_count(&arena, feed_range_stmt, counts[position]);
            lengths[position] = arena.len - offsets[position];
            seen |= 1U << (unsigned)position;
        }
    }
    sqlite3_reset(feed_range_stmt);
    if (rc != SQLITE_DONE || arena.failed) { free(arena.p); internal_error(r); return; }
    if (seen != (1U << count) - 1U) { free(arena.p); read_feed_reference(r); return; }
    TEXT(&r->b, "{\"posts\":[");
    for (unsigned i = 0; i < count; i++) {
        if (i) TEXT(&r->b, ",");
        append(&r->b, arena.p + offsets[i], lengths[i]);
    }
    TEXT(&r->b, "]}"); free(arena.p); r->pending_commit = true;
}
static void read_post(Reply *r, sqlite3_int64 id) {
    if (!begin_batch()) { internal_error(r); return; }
    if (native_plan) {
        size_t restore = r->b.len;
        TEXT(&r->b, "{\"post\":");
        NativeOutput output = {&r->b, true};
        int native_rc = native_read_posts(native_plan, &id, 1, native_post_output, &output);
        if (native_rc == SQLITE_OK) { native_reads++; TEXT(&r->b, "}"); r->pending_commit = true; return; }
        r->b.len = restore;
        native_fallbacks++;
    }
    sqlite3_bind_int64(post_stmt, 1, id);
    int rc = sqlite3_step(post_stmt);
    if (rc == SQLITE_ROW) { TEXT(&r->b, "{\"post\":"); post_object(&r->b, post_stmt); TEXT(&r->b, "}"); }
    else if (rc == SQLITE_DONE) api_error(r, 404, "post not found");
    else internal_error(r);
    sqlite3_reset(post_stmt);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) r->pending_commit = true;
}
static void create_post(Reply *r, User *user, Slice body) {
    yyjson_doc *doc = yyjson_read(body.p, body.n, 0);
    if (!doc) { api_error(r, 400, "malformed JSON body"); return; }
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), "body");
    if (!yyjson_is_str(value)) { yyjson_doc_free(doc); api_error(r, 400, "body is required"); return; }
    size_t characters;
    Slice trimmed = trim_body(yyjson_get_str(value), yyjson_get_len(value), &characters);
    if (!trimmed.n) { yyjson_doc_free(doc); api_error(r, 400, "body is required"); return; }
    if (characters > 500) { yyjson_doc_free(doc); api_error(r, 400, "body must be at most 500 characters"); return; }
    if (!begin_batch()) { yyjson_doc_free(doc); internal_error(r); return; }
    sqlite3_bind_int64(create_stmt, 1, user->id);
    sqlite3_bind_text(create_stmt, 2, trimmed.p, (int)trimmed.n, SQLITE_STATIC);
    int rc = sqlite3_step(create_stmt);
    if (rc == SQLITE_ROW) {
        TEXT(&r->b, "{\"post\":{\"id\":"); number(&r->b, (uint64_t)sqlite3_column_int64(create_stmt, 0));
        TEXT(&r->b, ",\"body\":"); string(&r->b, trimmed.p, trimmed.n);
        TEXT(&r->b, ",\"created_at\":"); string(&r->b, (const char *)sqlite3_column_text(create_stmt, 1), (size_t)sqlite3_column_bytes(create_stmt, 1));
        TEXT(&r->b, ",\"author\":"); string(&r->b, user->username, user->username_len);
        TEXT(&r->b, ",\"like_count\":0}}");
        rc = sqlite3_step(create_stmt); /* Finish RETURNING before batch COMMIT. */
    }
    sqlite3_reset(create_stmt); sqlite3_clear_bindings(create_stmt); yyjson_doc_free(doc);
    if (rc != SQLITE_DONE || sqlite3_get_autocommit(db)) {
        if (sqlite3_get_autocommit(db)) transaction_failed = true;
        internal_error(r);
    } else { r->status = 201; r->pending_commit = true; }
}
static void like_post(Reply *r, User *user, sqlite3_int64 id) {
    if (!begin_batch()) { internal_error(r); return; }
    sqlite3_bind_int64(like_stmt, 1, user->id); sqlite3_bind_int64(like_stmt, 2, id);
    int rc = sqlite3_step(like_stmt), changed = sqlite3_changes(db);
    sqlite3_reset(like_stmt);
    if (rc != SQLITE_DONE || sqlite3_get_autocommit(db)) {
        if (sqlite3_get_autocommit(db)) transaction_failed = true;
        internal_error(r); return;
    }
    if (!changed) {
        sqlite3_bind_int64(exists_stmt, 1, id); rc = sqlite3_step(exists_stmt); sqlite3_reset(exists_stmt);
        if (rc == SQLITE_DONE) { api_error(r, 404, "post not found"); return; }
        if (rc != SQLITE_ROW) { internal_error(r); return; }
    }
    r->status = changed ? 201 : 200;
    r->pending_commit = true;
    TEXT(&r->b, "{\"liked\":true,\"already_liked\":");
    if (changed) TEXT(&r->b, "false"); else TEXT(&r->b, "true");
    TEXT(&r->b, ",\"post_id\":"); number(&r->b, (uint64_t)id); TEXT(&r->b, "}");
}
void api_handle(Slice method, Slice path, Slice authorization, Slice body, Reply *r) {
    r->b.len = 256; r->status = 200;
    const char *query = memchr(path.p, '?', path.n);
    if (query) path.n = (size_t)(query - path.p);
    if (equal(method, "GET") && equal(path, "/health")) {
        int rc = sqlite3_step(health_stmt); sqlite3_reset(health_stmt);
        if (rc != SQLITE_ROW) {
            r->status = 503;
            TEXT(&r->b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
            string(&r->b, sqlite3_errmsg(db), strlen(sqlite3_errmsg(db))); TEXT(&r->b, "}"); return;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        time_t uptime = now.tv_sec - started.tv_sec - (now.tv_nsec < started.tv_nsec ? 1 : 0);
        TEXT(&r->b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
        number(&r->b, (uint64_t)(uptime < 0 ? 0 : uptime)); TEXT(&r->b, "}"); return;
    }
    if (equal(method, "GET") && equal(path, "/feed")) { read_feed(r); return; }
    bool create = equal(method, "POST") && equal(path, "/posts");
    bool prefix = path.n > 7 && memcmp(path.p, "/posts/", 7) == 0;
    bool like = equal(method, "POST") && prefix && path.n >= 12 && memcmp(path.p + path.n - 5, "/like", 5) == 0;
    bool get_post = equal(method, "GET") && prefix && !memchr(path.p + 7, '/', path.n - 7);
    if (!create && !like && !get_post) { api_error(r, 404, "not found"); return; }
    User user = {0};
    if (create || like) {
        int auth = authenticate(authorization, &user);
        if (auth) {
            const char *message = auth == 1 ? "missing bearer token" : auth == 2 ? "invalid or expired token" : "invalid token payload";
            api_error(r, 401, message); return;
        }
    }
    if (create) create_post(r, &user, body);
    else {
        Slice id_text = {path.p + 7, path.n - 7 - (like ? 5U : 0U)}; sqlite3_int64 id;
        if (!positive_integer(id_text, &id)) api_error(r, 400, "invalid post id");
        else if (like) like_post(r, &user, id);
        else read_post(r, id);
    }
    if (user.doc) yyjson_doc_free(user.doc);
}
int api_finish(Reply *r, bool close_connection, size_t *offset, size_t *length) {
    if (r->b.failed) return -1;
    const char *reason = r->status == 200 ? "OK" : r->status == 201 ? "Created" :
        r->status == 400 ? "Bad Request" : r->status == 401 ? "Unauthorized" :
        r->status == 404 ? "Not Found" : r->status == 503 ? "Service Unavailable" : "Internal Server Error";
    char header[256];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
                     r->status, reason, r->b.len - 256, close_connection ? "close" : "keep-alive");
    if (n < 0 || (size_t)n >= sizeof(header)) return -1;
    *offset = 256 - (size_t)n; *length = r->b.len - *offset;
    memcpy(r->b.p + *offset, header, (size_t)n); return 0;
}
static int prepare(sqlite3 *connection, sqlite3_stmt **stmt, const char *sql) {
    return sqlite3_prepare_v3(connection, sql, -1, SQLITE_PREPARE_PERSISTENT, stmt, NULL);
}
int api_init(const char *path, const char *secret) {
    /* Fork before this process initializes SQLite: each process owns its own
       single-threaded SQLite state and its own database connection. */
    if (start_checkpoint(path)) { fprintf(stderr, "checkpoint helper initialization failed\n"); return -1; }
    clock_gettime(CLOCK_MONOTONIC, &started);
    unsigned char key[64] = {0}, pad[64]; size_t key_length = strlen(secret);
    if (key_length > 64) SHA256((const unsigned char *)secret, key_length, key);
    else memcpy(key, secret, key_length);
    for (size_t i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    SHA256_Init(&hmac_inner); SHA256_Update(&hmac_inner, pad, 64);
    for (size_t i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5c;
    SHA256_Init(&hmac_outer); SHA256_Update(&hmac_outer, pad, 64);
    OPENSSL_cleanse(key, sizeof(key)); OPENSSL_cleanse(pad, sizeof(pad));
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(db, 1000);
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;"
                        "PRAGMA cache_size=-98304; PRAGMA mmap_size=536870912; PRAGMA wal_autocheckpoint=0;",
                     NULL, NULL, NULL) != SQLITE_OK) goto fail;
    const char *select = "SELECT p.id,p.body,p.created_at,u.username,"
        "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ";
    char sql[512];
    snprintf(sql, sizeof(sql), "%s ORDER BY p.created_at DESC,p.id DESC LIMIT 20", select);
    if (prepare(db, &feed_stmt, sql) != SQLITE_OK) goto fail;
    snprintf(sql, sizeof(sql), "%s WHERE p.id=?1", select);
    if (prepare(db, &post_stmt, sql) != SQLITE_OK ||
        prepare(db, &create_stmt, "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at") != SQLITE_OK ||
        prepare(db, &like_stmt, "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING") != SQLITE_OK ||
        prepare(db, &exists_stmt, "SELECT 1 FROM posts WHERE id=?1") != SQLITE_OK ||
        prepare(db, &health_stmt, "SELECT 1") != SQLITE_OK ||
        prepare(db, &feed_ids_stmt, "SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20") != SQLITE_OK ||
        prepare(db, &feed_counts_stmt, "SELECT post_id,count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id") != SQLITE_OK ||
        prepare(db, &feed_range_stmt, "SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2") != SQLITE_OK ||
        prepare(db, &begin_stmt, "BEGIN IMMEDIATE") != SQLITE_OK ||
        prepare(db, &commit_stmt, "COMMIT") != SQLITE_OK) goto fail;
    /* Notify the helper only after committed frames are visible. It uses its
       own SQLite process state; the fallback also runs after a commit. */
    sqlite3_wal_hook(db, checkpoint_notify, NULL);
    native_plan = native_plan_open(db);
    return 0;
fail:
    fprintf(stderr, "SQLite initialization failed: %s\n", db ? sqlite3_errmsg(db) : "open failed");
    api_shutdown(); return -1;
}
void api_shutdown(void) {
    stop_checkpoint();
    native_plan_close(native_plan); native_plan = NULL;
    sqlite3_finalize(feed_stmt); sqlite3_finalize(post_stmt); sqlite3_finalize(create_stmt);
    sqlite3_finalize(like_stmt); sqlite3_finalize(exists_stmt); sqlite3_finalize(health_stmt);
    sqlite3_finalize(begin_stmt); sqlite3_finalize(commit_stmt);
    sqlite3_finalize(feed_ids_stmt); sqlite3_finalize(feed_range_stmt); sqlite3_finalize(feed_counts_stmt);
    feed_stmt = post_stmt = create_stmt = like_stmt = exists_stmt = health_stmt = NULL;
    begin_stmt = commit_stmt = NULL;
    feed_ids_stmt = feed_range_stmt = feed_counts_stmt = NULL;
    if (db && transaction_open) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    transaction_open = transaction_failed = false;
    if (db) { sqlite3_close(db); db = NULL; }
}
void api_log_diagnostics(void) {
    fprintf(stderr, "native_reads=%llu native_fallbacks=%llu native_plan=%s\n",
            (unsigned long long)native_reads, (unsigned long long)native_fallbacks, native_plan ? "enabled" : "disabled");
    int used = 0, high = 0, misses = 0, hits = 0;
    sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_USED, &used, &high, 0);
    sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_MISS, &misses, &high, 0);
    sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_HIT, &hits, &high, 0);
    fprintf(stderr, "sqlite_cache_bytes=%d cache_hits=%d cache_misses=%d\n", used, hits, misses);
    fprintf(stderr, "wal_frames=%d checkpoint_fallbacks=%u\n", wal_frames, checkpoint_fallbacks);
}
