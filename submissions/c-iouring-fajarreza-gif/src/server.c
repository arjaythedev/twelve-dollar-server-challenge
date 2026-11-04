#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "sys.h"
#include "jwt.h"
#include "http.h"
#include "db.h"

#define MAX_SLOTS 65536
#define PBUF_ENTRIES 4096
#define PBUF_SIZE 2048
#define RESP_POOL_SIZE 4096
#define RESP_BUF_SIZE 8192

enum {
    OP_ACCEPT = 1,
    OP_POLL = 2,
    OP_RECV = 3,
    OP_SEND = 4,
    OP_CLOSE = 5
};

#define MAKE_UD(op, slot, gen) (((uint64_t)(op) << 48) | ((uint64_t)(slot) << 16) | ((uint64_t)(gen) & 0xFFFF))
#define UD_OP(ud) ((unsigned)((ud) >> 48))
#define UD_SLOT(ud) ((unsigned)(((ud) >> 16) & 0xFFFFFFFF))
#define UD_GEN(ud) ((uint16_t)((ud) & 0xFFFF))

typedef struct {
    uint16_t gen;
    uint8_t open;
    uint8_t keep_alive;
    uint8_t poll_armed;
} conn_t;

static conn_t g_conns[MAX_SLOTS];

/* Response buffer pool */
static char g_resp_pool[RESP_POOL_SIZE][RESP_BUF_SIZE];
static uint16_t g_resp_free_stack[RESP_POOL_SIZE];
static int g_resp_free_top = RESP_POOL_SIZE;

static inline int resp_alloc() {
    if (g_resp_free_top > 0) {
        return (int)g_resp_free_stack[--g_resp_free_top];
    }
    return -1;
}

static inline void resp_free(int idx) {
    if (idx >= 0 && idx < RESP_POOL_SIZE && g_resp_free_top < RESP_POOL_SIZE) {
        g_resp_free_stack[g_resp_free_top++] = (uint16_t)idx;
    }
}

/* Pending writes for group commit */
typedef struct {
    unsigned slot;
    uint16_t gen;
    int is_like; // 0 = post create, 1 = like
    int resp_idx;
    int keep_alive;
    // Post create fields
    int64_t user_id;
    char author[128];
    char body[512];
    size_t body_len;
    // Like fields
    int64_t post_id;
} pending_write_t;

#define MAX_PENDING_WRITES 256
static pending_write_t g_pending_writes[MAX_PENDING_WRITES];
static int g_num_pending_writes = 0;

static time_t g_server_start_time = 0;

