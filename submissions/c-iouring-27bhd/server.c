#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <liburing.h>

#include "sqlite3.h"
#include "auth.h"
#include "json.h"
#include "http.h"

#define MAX_CONNECTIONS 65536
#define BUF_RING_SIZE 4096
#define BUF_SIZE 4096
#define BUF_GROUP_ID 1
#define MAX_PENDING_WRITES 512

enum {
    EV_ACCEPT = 0,
    EV_RECV,
    EV_SEND,
};

typedef struct {
    uint8_t ev_type;
    uint8_t flags;
    uint16_t extra;
    int32_t fd_slot;
} __attribute__((packed)) user_data_t;

typedef struct {
    int32_t real_fd;
    int16_t resp_buf_idx;
    int16_t part_buf_idx:12;
    uint16_t active:1;
    uint16_t keep_alive:1;
    uint16_t has_pending_send:1;
    uint16_t write_queued:1;
    char *send_buf;
} conn_slot_t;

_Static_assert(sizeof(conn_slot_t) == 16, "conn_slot_t must be 16 bytes");

static conn_slot_t g_conns[MAX_CONNECTIONS];
static int g_registered_files = 0;
static int g_direct_fds[MAX_CONNECTIONS];
static bool g_pgo_training = false;
static time_t g_now = 0;

static int32_t g_free_slots[MAX_CONNECTIONS];
static int g_free_top = 0;

#define RESP_POOL_SIZE 1024
#define RESP_BUF_SIZE 16384
#define HDR_RESERVE 128
static char g_resp_pool[RESP_POOL_SIZE][RESP_BUF_SIZE] __attribute__((aligned(64)));
static int16_t g_resp_pool_stack[RESP_POOL_SIZE];
static int g_resp_pool_top = 0;

#define PART_POOL_SIZE 512
static char g_part_pool[PART_POOL_SIZE][BUF_SIZE] __attribute__((aligned(64)));
static uint16_t g_part_len[PART_POOL_SIZE];
static int16_t g_part_pool_stack[PART_POOL_SIZE];
static int g_part_pool_top = 0;

static inline int alloc_resp_buf(void) {
    if (g_resp_pool_top > 0) return g_resp_pool_stack[--g_resp_pool_top];
    return -1;
}

static inline void free_resp_buf(int idx) {
    if (idx >= 0 && idx < RESP_POOL_SIZE) {
        g_resp_pool_stack[g_resp_pool_top++] = (int16_t)idx;
    }
}

static inline int alloc_part_buf(void) {
    if (g_part_pool_top > 0) return g_part_pool_stack[--g_part_pool_top];
    return -1;
}

static inline void free_part_buf(int idx) {
    if (idx >= 0 && idx < PART_POOL_SIZE) {
        g_part_len[idx] = 0;
        g_part_pool_stack[g_part_pool_top++] = (int16_t)idx;
    }
}

static inline char *acquire_resp_buf(int32_t slot, bool *is_dynamic) {
    int idx = alloc_resp_buf();
    if (idx >= 0) {
        g_conns[slot].resp_buf_idx = (int16_t)idx;
        *is_dynamic = false;
        return g_resp_pool[idx];
    }
    g_conns[slot].resp_buf_idx = -1;
    *is_dynamic = true;
    return (char *)malloc(RESP_BUF_SIZE);
}

static inline size_t u64_to_str(char *out, uint64_t val) {
    char tmp[20];
    int i = 0;
    do {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val > 0);
    for (int j = 0; j < i; j++) {
        out[j] = tmp[i - 1 - j];
    }
    return (size_t)i;
}

static inline size_t i64_to_str(char *out, int64_t val) {
    if (val < 0) {
        *out = '-';
        return 1 + u64_to_str(out + 1, (uint64_t)(-val));
    }
    return u64_to_str(out, (uint64_t)val);
}

static inline char *prepend_http_header(
    char *body_ptr,
    size_t body_len,
    const char *prefix,
    size_t prefix_len,
    size_t *out_total_len
) {
    char cl_buf[24];
    size_t cl_len = u64_to_str(cl_buf, (uint64_t)body_len);
    size_t hlen = prefix_len + cl_len + 4;
    char *start = body_ptr - hlen;
    memcpy(start, prefix, prefix_len);
    memcpy(start + prefix_len, cl_buf, cl_len);
    memcpy(start + prefix_len + cl_len, "\r\n\r\n", 4);
    *out_total_len = hlen + body_len;
    return start;
}

