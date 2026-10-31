#pragma once

#include <string_view>
#include <cstdint>
#include <cstring>
#include <strings.h>
#include <charconv>

namespace http {

enum class Method {
    Get,
    Post,
    Other
};

enum class RouteType {
    Health,
    Feed,
    PostCreate,      // POST /posts
    PostGet,         // GET /posts/:id
    PostLike,        // POST /posts/:id/like
    InvalidId,       // e.g. /posts/abc or /posts/0
    NotFound
};

struct Request {
    Method method{Method::Other};
    std::string_view path;
    RouteType route{RouteType::NotFound};
    int64_t post_id{0}; // if PostGet or PostLike or InvalidId

    std::string_view auth_header;
    std::string_view body;
    bool keep_alive{true};
    size_t content_length{0};
};

inline bool parse_positive_id(std::string_view s, int64_t& out_id) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    if (s.size() == 1 && s[0] == '0') return false; // "0" is not positive
    if (s[0] == '0') return false; // leading zero disallowed
    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec != std::errc{} || ptr != s.data() + s.size() || val <= 0) return false;
    out_id = val;
    return true;
}

// Zero-copy HTTP/1.1 request parser
inline bool parse_request(std::string_view raw, Request& req, size_t& bytes_consumed) {
    bytes_consumed = 0;
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        // Need more data
        return false;
    }

    std::string_view header_section = raw.substr(0, header_end);
    size_t first_line_end = header_section.find("\r\n");
    std::string_view first_line;
    size_t headers_start = 0;
    if (first_line_end == std::string_view::npos) {
        first_line = header_section;
        headers_start = header_section.size();
    } else {
        first_line = header_section.substr(0, first_line_end);
        headers_start = first_line_end + 2;
    }

    // Parse Method
    if (first_line.starts_with("GET ")) {
        req.method = Method::Get;
        first_line = first_line.substr(4);
    } else if (first_line.starts_with("POST ")) {
        req.method = Method::Post;
        first_line = first_line.substr(5);
    } else {
        req.method = Method::Other;
        size_t sp = first_line.find(' ');
        if (sp == std::string_view::npos) return false;
        first_line = first_line.substr(sp + 1);
    }

    // Parse Path
    size_t path_end = first_line.find(' ');
    if (path_end == std::string_view::npos) {
        req.path = first_line;
    } else {
        req.path = first_line.substr(0, path_end);
    }

    // Parse Headers
    req.auth_header = {};
    req.content_length = 0;
    req.keep_alive = true; // HTTP/1.1 default

    size_t pos = headers_start;
    while (pos < header_section.size()) {
        size_t line_end = header_section.find("\r\n", pos);
        if (line_end == std::string_view::npos) line_end = header_section.size();
        std::string_view line = header_section.substr(pos, line_end - pos);
        pos = line_end + 2;

        if (line.size() > 14 && (strncasecmp(line.data(), "Authorization:", 14) == 0)) {
            size_t val_start = line.find_first_not_of(" \t", 14);
            if (val_start != std::string_view::npos) {
                req.auth_header = line.substr(val_start);
            }
        } else if (line.size() > 15 && (strncasecmp(line.data(), "Content-Length:", 15) == 0)) {
            size_t val_start = line.find_first_not_of(" \t", 15);
            if (val_start != std::string_view::npos) {
                std::from_chars(line.data() + val_start, line.data() + line.size(), req.content_length);
            }
        } else if (line.size() > 11 && (strncasecmp(line.data(), "Connection:", 11) == 0)) {
            size_t val_start = line.find_first_not_of(" \t", 11);
            if (val_start != std::string_view::npos) {
                std::string_view val = line.substr(val_start);
                if (val.size() >= 5 && strncasecmp(val.data(), "close", 5) == 0) {
                    req.keep_alive = false;
                }
            }
        }
    }

    size_t total_needed = header_end + 4 + req.content_length;
    if (raw.size() < total_needed) {
        // Incomplete body
        return false;
    }

    req.body = raw.substr(header_end + 4, req.content_length);
    bytes_consumed = total_needed;

    // Route matching
    req.route = RouteType::NotFound;
    req.post_id = 0;

    if (req.path == "/health" && req.method == Method::Get) {
        req.route = RouteType::Health;
    } else if (req.path == "/feed" && req.method == Method::Get) {
        req.route = RouteType::Feed;
    } else if (req.path == "/posts" && req.method == Method::Post) {
        req.route = RouteType::PostCreate;
    } else if (req.path.starts_with("/posts/")) {
        std::string_view rest = req.path.substr(7); // after "/posts/"
        if (rest.ends_with("/like") && req.method == Method::Post) {
            std::string_view id_part = rest.substr(0, rest.size() - 5);
            if (parse_positive_id(id_part, req.post_id)) {
                req.route = RouteType::PostLike;
            } else {
                req.route = RouteType::InvalidId;
            }
        } else if (req.method == Method::Get) {
            if (parse_positive_id(rest, req.post_id)) {
                req.route = RouteType::PostGet;
            } else {
                req.route = RouteType::InvalidId;
            }
        } else {
            req.route = RouteType::NotFound;
        }
    }

    return true;
}

// Direct buffer formatter for HTTP responses
inline size_t format_response(
    int status_code,
    std::string_view status_msg,
    std::string_view body,
    bool keep_alive,
    char* out,
    size_t max_out
) {
    // Construct standard headers
    // "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n"
    size_t written = 0;
    auto append = [&](std::string_view s) {
        if (written + s.size() <= max_out) {
            std::memcpy(out + written, s.data(), s.size());
            written += s.size();
        }
    };

    append("HTTP/1.1 ");
    char code_buf[16];
    auto [p1, _] = std::to_chars(code_buf, code_buf + sizeof(code_buf), status_code);
    append(std::string_view(code_buf, p1 - code_buf));
    append(" ");
    append(status_msg);
    append("\r\nContent-Type: application/json\r\nContent-Length: ");

    char len_buf[32];
    auto [p2, __] = std::to_chars(len_buf, len_buf + sizeof(len_buf), body.size());
    append(std::string_view(len_buf, p2 - len_buf));

    if (keep_alive) {
        append("\r\nConnection: keep-alive\r\n\r\n");
    } else {
        append("\r\nConnection: close\r\n\r\n");
    }

    append(body);
    return written;
}

// Pre-baked common error responses
inline size_t format_error(int status_code, std::string_view status_msg, std::string_view err_msg, bool keep_alive, char* out, size_t max_out) {
    char body[128];
    size_t blen = 0;
    std::memcpy(body, "{\"error\":\"", 10); blen += 10;
    std::memcpy(body + blen, err_msg.data(), err_msg.size()); blen += err_msg.size();
    body[blen++] = '"';
    body[blen++] = '}';
    return format_response(status_code, status_msg, std::string_view(body, blen), keep_alive, out, max_out);
}

} // namespace http