static void close_conn(struct ring *r, unsigned slot, uint16_t gen) {
    if (slot >= MAX_SLOTS || !g_conns[slot].open || g_conns[slot].gen != gen) return;
    g_conns[slot].open = 0;
    g_conns[slot].poll_armed = 0;
    g_conns[slot].gen++;

    uint64_t poll_ud = MAKE_UD(OP_POLL, slot, gen);
    uint64_t close_ud = MAKE_UD(OP_CLOSE, slot, g_conns[slot].gen);
    sqe_shutdown_close(r, slot, close_ud, poll_ud, close_ud);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    g_server_start_time = time(NULL);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        if (rl.rlim_cur < 65536) {
            rl.rlim_cur = (rl.rlim_max >= 65536) ? 65536 : rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    unsigned nslots = (rl.rlim_cur < MAX_SLOTS) ? (unsigned)rl.rlim_cur : MAX_SLOTS;

    const char *sqlite_path = getenv("SQLITE_PATH");
    if (!sqlite_path) sqlite_path = "feed.db";

    const char *jwt_secret = getenv("JWT_SECRET");
    if (!jwt_secret) jwt_secret = "twelve-dollar-challenge";

    const char *host = getenv("HOST");
    if (!host) host = "0.0.0.0";

    const char *port_str = getenv("PORT");
    int port = port_str ? atoi(port_str) : 80;

    printf("Starting c-iouring-clean on %s:%d\n", host, port);
    printf("SQLite DB: %s\n", sqlite_path);

    jwt_init(jwt_secret);

    if (db_init(sqlite_path) != 0) {
        fprintf(stderr, "Failed to initialize SQLite at %s\n", sqlite_path);
        return 1;
    }

    // Initialize response buffer pool free stack
    for (int i = 0; i < RESP_POOL_SIZE; i++) {
        g_resp_free_stack[i] = (uint16_t)i;
    }

    // Setup io_uring ring
    struct ring ring;
    const char *err = NULL;
    if (ring_init(&ring, 4096, 8192, nslots, PBUF_ENTRIES, PBUF_SIZE, 1, &err) != 0) {
        fprintf(stderr, "Failed to initialize io_uring: %s (errno %d: %s)\n",
                err ? err : "unknown", errno, strerror(errno));
        return 1;
    }

    // Create listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sin.sin_addr);

    if (bind(listen_fd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 4096) != 0) {
        perror("listen");
        return 1;
    }

    // Arm multishot accept
    sqe_accept_multishot(ring_sqe(&ring), listen_fd, MAKE_UD(OP_ACCEPT, 0, 0));
    ring_submit(&ring, 0, 0, NULL, 0);

    printf("Listening on %s:%d (io_uring ready)\n", host, port);

    // Event loop
    while (1) {
        // Wait for completions
        long ret = ring_wait(&ring, 1, 100);
        if (ret < 0 && ret != -EINTR && ret != -ETIME) {
            fprintf(stderr, "ring_wait error: %ld\n", ret);
            break;
        }

        g_num_pending_writes = 0;

        unsigned head = *ring.cq_head;
        unsigned tail = LOAD_ACQ(ring.cq_tail);
        unsigned count = 0;

        while (head != tail) {
            struct io_uring_cqe *cqe = &ring.cqes[head & ring.cq_mask];
            uint64_t ud = cqe->user_data;
            unsigned op = UD_OP(ud);
            unsigned slot = UD_SLOT(ud);
            uint16_t gen = UD_GEN(ud);
            int res = cqe->res;
            unsigned flags = cqe->flags;

            head++;
            count++;

            switch (op) {
                case OP_ACCEPT: {
                    if (res >= 0) {
                        unsigned new_slot = (unsigned)res;
                        if (new_slot < MAX_SLOTS) {
                            g_conns[new_slot].gen++;
                            g_conns[new_slot].open = 1;
                            g_conns[new_slot].keep_alive = 1;
                            g_conns[new_slot].poll_armed = 1;
                            // Arm multishot poll
                            sqe_poll_conn(ring_sqe(&ring), new_slot, MAKE_UD(OP_POLL, new_slot, g_conns[new_slot].gen));
                        }
                    } else if (res == -ECANCELED || res == -EINVAL) {
                        // Re-arm accept if needed
                        sqe_accept_multishot(ring_sqe(&ring), listen_fd, MAKE_UD(OP_ACCEPT, 0, 0));
                    }
                    break;
                }

                case OP_POLL: {
                    if (slot < MAX_SLOTS && g_conns[slot].open && g_conns[slot].gen == gen) {
                        if (!(flags & IORING_CQE_F_MORE)) {
                            g_conns[slot].poll_armed = 0;
                        }
                        if (res <= 0 || (res & (POLLERR | POLLHUP | POLLRDHUP))) {
                            close_conn(&ring, slot, gen);
                        } else if (res & POLLIN) {
                            sqe_recv_once(ring_sqe(&ring), slot, MAKE_UD(OP_RECV, slot, gen));
                        }
                    }
                    break;
                }

                case OP_RECV: {
                    if (slot < MAX_SLOTS && g_conns[slot].open && g_conns[slot].gen == gen) {
                        if (res <= 0) {
                            if (res != -EAGAIN) {
                                close_conn(&ring, slot, gen);
                            }
                        } else {
                            // Buffer was consumed
                            unsigned bid = flags >> 16;
                            const char *req_buf = (const char*)ring_buf_ptr(&ring, bid);
                            size_t req_len = (size_t)res;

                            http_request_t req;
                            int parse_rc = http_parse_request(req_buf, req_len, &req);

                            int resp_idx = resp_alloc();
                            if (resp_idx < 0) {
                                // Buffer exhaustion: close connection
                                ring_buf_return(&ring, bid);
                                close_conn(&ring, slot, gen);
                                break;
                            }

                            char *resp = g_resp_pool[resp_idx];
                            size_t resp_len = 0;
                            g_conns[slot].keep_alive = req.keep_alive;

                            if (parse_rc != 0) {
                                resp_len = format_error(resp, RESP_BUF_SIZE, 400, "Bad Request", "malformed request", 0);
                                g_conns[slot].keep_alive = 0;
                            } else if (req.method == METHOD_GET && req.path_len == 7 && memcmp(req.path, "/health", 7) == 0) {
                                char body[256];
                                int64_t uptime = (int64_t)(time(NULL) - g_server_start_time);
                                size_t blen = db_health(body, sizeof(body), uptime);
                                resp_len = format_response(resp, RESP_BUF_SIZE, 200, "OK", body, blen, req.keep_alive);
                            } else if (req.method == METHOD_GET && req.path_len == 5 && memcmp(req.path, "/feed", 5) == 0) {
                                char body[RESP_BUF_SIZE - 256];
                                size_t blen = db_query_feed(body, sizeof(body));
                                resp_len = format_response(resp, RESP_BUF_SIZE, 200, "OK", body, blen, req.keep_alive);
                            } else if (req.method == METHOD_GET && req.path_len > 7 && memcmp(req.path, "/posts/", 7) == 0) {
                                const char *id_str = req.path + 7;
                                size_t id_len = req.path_len - 7;
                                int valid_id = (id_len > 0);
                                for (size_t i = 0; i < id_len; i++) {
                                    if (id_str[i] < '0' || id_str[i] > '9') { valid_id = 0; break; }
                                }
                                if (id_str[0] == '0') valid_id = 0; // positive integer only

                                if (!valid_id) {
                                    resp_len = format_error(resp, RESP_BUF_SIZE, 400, "Bad Request", "invalid post id", req.keep_alive);
                                } else {
                                    int64_t post_id = strtoll(id_str, NULL, 10);
                                    char body[RESP_BUF_SIZE - 256];
                                    size_t blen = 0;
                                    if (!db_query_post(post_id, body, sizeof(body), &blen)) {
                                        resp_len = format_error(resp, RESP_BUF_SIZE, 404, "Not Found", "post not found", req.keep_alive);
                                    } else {
                                        resp_len = format_response(resp, RESP_BUF_SIZE, 200, "OK", body, blen, req.keep_alive);
                                    }
                                }
                            } else if (req.method == METHOD_POST && req.path_len == 6 && memcmp(req.path, "/posts", 6) == 0) {
                                // Auth check first
                                if (!req.has_auth_header || !req.is_bearer) {
                                    resp_len = format_error(resp, RESP_BUF_SIZE, 401, "Unauthorized", "missing bearer token", req.keep_alive);
                                } else {
                                    int64_t user_id = 0;
                                    char username[128] = {0};
                                    int vrc = jwt_verify(req.auth_token, req.auth_token_len, (int64_t)time(NULL), &user_id, username, sizeof(username));
                                    if (vrc == JWT_ERR_INVALID) {
                                        resp_len = format_error(resp, RESP_BUF_SIZE, 401, "Unauthorized", "invalid or expired token", req.keep_alive);
                                    } else if (vrc == JWT_ERR_PAYLOAD) {
                                        resp_len = format_error(resp, RESP_BUF_SIZE, 401, "Unauthorized", "invalid token payload", req.keep_alive);
                                    } else {
                                        // Body parsing
                                        char body_trimmed[512];
                                        size_t body_trimmed_len = 0;
                                        int prc = parse_post_body(req.body, req.body_len, body_trimmed, sizeof(body_trimmed), &body_trimmed_len);
                                        if (prc == POST_BODY_MALFORMED) {
                                            resp_len = format_error(resp, RESP_BUF_SIZE, 400, "Bad Request", "malformed JSON body", req.keep_alive);
                                        } else if (prc == POST_BODY_REQUIRED) {
                                            resp_len = format_error(resp, RESP_BUF_SIZE, 400, "Bad Request", "body is required", req.keep_alive);
                                        } else if (prc == POST_BODY_TOO_LONG) {
                                            resp_len = format_error(resp, RESP_BUF_SIZE, 400, "Bad Request", "body must be at most 500 characters", req.keep_alive);
                                        } else {
                                            // Queue write for group commit
                                            if (g_num_pending_writes < MAX_PENDING_WRITES) {
                                                pending_write_t *pw = &g_pending_writes[g_num_pending_writes++];
                                                pw->slot = slot;
                                                pw->gen = gen;
                                                pw->is_like = 0;
                                                pw->resp_idx = resp_idx;
                                                pw->keep_alive = req.keep_alive;
                                                pw->user_id = user_id;
                                                memcpy(pw->author, username, sizeof(pw->author));
                                                memcpy(pw->body, body_trimmed, body_trimmed_len);
                                                pw->body[body_trimmed_len] = '\0';
                                                pw->body_len = body_trimmed_len;
                                                resp_idx = -1; // Don't send now, queued
                                            } else {
                                                // Batch full: execute immediately
                                                char res_body[1024];
                                                size_t res_len = 0;
                                                db_create_post(user_id, body_trimmed, body_trimmed_len, username, res_body, sizeof(res_body), &res_len);
                                                resp_len = format_response(resp, RESP_BUF_SIZE, 201, "Created", res_body, res_len, req.keep_alive);
                                            }
                                        }
                                    }
                                }
                            } else if (req.method == METHOD_POST && req.path_len > 12 &&
                                       memcmp(req.path, "/posts/", 7) == 0 &&
                                       memcmp(req.path + req.path_len - 5, "/like", 5) == 0) {
                                // Auth check FIRST (before id check!)
                                if (!req.has_auth_header || !req.is_bearer) {
                                    resp_len = format_error(resp, RESP_BUF_SIZE, 401, "Unauthorized", "missing bearer token", req.keep_alive);
                                } else {
                                    int64_t user_id = 0;
                                    char username[128] = {0};
                                    int vrc = jwt_verify(req.auth_token, req.auth_token_len, (int64_t)time(NULL), &user_id, username, sizeof(username));
                                    if (vrc == JWT_ERR_INVALID) {
                                        resp_len = format_error(resp, RESP_BUF_SIZE, 401, "Unauthorized", "invalid or expired token", req.keep_alive);
                                    } else if (vrc == JWT_ERR_PAYLOAD) {
                                        resp_len = format_error(resp, RESP_BUF_SIZE, 401, "Unauthorized", "invalid token payload", req.keep_alive);
                                    } else {
                                        // Now validate post id
                                        const char *id_str = req.path + 7;
                                        size_t id_len = req.path_len - 12;
                                        int valid_id = (id_len > 0);
                                        for (size_t i = 0; i < id_len; i++) {
                                            if (id_str[i] < '0' || id_str[i] > '9') { valid_id = 0; break; }
                                        }
                                        if (id_str[0] == '0') valid_id = 0;

                                        if (!valid_id) {
                                            resp_len = format_error(resp, RESP_BUF_SIZE, 400, "Bad Request", "invalid post id", req.keep_alive);
                                        } else {
                                            int64_t post_id = strtoll(id_str, NULL, 10);
                                            if (!db_check_post_exists(post_id)) {
                                                resp_len = format_error(resp, RESP_BUF_SIZE, 404, "Not Found", "post not found", req.keep_alive);
                                            } else {
                                                // Queue like for group commit
                                                if (g_num_pending_writes < MAX_PENDING_WRITES) {
                                                    pending_write_t *pw = &g_pending_writes[g_num_pending_writes++];
                                                    pw->slot = slot;
                                                    pw->gen = gen;
                                                    pw->is_like = 1;
                                                    pw->resp_idx = resp_idx;
                                                    pw->keep_alive = req.keep_alive;
                                                    pw->user_id = user_id;
                                                    pw->post_id = post_id;
                                                    resp_idx = -1; // Queued
                                                } else {
                                                    int already_liked = 0;
                                                    char res_body[256];
                                                    size_t res_len = 0;
                                                    db_like_post(user_id, post_id, &already_liked, res_body, sizeof(res_body), &res_len);
                                                    int st = already_liked ? 200 : 201;
                                                    const char *st_str = already_liked ? "OK" : "Created";
                                                    resp_len = format_response(resp, RESP_BUF_SIZE, st, st_str, res_body, res_len, req.keep_alive);
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                resp_len = format_error(resp, RESP_BUF_SIZE, 404, "Not Found", "not found", req.keep_alive);
                            }

                            // Return received buffer back to provided buffer ring
                            ring_buf_return(&ring, bid);

                            // Send response if not queued
                            if (resp_idx >= 0) {
                                uint64_t send_ud = MAKE_UD(OP_SEND, slot, (uint16_t)resp_idx);
                                sqe_send(ring_sqe(&ring), slot, resp, (unsigned)resp_len, send_ud);
                            }

                            // If poll was un-armed and connection is keep-alive, re-arm
                            if (!g_conns[slot].poll_armed && g_conns[slot].keep_alive) {
                                g_conns[slot].poll_armed = 1;
                                sqe_poll_conn(ring_sqe(&ring), slot, MAKE_UD(OP_POLL, slot, gen));
                            }
                        }
                    }
                    break;
                }

                case OP_SEND: {
                    // Free response buffer
                    int resp_idx = (int)gen;
                    resp_free(resp_idx);
                    if (slot < MAX_SLOTS && g_conns[slot].open) {
                        if (!g_conns[slot].keep_alive) {
                            close_conn(&ring, slot, g_conns[slot].gen);
                        }
                    }
                    break;
                }

                case OP_CLOSE: {
                    // Closed
                    break;
                }
            }
        }

        // Commit pending CQEs
        if (count > 0) {
            STORE_REL(ring.cq_head, head);
        }

        // Process group commit for pending writes
        if (g_num_pending_writes > 0) {
            db_begin();
            for (int i = 0; i < g_num_pending_writes; i++) {
                pending_write_t *pw = &g_pending_writes[i];
                char *resp = g_resp_pool[pw->resp_idx];
                size_t resp_len = 0;

                if (pw->is_like == 0) {
                    char res_body[1024];
                    size_t res_len = 0;
                    db_create_post(pw->user_id, pw->body, pw->body_len, pw->author, res_body, sizeof(res_body), &res_len);
                    resp_len = format_response(resp, RESP_BUF_SIZE, 201, "Created", res_body, res_len, pw->keep_alive);
                } else {
                    int already_liked = 0;
                    char res_body[256];
                    size_t res_len = 0;
                    db_like_post(pw->user_id, pw->post_id, &already_liked, res_body, sizeof(res_body), &res_len);
                    int st = already_liked ? 200 : 201;
                    const char *st_str = already_liked ? "OK" : "Created";
                    resp_len = format_response(resp, RESP_BUF_SIZE, st, st_str, res_body, res_len, pw->keep_alive);
                }

                // Send response
                uint64_t send_ud = MAKE_UD(OP_SEND, pw->slot, (uint16_t)pw->resp_idx);
                sqe_send(ring_sqe(&ring), pw->slot, resp, (unsigned)resp_len, send_ud);
            }
            db_commit();
        }

        // Publish returned buffers and flush pending submissions
        ring_buf_publish(&ring);
        ring_submit(&ring, 0, 0, NULL, 0);
    }

    return 0;
}