static const char HDR_200_PREFIX[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ";
static const char HDR_201_PREFIX[] =
    "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ";
static const char HDR_503_PREFIX[] =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ";

static struct io_uring g_ring;
static struct io_uring_buf_ring *g_buf_ring = NULL;
static uint8_t g_buffers[BUF_RING_SIZE][BUF_SIZE] __attribute__((aligned(64)));

static hmac_midstate_t g_jwt_mid;
static sqlite3 *g_db = NULL;
static sqlite3_stmt *stmt_begin = NULL;
static sqlite3_stmt *stmt_commit = NULL;
static sqlite3_stmt *stmt_health = NULL;
static sqlite3_stmt *stmt_feed = NULL;
static sqlite3_stmt *stmt_post = NULL;
static sqlite3_stmt *stmt_insert_post = NULL;
static sqlite3_stmt *stmt_insert_like = NULL;
static sqlite3_stmt *stmt_post_exists = NULL;
static time_t g_start_time;

typedef struct {
    int32_t fd_slot;
    int64_t user_id;
    char author[256];
    size_t author_len;
    char body[2048];
    size_t body_len;
    int route;
    int64_t post_id;
    int status_code;
    int64_t new_post_id;
} pending_write_t;

static pending_write_t g_pending_writes[MAX_PENDING_WRITES];
static int g_pending_write_count = 0;
static const char *RESP_400_INVALID_POST_ID =
    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 27\r\n\r\n{\"error\":\"invalid post id\"}";

static const char *RESP_404_POST_NOT_FOUND =
    "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";

static const char *RESP_404_NOT_FOUND =
    "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}";

static const char *RESP_401_MISSING_TOKEN =
    "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 32\r\n\r\n{\"error\":\"missing bearer token\"}";

static const char *RESP_401_INVALID_OR_EXPIRED =
    "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 36\r\n\r\n{\"error\":\"invalid or expired token\"}";

static const char *RESP_401_INVALID_PAYLOAD =
    "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 33\r\n\r\n{\"error\":\"invalid token payload\"}";

static const char *RESP_400_MALFORMED_JSON =
    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 31\r\n\r\n{\"error\":\"malformed JSON body\"}";

static const char *RESP_400_BODY_REQUIRED =
    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 28\r\n\r\n{\"error\":\"body is required\"}";

static const char *RESP_400_BODY_TOO_LONG =
    "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 47\r\n\r\n{\"error\":\"body must be at most 500 characters\"}";

static const char *RESP_500_INTERNAL =
    "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";

static inline uint64_t pack_user_data(uint8_t ev, int32_t slot, uint16_t extra) {
    user_data_t u;
    u.ev_type = ev;
    u.flags = 0;
    u.extra = extra;
    u.fd_slot = slot;
    uint64_t val = 0;
    memcpy(&val, &u, sizeof(u));
    return val;
}

static inline void unpack_user_data(uint64_t val, uint8_t *ev, int32_t *slot, uint16_t *extra) {
    user_data_t u;
    memcpy(&u, &val, sizeof(u));
    *ev = u.ev_type;
    *slot = u.fd_slot;
    *extra = u.extra;
}

static void __attribute__((hot)) queue_send_ex(int32_t slot, const char *data, size_t len, char *dynamic_base);
static inline void queue_send(int32_t slot, const char *data, size_t len, bool dynamic_alloc) {
    queue_send_ex(slot, data, len, dynamic_alloc ? (char *)data : NULL);
}
static void __attribute__((hot)) submit_recv(int32_t slot);
static void __attribute__((hot)) close_connection(int32_t slot);
static void __attribute__((hot)) handle_request(int32_t slot, char *buf, size_t len);
static void __attribute__((hot)) flush_group_commit(void);
static void __attribute__((hot)) handle_feed(int32_t slot);
static void __attribute__((hot)) handle_get_post(int32_t slot, int64_t post_id);
static void __attribute__((hot)) handle_health(int32_t slot);

static void return_buffer(uint16_t bid) {
    if (g_buf_ring) {
        io_uring_buf_ring_add(g_buf_ring, g_buffers[bid], BUF_SIZE, bid, io_uring_buf_ring_mask(BUF_RING_SIZE), 0);
        io_uring_buf_ring_advance(g_buf_ring, 1);
    }
}

static void handle_feed(int32_t slot) {
    bool is_dynamic = false;
    char *buf = acquire_resp_buf(slot, &is_dynamic);
    if (!buf) {
        queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
        return;
    }

    char *body_start = buf + HDR_RESERVE;
    char *p = body_start;
    memcpy(p, "{\"posts\":[", 10);
    p += 10;

    bool first = true;
    while (sqlite3_step(stmt_feed) == SQLITE_ROW) {
        if (!first) *p++ = ',';
        first = false;

        int64_t id = sqlite3_column_int64(stmt_feed, 0);
        const char *body = (const char *)sqlite3_column_text(stmt_feed, 1);
        int body_bytes = sqlite3_column_bytes(stmt_feed, 1);
        const char *created_at = (const char *)sqlite3_column_text(stmt_feed, 2);
        int created_bytes = sqlite3_column_bytes(stmt_feed, 2);
        const char *author = (const char *)sqlite3_column_text(stmt_feed, 3);
        int author_bytes = sqlite3_column_bytes(stmt_feed, 3);
        int64_t like_count = sqlite3_column_int64(stmt_feed, 4);

        memcpy(p, "{\"id\":", 6); p += 6;
        p += i64_to_str(p, id);
        memcpy(p, ",\"body\":\"", 9); p += 9;
        p += json_escape_string(body, (size_t)body_bytes, p, 3072);
        memcpy(p, "\",\"created_at\":\"", 16); p += 16;
        memcpy(p, created_at, (size_t)created_bytes); p += created_bytes;
        memcpy(p, "\",\"author\":\"", 12); p += 12;
        memcpy(p, author, (size_t)author_bytes); p += author_bytes;
        memcpy(p, "\",\"like_count\":", 15); p += 15;
        p += i64_to_str(p, like_count);
        *p++ = '}';
    }
    sqlite3_reset(stmt_feed);
    *p++ = ']';
    *p++ = '}';

    size_t body_len = (size_t)(p - body_start);
    size_t total_len = 0;
    char *send_ptr = prepend_http_header(body_start, body_len, HDR_200_PREFIX, sizeof(HDR_200_PREFIX) - 1, &total_len);
    queue_send_ex(slot, send_ptr, total_len, is_dynamic ? buf : NULL);
}

static void handle_get_post(int32_t slot, int64_t post_id) {
    sqlite3_bind_int64(stmt_post, 1, post_id);

    if (sqlite3_step(stmt_post) != SQLITE_ROW) {
        sqlite3_reset(stmt_post);
        queue_send(slot, RESP_404_POST_NOT_FOUND, strlen(RESP_404_POST_NOT_FOUND), false);
        return;
    }

    bool is_dynamic = false;
    char *buf = acquire_resp_buf(slot, &is_dynamic);
    if (!buf) {
        sqlite3_reset(stmt_post);
        queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
        return;
    }

    char *body_start = buf + HDR_RESERVE;
    char *p = body_start;
    int64_t id = sqlite3_column_int64(stmt_post, 0);
    const char *body = (const char *)sqlite3_column_text(stmt_post, 1);
    int body_bytes = sqlite3_column_bytes(stmt_post, 1);
    const char *created_at = (const char *)sqlite3_column_text(stmt_post, 2);
    int created_bytes = sqlite3_column_bytes(stmt_post, 2);
    const char *author = (const char *)sqlite3_column_text(stmt_post, 3);
    int author_bytes = sqlite3_column_bytes(stmt_post, 3);
    int64_t like_count = sqlite3_column_int64(stmt_post, 4);

    memcpy(p, "{\"post\":{\"id\":", 14); p += 14;
    p += i64_to_str(p, id);
    memcpy(p, ",\"body\":\"", 9); p += 9;
    p += json_escape_string(body, (size_t)body_bytes, p, 3072);
    memcpy(p, "\",\"created_at\":\"", 16); p += 16;
    memcpy(p, created_at, (size_t)created_bytes); p += created_bytes;
    memcpy(p, "\",\"author\":\"", 12); p += 12;
    memcpy(p, author, (size_t)author_bytes); p += author_bytes;
    memcpy(p, "\",\"like_count\":", 15); p += 15;
    p += i64_to_str(p, like_count);
    memcpy(p, "}}", 2); p += 2;
    sqlite3_reset(stmt_post);

    size_t body_len = (size_t)(p - body_start);
    size_t total_len = 0;
    char *send_ptr = prepend_http_header(body_start, body_len, HDR_200_PREFIX, sizeof(HDR_200_PREFIX) - 1, &total_len);
    queue_send_ex(slot, send_ptr, total_len, is_dynamic ? buf : NULL);
}

static void handle_health(int32_t slot) {
    int rc = sqlite3_step(stmt_health);
    sqlite3_reset(stmt_health);
    if (rc != SQLITE_ROW) {
        bool is_dynamic = false;
        char *buf = acquire_resp_buf(slot, &is_dynamic);
        if (!buf) {
            queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
            return;
        }
        char *body_start = buf + HDR_RESERVE;
        int elen = snprintf(body_start, 256, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"%s\"}", sqlite3_errmsg(g_db));
        size_t total_len = 0;
        char *send_ptr = prepend_http_header(body_start, (size_t)elen, HDR_503_PREFIX, sizeof(HDR_503_PREFIX) - 1, &total_len);
        queue_send_ex(slot, send_ptr, total_len, is_dynamic ? buf : NULL);
        return;
    }

    bool is_dynamic = false;
    char *buf = acquire_resp_buf(slot, &is_dynamic);
    if (!buf) {
        queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
        return;
    }
    char *body_start = buf + HDR_RESERVE;
    char *p = body_start;
    memcpy(p, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36); p += 36;
    p += i64_to_str(p, (int64_t)(g_now - g_start_time));
    *p++ = '}';

    size_t body_len = (size_t)(p - body_start);
    size_t total_len = 0;
    char *send_ptr = prepend_http_header(body_start, body_len, HDR_200_PREFIX, sizeof(HDR_200_PREFIX) - 1, &total_len);
    queue_send_ex(slot, send_ptr, total_len, is_dynamic ? buf : NULL);
}

static void flush_group_commit(void) {
    if (g_pending_write_count == 0) return;

    struct timespec bts;
    clock_gettime(CLOCK_REALTIME, &bts);
    struct tm btm;
    gmtime_r(&bts.tv_sec, &btm);
    char batch_created_at[32];
    int bclen = sprintf(batch_created_at, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                        btm.tm_year + 1900, btm.tm_mon + 1, btm.tm_mday,
                        btm.tm_hour, btm.tm_min, btm.tm_sec,
                        (int)(bts.tv_nsec / 1000000));

    sqlite3_step(stmt_begin);
    sqlite3_reset(stmt_begin);

    for (int i = 0; i < g_pending_write_count; i++) {
        pending_write_t *pw = &g_pending_writes[i];
        int32_t slot = pw->fd_slot;
        g_conns[slot].write_queued = 0;

        if (!g_conns[slot].active) {
            pw->status_code = -1;
            continue;
        }

        if (pw->route == ROUTE_CREATE_POST) {
            sqlite3_bind_int64(stmt_insert_post, 1, pw->user_id);
            sqlite3_bind_text(stmt_insert_post, 2, pw->body, (int)pw->body_len, SQLITE_STATIC);
            sqlite3_bind_text(stmt_insert_post, 3, batch_created_at, bclen, SQLITE_STATIC);

            if (sqlite3_step(stmt_insert_post) == SQLITE_ROW) {
                pw->status_code = 201;
                pw->new_post_id = sqlite3_column_int64(stmt_insert_post, 0);
                sqlite3_reset(stmt_insert_post);
            } else {
                pw->status_code = 500;
                sqlite3_reset(stmt_insert_post);
            }
        } else if (pw->route == ROUTE_LIKE_POST) {
            sqlite3_bind_int64(stmt_insert_like, 1, pw->user_id);
            sqlite3_bind_int64(stmt_insert_like, 2, pw->post_id);
            sqlite3_bind_text(stmt_insert_like, 3, batch_created_at, bclen, SQLITE_STATIC);
            sqlite3_step(stmt_insert_like);

            int changes = sqlite3_changes(g_db);
            sqlite3_reset(stmt_insert_like);

            if (changes == 1) {
                pw->status_code = 201;
            } else {
                sqlite3_bind_int64(stmt_post_exists, 1, pw->post_id);
                if (sqlite3_step(stmt_post_exists) == SQLITE_ROW) {
                    pw->status_code = 200;
                } else {
                    pw->status_code = 404;
                }
                sqlite3_reset(stmt_post_exists);
            }
        }
    }

    sqlite3_step(stmt_commit);
    sqlite3_reset(stmt_commit);

    for (int i = 0; i < g_pending_write_count; i++) {
        pending_write_t *pw = &g_pending_writes[i];
        int32_t slot = pw->fd_slot;
        if (!g_conns[slot].active || pw->status_code < 0) continue;

        if (pw->route == ROUTE_CREATE_POST) {
            if (pw->status_code == 201) {
                bool is_dynamic = false;
                char *buf = acquire_resp_buf(slot, &is_dynamic);
                if (!buf) {
                    queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
                    continue;
                }
                char *body_start = buf + HDR_RESERVE;
                char *p = body_start;
                memcpy(p, "{\"post\":{\"id\":", 14); p += 14;
                p += i64_to_str(p, pw->new_post_id);
                memcpy(p, ",\"body\":\"", 9); p += 9;
                p += json_escape_string(pw->body, pw->body_len, p, 3072);
                memcpy(p, "\",\"created_at\":\"", 16); p += 16;
                memcpy(p, batch_created_at, (size_t)bclen); p += bclen;
                memcpy(p, "\",\"author\":\"", 12); p += 12;
                memcpy(p, pw->author, pw->author_len); p += pw->author_len;
                memcpy(p, "\",\"like_count\":0}}", 18); p += 18;

                size_t blen = (size_t)(p - body_start);
                size_t total_len = 0;
                char *send_ptr = prepend_http_header(body_start, blen, HDR_201_PREFIX, sizeof(HDR_201_PREFIX) - 1, &total_len);
                queue_send_ex(slot, send_ptr, total_len, is_dynamic ? buf : NULL);
            } else {
                queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
            }
        } else if (pw->route == ROUTE_LIKE_POST) {
            if (pw->status_code == 201 || pw->status_code == 200) {
                bool is_dynamic = false;
                char *buf = acquire_resp_buf(slot, &is_dynamic);
                if (!buf) {
                    queue_send(slot, RESP_500_INTERNAL, strlen(RESP_500_INTERNAL), false);
                    continue;
                }
                char *body_start = buf + HDR_RESERVE;
                char *p = body_start;
                if (pw->status_code == 201) {
                    memcpy(p, "{\"liked\":true,\"already_liked\":false,\"post_id\":", 46);
                    p += 46;
                } else {
                    memcpy(p, "{\"liked\":true,\"already_liked\":true,\"post_id\":", 45);
                    p += 45;
                }
                p += i64_to_str(p, pw->post_id);
                *p++ = '}';

                size_t blen = (size_t)(p - body_start);
                size_t total_len = 0;
                const char *hdr_prefix = (pw->status_code == 201) ? HDR_201_PREFIX : HDR_200_PREFIX;
                size_t hdr_prefix_len = (pw->status_code == 201) ? (sizeof(HDR_201_PREFIX) - 1) : (sizeof(HDR_200_PREFIX) - 1);
                char *send_ptr = prepend_http_header(body_start, blen, hdr_prefix, hdr_prefix_len, &total_len);
                queue_send_ex(slot, send_ptr, total_len, is_dynamic ? buf : NULL);
            } else {
                queue_send(slot, RESP_404_POST_NOT_FOUND, strlen(RESP_404_POST_NOT_FOUND), false);
            }
        }
    }

    g_pending_write_count = 0;
}

static inline int get_free_slot(void) {
    if (g_free_top > 0) return g_free_slots[--g_free_top];
    return -1;
}

static void close_connection(int32_t slot) {
    if (slot < 0 || slot >= MAX_CONNECTIONS || !g_conns[slot].active) return;

    if (g_conns[slot].resp_buf_idx >= 0) {
        free_resp_buf(g_conns[slot].resp_buf_idx);
        g_conns[slot].resp_buf_idx = -1;
    }
    if (g_conns[slot].part_buf_idx >= 0) {
        free_part_buf(g_conns[slot].part_buf_idx);
        g_conns[slot].part_buf_idx = -1;
    }
    if (g_conns[slot].send_buf) {
        free(g_conns[slot].send_buf);
        g_conns[slot].send_buf = NULL;
    }

    int real_fd = g_conns[slot].real_fd;
    g_conns[slot].active = 0;
    g_conns[slot].has_pending_send = 0;
    g_conns[slot].write_queued = 0;
    g_conns[slot].real_fd = -1;

    g_free_slots[g_free_top++] = slot;

    if (slot < g_registered_files) {
        int empty_fd = -1;
        io_uring_register_files_update(&g_ring, slot, &empty_fd, 1);
    }
    if (real_fd >= 0) close(real_fd);
}

static void queue_send_ex(int32_t slot, const char *data, size_t len, char *dynamic_base) {
    if (g_pgo_training) {
        if (dynamic_base) free(dynamic_base);
        if (g_conns[slot].resp_buf_idx >= 0) {
            free_resp_buf(g_conns[slot].resp_buf_idx);
            g_conns[slot].resp_buf_idx = -1;
        }
        return;
    }

    if (!g_conns[slot].active) {
        if (dynamic_base) free(dynamic_base);
        if (g_conns[slot].resp_buf_idx >= 0) {
            free_resp_buf(g_conns[slot].resp_buf_idx);
            g_conns[slot].resp_buf_idx = -1;
        }
        return;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) {
        io_uring_submit(&g_ring);
        sqe = io_uring_get_sqe(&g_ring);
        if (!sqe) {
            if (dynamic_base) free(dynamic_base);
            if (g_conns[slot].resp_buf_idx >= 0) {
                free_resp_buf(g_conns[slot].resp_buf_idx);
                g_conns[slot].resp_buf_idx = -1;
            }
            close_connection(slot);
            return;
        }
    }

    g_conns[slot].has_pending_send = 1;
    g_conns[slot].send_buf = dynamic_base;

    if (slot < g_registered_files) {
        io_uring_prep_send(sqe, slot, data, len, MSG_NOSIGNAL);
        sqe->flags |= IOSQE_FIXED_FILE;
    } else {
        io_uring_prep_send(sqe, g_conns[slot].real_fd, data, len, MSG_NOSIGNAL);
    }
    io_uring_sqe_set_data64(sqe, pack_user_data(EV_SEND, slot, dynamic_base ? 1 : 0));
}

static void submit_recv(int32_t slot) {
    if (!g_conns[slot].active || g_conns[slot].has_pending_send || g_conns[slot].write_queued) return;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) {
        io_uring_submit(&g_ring);
        sqe = io_uring_get_sqe(&g_ring);
        if (!sqe) {
            close_connection(slot);
            return;
        }
    }

    int target_fd = (slot < g_registered_files) ? slot : g_conns[slot].real_fd;
    if (g_buf_ring) {
        io_uring_prep_recv(sqe, target_fd, NULL, BUF_SIZE - 1, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        if (slot < g_registered_files) sqe->flags |= IOSQE_FIXED_FILE;
        sqe->buf_group = BUF_GROUP_ID;
    } else {
        io_uring_prep_recv(sqe, target_fd, g_buffers[slot % BUF_RING_SIZE], BUF_SIZE - 1, 0);
        if (slot < g_registered_files) sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data64(sqe, pack_user_data(EV_RECV, slot, 0));
}

static bool g_has_multishot_accept = true;

static void submit_accept(int listen_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) {
        io_uring_submit(&g_ring);
        sqe = io_uring_get_sqe(&g_ring);
    }
    if (g_has_multishot_accept) {
        io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } else {
        io_uring_prep_accept(sqe, listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    }
    io_uring_sqe_set_data64(sqe, pack_user_data(EV_ACCEPT, -1, 0));
}

static void handle_request(int32_t slot, char *buf, size_t len) {
    http_request_t req;
    int pr = http_parse_request(buf, len, &req);

    if (pr == HTTP_PARSE_INCOMPLETE) {
        if (len < BUF_SIZE - 1 && !g_pgo_training) {
            int pidx = g_conns[slot].part_buf_idx;
            if (pidx < 0) {
                pidx = alloc_part_buf();
                if (pidx >= 0) {
                    g_conns[slot].part_buf_idx = (int16_t)pidx;
                    memcpy(g_part_pool[pidx], buf, len);
                    g_part_pool[pidx][len] = '\0';
                    g_part_len[pidx] = (uint16_t)len;
                    submit_recv(slot);
                    return;
                }
            } else {
                g_part_len[pidx] = (uint16_t)len;
                submit_recv(slot);
                return;
            }
        }
        if (g_conns[slot].part_buf_idx >= 0) {
            free_part_buf(g_conns[slot].part_buf_idx);
            g_conns[slot].part_buf_idx = -1;
        }
        queue_send(slot, RESP_400_MALFORMED_JSON, strlen(RESP_400_MALFORMED_JSON), false);
        return;
    }

    if (pr != HTTP_PARSE_OK) {
        if (g_conns[slot].part_buf_idx >= 0) {
            free_part_buf(g_conns[slot].part_buf_idx);
            g_conns[slot].part_buf_idx = -1;
        }
        queue_send(slot, RESP_400_MALFORMED_JSON, strlen(RESP_400_MALFORMED_JSON), false);
        return;
    }

    if (!req.keep_alive) {
        g_conns[slot].keep_alive = 0;
    }

    switch (req.route) {
        case ROUTE_HEALTH:
            handle_health(slot);
            break;
        case ROUTE_FEED:
            handle_feed(slot);
            break;
        case ROUTE_GET_POST:
            if (!req.id_valid) {
                queue_send(slot, RESP_400_INVALID_POST_ID, strlen(RESP_400_INVALID_POST_ID), false);
            } else {
                handle_get_post(slot, req.post_id);
            }
            break;
        case ROUTE_CREATE_POST: {
            if (!req.token) {
                queue_send(slot, RESP_401_MISSING_TOKEN, strlen(RESP_401_MISSING_TOKEN), false);
                break;
            }
            int64_t uid = 0;
            char username[256];
            size_t ulen = 0;
            int ares = auth_verify_jwt(&g_jwt_mid, req.token, req.token_len, g_now, &uid, username, sizeof(username), &ulen);
            if (ares == AUTH_ERR_INVALID_OR_EXPIRED) {
                queue_send(slot, RESP_401_INVALID_OR_EXPIRED, strlen(RESP_401_INVALID_OR_EXPIRED), false);
                break;
            }
            if (ares == AUTH_ERR_INVALID_PAYLOAD) {
                queue_send(slot, RESP_401_INVALID_PAYLOAD, strlen(RESP_401_INVALID_PAYLOAD), false);
                break;
            }

            pending_write_t *pw = &g_pending_writes[g_pending_write_count];
            size_t blen = 0;
            int jres = json_parse_post_body(req.body, req.body_len, pw->body, sizeof(pw->body), &blen);
            if (jres == JSON_PARSE_MALFORMED) {
                queue_send(slot, RESP_400_MALFORMED_JSON, strlen(RESP_400_MALFORMED_JSON), false);
            } else if (jres == JSON_PARSE_BODY_REQUIRED) {
                queue_send(slot, RESP_400_BODY_REQUIRED, strlen(RESP_400_BODY_REQUIRED), false);
            } else if (jres == JSON_PARSE_BODY_TOO_LONG) {
                queue_send(slot, RESP_400_BODY_TOO_LONG, strlen(RESP_400_BODY_TOO_LONG), false);
            } else {
                g_pending_write_count++;
                pw->fd_slot = slot;
                pw->user_id = uid;
                memcpy(pw->author, username, ulen);
                pw->author[ulen] = '\0';
                pw->author_len = ulen;
                pw->body_len = blen;
                pw->route = ROUTE_CREATE_POST;
                g_conns[slot].write_queued = 1;
                if (g_pending_write_count >= MAX_PENDING_WRITES) flush_group_commit();
            }
            break;
        }
        case ROUTE_LIKE_POST: {
            if (!req.token) {
                queue_send(slot, RESP_401_MISSING_TOKEN, strlen(RESP_401_MISSING_TOKEN), false);
                break;
            }
            int64_t uid = 0;
            int ares = auth_verify_jwt(&g_jwt_mid, req.token, req.token_len, g_now, &uid, NULL, 256, NULL);
            if (ares == AUTH_ERR_INVALID_OR_EXPIRED) {
                queue_send(slot, RESP_401_INVALID_OR_EXPIRED, strlen(RESP_401_INVALID_OR_EXPIRED), false);
                break;
            }
            if (ares == AUTH_ERR_INVALID_PAYLOAD) {
                queue_send(slot, RESP_401_INVALID_PAYLOAD, strlen(RESP_401_INVALID_PAYLOAD), false);
                break;
            }

            if (!req.id_valid) {
                queue_send(slot, RESP_400_INVALID_POST_ID, strlen(RESP_400_INVALID_POST_ID), false);
            } else {
                pending_write_t *pw = &g_pending_writes[g_pending_write_count++];
                pw->fd_slot = slot;
                pw->user_id = uid;
                pw->post_id = req.post_id;
                pw->route = ROUTE_LIKE_POST;
                g_conns[slot].write_queued = 1;
                if (g_pending_write_count >= MAX_PENDING_WRITES) flush_group_commit();
            }
            break;
        }
        default:
            queue_send(slot, RESP_404_NOT_FOUND, strlen(RESP_404_NOT_FOUND), false);
            break;
    }

    if (g_conns[slot].part_buf_idx >= 0) {
        free_part_buf(g_conns[slot].part_buf_idx);
        g_conns[slot].part_buf_idx = -1;
    }
}

static void init_db(const char *path) {
    sqlite3_initialize();
    int rc = sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database %s: %s\n", path, sqlite3_errmsg(g_db));
        exit(1);
    }

    sqlite3_exec(g_db, "PRAGMA locking_mode = EXCLUSIVE;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA mmap_size = 67108864;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA cache_size = -16000;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA busy_timeout = 5000;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA wal_autocheckpoint = 0;", NULL, NULL, NULL);

    if (strcmp(path, ":memory:") == 0) {
        const char *schema =
            "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
            "CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));"
            "CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));"
            "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);"
            "CREATE INDEX posts_user_id_idx ON posts (user_id);"
            "CREATE INDEX likes_post_id_idx ON likes (post_id);";
        sqlite3_exec(g_db, schema, NULL, NULL, NULL);
    }

    sqlite3_prepare_v2(g_db, "BEGIN IMMEDIATE;", -1, &stmt_begin, NULL);
    sqlite3_prepare_v2(g_db, "COMMIT;", -1, &stmt_commit, NULL);
    sqlite3_prepare_v2(g_db, "SELECT 1;", -1, &stmt_health, NULL);

    const char *feed_sql =
        "SELECT p.id, p.body, p.created_at, u.username,"
        "       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
        "  FROM posts p"
        "  JOIN users u ON u.id = p.user_id"
        " ORDER BY p.created_at DESC, p.id DESC"
        " LIMIT 20;";
    sqlite3_prepare_v2(g_db, feed_sql, -1, &stmt_feed, NULL);

    const char *post_sql =
        "SELECT p.id, p.body, p.created_at, u.username,"
        "       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
        "  FROM posts p JOIN users u ON u.id = p.user_id"
        " WHERE p.id = ?;";
    sqlite3_prepare_v2(g_db, post_sql, -1, &stmt_post, NULL);

    const char *insert_post_sql =
        "INSERT INTO posts (user_id, body, created_at) VALUES (?, ?, ?) RETURNING id;";
    sqlite3_prepare_v2(g_db, insert_post_sql, -1, &stmt_insert_post, NULL);

    const char *insert_like_sql =
        "INSERT INTO likes (user_id, post_id, created_at)"
        "SELECT ?1, ?2, ?3 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
        "ON CONFLICT (user_id, post_id) DO NOTHING;";
    sqlite3_prepare_v2(g_db, insert_like_sql, -1, &stmt_insert_like, NULL);

    sqlite3_prepare_v2(g_db, "SELECT 1 FROM posts WHERE id = ?;", -1, &stmt_post_exists, NULL);
}

static void run_pgo_train(void) {
    g_pgo_training = true;
    g_now = time(NULL);
    g_start_time = g_now - 10;

    const char *secret = "twelve-dollar-challenge";
    hmac_precompute_midstate(secret, strlen(secret), &g_jwt_mid);
    init_db(":memory:");

    for (int i = 0; i < RESP_POOL_SIZE; i++) {
        g_resp_pool_stack[i] = (int16_t)i;
    }
    g_resp_pool_top = RESP_POOL_SIZE;
    for (int i = 0; i < PART_POOL_SIZE; i++) {
        g_part_pool_stack[i] = (int16_t)i;
        g_part_len[i] = 0;
    }
    g_part_pool_top = PART_POOL_SIZE;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        g_free_slots[i] = (int32_t)(MAX_CONNECTIONS - 1 - i);
        g_conns[i].resp_buf_idx = -1;
        g_conns[i].part_buf_idx = -1;
    }
    g_free_top = MAX_CONNECTIONS;

    sqlite3_exec(g_db, "INSERT INTO users (id, username) VALUES (1, 'golden_ember_1');", NULL, NULL, NULL);
    sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    for (int i = 1; i <= 25000; i++) {
        char sql[128];
        sprintf(sql, "INSERT INTO posts (id, user_id, body, created_at) VALUES (%d, 1, 'Post body content', '2025-01-01T00:00:00.000Z');", i);
        sqlite3_exec(g_db, sql, NULL, NULL, NULL);
    }
    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);

    const char *hdr_b64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    const char *payload_json = "{\"sub\":\"1\",\"username\":\"golden_ember_1\",\"iat\":1700000000,\"exp\":2500000000}";
    char pay_b64[256];
    base64url_encode((const uint8_t *)payload_json, strlen(payload_json), pay_b64, sizeof(pay_b64));

    char sign_input[512];
    int silen = sprintf(sign_input, "%s.%s", hdr_b64, pay_b64);
    uint8_t sig_bin[32];
    hmac_sha256_fast(&g_jwt_mid, sign_input, (size_t)silen, sig_bin);
    char sig_b64[44];
    base64url_encode32(sig_bin, sig_b64);

    char token[512];
    sprintf(token, "%s.%s.%s", hdr_b64, pay_b64, sig_b64);

    char req_feed[] = "GET /feed HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    char req_post[] = "GET /posts/1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    char req_health[] = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    char req_create[1024];
    sprintf(req_create, "POST /posts HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 30\r\n\r\n{\"body\":\"pgo training post\"}", token);

    g_conns[0].active = 1;
    g_conns[0].keep_alive = 1;
    g_conns[0].real_fd = -1;

    for (int iter = 0; iter < 50000; iter++) {
        handle_request(0, req_feed, strlen(req_feed));
        handle_request(0, req_post, strlen(req_post));
        handle_request(0, req_health, strlen(req_health));
        handle_request(0, req_create, strlen(req_create));

        int like_pid = (iter / 2) + 1;
        char req_like[1024];
        int rlen = sprintf(req_like, "POST /posts/%d/like HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n", like_pid, token);
        handle_request(0, req_like, (size_t)rlen);

        if (g_pending_write_count >= 16) {
            flush_group_commit();
        }
    }
    if (g_pending_write_count > 0) {
        flush_group_commit();
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--pgo-train") == 0) {
        run_pgo_train();
        return 0;
    }

    const char *sqlite_path = getenv("SQLITE_PATH");
    if (!sqlite_path) sqlite_path = "feed.db";

    const char *jwt_secret = getenv("JWT_SECRET");
    if (!jwt_secret) jwt_secret = "twelve-dollar-challenge";

    const char *host_str = getenv("HOST");
    if (!host_str) host_str = "0.0.0.0";

    const char *port_str = getenv("PORT");
    int port = port_str ? atoi(port_str) : 80;

    g_start_time = time(NULL);
    g_now = g_start_time;
    hmac_precompute_midstate(jwt_secret, strlen(jwt_secret), &g_jwt_mid);
    init_db(sqlite_path);

    memset(g_conns, 0, sizeof(g_conns));
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        g_direct_fds[i] = -1;
        g_conns[i].resp_buf_idx = -1;
        g_conns[i].part_buf_idx = -1;
    }
    for (int i = 0; i < RESP_POOL_SIZE; i++) {
        g_resp_pool_stack[i] = (int16_t)i;
    }
    g_resp_pool_top = RESP_POOL_SIZE;
    for (int i = 0; i < PART_POOL_SIZE; i++) {
        g_part_pool_stack[i] = (int16_t)i;
        g_part_len[i] = 0;
    }
    g_part_pool_top = PART_POOL_SIZE;

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = (rl.rlim_max > 131072) ? 131072 : rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN;

    int ret = io_uring_queue_init_params(32768, &g_ring, &params);
    if (ret < 0) {
        memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_SINGLE_ISSUER;
        ret = io_uring_queue_init_params(16384, &g_ring, &params);
        if (ret < 0) {
            ret = io_uring_queue_init(8192, &g_ring, 0);
            if (ret < 0) {
                fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
                exit(1);
            }
        }
    }

    int reg_count = MAX_CONNECTIONS;
    ret = io_uring_register_files(&g_ring, g_direct_fds, reg_count);
    while (ret < 0 && reg_count > 1024) {
        reg_count /= 2;
        ret = io_uring_register_files(&g_ring, g_direct_fds, reg_count);
    }
    g_registered_files = (ret >= 0) ? reg_count : 0;

    g_free_top = 0;
    for (int i = MAX_CONNECTIONS - 1; i >= 0; i--) {
        g_free_slots[g_free_top++] = (int32_t)i;
    }

    fprintf(stderr, "Server listening on %s:%d, registered %d files, total slots %d\n", host_str, port, g_registered_files, MAX_CONNECTIONS);

    g_buf_ring = io_uring_setup_buf_ring(&g_ring, BUF_RING_SIZE, BUF_GROUP_ID, 0, &ret);
    if (g_buf_ring) {
        for (int i = 0; i < BUF_RING_SIZE; i++) {
            io_uring_buf_ring_add(g_buf_ring, g_buffers[i], BUF_SIZE, i, io_uring_buf_ring_mask(BUF_RING_SIZE), i);
        }
        io_uring_buf_ring_advance(g_buf_ring, BUF_RING_SIZE);
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, host_str, &sin.sin_addr);

    if (bind(listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(listen_fd, 65535) < 0) {
        perror("listen");
        exit(1);
    }

    submit_accept(listen_fd);
    io_uring_submit(&g_ring);

    int commit_counter = 0;
    int last_batch_count = 0;
    struct __kernel_timespec coalesce_ts = { .tv_sec = 0, .tv_nsec = 1000000 };

    while (1) {
        struct io_uring_cqe *cqe;
        unsigned head;
        int count = 0;

        if (last_batch_count >= 16) {
            io_uring_submit_and_wait_timeout(&g_ring, &cqe, 32, &coalesce_ts, NULL);
        } else {
            ret = io_uring_submit_and_wait(&g_ring, 1);
            if (ret < 0 && ret != -EINTR) {
                break;
            }
        }

        g_now = time(NULL);

        io_uring_for_each_cqe(&g_ring, head, cqe) {
            count++;
            uint8_t ev;
            int32_t slot;
            uint16_t extra;
            unpack_user_data(cqe->user_data, &ev, &slot, &extra);

            if (ev == EV_ACCEPT) {
                if (cqe->res == -EINVAL && g_has_multishot_accept) {
                    g_has_multishot_accept = false;
                    submit_accept(listen_fd);
                } else {
                    if (cqe->res >= 0) {
                        int cfd = cqe->res;

                        int new_slot = get_free_slot();
                        if (new_slot >= 0) {
                            g_conns[new_slot].active = 1;
                            g_conns[new_slot].keep_alive = 1;
                            g_conns[new_slot].has_pending_send = 0;
                            g_conns[new_slot].write_queued = 0;
                            g_conns[new_slot].resp_buf_idx = -1;
                            g_conns[new_slot].part_buf_idx = -1;
                            g_conns[new_slot].send_buf = NULL;
                            g_conns[new_slot].real_fd = cfd;

                            if (new_slot < g_registered_files) {
                                io_uring_register_files_update(&g_ring, new_slot, &cfd, 1);
                            }
                            submit_recv(new_slot);
                        } else {
                            close(cfd);
                        }
                    }
                    if (!g_has_multishot_accept || !(cqe->flags & IORING_CQE_F_MORE)) {
                        submit_accept(listen_fd);
                    }
                }
            } else if (ev == EV_RECV) {
                bool has_buf = (cqe->flags & IORING_CQE_F_BUFFER);
                uint16_t bid = has_buf ? (cqe->flags >> IORING_CQE_BUFFER_SHIFT) : 0;

                if (slot < 0 || slot >= MAX_CONNECTIONS || !g_conns[slot].active) {
                    if (has_buf) return_buffer(bid);
                    continue;
                }

                if (cqe->res <= 0) {
                    if (has_buf) return_buffer(bid);
                    if (cqe->res == -ENOBUFS || cqe->res == -EAGAIN || cqe->res == -EINTR) {
                        submit_recv(slot);
                    } else {
                        close_connection(slot);
                    }
                } else {
                    char *buf;
                    size_t req_len = (size_t)cqe->res;
                    int pidx = g_conns[slot].part_buf_idx;

                    if (pidx >= 0) {
                        uint16_t plen = g_part_len[pidx];
                        char *pbuf = g_part_pool[pidx];
                        char *in_buf = (g_buf_ring && has_buf) ? (char *)g_buffers[bid] : (char *)g_buffers[slot % BUF_RING_SIZE];

                        size_t space = (BUF_SIZE - 1 > plen) ? (BUF_SIZE - 1 - plen) : 0;
                        size_t copy_n = (req_len < space) ? req_len : space;
                        if (copy_n > 0) memcpy(pbuf + plen, in_buf, copy_n);
                        pbuf[plen + copy_n] = '\0';
                        req_len = plen + copy_n;
                        buf = pbuf;

                        if (has_buf) return_buffer(bid);
                        handle_request(slot, buf, req_len);
                    } else {
                        buf = (g_buf_ring && has_buf) ? (char *)g_buffers[bid] : (char *)g_buffers[slot % BUF_RING_SIZE];
                        buf[req_len] = '\0';
                        handle_request(slot, buf, req_len);
                        if (has_buf) return_buffer(bid);
                    }
                }
            } else if (ev == EV_SEND) {
                if (slot < 0 || slot >= MAX_CONNECTIONS || !g_conns[slot].active) {
                    continue;
                }
                g_conns[slot].has_pending_send = 0;
                if (g_conns[slot].resp_buf_idx >= 0) {
                    free_resp_buf(g_conns[slot].resp_buf_idx);
                    g_conns[slot].resp_buf_idx = -1;
                }
                if (extra == 1 && g_conns[slot].send_buf) {
                    free(g_conns[slot].send_buf);
                    g_conns[slot].send_buf = NULL;
                }
                if (cqe->res < 0 || !g_conns[slot].keep_alive) {
                    close_connection(slot);
                } else {
                    submit_recv(slot);
                }
            }
        }

        io_uring_cq_advance(&g_ring, count);
        last_batch_count = count;

        if (g_pending_write_count > 0) {
            flush_group_commit();
            commit_counter++;
            if (commit_counter >= 100) {
                commit_counter = 0;
                sqlite3_wal_checkpoint_v2(g_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
            }
        }
    }

    return 0;
}

