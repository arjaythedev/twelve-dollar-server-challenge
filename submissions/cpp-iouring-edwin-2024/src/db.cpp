#include "db.hpp"
#include <sqlite3.h>
#include <cstring>
#include <charconv>

namespace db {

Database::Database() = default;

Database::~Database() {
    close();
}

bool Database::open(const char* path) {
    if (sqlite3_open_v2(path, &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        return false;
    }

    // Set high-performance PRAGMAs
    sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA mmap_size = 2147483648;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size = -64000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA locking_mode = EXCLUSIVE;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA threads = 0;", nullptr, nullptr, nullptr);

    // Prepare statements
    const char* sql_health = "SELECT 1;";
    sqlite3_prepare_v2(db_, sql_health, -1, &stmt_health_, nullptr);

    const char* sql_feed =
        "SELECT p.id, p.body, p.created_at, u.username, "
        "       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
        "  FROM posts p JOIN users u ON u.id = p.user_id "
        " ORDER BY p.created_at DESC, p.id DESC LIMIT 20;";
    sqlite3_prepare_v2(db_, sql_feed, -1, &stmt_feed_, nullptr);

    const char* sql_get_post =
        "SELECT p.id, p.body, p.created_at, u.username, "
        "       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
        "  FROM posts p JOIN users u ON u.id = p.user_id "
        " WHERE p.id = ?;";
    sqlite3_prepare_v2(db_, sql_get_post, -1, &stmt_get_post_, nullptr);

    const char* sql_create = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at;";
    sqlite3_prepare_v2(db_, sql_create, -1, &stmt_create_post_, nullptr);

    const char* sql_check = "SELECT 1 FROM posts WHERE id = ?;";
    sqlite3_prepare_v2(db_, sql_check, -1, &stmt_check_post_, nullptr);

    const char* sql_like = "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING;";
    sqlite3_prepare_v2(db_, sql_like, -1, &stmt_like_post_, nullptr);

    const char* sql_begin = "BEGIN IMMEDIATE;";
    sqlite3_prepare_v2(db_, sql_begin, -1, &stmt_begin_, nullptr);

    const char* sql_commit = "COMMIT;";
    sqlite3_prepare_v2(db_, sql_commit, -1, &stmt_commit_, nullptr);

    const char* sql_rollback = "ROLLBACK;";
    sqlite3_prepare_v2(db_, sql_rollback, -1, &stmt_rollback_, nullptr);

    return true;
}

void Database::close() {
    if (stmt_health_) { sqlite3_finalize(stmt_health_); stmt_health_ = nullptr; }
    if (stmt_feed_) { sqlite3_finalize(stmt_feed_); stmt_feed_ = nullptr; }
    if (stmt_get_post_) { sqlite3_finalize(stmt_get_post_); stmt_get_post_ = nullptr; }
    if (stmt_create_post_) { sqlite3_finalize(stmt_create_post_); stmt_create_post_ = nullptr; }
    if (stmt_check_post_) { sqlite3_finalize(stmt_check_post_); stmt_check_post_ = nullptr; }
    if (stmt_like_post_) { sqlite3_finalize(stmt_like_post_); stmt_like_post_ = nullptr; }
    if (stmt_begin_) { sqlite3_finalize(stmt_begin_); stmt_begin_ = nullptr; }
    if (stmt_commit_) { sqlite3_finalize(stmt_commit_); stmt_commit_ = nullptr; }
    if (stmt_rollback_) { sqlite3_finalize(stmt_rollback_); stmt_rollback_ = nullptr; }

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::check_health() {
    if (!stmt_health_) return false;
    sqlite3_reset(stmt_health_);
    int rc = sqlite3_step(stmt_health_);
    sqlite3_reset(stmt_health_);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE);
}

void Database::begin_batch() {
    in_transaction_ = false;
}

bool Database::ensure_transaction() {
    if (in_transaction_) return true;
    sqlite3_reset(stmt_begin_);
    if (sqlite3_step(stmt_begin_) == SQLITE_DONE) {
        sqlite3_reset(stmt_begin_);
        in_transaction_ = true;
        return true;
    }
    sqlite3_reset(stmt_begin_);
    return false;
}

bool Database::commit_batch() {
    if (!in_transaction_) return true;
    sqlite3_reset(stmt_commit_);
    int rc = sqlite3_step(stmt_commit_);
    sqlite3_reset(stmt_commit_);
    in_transaction_ = false;
    return (rc == SQLITE_DONE);
}

void Database::rollback_batch() {
    if (!in_transaction_) return;
    sqlite3_reset(stmt_rollback_);
    sqlite3_step(stmt_rollback_);
    sqlite3_reset(stmt_rollback_);
    in_transaction_ = false;
}

size_t Database::get_feed(char* out, size_t max_out) {
    sqlite3_reset(stmt_feed_);

    size_t written = 0;
    auto append = [&](std::string_view s) {
        if (written + s.size() <= max_out) {
            std::memcpy(out + written, s.data(), s.size());
            written += s.size();
        }
    };

    append("{\"posts\":[");
    bool first = true;

    while (sqlite3_step(stmt_feed_) == SQLITE_ROW) {
        if (!first) append(",");
        first = false;

        int64_t id = sqlite3_column_int64(stmt_feed_, 0);
        const char* body_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt_feed_, 1));
        int body_bytes = sqlite3_column_bytes(stmt_feed_, 1);
        const char* created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt_feed_, 2));
        int created_at_bytes = sqlite3_column_bytes(stmt_feed_, 2);
        const char* author = reinterpret_cast<const char*>(sqlite3_column_text(stmt_feed_, 3));
        int author_bytes = sqlite3_column_bytes(stmt_feed_, 3);
        int64_t like_count = sqlite3_column_int64(stmt_feed_, 4);

        append("{\"id\":");
        char id_buf[24];
        auto [p1, _1] = std::to_chars(id_buf, id_buf + sizeof(id_buf), id);
        append(std::string_view(id_buf, p1 - id_buf));

        append(",\"body\":\"");
        if (written < max_out) {
            written += json::escape_json_string(std::string_view(body_raw, body_bytes), out + written, max_out - written);
        }
        append("\",\"created_at\":\"");
        append(std::string_view(created_at, created_at_bytes));
        append("\",\"author\":\"");
        append(std::string_view(author, author_bytes));
        append("\",\"like_count\":");

        char like_buf[24];
        auto [p2, _2] = std::to_chars(like_buf, like_buf + sizeof(like_buf), like_count);
        append(std::string_view(like_buf, p2 - like_buf));
        append("}");
    }

    sqlite3_reset(stmt_feed_);
    append("]}");
    return written;
}

