#pragma once

#include <string_view>
#include <cstdint>
#include <cstddef>
#include "json.hpp"

// Forward declaration of sqlite3
struct sqlite3;
struct sqlite3_stmt;

namespace db {

struct PostResult {
    bool found{false};
    size_t json_len{0};
};

struct LikeResult {
    enum Status {
        SuccessNew,       // 201
        SuccessAlready,   // 200
        PostNotFound,     // 404
        DbError           // 500
    };
    Status status{Status::PostNotFound};
    size_t json_len{0};
};

class Database {
public:
    Database();
    ~Database();

    bool open(const char* path);
    void close();

    bool check_health();

    // Query methods
    size_t get_feed(char* out, size_t max_out);
    PostResult get_post(int64_t post_id, char* out, size_t max_out);

    // Write methods (can participate in group commit)
    PostResult create_post(int64_t user_id, std::string_view author, std::string_view body, char* out, size_t max_out);
    LikeResult like_post(int64_t user_id, int64_t post_id, char* out, size_t max_out);

    // Group commit support
    void begin_batch();
    bool commit_batch();
    void rollback_batch();

private:
    sqlite3* db_{nullptr};

    // Prepared statements
    sqlite3_stmt* stmt_health_{nullptr};
    sqlite3_stmt* stmt_feed_{nullptr};
    sqlite3_stmt* stmt_get_post_{nullptr};
    sqlite3_stmt* stmt_create_post_{nullptr};
    sqlite3_stmt* stmt_check_post_{nullptr};
    sqlite3_stmt* stmt_like_post_{nullptr};
    sqlite3_stmt* stmt_begin_{nullptr};
    sqlite3_stmt* stmt_commit_{nullptr};
    sqlite3_stmt* stmt_rollback_{nullptr};

    bool in_transaction_{false};
    bool ensure_transaction();
};

} // namespace db
