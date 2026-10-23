#ifdef HAVE_SQLITE_AMALGAMATION
#include "sqlite3.c"
#else
#include <sqlite3.h>
#endif

#include "../shim.h"
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifdef HAVE_SQLITE_AMALGAMATION
static inline int64_t col_int64(sqlite3_stmt *s, int i) {
    return ((Vdbe *)s)->pResultRow[i].u.i;
}
static inline const unsigned char *col_text(sqlite3_stmt *s, int i) {
    return (const unsigned char *)((Vdbe *)s)->pResultRow[i].z;
}
static inline int col_bytes(sqlite3_stmt *s, int i) {
    return ((Vdbe *)s)->pResultRow[i].n;
}
static inline int64_t db_changes(sqlite3 *db) {
    return db->nChange;
}
static inline int64_t db_last_rowid(sqlite3 *db) {
    return db->lastRowid;
}
static inline int db_autocommit(sqlite3 *db) {
    return db->autoCommit;
}
static inline void bind_int64(sqlite3_stmt *s, int i, int64_t val) {
    Mem *pVar = &((Vdbe *)s)->aVar[i - 1];
    pVar->u.i = val;
    pVar->flags = MEM_Int;
}
static inline void bind_text_static(sqlite3_stmt *s, int i, const char *z, int n) {
    Mem *pVar = &((Vdbe *)s)->aVar[i - 1];
    pVar->z = (char *)z;
    pVar->n = n;
    pVar->flags = MEM_Str | MEM_Static;
    pVar->enc = SQLITE_UTF8;
}
#else
static inline int64_t col_int64(sqlite3_stmt *s, int i) {
    return sqlite3_column_int64(s, i);
}
static inline const unsigned char *col_text(sqlite3_stmt *s, int i) {
    return sqlite3_column_text(s, i);
}
static inline int col_bytes(sqlite3_stmt *s, int i) {
    return sqlite3_column_bytes(s, i);
}
static inline int64_t db_changes(sqlite3 *db) {
    return sqlite3_changes(db);
}
static inline int64_t db_last_rowid(sqlite3 *db) {
    return sqlite3_last_insert_rowid(db);
}
static inline int db_autocommit(sqlite3 *db) {
    return sqlite3_get_autocommit(db);
}
static inline void bind_int64(sqlite3_stmt *s, int i, int64_t val) {
    sqlite3_bind_int64(s, i, val);
}
static inline void bind_text_static(sqlite3_stmt *s, int i, const char *z, int n) {
    sqlite3_bind_text(s, i, z, n, SQLITE_STATIC);
}
#endif

static int on_wal_hook(void *arg, sqlite3 *db, const char *name, int pages) {
    (void)db;
    (void)name;
    db_ctx *ctx = (db_ctx *)arg;
    ctx->wal_pages = pages;
    return SQLITE_OK;
}

static const char digits2[200] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static void format_iso8601_ms(char out[32], int64_t now_ms) {
    if (now_ms < 0) now_ms = 0;
    int64_t sec = now_ms / 1000;
    int ms = (int)(now_ms - sec * 1000);
    int64_t days = sec / 86400;
    int rem = (int)(sec - days * 86400);
    int hour = rem / 3600;
    rem -= hour * 3600;
    int min = rem / 60;
    int s = rem - min * 60;

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)((int64_t)yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);

    memcpy(out,      digits2 + (y / 100) * 2, 2);
    memcpy(out + 2,  digits2 + (y % 100) * 2, 2);
    out[4] = '-';
    memcpy(out + 5,  digits2 + m * 2, 2);
    out[7] = '-';
    memcpy(out + 8,  digits2 + d * 2, 2);
    out[10] = 'T';
    memcpy(out + 11, digits2 + hour * 2, 2);
    out[13] = ':';
    memcpy(out + 14, digits2 + min * 2, 2);
    out[16] = ':';
    memcpy(out + 17, digits2 + s * 2, 2);
    out[19] = '.';
    out[20] = (char)('0' + (ms / 100));
    memcpy(out + 21, digits2 + (ms % 100) * 2, 2);
    out[23] = 'Z';
    out[24] = '\0';
}

