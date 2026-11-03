#pragma once

#include <string_view>
#include <string>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace json {

enum class ParseError {
    None,
    MalformedJson,
    BodyRequired,
    BodyTooLong
};

struct CreatePostPayload {
    ParseError error{ParseError::None};
    char body[2048]{};
    size_t body_len{0};

    std::string_view get_body() const {
        return std::string_view(body, body_len);
    }
};

inline size_t utf8_length(std::string_view s) {
    size_t count = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        // Count code points: leading bytes only (not continuation bytes 10xxxxxx)
        if ((static_cast<uint8_t>(s[i]) & 0xc0) != 0x80) {
            count++;
        }
    }
    return count;
}

inline std::string_view trim_whitespace(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
        start++;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r')) {
        end--;
    }
    return s.substr(start, end - start);
}

// Parses {"body": "..."}
inline CreatePostPayload parse_create_post(std::string_view raw) {
    CreatePostPayload res;
    std::string_view s = trim_whitespace(raw);
    if (s.empty() || s.front() != '{' || s.back() != '}') {
        res.error = ParseError::MalformedJson;
        return res;
    }

    // Skip '{'
    s = trim_whitespace(s.substr(1, s.size() - 2));

    // Look for key "body"
    if (s.empty()) {
        res.error = ParseError::BodyRequired; // empty object {}
        return res;
    }

    // Fast path: {"body": "..."} (standard case)
    if (s.starts_with("\"body\"")) {
        s = trim_whitespace(s.substr(6));
        if (s.empty() || s.front() != ':') {
            res.error = ParseError::MalformedJson;
            return res;
        }
        s = trim_whitespace(s.substr(1));
        if (s.empty()) {
            res.error = ParseError::MalformedJson;
            return res;
        }
        if (s.front() != '"') {
            res.error = ParseError::BodyRequired;
            return res;
        }
    } else {
        // Multi-field or non-"body" first key: parse keys sequentially
        size_t p = 0;
        bool found_body = false;
        while (p < s.size()) {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n' || s[p] == ',')) p++;
            if (p >= s.size()) break;
            if (s[p] != '"') {
                res.error = ParseError::MalformedJson;
                return res;
            }
            p++;
            size_t key_start = p;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\') p += 2;
                else p++;
            }
            if (p >= s.size()) {
                res.error = ParseError::MalformedJson;
                return res;
            }
            std::string_view key = s.substr(key_start, p - key_start);
            p++; // skip closing quote
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
            if (p >= s.size() || s[p] != ':') {
                res.error = ParseError::MalformedJson;
                return res;
            }
            p++; // skip ':'
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
            if (p >= s.size()) {
                res.error = ParseError::MalformedJson;
                return res;
            }

            if (key == "body") {
                if (s[p] != '"') {
                    res.error = ParseError::BodyRequired;
                    return res;
                }
                s = s.substr(p);
                found_body = true;
                break;
            } else {
                // Skip value
                if (s[p] == '"') {
                    p++;
                    while (p < s.size() && s[p] != '"') {
                        if (s[p] == '\\') p += 2;
                        else p++;
                    }
                    if (p < s.size()) p++;
                } else if (s[p] == '{' || s[p] == '[') {
                    res.error = ParseError::MalformedJson;
                    return res;
                } else {
                    while (p < s.size() && s[p] != ',' && s[p] != '}') p++;
                }
            }
        }
        if (!found_body) {
            res.error = ParseError::BodyRequired;
            return res;
        }
    }

    // Unescape JSON string
    size_t idx = 1;
    char unescaped[2048];
    size_t out_len = 0;
    bool closed = false;

    while (idx < s.size()) {
        char c = s[idx++];
        if (c == '"') {
            closed = true;
            break;
        }
        if (c == '\\') {
            if (idx >= s.size()) {
                res.error = ParseError::MalformedJson;
                return res;
            }
            char esc = s[idx++];
            switch (esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    if (idx + 4 > s.size()) {
                        res.error = ParseError::MalformedJson;
                        return res;
                    }
                    // Parse 4 hex digits
                    uint32_t codepoint = 0;
                    for (int h = 0; h < 4; ++h) {
                        char hc = s[idx++];
                        uint32_t val = 0;
                        if (hc >= '0' && hc <= '9') val = hc - '0';
                        else if (hc >= 'a' && hc <= 'f') val = hc - 'a' + 10;
                        else if (hc >= 'A' && hc <= 'F') val = hc - 'A' + 10;
                        else {
                            res.error = ParseError::MalformedJson;
                            return res;
                        }
                        codepoint = (codepoint << 4) | val;
                    }
                    // Handle surrogate pairs if any
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (idx + 6 <= s.size() && s[idx] == '\\' && s[idx + 1] == 'u') {
                            idx += 2;
                            uint32_t low = 0;
                            for (int h = 0; h < 4; ++h) {
                                char hc = s[idx++];
                                uint32_t val = 0;
                                if (hc >= '0' && hc <= '9') val = hc - '0';
                                else if (hc >= 'a' && hc <= 'f') val = hc - 'a' + 10;
                                else if (hc >= 'A' && hc <= 'F') val = hc - 'A' + 10;
                                else {
                                    res.error = ParseError::MalformedJson;
                                    return res;
                                }
                                low = (low << 4) | val;
                            }
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                codepoint = 0x10000 + (((codepoint & 0x3FF) << 10) | (low & 0x3FF));
                            }
                        }
                    }
                    // Encode codepoint to UTF-8 into unescaped
                    if (codepoint <= 0x7F) {
                        if (out_len >= sizeof(unescaped)) { res.error = ParseError::BodyTooLong; return res; }
                        unescaped[out_len++] = static_cast<char>(codepoint);
                    } else if (codepoint <= 0x7FF) {
                        if (out_len + 2 > sizeof(unescaped)) { res.error = ParseError::BodyTooLong; return res; }
                        unescaped[out_len++] = static_cast<char>(0xC0 | (codepoint >> 6));
                        unescaped[out_len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
                    } else if (codepoint <= 0xFFFF) {
                        if (out_len + 3 > sizeof(unescaped)) { res.error = ParseError::BodyTooLong; return res; }
                        unescaped[out_len++] = static_cast<char>(0xE0 | (codepoint >> 12));
                        unescaped[out_len++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        unescaped[out_len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
                    } else {
                        if (out_len + 4 > sizeof(unescaped)) { res.error = ParseError::BodyTooLong; return res; }
                        unescaped[out_len++] = static_cast<char>(0xF0 | (codepoint >> 18));
                        unescaped[out_len++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                        unescaped[out_len++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        unescaped[out_len++] = static_cast<char>(0x80 | (codepoint & 0x3F));
                    }
                    continue;
                }
                default:
                    res.error = ParseError::MalformedJson;
                    return res;
            }
        }
        if (out_len >= sizeof(unescaped)) {
            res.error = ParseError::BodyTooLong;
            return res;
        }
        unescaped[out_len++] = c;
    }

    if (!closed) {
        res.error = ParseError::MalformedJson;
        return res;
    }

    std::string_view unescaped_sv(unescaped, out_len);
    std::string_view trimmed = trim_whitespace(unescaped_sv);

    if (trimmed.empty()) {
        res.error = ParseError::BodyRequired;
        return res;
    }

    size_t ulen = utf8_length(trimmed);
    if (ulen > 500) {
        res.error = ParseError::BodyTooLong;
        return res;
    }

    if (trimmed.size() >= sizeof(res.body)) {
        res.error = ParseError::BodyTooLong;
        return res;
    }

    std::memcpy(res.body, trimmed.data(), trimmed.size());
    res.body_len = trimmed.size();
    res.body[res.body_len] = '\0';
    res.error = ParseError::None;
    return res;
}

// Writes string escaped into output buffer, returns bytes written
inline size_t escape_json_string(std::string_view in, char* out, size_t max_out) {
    size_t i = 0;
    while (i < in.size() && static_cast<unsigned char>(in[i]) >= 32 && in[i] != '"' && in[i] != '\\') {
        ++i;
    }
    if (i == in.size()) {
        size_t to_copy = (in.size() < max_out) ? in.size() : max_out;
        std::memcpy(out, in.data(), to_copy);
        return to_copy;
    }

    size_t written = 0;
    if (i > 0) {
        size_t to_copy = (i < max_out) ? i : max_out;
        std::memcpy(out, in.data(), to_copy);
        written = to_copy;
    }

    for (; i < in.size(); ++i) {
        unsigned char c = in[i];
        if (c == '"') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = '"';
        } else if (c == '\\') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = '\\';
        } else if (c == '\b') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = 'b';
        } else if (c == '\f') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = 'f';
        } else if (c == '\n') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = 'n';
        } else if (c == '\r') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = 'r';
        } else if (c == '\t') {
            if (written + 2 > max_out) break;
            out[written++] = '\\'; out[written++] = 't';
        } else if (c < 32) {
            // Control characters \u00xx
            if (written + 6 > max_out) break;
            out[written++] = '\\'; out[written++] = 'u'; out[written++] = '0'; out[written++] = '0';
            const char* hex = "0123456789abcdef";
            out[written++] = hex[(c >> 4) & 0xf];
            out[written++] = hex[c & 0xf];
        } else {
            if (written + 1 > max_out) break;
            out[written++] = c;
        }
    }
    return written;
}

} // namespace json
