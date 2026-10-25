#include "sqlite3.h"

#include <stdint.h>
#include <string.h>

/* LTO would drop these: Java looks them up, nothing in this library calls them. */
#if defined(__GNUC__) && !defined(__clang__)
#define CRUX_EXPORT __attribute__((externally_visible))
#else
#define CRUX_EXPORT
#endif

/*
 * One thread owns the connection (SQLITE_THREADSAFE=0). The scratch below is the
 * narrow-feed working set: ids, like counts, and column bytes, then the JSON.
 * A span of 256 or more, or a buffer that cannot hold the object, returns 0 so
 * the caller can use the json_object query. A negative return is -sqlite_rc.
 */

static int64_t ids_buf[20];
static int64_t counts_buf[20];
static int32_t slot_of[256];
static unsigned char field_buf[128 * 1024];
static int body_off[20], body_len[20];
static int created_off[20], created_len[20];
static int author_off[20], author_len[20];

static int emit(unsigned char *dst, int cap, int p, const void *src, int n) {
    if (n < 0 || p < 0 || p > cap || n > cap - p) return -1;
    if (n > 0) memcpy(dst + p, src, (size_t) n);
    return p + n;
}

static int emit_u64(unsigned char *dst, int cap, int p, uint64_t v) {
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = (char) ('0' + (v % 10));
        v /= 10;
    } while (v);
    if (p < 0 || p > cap || n > cap - p) return -1;
    while (n > 0) dst[p++] = (unsigned char) tmp[--n];
    return p;
}

/* Same escapes as SQLite json_object for a text value: quote, backslash, and controls. */
static int emit_json_str(unsigned char *dst, int cap, int p, const unsigned char *s, int n) {
    static const char hex[] = "0123456789abcdef";
    int i;
    if (p < 0 || n < 0) return -1;
    for (i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') {
            if (p > cap - 2) return -1;
            dst[p++] = '\\';
            dst[p++] = c;
        } else if (c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            if (p > cap - 2) return -1;
            dst[p++] = '\\';
            dst[p++] = (unsigned char) (c == '\b' ? 'b' : c == '\f' ? 'f' : c == '\n' ? 'n' : c == '\r' ? 'r' : 't');
        } else if (c < 0x20) {
            if (p > cap - 6) return -1;
            dst[p++] = '\\';
            dst[p++] = 'u';
            dst[p++] = '0';
            dst[p++] = '0';
            dst[p++] = (unsigned char) hex[c >> 4];
            dst[p++] = (unsigned char) hex[c & 0xf];
        } else {
            if (p >= cap) return -1;
            dst[p++] = c;
        }
    }
    return p;
}

static int copy_text(sqlite3_stmt *st, int col, int *off, int *len, int *used) {
    int n = sqlite3_column_bytes(st, col);
    const unsigned char *p = sqlite3_column_text(st, col);
    if (n < 0 || *used > (int) sizeof field_buf || n > (int) sizeof field_buf - *used) return -1;
    if (n > 0 && p != 0) memcpy(field_buf + *used, p, (size_t) n);
    *off = *used;
    *len = n;
    *used += n;
    return 0;
}