static inline char *u64toa(char *p, uint64_t v) {
    if (v < 10) {
        *p++ = (char)('0' + v);
        return p;
    }
    if (v < 100) {
        memcpy(p, digits2 + (v * 2), 2);
        return p + 2;
    }
    char tmp[32];
    char *end = tmp + 24;
    char *cur = end;
    while (v >= 100) {
        uint64_t q = v / 100;
        uint64_t r = (v - q * 100) * 2;
        v = q;
        cur -= 2;
        memcpy(cur, digits2 + r, 2);
    }
    if (v < 10) {
        *--cur = (char)('0' + v);
    } else {
        cur -= 2;
        memcpy(cur, digits2 + (v * 2), 2);
    }
    size_t n = (size_t)(end - cur);
    if (__builtin_expect(n <= 8, 1)) {
        memcpy(p, cur, 8);
    } else {
        memcpy(p, cur, n);
    }
    return p + n;
}

static const char hex_digits[16] = "0123456789abcdef";

static inline char *append_json_str(char *dst, const unsigned char *src, int len) {
    *dst++ = '"';
    int i = 0;
#if defined(__SSE2__)
    const __m128i v_quote = _mm_set1_epi8('"');
    const __m128i v_bslash = _mm_set1_epi8('\\');
    const __m128i v_ctrl = _mm_set1_epi8(0x1f);
    while (i + 16 <= len) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i cmp_q = _mm_cmpeq_epi8(chunk, v_quote);
        __m128i cmp_b = _mm_cmpeq_epi8(chunk, v_bslash);
        __m128i cmp_c = _mm_cmpeq_epi8(_mm_max_epu8(chunk, v_ctrl), v_ctrl);
        __m128i bad = _mm_or_si128(_mm_or_si128(cmp_q, cmp_b), cmp_c);
        int mask = _mm_movemask_epi8(bad);
        if (__builtin_expect(mask == 0, 1)) {
            _mm_storeu_si128((__m128i *)dst, chunk);
            dst += 16;
            i += 16;
            continue;
        }
        int adv = __builtin_ctz(mask);
        if (adv > 0) {
            memcpy(dst, src + i, adv);
            dst += adv;
            i += adv;
        }
        unsigned char c = src[i++];
        switch (c) {
            case '"':  *dst++ = '\\'; *dst++ = '"'; break;
            case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
            case '\n': *dst++ = '\\'; *dst++ = 'n'; break;
            case '\r': *dst++ = '\\'; *dst++ = 'r'; break;
            case '\t': *dst++ = '\\'; *dst++ = 't'; break;
            case '\b': *dst++ = '\\'; *dst++ = 'b'; break;
            case '\f': *dst++ = '\\'; *dst++ = 'f'; break;
            default:
                *dst++ = '\\'; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
                *dst++ = hex_digits[c >> 4];
                *dst++ = hex_digits[c & 0xf];
                break;
        }
    }
#elif defined(__aarch64__)
    const uint8x16_t v_quote = vdupq_n_u8('"');
    const uint8x16_t v_bslash = vdupq_n_u8('\\');
    const uint8x16_t v_space = vdupq_n_u8(0x20);
    while (i + 16 <= len) {
        uint8x16_t chunk = vld1q_u8(src + i);
        uint8x16_t bad = vorrq_u8(
            vorrq_u8(vceqq_u8(chunk, v_quote), vceqq_u8(chunk, v_bslash)),
            vcltq_u8(chunk, v_space)
        );
        if (__builtin_expect(vmaxvq_u8(bad) == 0, 1)) {
            vst1q_u8((uint8_t *)dst, chunk);
            dst += 16;
            i += 16;
            continue;
        }
        break;
    }
#endif
    while (i < len) {
        unsigned char c = src[i++];
        if (__builtin_expect(c >= 0x20 && c != '"' && c != '\\', 1)) {
            *dst++ = (char)c;
            continue;
        }
        switch (c) {
            case '"':  *dst++ = '\\'; *dst++ = '"'; break;
            case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
            case '\n': *dst++ = '\\'; *dst++ = 'n'; break;
            case '\r': *dst++ = '\\'; *dst++ = 'r'; break;
            case '\t': *dst++ = '\\'; *dst++ = 't'; break;
            case '\b': *dst++ = '\\'; *dst++ = 'b'; break;
            case '\f': *dst++ = '\\'; *dst++ = 'f'; break;
            default:
                *dst++ = '\\'; *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
                *dst++ = hex_digits[c >> 4];
                *dst++ = hex_digits[c & 0xf];
                break;
        }
    }
    *dst++ = '"';
    return dst;
}

