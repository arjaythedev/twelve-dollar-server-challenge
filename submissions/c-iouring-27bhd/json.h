#ifndef JSON_H
#define JSON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define JSON_PARSE_OK 0
#define JSON_PARSE_MALFORMED 1
#define JSON_PARSE_BODY_REQUIRED 2
#define JSON_PARSE_BODY_TOO_LONG 3

/* Checks JavaScript String.prototype.trim() whitespace character */
static inline bool is_js_whitespace(const uint8_t *p, size_t rem, size_t *char_bytes) {
    if (rem == 0) return false;
    uint8_t b0 = p[0];
    if (b0 == ' ' || b0 == '\t' || b0 == '\n' || b0 == '\r' || b0 == '\v' || b0 == '\f') {
        *char_bytes = 1;
        return true;
    }
    if (rem >= 2 && b0 == 0xC2 && p[1] == 0xA0) { /* NBSP */
        *char_bytes = 2;
        return true;
    }
    if (rem >= 3) {
        if (b0 == 0xE1 && p[1] == 0x9A && p[2] == 0x80) { /* Ogham space mark */
            *char_bytes = 3;
            return true;
        }
        if (b0 == 0xE2) {
            if (p[1] == 0x80) {
                if ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xA8 || p[2] == 0xA9 || p[2] == 0xAF) {
                    *char_bytes = 3;
                    return true;
                }
            } else if (p[1] == 0x81 && p[2] == 0x9F) {
                *char_bytes = 3;
                return true;
            }
        }
        if (b0 == 0xE3 && p[1] == 0x80 && p[2] == 0x80) { /* Ideographic space */
            *char_bytes = 3;
            return true;
        }
        if (b0 == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { /* BOM */
            *char_bytes = 3;
            return true;
        }
    }
    return false;
}

static inline size_t json_escape_string(const char *in, size_t in_len, char *out, size_t max_out) {
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t c = (uint8_t)in[i];
        /* Fast path for normal printable ASCII characters */
        if (__builtin_expect(c >= 0x20 && c != '"' && c != '\\', 1)) {
            if (o + 1 > max_out) break;
            out[o++] = (char)c;
        } else if (c == '"') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = '"';
        } else if (c == '\\') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = '\\';
        } else if (c == '\n') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = 't';
        } else if (c == '\b') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = 'b';
        } else if (c == '\f') {
            if (o + 2 > max_out) break;
            out[o++] = '\\'; out[o++] = 'f';
        } else {
            if (o + 6 > max_out) break;
            snprintf(out + o, 7, "\\u%04x", c);
            o += 6;
        }
    }
    if (o < max_out) out[o] = '\0';
    return o;
}

static inline const char *find_in_buf(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) return haystack + i;
    }
    return NULL;
}

/* Parses body for POST /posts */
static inline int json_parse_post_body(const char *raw, size_t len, char *out_body, size_t max_out, size_t *out_body_len) {
    if (len == 0) return JSON_PARSE_MALFORMED;
    size_t i = 0;
    while (i < len && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n')) i++;
    if (i >= len || raw[i] != '{') return JSON_PARSE_MALFORMED;

    size_t end = len;
    while (end > i && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\r' || raw[end - 1] == '\n')) end--;
    if (end <= i || raw[end - 1] != '}') return JSON_PARSE_MALFORMED;

    const char *key = "\"body\"";
    const char *kpos = find_in_buf(raw + i, end - i, key, 6);
    if (!kpos) {
        return JSON_PARSE_BODY_REQUIRED;
    }

    const char *p = kpos + 6;
    while (p < raw + end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= raw + end || *p != ':') return JSON_PARSE_MALFORMED;
    p++;
    while (p < raw + end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= raw + end) return JSON_PARSE_MALFORMED;

    if (*p != '"') {
        if (*p == 'n' || *p == 't' || *p == 'f' || *p == '[' || *p == '{' || *p == '-' || (*p >= '0' && *p <= '9')) {
            return JSON_PARSE_BODY_REQUIRED;
        }
        return JSON_PARSE_MALFORMED;
    }
    p++; /* Skip starting quote */

    /* Unescape string value */
    char buf[4096];
    size_t blen = 0;
    bool closed = false;

    while (p < raw + end) {
        if (*p == '"') {
            closed = true;
            p++;
            break;
        }
        if ((uint8_t)*p < 0x20) {
            return JSON_PARSE_MALFORMED;
        }
        if (*p == '\\') {
            p++;
            if (p >= raw + end) return JSON_PARSE_MALFORMED;
            char esc = *p++;
            if (esc == '"' || esc == '\\' || esc == '/') {
                if (blen < sizeof(buf) - 1) buf[blen++] = esc;
            } else if (esc == 'b') {
                if (blen < sizeof(buf) - 1) buf[blen++] = '\b';
            } else if (esc == 'f') {
                if (blen < sizeof(buf) - 1) buf[blen++] = '\f';
            } else if (esc == 'n') {
                if (blen < sizeof(buf) - 1) buf[blen++] = '\n';
            } else if (esc == 'r') {
                if (blen < sizeof(buf) - 1) buf[blen++] = '\r';
            } else if (esc == 't') {
                if (blen < sizeof(buf) - 1) buf[blen++] = '\t';
            } else if (esc == 'u') {
                if (p + 4 > raw + end) return JSON_PARSE_MALFORMED;
                uint32_t cp = 0;
                for (int h = 0; h < 4; h++) {
                    char c = *p++;
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (c - 'A' + 10);
                    else return JSON_PARSE_MALFORMED;
                }
                if (cp < 0x80) {
                    if (blen < sizeof(buf) - 1) buf[blen++] = (char)cp;
                } else if (cp < 0x800) {
                    if (blen + 2 < sizeof(buf)) {
                        buf[blen++] = (char)(0xC0 | (cp >> 6));
                        buf[blen++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else {
                    if (blen + 3 < sizeof(buf)) {
                        buf[blen++] = (char)(0xE0 | (cp >> 12));
                        buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[blen++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
            } else {
                return JSON_PARSE_MALFORMED;
            }
        } else {
            if (blen < sizeof(buf) - 1) buf[blen++] = *p++;
            else p++;
        }
    }
    if (!closed) return JSON_PARSE_MALFORMED;

    /* Trim leading JS whitespace */
    const uint8_t *start = (const uint8_t *)buf;
    size_t rem = blen;
    size_t c_bytes = 0;
    while (rem > 0 && is_js_whitespace(start, rem, &c_bytes)) {
        start += c_bytes;
        rem -= c_bytes;
    }

    /* Trim trailing JS whitespace */
    while (rem > 0) {
        bool found_trailing = false;
        for (size_t look = 1; look <= 4 && look <= rem; look++) {
            const uint8_t *cand = start + rem - look;
            size_t cb = 0;
            if (is_js_whitespace(cand, look, &cb) && cb == look) {
                rem -= look;
                found_trailing = true;
                break;
            }
        }
        if (!found_trailing) break;
    }

    if (rem == 0) return JSON_PARSE_BODY_REQUIRED;

    /* Count UTF-8 characters (code points) for 500 length check */
    size_t char_count = 0;
    for (size_t k = 0; k < rem; k++) {
        if ((start[k] & 0xC0) != 0x80) char_count++;
    }
    if (char_count > 500) return JSON_PARSE_BODY_TOO_LONG;

    if (rem >= max_out) return JSON_PARSE_BODY_TOO_LONG;
    memcpy(out_body, start, rem);
    out_body[rem] = '\0';
    if (out_body_len) *out_body_len = rem;

    return JSON_PARSE_OK;
}

#endif /* JSON_H */