CRUX_EXPORT int crux_feed_json(sqlite3_stmt *ids, sqlite3_stmt *likes, sqlite3_stmt *cols, unsigned char *dst, int cap) {
    int n = 0;
    int rc = SQLITE_DONE;
    int i;
    int64_t min, max, span;
    int nslots;
    int used = 0;
    int got = 0;
    int p;

    sqlite3_reset(ids);
    while (n < 20 && (rc = sqlite3_step(ids)) == SQLITE_ROW) {
        ids_buf[n++] = sqlite3_column_int64(ids, 0);
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -rc;
    if (n == 0) return emit(dst, cap, 0, "{\"posts\":[]}", 11);

    min = max = ids_buf[0];
    for (i = 1; i < n; i++) {
        if (ids_buf[i] < min) min = ids_buf[i];
        if (ids_buf[i] > max) max = ids_buf[i];
    }
    span = max - min;
    if (span < 0 || span >= 256) return 0;
    nslots = (int) span + 1;
    memset(slot_of, 0xff, sizeof slot_of);
    for (i = 0; i < n; i++) {
        counts_buf[i] = 0;
        slot_of[(int) (ids_buf[i] - min)] = i;
    }

    sqlite3_reset(likes);
    if ((rc = sqlite3_bind_int64(likes, 1, min)) != SQLITE_OK) return -rc;
    if ((rc = sqlite3_bind_int64(likes, 2, min + (nslots - 1))) != SQLITE_OK) return -rc;
    while ((rc = sqlite3_step(likes)) == SQLITE_ROW) {
        int64_t idx = sqlite3_column_int64(likes, 0) - min;
        if ((uint64_t) idx < (uint64_t) nslots) {
            int slot = slot_of[idx];
            if (slot >= 0) counts_buf[slot]++;
        }
    }
    if (rc != SQLITE_DONE) return -rc;

    sqlite3_reset(cols);
    if ((rc = sqlite3_bind_int64(cols, 1, min)) != SQLITE_OK) return -rc;
    if ((rc = sqlite3_bind_int64(cols, 2, min + (nslots - 1))) != SQLITE_OK) return -rc;
    while ((rc = sqlite3_step(cols)) == SQLITE_ROW) {
        int64_t idx = sqlite3_column_int64(cols, 0) - min;
        int slot;
        if ((uint64_t) idx >= (uint64_t) nslots) continue;
        slot = slot_of[idx];
        if (slot < 0) continue;
        if (copy_text(cols, 1, &body_off[slot], &body_len[slot], &used) < 0) return 0;
        if (copy_text(cols, 2, &created_off[slot], &created_len[slot], &used) < 0) return 0;
        if (copy_text(cols, 3, &author_off[slot], &author_len[slot], &used) < 0) return 0;
        got++;
    }
    if (rc != SQLITE_DONE) return -rc;
    if (got != n) return 0;

    p = emit(dst, cap, 0, "{\"posts\":[", 10);
    for (i = 0; i < n; i++) {
        if (i > 0) p = emit(dst, cap, p, ",", 1);
        p = emit(dst, cap, p, "{\"id\":", 6);
        if (p < 0) return 0;
        p = emit_u64(dst, cap, p, (uint64_t) ids_buf[i]);
        p = emit(dst, cap, p, ",\"body\":\"", 9);
        if (p < 0) return 0;
        p = emit_json_str(dst, cap, p, field_buf + body_off[i], body_len[i]);
        p = emit(dst, cap, p, "\",\"created_at\":\"", 16);
        if (p < 0) return 0;
        p = emit_json_str(dst, cap, p, field_buf + created_off[i], created_len[i]);
        p = emit(dst, cap, p, "\",\"author\":\"", 12);
        if (p < 0) return 0;
        p = emit_json_str(dst, cap, p, field_buf + author_off[i], author_len[i]);
        p = emit(dst, cap, p, "\",\"like_count\":", 15);
        if (p < 0) return 0;
        p = emit_u64(dst, cap, p, (uint64_t) counts_buf[i]);
        p = emit(dst, cap, p, "}", 1);
        if (p < 0) return 0;
    }
    p = emit(dst, cap, p, "]}", 2);
    return p < 0 ? 0 : p;
}

/* {"post":{...}} for one id. 0 = missing, -1000000 = dst too small, else -sqlite_rc. */
CRUX_EXPORT int crux_post_json(sqlite3_stmt *st, int64_t id, unsigned char *dst, int cap) {
    int rc = sqlite3_reset(st);
    const unsigned char *body, *created, *author;
    int body_n, created_n, author_n;
    int64_t likes;
    int p;
    if (rc != SQLITE_OK) return -rc;
    if ((rc = sqlite3_bind_int64(st, 1, id)) != SQLITE_OK) return -rc;
    rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_ROW) return -rc;
    body = sqlite3_column_text(st, 0);
    body_n = sqlite3_column_bytes(st, 0);
    created = sqlite3_column_text(st, 1);
    created_n = sqlite3_column_bytes(st, 1);
    author = sqlite3_column_text(st, 2);
    author_n = sqlite3_column_bytes(st, 2);
    likes = sqlite3_column_int64(st, 3);
    if (body_n < 0 || created_n < 0 || author_n < 0) return -SQLITE_ERROR;
    if (body == 0) body_n = 0;
    if (created == 0) created_n = 0;
    if (author == 0) author_n = 0;
    p = emit(dst, cap, 0, "{\"post\":{\"id\":", 14);
    p = emit_u64(dst, cap, p, (uint64_t) id);
    p = emit(dst, cap, p, ",\"body\":\"", 9);
    p = emit_json_str(dst, cap, p, body, body_n);
    p = emit(dst, cap, p, "\",\"created_at\":\"", 16);
    p = emit_json_str(dst, cap, p, created, created_n);
    p = emit(dst, cap, p, "\",\"author\":\"", 12);
    p = emit_json_str(dst, cap, p, author, author_n);
    p = emit(dst, cap, p, "\",\"like_count\":", 15);
    p = emit_u64(dst, cap, p, (uint64_t) likes);
    p = emit(dst, cap, p, "}}", 2);
    return p < 0 ? -1000000 : p;
}