static inline char *format_post_obj(
    char *p,
    int64_t id,
    const unsigned char *body, int body_len,
    const unsigned char *created, int created_len,
    const unsigned char *author, int author_len,
    int64_t likes
) {
    memcpy(p, "{\"id\":", 6); p += 6;
    p = u64toa(p, (uint64_t)id);
    memcpy(p, ",\"body\":", 8); p += 8;
    p = append_json_str(p, body, body_len);
    memcpy(p, ",\"created_at\":\"", 15); p += 15;
    if (__builtin_expect(created_len == 24, 1)) {
        memcpy(p, created, 24);
        p += 24;
    } else if (created && created_len > 0) {
        memcpy(p, created, (size_t)created_len);
        p += created_len;
    }
    memcpy(p, "\",\"author\":", 11); p += 11;
    p = append_json_str(p, author, author_len);
    memcpy(p, ",\"like_count\":", 14); p += 14;
    p = u64toa(p, (uint64_t)likes);
    *p++ = '}';
    return p;
}

static inline void finalize_http_resp(db_ctx *ctx, db_op *op, int status, char *body_start, char *body_end, int32_t *pos) {
    char *p;
    if (__builtin_expect(op->close_conn == 0, 1)) {
        p = body_start - 4;
        memcpy(p, "\r\n\r\n", 4);
    } else {
        p = body_start - 23;
        memcpy(p, "\r\nConnection: close\r\n\r\n", 23);
    }
    uint32_t v = (uint32_t)(body_end - body_start);
    while (v >= 100) {
        uint32_t q = v / 100;
        uint32_t r = (v - q * 100) * 2;
        v = q;
        p -= 2;
        memcpy(p, digits2 + r, 2);
    }
    if (v < 10) {
        *--p = (char)('0' + v);
    } else {
        p -= 2;
        memcpy(p, digits2 + (v * 2), 2);
    }
    switch (status) {
        case 200:
            p -= 65;
            memcpy(p, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ", 65);
            break;
        case 201:
            p -= 70;
            memcpy(p, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ", 70);
            break;
        case 400:
            p -= 74;
            memcpy(p, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ", 74);
            break;
        case 401:
            p -= 75;
            memcpy(p, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ", 75);
            break;
        case 404:
            p -= 72;
            memcpy(p, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ", 72);
            break;
        case 503:
            p -= 82;
            memcpy(p, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ", 82);
            break;
        default:
            p -= 84;
            memcpy(p, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ", 84);
            break;
    }
    op->resp_off = (int32_t)(p - ctx->out_buf);
    op->resp_len = (int32_t)(body_end - p);
    int32_t next = (int32_t)(body_end - ctx->out_buf);
    *pos = next > *pos + 256 ? next : *pos + 256;
}

static inline int write_static_err(db_ctx *ctx, db_op *op, int status, const char *json_body, int json_len, int32_t *pos) {
    if (json_len < 0 || (size_t)json_len + 128 > (size_t)(ctx->out_cap - *pos)) return 0;
    char *body_start = ctx->out_buf + *pos + 128;
    memcpy(body_start, json_body, (size_t)json_len);
    finalize_http_resp(ctx, op, status, body_start, body_start + json_len, pos);
    return 1;
}

static int prep_one(sqlite3 *db, const char *sql, sqlite3_stmt **out) {
    return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, out, NULL);
}

void db_free(db_ctx *ctx) {
    if (!ctx) return;
    if (ctx->db) {
        sqlite3_stmt *s;
        while ((s = sqlite3_next_stmt(ctx->db, NULL)) != NULL) sqlite3_finalize(s);
        sqlite3_close(ctx->db);
    }
    free(ctx->ops);
    free(ctx->req_buf);
    free(ctx->out_buf);
    free(ctx);
}

db_ctx *db_init(const char *path, const char *pragma_sql, int max_ops, int req_cap, int out_cap, char **err_out) {
    if (max_ops <= 0 || req_cap <= 0 || out_cap < 256) return NULL;
    db_ctx *ctx = (db_ctx *)calloc(1, sizeof(db_ctx));
    if (!ctx) return NULL;
    ctx->max_ops = max_ops;
    ctx->req_cap = req_cap;
    ctx->out_cap = out_cap;
    ctx->last_ms = -1;
    ctx->ops = (db_op *)calloc((size_t)max_ops, sizeof(db_op));
    ctx->req_buf = (char *)malloc((size_t)req_cap);
    ctx->out_buf = (char *)malloc((size_t)out_cap);
    if (!ctx->ops || !ctx->req_buf || !ctx->out_buf) {
        db_free(ctx);
        return NULL;
    }

    sqlite3_initialize();
    int rc = sqlite3_open_v2(path, &ctx->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        *err_out = strdup(sqlite3_errstr(rc));
        db_free(ctx);
        return NULL;
    }
    char *errmsg = NULL;
    if (sqlite3_exec(ctx->db, pragma_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        *err_out = strdup(errmsg ? errmsg : sqlite3_errmsg(ctx->db));
        sqlite3_free(errmsg);
        db_free(ctx);
        return NULL;
    }
    sqlite3_exec(ctx->db, "SELECT 1;", NULL, NULL, NULL);

    if (prep_one(ctx->db, "SELECT 1", &ctx->s_health) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20", &ctx->s_ids) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC", &ctx->s_range_posts_desc) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT post_id FROM likes WHERE post_id >= ?1 ORDER BY post_id", &ctx->s_range_likes) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &ctx->s_feed_ref) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT p.body, p.created_at, u.username, (SELECT count(*) FROM likes WHERE post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1", &ctx->s_post) != SQLITE_OK ||
        prep_one(ctx->db, "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)", &ctx->s_insert) != SQLITE_OK ||
        prep_one(ctx->db, "INSERT OR IGNORE INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3)", &ctx->s_like) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT 1 FROM posts WHERE id = ?1", &ctx->s_exists) != SQLITE_OK ||
        prep_one(ctx->db, "BEGIN DEFERRED", &ctx->s_begin_read) != SQLITE_OK ||
        prep_one(ctx->db, "BEGIN IMMEDIATE", &ctx->s_begin_write) != SQLITE_OK ||
        prep_one(ctx->db, "COMMIT", &ctx->s_commit) != SQLITE_OK ||
        prep_one(ctx->db, "ROLLBACK", &ctx->s_rollback) != SQLITE_OK) {
        *err_out = strdup(sqlite3_errmsg(ctx->db));
        db_free(ctx);
        return NULL;
    }

    format_iso8601_ms(ctx->now_iso, 0);
    sqlite3_bind_text(ctx->s_insert, 2, "", 0, SQLITE_STATIC);
    sqlite3_bind_text(ctx->s_insert, 3, ctx->now_iso, 24, SQLITE_STATIC);
    sqlite3_bind_text(ctx->s_like, 3, ctx->now_iso, 24, SQLITE_STATIC);

    sqlite3_wal_hook(ctx->db, on_wal_hook, ctx);
    return ctx;
}

static inline int step_simple(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc;
}

static inline int exec_feed(db_ctx *ctx, char *body_start, char *end, char **body_end_out) {
    if (end - body_start < 65536) return SQLITE_TOOBIG;
    int64_t ids[20], min_id = INT64_MAX, max_id = 0;
    int n = 0, contiguous_desc = 1;
    int rc = SQLITE_DONE;
    sqlite3_stmt *s_ids = ctx->s_ids;
    while (n < 20 && (rc = sqlite3_step(s_ids)) == SQLITE_ROW) {
        int64_t id = col_int64(s_ids, 0);
        if (n > 0 && id != ids[n - 1] - 1) contiguous_desc = 0;
        ids[n++] = id;
        if (id < min_id) min_id = id;
        if (id > max_id) max_id = id;
    }
    sqlite3_reset(s_ids);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return rc;

    if (n > 0 && contiguous_desc) {
        uint32_t likes[20] = {0};
        sqlite3_stmt *s_rl = ctx->s_range_likes;
        bind_int64(s_rl, 1, min_id);
        while ((rc = sqlite3_step(s_rl)) == SQLITE_ROW) {
            int64_t pid = col_int64(s_rl, 0);
            if (__builtin_expect(pid > max_id, 0)) {
                rc = SQLITE_DONE;
                break;
            }
            likes[pid - min_id]++;
        }
        sqlite3_reset(s_rl);
        if (rc != SQLITE_DONE) return rc;

        char *op = body_start;
        memcpy(op, "{\"posts\":[", 10); op += 10;
        sqlite3_stmt *s_rp = ctx->s_range_posts_desc;
        bind_int64(s_rp, 1, min_id);
        bind_int64(s_rp, 2, max_id);
        int rows = 0;
        while (rows < n && (rc = sqlite3_step(s_rp)) == SQLITE_ROW) {
            if (rows > 0) *op++ = ',';
            rows++;
            int64_t id = col_int64(s_rp, 0);
            op = format_post_obj(
                op, id,
                col_text(s_rp, 1), col_bytes(s_rp, 1),
                col_text(s_rp, 2), col_bytes(s_rp, 2),
                col_text(s_rp, 3), col_bytes(s_rp, 3),
                likes[id - min_id]
            );
        }
        sqlite3_reset(s_rp);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) return rc;
        if (rows == n) {
            memcpy(op, "]}", 2); op += 2;
            *body_end_out = op;
            return SQLITE_DONE;
        }
    } else if (n > 0 && (max_id - min_id) < 256) {
        int span = (int)(max_id - min_id + 1);
        uint32_t likes[256];
        uint32_t off[256];
        uint32_t plen[256];
        uint8_t selected[256] = {0};
        memset(likes, 0, (size_t)span * sizeof(uint32_t));
        memset(plen, 0, (size_t)span * sizeof(uint32_t));
        for (int i = 0; i < n; i++) selected[ids[i] - min_id] = 1;

        sqlite3_stmt *s_rl = ctx->s_range_likes;
        bind_int64(s_rl, 1, min_id);
        while ((rc = sqlite3_step(s_rl)) == SQLITE_ROW) {
            int64_t pid = col_int64(s_rl, 0);
            if (__builtin_expect(pid > max_id, 0)) {
                rc = SQLITE_DONE;
                break;
            }
            likes[pid - min_id]++;
        }
        sqlite3_reset(s_rl);
        if (rc != SQLITE_DONE) return rc;

        sqlite3_stmt *s_rp = ctx->s_range_posts_desc;
        bind_int64(s_rp, 1, min_id);
        bind_int64(s_rp, 2, max_id);
        char *sp = ctx->scratch;
        while ((rc = sqlite3_step(s_rp)) == SQLITE_ROW) {
            int64_t id = col_int64(s_rp, 0);
            int idx = (int)(id - min_id);
            if (!selected[idx]) continue;
            char *st = sp;
            sp = format_post_obj(
                sp, id,
                col_text(s_rp, 1), col_bytes(s_rp, 1),
                col_text(s_rp, 2), col_bytes(s_rp, 2),
                col_text(s_rp, 3), col_bytes(s_rp, 3),
                likes[idx]
            );
            off[idx] = (uint32_t)(st - ctx->scratch);
            plen[idx] = (uint32_t)(sp - st);
        }
        sqlite3_reset(s_rp);
        if (rc != SQLITE_DONE) return rc;

        char *op = body_start;
        memcpy(op, "{\"posts\":[", 10); op += 10;
        int ok_all = 1;
        for (int i = 0; i < n; i++) {
            int idx = (int)(ids[i] - min_id);
            if (plen[idx] == 0) { ok_all = 0; break; }
            if (i > 0) *op++ = ',';
            memcpy(op, ctx->scratch + off[idx], plen[idx]);
            op += plen[idx];
        }
        if (ok_all) {
            memcpy(op, "]}", 2); op += 2;
            *body_end_out = op;
            return SQLITE_DONE;
        }
    }

    // Reference fallback
    char *op = body_start;
    memcpy(op, "{\"posts\":[", 10); op += 10;
    int first = 1;
    sqlite3_stmt *s_ref = ctx->s_feed_ref;
    while ((rc = sqlite3_step(s_ref)) == SQLITE_ROW) {
        if (!first) *op++ = ',';
        first = 0;
        op = format_post_obj(
            op, col_int64(s_ref, 0),
            col_text(s_ref, 1), col_bytes(s_ref, 1),
            col_text(s_ref, 2), col_bytes(s_ref, 2),
            col_text(s_ref, 3), col_bytes(s_ref, 3),
            col_int64(s_ref, 4)
        );
    }
    sqlite3_reset(s_ref);
    if (rc != SQLITE_DONE) return rc;
    memcpy(op, "]}", 2); op += 2;
    *body_end_out = op;
    return SQLITE_DONE;
}

static void write_health(db_ctx *ctx, db_op *op, int64_t uptime_s, int32_t *pos) {
    int rc = step_simple(ctx->s_health);
    char *bs = ctx->out_buf + *pos + 128;
    char *p = bs;
    if (rc == SQLITE_ROW) {
        memcpy(p, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36); p += 36;
        p = u64toa(p, (uint64_t)(uptime_s >= 0 ? uptime_s : 0));
        *p++ = '}';
        finalize_http_resp(ctx, op, 200, bs, p, pos);
    } else {
        const char *em = sqlite3_errmsg(ctx->db);
        int len = (int)strlen(em);
        if (len > 12) { em = "db error"; len = 8; }
        memcpy(p, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":", 48); p += 48;
        p = append_json_str(p, (const unsigned char *)em, len);
        *p++ = '}';
        finalize_http_resp(ctx, op, 503, bs, p, pos);
    }
}

int db_exec_batch(db_ctx *ctx, int first_op, int n_ops, int64_t now_ms, int64_t uptime_s) {
    if (first_op < 0 || n_ops <= 0 || first_op > ctx->max_ops - n_ops) return 0;
    db_op *ops = ctx->ops + first_op;
    int has_write = 0;
    int read_stmts = 0;
    for (int i = 0; i < n_ops; i++) {
        uint8_t k = ops[i].kind;
        if (k == OP_CREATE_POST || k == OP_LIKE_POST) has_write = 1;
        else if (k == OP_FEED) read_stmts += 3;
        else if (k == OP_GET_POST) read_stmts += 1;
    }

    int tx_mode = TX_NONE;
    int tx_failed = 0;
    if (has_write) {
        if (now_ms != ctx->last_ms) {
            ctx->last_ms = now_ms;
            format_iso8601_ms(ctx->now_iso, now_ms);
        }
        if (step_simple(ctx->s_begin_write) == SQLITE_DONE) {
            tx_mode = TX_WRITE;
        } else {
            tx_failed = 1;
        }
    } else if (read_stmts > 1) {
        if (step_simple(ctx->s_begin_read) == SQLITE_DONE) {
            tx_mode = TX_READ;
        } else {
            tx_failed = 1;
        }
    }

    int32_t pos = 0;
    int i;
    for (i = 0; i < n_ops; i++) {
        if (ctx->out_cap - pos < 4096) break;
        int32_t op_start = pos;
        db_op *op = &ops[i];
        if (op->kind == OP_STATIC) {
            if (!write_static_err(ctx, op, op->status, ctx->req_buf + op->text_off, op->text_len, &pos)) goto response_full;
            continue;
        }
        if (op->kind == OP_HEALTH) {
            write_health(ctx, op, uptime_s, &pos);
            continue;
        }

        if (tx_failed) {
            write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
            continue;
        }

        switch (op->kind) {
            case OP_FEED: {
                char *bs = ctx->out_buf + pos + 128;
                char *be = bs;
                int rc = exec_feed(ctx, bs, ctx->out_buf + ctx->out_cap, &be);
                if (rc == SQLITE_TOOBIG) goto response_full;
                if (rc == SQLITE_DONE) {
                    finalize_http_resp(ctx, op, 200, bs, be, &pos);
                } else {
                    if (tx_mode != TX_NONE && db_autocommit(ctx->db) != 0) tx_failed = 1;
                    write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                }
                break;
            }
            case OP_GET_POST: {
                sqlite3_stmt *s_post = ctx->s_post;
                bind_int64(s_post, 1, op->arg1);
                int rc = sqlite3_step(s_post);
                if (rc == SQLITE_ROW) {
                    const unsigned char *body = col_text(s_post, 0);
                    int body_len = col_bytes(s_post, 0);
                    const unsigned char *created = col_text(s_post, 1);
                    int created_len = col_bytes(s_post, 1);
                    const unsigned char *author = col_text(s_post, 2);
                    int author_len = col_bytes(s_post, 2);
                    int64_t likes = col_int64(s_post, 3);
                    char *bs = ctx->out_buf + pos + 128;
                    char *p = bs;
                    memcpy(p, "{\"post\":", 8); p += 8;
                    p = format_post_obj(p, op->arg1, body, body_len, created, created_len, author, author_len, likes);
                    *p++ = '}';
                    sqlite3_reset(s_post);
                    finalize_http_resp(ctx, op, 200, bs, p, &pos);
                } else if (rc == SQLITE_DONE) {
                    sqlite3_reset(s_post);
                    write_static_err(ctx, op, 404, "{\"error\":\"post not found\"}", 26, &pos);
                } else {
                    sqlite3_reset(s_post);
                    if (tx_mode != TX_NONE && db_autocommit(ctx->db) != 0) tx_failed = 1;
                    write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                }
                break;
            }
            case OP_CREATE_POST: {
                sqlite3_stmt *s_ins = ctx->s_insert;
                bind_int64(s_ins, 1, op->arg1);
                bind_text_static(s_ins, 2, ctx->req_buf + op->text_off, op->text_len);
                int rc = step_simple(s_ins);
                if (__builtin_expect(rc == SQLITE_DONE, 1)) {
                    int64_t new_id = db_last_rowid(ctx->db);
                    char *bs = ctx->out_buf + pos + 128;
                    char *p = bs;
                    memcpy(p, "{\"post\":", 8); p += 8;
                    p = format_post_obj(
                        p, new_id,
                        (const unsigned char *)(ctx->req_buf + op->text_off), op->text_len,
                        (const unsigned char *)ctx->now_iso, 24,
                        (const unsigned char *)(ctx->req_buf + op->user_off), op->user_len,
                        0
                    );
                    *p++ = '}';
                    finalize_http_resp(ctx, op, 201, bs, p, &pos);
                    break;
                }
                if (db_autocommit(ctx->db) != 0) tx_failed = 1;
                write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                break;
            }
            case OP_LIKE_POST: {
                bind_int64(ctx->s_exists, 1, op->arg2);
                int rc_ex = step_simple(ctx->s_exists);
                if (__builtin_expect(rc_ex == SQLITE_ROW, 1)) {
                    bind_int64(ctx->s_like, 1, op->arg1);
                    bind_int64(ctx->s_like, 2, op->arg2);
                    int rc = step_simple(ctx->s_like);
                    if (__builtin_expect(rc == SQLITE_DONE, 1)) {
                        char *bs = ctx->out_buf + pos + 128;
                        char *p = bs;
                        if (db_changes(ctx->db) == 1) {
                            memcpy(p, "{\"liked\":true,\"already_liked\":false,\"post_id\":", 46); p += 46;
                            p = u64toa(p, (uint64_t)op->arg2);
                            *p++ = '}';
                            finalize_http_resp(ctx, op, 201, bs, p, &pos);
                        } else {
                            memcpy(p, "{\"liked\":true,\"already_liked\":true,\"post_id\":", 45); p += 45;
                            p = u64toa(p, (uint64_t)op->arg2);
                            *p++ = '}';
                            finalize_http_resp(ctx, op, 200, bs, p, &pos);
                        }
                        break;
                    }
                } else if (rc_ex == SQLITE_DONE) {
                    write_static_err(ctx, op, 404, "{\"error\":\"post not found\"}", 26, &pos);
                    break;
                }
                if (db_autocommit(ctx->db) != 0) tx_failed = 1;
                write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                break;
            }
        }
        continue;

response_full:
        pos = op_start;
        if (i == 0) {
            // Even an individually oversized database row must make progress.
            write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
            i++;
        }
        break;
    }
    int done = i;

    if (tx_mode != TX_NONE) {
        if (tx_failed) {
            if (db_autocommit(ctx->db) == 0) {
                step_simple(ctx->s_rollback);
            }
        } else {
            if (step_simple(ctx->s_commit) != SQLITE_DONE) {
                if (db_autocommit(ctx->db) == 0) {
                    step_simple(ctx->s_rollback);
                }
                tx_failed = 1;
            }
        }
    }

    if (tx_failed && has_write) {
        pos = 0;
        for (int i = 0; i < done; i++) {
            db_op *op = &ops[i];
            if (op->kind == OP_STATIC) {
                write_static_err(ctx, op, op->status, ctx->req_buf + op->text_off, op->text_len, &pos);
            } else if (op->kind == OP_HEALTH) {
                write_health(ctx, op, uptime_s, &pos);
            } else {
                write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
            }
        }
    }
    return done;
}

int db_checkpoint_if_due(db_ctx *ctx) {
    if (ctx->wal_pages < DB_CHECKPOINT_FRAMES || !db_autocommit(ctx->db)) return SQLITE_OK;
    ctx->wal_pages = 0;
    return sqlite3_wal_checkpoint_v2(ctx->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}