PostResult Database::get_post(int64_t post_id, char* out, size_t max_out) {
    PostResult res;
    sqlite3_reset(stmt_get_post_);
    sqlite3_bind_int64(stmt_get_post_, 1, post_id);

    int rc = sqlite3_step(stmt_get_post_);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(stmt_get_post_);
        sqlite3_clear_bindings(stmt_get_post_);
        res.found = false;
        return res;
    }

    int64_t id = sqlite3_column_int64(stmt_get_post_, 0);
    const char* body_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_post_, 1));
    int body_bytes = sqlite3_column_bytes(stmt_get_post_, 1);
    const char* created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_post_, 2));
    int created_at_bytes = sqlite3_column_bytes(stmt_get_post_, 2);
    const char* author = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_post_, 3));
    int author_bytes = sqlite3_column_bytes(stmt_get_post_, 3);
    int64_t like_count = sqlite3_column_int64(stmt_get_post_, 4);

    size_t written = 0;
    auto append = [&](std::string_view s) {
        if (written + s.size() <= max_out) {
            std::memcpy(out + written, s.data(), s.size());
            written += s.size();
        }
    };

    append("{\"post\":{\"id\":");
    char id_buf[24];
    auto [p1, _1] = std::to_chars(id_buf, id_buf + sizeof(id_buf), id);
    append(std::string_view(id_buf, p1 - id_buf));

    append(",\"body\":\"");
    if (written < max_out) {
        written += json::escape_json_string(std::string_view(body_raw, body_bytes), out + written, max_out - written);
    }
    append("\",\"created_at\":\"");
    append(std::string_view(created_at, created_at_bytes));
    append("\",\"author\":\"");
    append(std::string_view(author, author_bytes));
    append("\",\"like_count\":");

    char like_buf[24];
    auto [p2, _2] = std::to_chars(like_buf, like_buf + sizeof(like_buf), like_count);
    append(std::string_view(like_buf, p2 - like_buf));
    append("}}");

    sqlite3_reset(stmt_get_post_);
    sqlite3_clear_bindings(stmt_get_post_);

    res.found = true;
    res.json_len = written;
    return res;
}

