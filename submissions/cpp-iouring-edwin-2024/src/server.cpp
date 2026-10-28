#include "server.hpp"
#include "http.hpp"
#include "json.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <cstring>

#ifdef __linux__
#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace net {

#ifdef __linux__

constexpr uint16_t BUF_GROUP_ID = 1;
constexpr uint32_t BUF_COUNT = 4096;
constexpr size_t BUF_SIZE = 4096;
constexpr size_t SEND_BUF_SIZE = 16384;
constexpr uint32_t SEND_BUF_POOL_SIZE = 4096;

enum class OpType : uint8_t {
    Accept = 1,
    Recv = 2,
    Send = 3
};

inline uint64_t make_user_data(OpType op, uint32_t slot, uint32_t gen, uint16_t buf_id = 0) {
    return (static_cast<uint64_t>(op) << 56) |
           (static_cast<uint64_t>(buf_id) << 40) |
           ((static_cast<uint64_t>(slot) & 0xfffff) << 20) |
           (static_cast<uint64_t>(gen) & 0xfffff);
}

inline void unpack_user_data(uint64_t val, OpType& op, uint32_t& slot, uint32_t& gen, uint16_t& buf_id) {
    op = static_cast<OpType>(val >> 56);
    buf_id = static_cast<uint16_t>((val >> 40) & 0xffff);
    slot = static_cast<uint32_t>((val >> 20) & 0xfffff);
    gen = static_cast<uint32_t>(val & 0xfffff);
}

struct ConnSlot {
    uint32_t generation{1};
    int fd{-1};
    bool active{false};
    bool keep_alive{true};
    std::vector<char>* partial{nullptr};
};

inline void cleanup_slot(ConnSlot& slot) {
    slot.active = false;
    if (slot.partial) {
        delete slot.partial;
        slot.partial = nullptr;
    }
}

struct SendItem {
    uint32_t slot{0};
    uint32_t gen{0};
    int fd{-1};
    uint16_t buf_id{0};
    char* buf_ptr{nullptr};
    size_t len{0};
};

struct DynamicSendBuf {
    uint16_t id{0};
    std::unique_ptr<char[]> ptr;
};

inline struct io_uring_sqe* get_sqe_safe(struct io_uring* ring) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (__builtin_expect(!sqe, 0)) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

#endif

Server::Server(ServerConfig config) : config_(std::move(config)) {}

Server::~Server() {
    stop();
}

bool Server::setup_listener() {
#ifdef __linux__
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        std::cerr << "Failed to create socket\n";
        return false;
    }

    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Defer accept until first data packet arrives
    int defer = 1;
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof(defer));

    // Fast open queue
    int qlen = 65535;
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to bind to " << config_.host << ":" << config_.port << "\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 65535) < 0) {
        std::cerr << "Failed to listen\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    return true;
#else
    std::cout << "[Mock] Listener configured on " << config_.host << ":" << config_.port << "\n";
    return true;
#endif
}

bool Server::init() {
    if (!db_.open(config_.sqlite_path.c_str())) {
        std::cerr << "Failed to open SQLite database: " << config_.sqlite_path << "\n";
        return false;
    }
    jwt_.init(config_.jwt_secret);
    start_time_s_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    return setup_listener();
}

