/* SQLite utilities for HolyC */
#ifndef SQLLITE_HC
#define SQLLITE_HC

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#define SQL_INT 1
#define SQL_TEXT 2
#define SQL_FLOAT 3
#define SQL_NULL 4

#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)

typedef struct {
    sqlite3 *db;
    U8 *path;
} SqlCtx;

typedef struct {
    I32 type;
    I64 i64;
    F64 d;
    U8 *str;
} SqlParam;

typedef struct {
    sqlite3_stmt *stmt;
    U8 **col;
    I32 col_count;
    I32 row_count;
} SqlRow;

/* Global prepared statements */
sqlite3_stmt *stmt_health = NULL;
sqlite3_stmt *stmt_feed = NULL;
sqlite3_stmt *stmt_post_get = NULL;
sqlite3_stmt *stmt_post_create = NULL;
sqlite3_stmt *stmt_like_insert = NULL;

SqlCtx *SqlCtxNew(U8 *path, I32 create) {
    SqlCtx *ctx = (SqlCtx *)malloc(sizeof(SqlCtx));
    ctx->path = (U8 *)strdup((char*)path);
    int rc = sqlite3_open((char*)path, &ctx->db);
    if (rc != SQLITE_OK) {
        sqlite3_close(ctx->db);
        free(ctx);
        return NULL;
    }
    sqlite3_exec(ctx->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(ctx->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    return ctx;
}

U8 *SqlExecRaw(SqlCtx *ctx, U8 *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(ctx->db, (char*)sql, NULL, NULL, &err);
    if (rc != SQLITE_OK && err) {
        U8 *res = (U8 *)strdup(err);
        sqlite3_free(err);
        return res;
    }
    if (err) sqlite3_free(err);
    return NULL;
}

sqlite3_stmt *SqlPrepare(SqlCtx *ctx, U8 *sql) {
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(ctx->db, (char*)sql, -1, &stmt, NULL);
    return stmt;
}

I32 SqlExecPrepared(sqlite3_stmt *stmt, SqlRow *row, SqlParam *params) {
    if (!stmt) return 0;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    /* Simple binding */
    if (params) {
        /* TODO: bind params */
    }
    int rc = sqlite3_step(stmt);
    if (row) {
        row->stmt = stmt;
    }
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) return 1;
    return 0;
}

void init_statements() {
    extern SqlCtx *sql_ctx;
    stmt_feed = SqlPrepare(sql_ctx, (U8 *)
        "SELECT p.id, p.body, p.created_at, u.username, "
        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
        "FROM posts p JOIN users u ON u.id = p.user_id "
        "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    stmt_post_get = SqlPrepare(sql_ctx, (U8 *)
        "SELECT p.id, p.body, p.created_at, u.username, "
        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
        "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?");
    stmt_post_create = SqlPrepare(sql_ctx, (U8 *)
        "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
    stmt_like_insert = SqlPrepare(sql_ctx, (U8 *)
        "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING");
    stmt_health = SqlPrepare(sql_ctx, (U8 *)"SELECT 1");
}

/* Base64url decode to plain text */
static int b64url_to_b64(const char *in, char *out, int outsize) {
    int len = strlen(in);
    int j = 0;
    for (int i = 0; i < len && j < outsize - 1; i++) {
        if (in[i] == '-') out[j++] = '+';
        else if (in[i] == '_') out[j++] = '/';
        else out[j++] = in[i];
    }
    /* Add padding */
    while (j % 4) out[j++] = '=';
    out[j] = '\0';
    return j;
}

static int b64_decode(const char *in, char *out, int outsize) {
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bmem = BIO_new_mem_buf((void*)in, -1);
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NEWLINE);
    int len = BIO_read(b64, out, outsize - 1);
    if (len < 0) len = 0;
    out[len] = '\0';
    BIO_free_all(b64);
    return len;
}

/* JWT verification */
I32 verify_jwt_fast(U8 *auth_header, I32 *user_id_out, U8 *username_out, I32 username_size) {
    char *auth = (char*)auth_header;
    char *bearer = strstr(auth, "Bearer ");
    if (!bearer) return FALSE;
    bearer += 7;
    /* Find end of token */
    char *end = bearer;
    while (*end && *end != '\r' && *end != '\n' && *end != ' ' && *end != ',') end++;
    *end = '\0';
    
    if (strlen(bearer) == 0) return FALSE;
    
    char *token = strdup(bearer);
    char *h = strtok(token, ".");
    char *p = strtok(NULL, ".");
    char *s = strtok(NULL, ".");
    if (!h || !p || !s) { free(token); return FALSE; }
    /* Check for extra parts or alg=none case - alg none would mean header is base64 of {"alg":"none"...} */
    /* Check header */
    
    extern U8 *JWT_SECRET;
    char data[2048];
    snprintf(data, sizeof(data), "%s.%s", h, p);
    
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    HMAC(EVP_sha256(), (char*)JWT_SECRET, strlen((char*)JWT_SECRET), (unsigned char*)data, strlen(data), md, &md_len);
    
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NEWLINE);
    BIO_write(b64, md, md_len);
    BIO_flush(b64);
    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);
    char sig_b64[1024];
    memcpy(sig_b64, bptr->data, bptr->length);
    sig_b64[bptr->length] = '\0';
    /* Convert to base64url */
    for (int i = 0; sig_b64[i]; i++) {
        if (sig_b64[i] == '+') sig_b64[i] = '-';
        if (sig_b64[i] == '/') sig_b64[i] = '_';
    }
    /* Remove padding */
    char *eq = strchr(sig_b64, '=');
    if (eq) *eq = '\0';
    
    if (strcmp(sig_b64, s) != 0) {
        BIO_free_all(b64);
        free(token);
        return FALSE;
    }
    BIO_free_all(b64);
    
    /* Decode header to check alg */
    char h_b64[512], h_dec[512];
    b64url_to_b64(h, h_b64, sizeof(h_b64));
    b64_decode(h_b64, h_dec, sizeof(h_dec));
    if (strstr(h_dec, "\"alg\":\"none\"") || strstr(h_dec, "alg\":\"none\"")) {
        free(token);
        return FALSE;
    }
    if (!strstr(h_dec, "HS256")) {
        free(token);
        return FALSE;
    }
    
    /* Decode payload */
    char p_b64[1024], p_dec[2048];
    b64url_to_b64(p, p_b64, sizeof(p_b64));
    b64_decode(p_b64, p_dec, sizeof(p_dec));
    
    /* Check exp */
    /* Simple check - look for exp */
    char *exp_ptr = strstr(p_dec, "\"exp\"");
    if (exp_ptr) {
        /* skip to value */
        char *colon = strchr(exp_ptr, ':');
        if (colon) {
            long exp_ts = atol(colon + 1);
            long now = time(NULL);
            if (exp_ts < now) {
                free(token);
                return FALSE;
            }
        }
    }
    
    /* Extract sub */
    char *sub_ptr = strstr(p_dec, "\"sub\"");
    if (!sub_ptr) { free(token); return FALSE; }
    char *colon = strchr(sub_ptr, ':');
    if (!colon) { free(token); return FALSE; }
    colon++;
    while (*colon == ' ' || *colon == '"') colon++;
    char *sub_end = colon;
    while (*sub_end >= '0' && *sub_end <= '9') sub_end++;
    if (sub_end == colon) { free(token); return FALSE; } /* not integer */
    *sub_end = '\0';
    *user_id_out = atoi(colon);
    
    /* Extract username */
    char *uname_ptr = strstr(p_dec, "\"username\"");
    if (!uname_ptr) { free(token); return FALSE; }
    colon = strchr(uname_ptr, ':');
    if (!colon) { free(token); return FALSE; }
    colon++;
    while (*colon == ' ' || *colon == '"') colon++;
    char *uname_end = colon;
    while (*uname_end && *uname_end != '"' && (uname_end - colon) < username_size - 1) uname_end++;
    if (*uname_end != '"') { /* might be end without quote in some cases */ }
    int uname_len = uname_end - colon;
    if (uname_len <= 0 || uname_len >= username_size) { free(token); return FALSE; }
    strncpy((char*)username_out, colon, uname_len);
    username_out[uname_len] = '\0';
    
    free(token);
    return TRUE;
}

#endif
