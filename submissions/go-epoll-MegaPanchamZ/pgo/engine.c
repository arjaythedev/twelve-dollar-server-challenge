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
static inline int col_is_null(sqlite3_stmt *s, int i) {
    return (((Vdbe *)s)->pResultRow[i].flags & MEM_Null) != 0;
}
static inline int64_t db_changes(sqlite3 *db) {
    return db->nChange;
}
static inline int db_autocommit(sqlite3 *db) {
    return db->autoCommit;
}
static inline void bind_int64(sqlite3_stmt *s, int i, int64_t val) {
    Mem *pVar = &((Vdbe *)s)->aVar[i - 1];
    pVar->u.i = val;
    pVar->flags = MEM_Int;
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
static inline int col_is_null(sqlite3_stmt *s, int i) {
    return sqlite3_column_type(s, i) == SQLITE_NULL;
}
static inline int64_t db_changes(sqlite3 *db) {
    return sqlite3_changes(db);
}
static inline int db_autocommit(sqlite3 *db) {
    return sqlite3_get_autocommit(db);
}
static inline void bind_int64(sqlite3_stmt *s, int i, int64_t val) {
    sqlite3_bind_int64(s, i, val);
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

static inline char *u64toa(char *p, uint64_t v) {
    if (v < 10) {
        *p++ = (char)('0' + v);
        return p;
    }
    if (v < 100) {
        memcpy(p, digits2 + (v * 2), 2);
        return p + 2;
    }
    char tmp[24];
    char *end = tmp + sizeof(tmp);
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
    memcpy(p, cur, n);
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
    char hdr[128];
    char *hp = hdr;
    const char *st_line;
    int st_len;
    switch (status) {
        case 200: st_line = "HTTP/1.1 200 OK\r\n"; st_len = 17; break;
        case 201: st_line = "HTTP/1.1 201 Created\r\n"; st_len = 22; break;
        case 400: st_line = "HTTP/1.1 400 Bad Request\r\n"; st_len = 26; break;
        case 401: st_line = "HTTP/1.1 401 Unauthorized\r\n"; st_len = 27; break;
        case 404: st_line = "HTTP/1.1 404 Not Found\r\n"; st_len = 24; break;
        case 503: st_line = "HTTP/1.1 503 Service Unavailable\r\n"; st_len = 34; break;
        default:  st_line = "HTTP/1.1 500 Internal Server Error\r\n"; st_len = 36; break;
    }
    memcpy(hp, st_line, (size_t)st_len); hp += st_len;
    memcpy(hp, "Content-Type: application/json\r\nContent-Length: ", 48); hp += 48;
    int body_len = (int)(body_end - body_start);
    hp = u64toa(hp, (uint64_t)body_len);
    if (op->close_conn) {
        memcpy(hp, "\r\nConnection: close\r\n\r\n", 23); hp += 23;
    } else {
        memcpy(hp, "\r\n\r\n", 4); hp += 4;
    }
    int hdr_len = (int)(hp - hdr);
    char *resp_start = body_start - hdr_len;
    memcpy(resp_start, hdr, (size_t)hdr_len);
    op->resp_off = (int32_t)(resp_start - ctx->out_buf);
    op->resp_len = hdr_len + body_len;
    *pos = (int32_t)(body_end - ctx->out_buf);
}

static inline void write_static_err(db_ctx *ctx, db_op *op, int status, const char *json_body, int json_len, int32_t *pos) {
    char *body_start = ctx->out_buf + *pos + 128;
    memcpy(body_start, json_body, (size_t)json_len);
    finalize_http_resp(ctx, op, status, body_start, body_start + json_len, pos);
}

static int prep_one(sqlite3 *db, const char *sql, sqlite3_stmt **out) {
    return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, out, NULL);
}

db_ctx *db_init(const char *path, const char *pragma_sql, int max_ops, int req_cap, int out_cap, char **err_out) {
    db_ctx *ctx = (db_ctx *)calloc(1, sizeof(db_ctx));
    if (!ctx) return NULL;
    ctx->max_ops = max_ops;
    ctx->req_cap = req_cap;
    ctx->out_cap = out_cap;
    ctx->ops = (db_op *)calloc((size_t)max_ops, sizeof(db_op));
    ctx->req_buf = (char *)malloc((size_t)req_cap);
    ctx->out_buf = (char *)malloc((size_t)out_cap);

    sqlite3_initialize();
    int rc = sqlite3_open_v2(path, &ctx->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        *err_out = strdup(sqlite3_errstr(rc));
        return NULL;
    }
    char *errmsg = NULL;
    if (sqlite3_exec(ctx->db, pragma_sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        *err_out = errmsg;
        return NULL;
    }
    sqlite3_exec(ctx->db, "SELECT 1;", NULL, NULL, NULL);

    if (prep_one(ctx->db, "SELECT 1", &ctx->s_health) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20", &ctx->s_ids) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC", &ctx->s_range_posts_desc) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2", &ctx->s_range_likes) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20", &ctx->s_feed_ref) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT p.body, p.created_at, (SELECT username FROM users WHERE id = p.user_id), (SELECT count(*) FROM likes WHERE post_id = p.id) FROM posts p WHERE p.id = ?1", &ctx->s_post) != SQLITE_OK ||
        prep_one(ctx->db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at", &ctx->s_insert) != SQLITE_OK ||
        prep_one(ctx->db, "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING", &ctx->s_like) != SQLITE_OK ||
        prep_one(ctx->db, "SELECT 1 FROM posts WHERE id = ?1", &ctx->s_exists) != SQLITE_OK ||
        prep_one(ctx->db, "BEGIN DEFERRED", &ctx->s_begin_read) != SQLITE_OK ||
        prep_one(ctx->db, "BEGIN IMMEDIATE", &ctx->s_begin_write) != SQLITE_OK ||
        prep_one(ctx->db, "COMMIT", &ctx->s_commit) != SQLITE_OK ||
        prep_one(ctx->db, "ROLLBACK", &ctx->s_rollback) != SQLITE_OK) {
        *err_out = strdup(sqlite3_errmsg(ctx->db));
        return NULL;
    }

    sqlite3_wal_hook(ctx->db, on_wal_hook, ctx);
    return ctx;
}

static inline int step_simple(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc;
}

static inline int exec_feed(db_ctx *ctx, char *body_start, char **body_end_out) {
    int64_t ids[20], min_id = INT64_MAX, max_id = 0;
    int n = 0, contiguous_desc = 1;
    int rc;
    sqlite3_stmt *s_ids = ctx->s_ids;
    while ((rc = sqlite3_step(s_ids)) == SQLITE_ROW && n < 20) {
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
        bind_int64(s_rl, 2, max_id);
        while ((rc = sqlite3_step(s_rl)) == SQLITE_ROW) {
            int64_t pid = col_int64(s_rl, 0);
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
        while ((rc = sqlite3_step(s_rp)) == SQLITE_ROW) {
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
        if (rc != SQLITE_DONE) return rc;
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
        memset(likes, 0, (size_t)span * sizeof(uint32_t));
        memset(plen, 0, (size_t)span * sizeof(uint32_t));

        sqlite3_stmt *s_rl = ctx->s_range_likes;
        bind_int64(s_rl, 1, min_id);
        bind_int64(s_rl, 2, max_id);
        while ((rc = sqlite3_step(s_rl)) == SQLITE_ROW) {
            int64_t pid = col_int64(s_rl, 0);
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

void db_exec_batch(db_ctx *ctx, int n_ops, int64_t uptime_s) {
    int has_write = 0;
    int read_stmts = 0;
    for (int i = 0; i < n_ops; i++) {
        uint8_t k = ctx->ops[i].kind;
        if (k == OP_CREATE_POST || k == OP_LIKE_POST) has_write = 1;
        else if (k == OP_FEED) { read_stmts += 3; ctx->has_feed = 1; }
        else if (k == OP_GET_POST) read_stmts += 1;
    }

    int tx_mode = TX_NONE;
    int tx_failed = 0;
    if (has_write) {
        if (step_simple(ctx->s_begin_write) == SQLITE_DONE) {
            tx_mode = TX_WRITE;
        } else {
            tx_failed = 1;
        }
    } else if (read_stmts > 1) {
        if (step_simple(ctx->s_begin_read) == SQLITE_DONE) {
            tx_mode = TX_READ;
        }
    }

    int32_t pos = 0;
    for (int i = 0; i < n_ops; i++) {
        db_op *op = &ctx->ops[i];
        if (op->kind == OP_STATIC) {
            write_static_err(ctx, op, op->status, ctx->req_buf + op->text_off, op->text_len, &pos);
            continue;
        }
        if (op->kind == OP_HEALTH) {
            int rc = step_simple(ctx->s_health);
            char *bs = ctx->out_buf + pos + 128;
            char *p = bs;
            if (rc == SQLITE_ROW) {
                memcpy(p, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", 36); p += 36;
                p = u64toa(p, (uint64_t)(uptime_s >= 0 ? uptime_s : 0));
                *p++ = '}';
                finalize_http_resp(ctx, op, 200, bs, p, &pos);
            } else {
                const char *em = sqlite3_errmsg(ctx->db);
                memcpy(p, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":", 48); p += 48;
                p = append_json_str(p, (const unsigned char *)em, (int)strlen(em));
                *p++ = '}';
                finalize_http_resp(ctx, op, 503, bs, p, &pos);
            }
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
                if (exec_feed(ctx, bs, &be) == SQLITE_DONE) {
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
                    if (col_is_null(s_post, 2)) {
                        sqlite3_reset(s_post);
                        write_static_err(ctx, op, 404, "{\"error\":\"post not found\"}", 26, &pos);
                        break;
                    }
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
                sqlite3_bind_text(s_ins, 2, ctx->req_buf + op->text_off, op->text_len, SQLITE_STATIC);
                int rc = sqlite3_step(s_ins);
                if (rc == SQLITE_ROW) {
                    int64_t new_id = col_int64(s_ins, 0);
                    const unsigned char *created = col_text(s_ins, 1);
                    int created_len = col_bytes(s_ins, 1);
                    char created_copy[40];
                    if (created_len > 39) created_len = 39;
                    memcpy(created_copy, created, (size_t)created_len);
                    int rc2 = sqlite3_step(s_ins);
                    sqlite3_reset(s_ins);
                    if (rc2 == SQLITE_DONE) {
                        char *bs = ctx->out_buf + pos + 128;
                        char *p = bs;
                        memcpy(p, "{\"post\":", 8); p += 8;
                        p = format_post_obj(
                            p, new_id,
                            (const unsigned char *)(ctx->req_buf + op->text_off), op->text_len,
                            (const unsigned char *)created_copy, created_len,
                            (const unsigned char *)(ctx->req_buf + op->user_off), op->user_len,
                            0
                        );
                        *p++ = '}';
                        finalize_http_resp(ctx, op, 201, bs, p, &pos);
                        break;
                    }
                } else {
                    sqlite3_reset(s_ins);
                }
                if (db_autocommit(ctx->db) != 0) tx_failed = 1;
                write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                break;
            }
            case OP_LIKE_POST: {
                bind_int64(ctx->s_like, 1, op->arg1);
                bind_int64(ctx->s_like, 2, op->arg2);
                int rc = step_simple(ctx->s_like);
                if (rc != SQLITE_DONE) {
                    if (db_autocommit(ctx->db) != 0) tx_failed = 1;
                    write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                    break;
                }
                if (db_changes(ctx->db) == 1) {
                    char *bs = ctx->out_buf + pos + 128;
                    char *p = bs;
                    memcpy(p, "{\"liked\":true,\"already_liked\":false,\"post_id\":", 46); p += 46;
                    p = u64toa(p, (uint64_t)op->arg2);
                    *p++ = '}';
                    finalize_http_resp(ctx, op, 201, bs, p, &pos);
                    break;
                }
                bind_int64(ctx->s_exists, 1, op->arg2);
                int rc_ex = step_simple(ctx->s_exists);
                if (rc_ex == SQLITE_ROW) {
                    char *bs = ctx->out_buf + pos + 128;
                    char *p = bs;
                    memcpy(p, "{\"liked\":true,\"already_liked\":true,\"post_id\":", 45); p += 45;
                    p = u64toa(p, (uint64_t)op->arg2);
                    *p++ = '}';
                    finalize_http_resp(ctx, op, 200, bs, p, &pos);
                } else if (rc_ex == SQLITE_DONE) {
                    write_static_err(ctx, op, 404, "{\"error\":\"post not found\"}", 26, &pos);
                } else {
                    if (db_autocommit(ctx->db) != 0) tx_failed = 1;
                    write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
                }
                break;
            }
        }
    }

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
        for (int i = 0; i < n_ops; i++) {
            db_op *op = &ctx->ops[i];
            if (op->kind == OP_STATIC) {
                write_static_err(ctx, op, op->status, ctx->req_buf + op->text_off, op->text_len, &pos);
            } else if (op->kind != OP_HEALTH) {
                write_static_err(ctx, op, 500, "{\"error\":\"internal server error\"}", 33, &pos);
            }
        }
    }
}

int db_checkpoint_if_due(db_ctx *ctx) {
    int limit = ctx->has_feed ? 4000 : 32000;
    if (ctx->wal_pages < limit) return SQLITE_OK;
    ctx->wal_pages = 0;
    ctx->has_feed = 0;
    return sqlite3_wal_checkpoint_v2(ctx->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}