void Server::run() {
    running_ = true;
#ifdef __linux__
    struct io_uring ring;
    struct io_uring_params params{};

    // Attempt DEFER_TASKRUN + SINGLE_ISSUER + COOP_TASKRUN for optimal single-threaded speed
#ifdef IORING_SETUP_DEFER_TASKRUN
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CLAMP | IORING_SETUP_CQSIZE;
    params.cq_entries = 16384;
    int ret = io_uring_queue_init_params(8192, &ring, &params);
#else
    int ret = -1;
#endif
    if (ret < 0) {
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CLAMP | IORING_SETUP_CQSIZE;
        params.cq_entries = 16384;
        ret = io_uring_queue_init_params(8192, &ring, &params);
        if (ret < 0) {
            std::memset(&params, 0, sizeof(params));
            ret = io_uring_queue_init_params(4096, &ring, &params);
            if (ret < 0) {
                std::cerr << "Failed to initialize io_uring: " << ret << "\n";
                return;
            }
        }
    }

    // Allocate provided buffer ring
    void* buf_ring_mem = nullptr;
    if (posix_memalign(&buf_ring_mem, 4096, BUF_COUNT * sizeof(struct io_uring_buf)) != 0) {
        std::cerr << "Failed to allocate buffer ring memory\n";
        return;
    }

    struct io_uring_buf_ring* br = static_cast<struct io_uring_buf_ring*>(buf_ring_mem);
    io_uring_buf_ring_init(br);

    struct io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<uint64_t>(br);
    reg.ring_entries = BUF_COUNT;
    reg.bgid = BUF_GROUP_ID;

    ret = io_uring_register_buf_ring(&ring, &reg, 0);
    if (ret < 0) {
        std::cerr << "Failed to register buffer ring: " << ret << "\n";
        return;
    }

    // Allocate receive buffers
    uint8_t* raw_bufs = static_cast<uint8_t*>(aligned_alloc(4096, BUF_COUNT * BUF_SIZE));
    for (uint32_t i = 0; i < BUF_COUNT; ++i) {
        io_uring_buf_ring_add(br, raw_bufs + (i * BUF_SIZE), BUF_SIZE, i, io_uring_buf_ring_mask(BUF_COUNT), i);
    }
    io_uring_buf_ring_advance(br, BUF_COUNT);

    // Pre-allocated send buffer pool
    std::unique_ptr<char[]> send_bufs_mem(new char[static_cast<size_t>(SEND_BUF_POOL_SIZE) * SEND_BUF_SIZE]);
    std::vector<uint16_t> free_send_bufs;
    free_send_bufs.reserve(SEND_BUF_POOL_SIZE);
    for (int i = static_cast<int>(SEND_BUF_POOL_SIZE) - 1; i >= 0; --i) {
        free_send_bufs.push_back(static_cast<uint16_t>(i));
    }

    // Temporary work buffer for SQLite responses (avoids stack allocations)
    std::unique_ptr<char[]> work_buf(new char[SEND_BUF_SIZE]);

    uint32_t max_conns = config_.max_conns;
    std::vector<ConnSlot> slots(max_conns);
    std::vector<uint32_t> free_slots;
    free_slots.reserve(max_conns);
    for (int i = static_cast<int>(max_conns) - 1; i >= 0; --i) {
        free_slots.push_back(i);
    }

    // Arm multishot accept
    struct io_uring_sqe* sqe = get_sqe_safe(&ring);
    io_uring_prep_multishot_accept(sqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    sqe->user_data = make_user_data(OpType::Accept, 0, 0);
    io_uring_submit(&ring);

    std::vector<SendItem> pending_sends;
    pending_sends.reserve(256);
    std::vector<DynamicSendBuf> dynamic_send_bufs;
    uint32_t next_dynamic_id = 0;

    std::cout << "Server running on http://" << config_.host << ":" << config_.port << "\n";

    constexpr unsigned BATCH_SIZE = 128;
    struct io_uring_cqe* cqes[BATCH_SIZE];

    while (running_) {
        unsigned count = io_uring_peek_batch_cqe(&ring, cqes, BATCH_SIZE);
        if (count == 0) {
            struct io_uring_cqe* cqe = nullptr;
            ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret < 0) {
                if (ret == -EINTR) continue;
                break;
            }
            count = io_uring_peek_batch_cqe(&ring, cqes, BATCH_SIZE);
            if (count == 0) {
                cqes[0] = cqe;
                count = 1;
            }
        }

        db_.begin_batch();
        int bufs_to_advance = 0;
        int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        for (unsigned i = 0; i < count; ++i) {
            struct io_uring_cqe* cqe = cqes[i];
            uint64_t udata = cqe->user_data;
            int res = cqe->res;

            OpType op;
            uint32_t slot_idx;
            uint32_t gen;
            uint16_t buf_id;
            unpack_user_data(udata, op, slot_idx, gen, buf_id);

            if (op == OpType::Accept) {
                if (res >= 0) {
                    int client_fd = res;
                    if (!free_slots.empty()) {
                        uint32_t free_idx = free_slots.back();
                        free_slots.pop_back();

                        // Set TCP options on accepted socket
                        int one = 1;
                        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                        setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
                        int rcvbuf = 16384;
                        int sndbuf = 32768;
                        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
                        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

                        ConnSlot& slot = slots[free_idx];
                        slot.active = true;
                        slot.fd = client_fd;
                        slot.keep_alive = true;
                        if (slot.partial) {
                            delete slot.partial;
                            slot.partial = nullptr;
                        }
                        slot.generation++;

                        // Arm multishot recv
                        struct io_uring_sqe* rsqe = get_sqe_safe(&ring);
                        if (rsqe) {
                            io_uring_prep_recv_multishot(rsqe, client_fd, nullptr, 0, 0);
                            rsqe->buf_group = BUF_GROUP_ID;
                            rsqe->flags |= IOSQE_BUFFER_SELECT;
                            rsqe->user_data = make_user_data(OpType::Recv, free_idx, slot.generation);
                        }
                    } else {
                        close(client_fd);
                    }
                }

                if (!(cqe->flags & IORING_CQE_F_MORE)) {
                    struct io_uring_sqe* asqe = get_sqe_safe(&ring);
                    if (asqe) {
                        io_uring_prep_multishot_accept(asqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        asqe->user_data = make_user_data(OpType::Accept, 0, 0);
                    }
                }
                continue;
            }

            if (op == OpType::Send) {
                if (buf_id < SEND_BUF_POOL_SIZE) {
                    free_send_bufs.push_back(buf_id);
                } else {
                    auto it = std::find_if(dynamic_send_bufs.begin(), dynamic_send_bufs.end(),
                        [buf_id](const DynamicSendBuf& b) { return b.id == buf_id; });
                    if (it != dynamic_send_bufs.end()) {
                        dynamic_send_bufs.erase(it);
                    }
                }
                if (slot_idx < max_conns) {
                    ConnSlot& slot = slots[slot_idx];
                    if (res <= 0 || !slot.keep_alive) {
                        if (slot.active && slot.generation == gen) {
                            close(slot.fd);
                            slot.fd = -1;
                            cleanup_slot(slot);
                            free_slots.push_back(slot_idx);
                        }
                    }
                }
                continue;
            }

            if (op == OpType::Recv) {
                if (slot_idx >= max_conns) continue;
                ConnSlot& slot = slots[slot_idx];

                if (res <= 0 || !slot.active || slot.generation != gen) {
                    if (slot.active && slot.generation == gen) {
                        close(slot.fd);
                        slot.fd = -1;
                        cleanup_slot(slot);
                        free_slots.push_back(slot_idx);
                    }
                    continue;
                }

                if (cqe->flags & IORING_CQE_F_BUFFER) {
                    uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                    uint8_t* in_data = raw_bufs + (bid * BUF_SIZE);

                    std::string_view req_data;
                    if (slot.partial != nullptr && !slot.partial->empty()) {
                        slot.partial->insert(slot.partial->end(), reinterpret_cast<char*>(in_data), reinterpret_cast<char*>(in_data) + res);
                        req_data = std::string_view(slot.partial->data(), slot.partial->size());
                    } else {
                        req_data = std::string_view(reinterpret_cast<char*>(in_data), res);
                    }

                    while (!req_data.empty()) {
                        http::Request req;
                        size_t bytes_consumed = 0;
                        bool parsed = http::parse_request(req_data, req, bytes_consumed);
                        if (!parsed) {
                            break;
                        }

                        uint16_t sbuf_id = 0;
                        char* out_buf = nullptr;
                        if (!free_send_bufs.empty()) {
                            sbuf_id = free_send_bufs.back();
                            free_send_bufs.pop_back();
                            out_buf = send_bufs_mem.get() + (static_cast<size_t>(sbuf_id) * SEND_BUF_SIZE);
                        } else {
                            sbuf_id = static_cast<uint16_t>(SEND_BUF_POOL_SIZE + (next_dynamic_id++ & 0x7fff));
                            auto dyn_buf = std::make_unique<char[]>(SEND_BUF_SIZE);
                            out_buf = dyn_buf.get();
                            dynamic_send_bufs.push_back({sbuf_id, std::move(dyn_buf)});
                        }

                        slot.keep_alive = req.keep_alive;
                        SendItem item;
                        item.slot = slot_idx;
                        item.gen = gen;
                        item.fd = slot.fd;
                        item.buf_id = sbuf_id;
                        item.buf_ptr = out_buf;

                        // Route Handling
                        switch (req.route) {
                            case http::RouteType::Health: {
                                bool db_ok = db_.check_health();
                                if (db_ok) {
                                    int uptime = static_cast<int>(now_s - start_time_s_);
                                    char body[128];
                                    int blen = snprintf(body, sizeof(body),
                                        "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%d}", uptime);
                                    item.len = http::format_response(200, "OK", std::string_view(body, blen), slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else {
                                    item.len = http::format_response(503, "Service Unavailable", "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database error\"}", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                }
                                break;
                            }
                            case http::RouteType::Feed: {
                                size_t flen = db_.get_feed(work_buf.get(), SEND_BUF_SIZE);
                                item.len = http::format_response(200, "OK", std::string_view(work_buf.get(), flen), slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                break;
                            }
                            case http::RouteType::PostGet: {
                                auto pres = db_.get_post(req.post_id, work_buf.get(), SEND_BUF_SIZE);
                                if (pres.found) {
                                    item.len = http::format_response(200, "OK", std::string_view(work_buf.get(), pres.json_len), slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else {
                                    item.len = http::format_error(404, "Not Found", "post not found", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                }
                                break;
                            }
                            case http::RouteType::PostCreate: {
                                auto auth = jwt_.verify(req.auth_header, now_s);
                                if (auth.error == sec::AuthError::MissingBearer) {
                                    item.len = http::format_error(401, "Unauthorized", "missing bearer token", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else if (auth.error == sec::AuthError::InvalidOrExpired) {
                                    item.len = http::format_error(401, "Unauthorized", "invalid or expired token", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else if (auth.error == sec::AuthError::InvalidPayload) {
                                    item.len = http::format_error(401, "Unauthorized", "invalid token payload", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else {
                                    auto post_payload = json::parse_create_post(req.body);
                                    if (post_payload.error == json::ParseError::MalformedJson) {
                                        item.len = http::format_error(400, "Bad Request", "malformed JSON body", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    } else if (post_payload.error == json::ParseError::BodyRequired) {
                                        item.len = http::format_error(400, "Bad Request", "body is required", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    } else if (post_payload.error == json::ParseError::BodyTooLong) {
                                        item.len = http::format_error(400, "Bad Request", "body must be at most 500 characters", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    } else {
                                        auto c_res = db_.create_post(auth.user_id, auth.username(), post_payload.get_body(), work_buf.get(), SEND_BUF_SIZE);
                                        if (c_res.found) {
                                            item.len = http::format_response(201, "Created", std::string_view(work_buf.get(), c_res.json_len), slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                        } else {
                                            item.len = http::format_error(500, "Internal Server Error", "internal server error", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                        }
                                    }
                                }
                                break;
                            }
                            case http::RouteType::PostLike: {
                                auto auth = jwt_.verify(req.auth_header, now_s);
                                if (auth.error == sec::AuthError::MissingBearer) {
                                    item.len = http::format_error(401, "Unauthorized", "missing bearer token", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else if (auth.error == sec::AuthError::InvalidOrExpired) {
                                    item.len = http::format_error(401, "Unauthorized", "invalid or expired token", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else if (auth.error == sec::AuthError::InvalidPayload) {
                                    item.len = http::format_error(401, "Unauthorized", "invalid token payload", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                } else {
                                    auto lres = db_.like_post(auth.user_id, req.post_id, work_buf.get(), SEND_BUF_SIZE);
                                    if (lres.status == db::LikeResult::SuccessNew) {
                                        item.len = http::format_response(201, "Created", std::string_view(work_buf.get(), lres.json_len), slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    } else if (lres.status == db::LikeResult::SuccessAlready) {
                                        item.len = http::format_response(200, "OK", std::string_view(work_buf.get(), lres.json_len), slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    } else if (lres.status == db::LikeResult::PostNotFound) {
                                        item.len = http::format_error(404, "Not Found", "post not found", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    } else {
                                        item.len = http::format_error(500, "Internal Server Error", "internal server error", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                    }
                                }
                                break;
                            }
                            case http::RouteType::InvalidId: {
                                if (req.path.starts_with("/posts/") && req.path.ends_with("/like")) {
                                    auto auth = jwt_.verify(req.auth_header, now_s);
                                    if (auth.error == sec::AuthError::MissingBearer) {
                                        item.len = http::format_error(401, "Unauthorized", "missing bearer token", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                        break;
                                    } else if (auth.error == sec::AuthError::InvalidOrExpired) {
                                        item.len = http::format_error(401, "Unauthorized", "invalid or expired token", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                        break;
                                    } else if (auth.error == sec::AuthError::InvalidPayload) {
                                        item.len = http::format_error(401, "Unauthorized", "invalid token payload", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                        break;
                                    }
                                }
                                item.len = http::format_error(400, "Bad Request", "invalid post id", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                break;
                            }
                            case http::RouteType::NotFound:
                            default: {
                                item.len = http::format_error(404, "Not Found", "not found", slot.keep_alive, out_buf, SEND_BUF_SIZE);
                                break;
                            }
                        }

                        pending_sends.push_back(item);
                        req_data.remove_prefix(bytes_consumed);

                        if (!slot.keep_alive) {
                            break;
                        }
                    }

                    // Compact or update leftover partial data
                    if (!req_data.empty()) {
                        if (slot.partial == nullptr) {
                            slot.partial = new std::vector<char>(req_data.begin(), req_data.end());
                        } else {
                            if (req_data.data() != slot.partial->data()) {
                                std::memmove(slot.partial->data(), req_data.data(), req_data.size());
                            }
                            slot.partial->resize(req_data.size());
                        }

                        if (slot.partial->size() > 16384) {
                            close(slot.fd);
                            slot.fd = -1;
                            cleanup_slot(slot);
                            free_slots.push_back(slot_idx);
                        }
                    } else {
                        if (slot.partial != nullptr) {
                            slot.partial->clear();
                        }
                    }

                    // Recycle buffer into ring
                    io_uring_buf_ring_add(br, in_data, BUF_SIZE, bid, io_uring_buf_ring_mask(BUF_COUNT), bufs_to_advance);
                    bufs_to_advance++;
                }

                // If multishot recv ended on active connection, re-arm
                if (!(cqe->flags & IORING_CQE_F_MORE) && slot.active && slot.generation == gen) {
                    struct io_uring_sqe* rsqe = get_sqe_safe(&ring);
                    if (rsqe) {
                        io_uring_prep_recv_multishot(rsqe, slot.fd, nullptr, 0, 0);
                        rsqe->buf_group = BUF_GROUP_ID;
                        rsqe->flags |= IOSQE_BUFFER_SELECT;
                        rsqe->user_data = make_user_data(OpType::Recv, slot_idx, slot.generation);
                    }
                }
            }
        }

        if (bufs_to_advance > 0) {
            io_uring_buf_ring_advance(br, bufs_to_advance);
        }

        // Commit all batched SQLite writes synchronously before replying
        db_.commit_batch();

        // Submit queued responses
        for (const auto& item : pending_sends) {
            struct io_uring_sqe* ssqe = get_sqe_safe(&ring);
            if (ssqe) {
                io_uring_prep_send(ssqe, item.fd, item.buf_ptr, item.len, MSG_NOSIGNAL);
                ssqe->user_data = make_user_data(OpType::Send, item.slot, item.gen, item.buf_id);
            }
        }
        pending_sends.clear();

        io_uring_submit(&ring);
        io_uring_cq_advance(&ring, count);
    }

    io_uring_queue_exit(&ring);
#else
    std::cout << "[Mock] Server running loop (non-Linux mock)\n";
#endif
}

void Server::stop() {
    running_ = false;
    db_.close();
#ifdef __linux__
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
#endif
}

} // namespace net