PostResult Database::create_post(int64_t user_id, std::string_view author, std::string_view body, char* out, size_t max_out) {
    PostResult res;
    if (!ensure_transaction()) {
        res.found = false;
        return res;
    }

    sqlite3_reset(stmt_create_post_);
    sqlite3_bind_int64(stmt_create_post_, 1, user_id);
    sqlite3_bind_text(stmt_create_post_, 2, body.data(), static_cast<int>(body.size()), SQLITE_STATIC);

    int rc = sqlite3_step(stmt_create_post_);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(stmt_create_post_);
        sqlite3_clear_bindings(stmt_create_post_);
        res.found = false;
        return res;
    }

    int64_t new_id = sqlite3_column_int64(stmt_create_post_, 0);
    const char* created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt_create_post_, 1));
    int created_at_bytes = sqlite3_column_bytes(stmt_create_post_, 1);

    size_t written = 0;
    auto append = [&](std::string_view s) {
        if (written + s.size() <= max_out) {
            std::memcpy(out + written, s.data(), s.size());
            written += s.size();
        }
    };

    append("{\"post\":{\"id\":");
    char id_buf[24];
    auto [p1, _1] = std::to_chars(id_buf, id_buf + sizeof(id_buf), new_id);
    append(std::string_view(id_buf, p1 - id_buf));

    append(",\"body\":\"");
    if (written < max_out) {
        written += json::escape_json_string(body, out + written, max_out - written);
    }
    append("\",\"created_at\":\"");
    append(std::string_view(created_at, created_at_bytes));
    append("\",\"author\":\"");
    append(author);
    append("\",\"like_count\":0}}");

    sqlite3_reset(stmt_create_post_);
    sqlite3_clear_bindings(stmt_create_post_);

    res.found = true;
    res.json_len = written;
    return res;
}

LikeResult Database::like_post(int64_t user_id, int64_t post_id, char* out, size_t max_out) {
    LikeResult res;

    // Check if post exists
    sqlite3_reset(stmt_check_post_);
    sqlite3_bind_int64(stmt_check_post_, 1, post_id);
    int rc = sqlite3_step(stmt_check_post_);
    sqlite3_reset(stmt_check_post_);
    sqlite3_clear_bindings(stmt_check_post_);

    if (rc != SQLITE_ROW) {
        res.status = LikeResult::PostNotFound;
        return res;
    }

    if (!ensure_transaction()) {
        res.status = LikeResult::DbError;
        return res;
    }

    sqlite3_reset(stmt_like_post_);
    sqlite3_bind_int64(stmt_like_post_, 1, user_id);
    sqlite3_bind_int64(stmt_like_post_, 2, post_id);
    rc = sqlite3_step(stmt_like_post_);
    sqlite3_reset(stmt_like_post_);
    sqlite3_clear_bindings(stmt_like_post_);

    if (rc != SQLITE_DONE) {
        res.status = LikeResult::DbError;
        return res;
    }

    int changes = sqlite3_changes(db_);
    bool already_liked = (changes == 0);

    size_t written = 0;
    auto append = [&](std::string_view s) {
        if (written + s.size() <= max_out) {
            std::memcpy(out + written, s.data(), s.size());
            written += s.size();
        }
    };

    append("{\"liked\":true,\"already_liked\":");
    append(already_liked ? "true" : "false");
    append(",\"post_id\":");
    char id_buf[24];
    auto [p1, _1] = std::to_chars(id_buf, id_buf + sizeof(id_buf), post_id);
    append(std::string_view(id_buf, p1 - id_buf));
    append("}");

    res.status = already_liked ? LikeResult::SuccessAlready : LikeResult::SuccessNew;
    res.json_len = written;
    return res;
}

} // namespace db
