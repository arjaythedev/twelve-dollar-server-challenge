#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <signal.h>
#include <linux/io_uring.h>
#include <sqlite3.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#define K_RING_COUNT 8
#define SLOTS_PER_RING 65472
#define ACCEPT_DEPTH 1024
#define N_BUFS_PER_RING 1024
#define BUF_SIZE 2048
#define BGID 1
#define POOL_COUNT 16384
#define POOL_BUF_SIZE 16384
#define K_LISTENER_COUNT 8

#define FLAG_OPEN 1
#define FLAG_CLOSING 2

void bf_execute(int64_t *tape);

static sqlite3 *db = NULL;
static sqlite3_stmt *st_feed = NULL;
static sqlite3_stmt *st_post = NULL;
static sqlite3_stmt *st_check = NULL;
static sqlite3_stmt *st_insert = NULL;
static sqlite3_stmt *st_like = NULL;
static sqlite3_stmt *st_unlike = NULL;
static sqlite3_stmt *st_count = NULL;
static char jwt_secret[256];
static time_t start_time = 0;

typedef struct {
    uint32_t gen;
    uint16_t flags;
    uint8_t ring_idx;
    uint8_t keep_alive;
    char *out_buf;
    uint32_t out_len;
    uint32_t out_off;
    char *in_buf;
    uint32_t in_len;
    uint32_t in_cap;
} Conn;

typedef struct {
    uint32_t slot;
    uint32_t gen;
    uint8_t ring_idx;
    int keep_alive;
    char method;
    char path[256];
    int path_len;
    char auth_header[512];
    int has_auth;
    char body[2048];
    int body_len;
    int64_t auth_uid;
    char auth_uname[128];
    int auth_status;
    int status_code;
    char content_type[64];
    char *resp_buf;
    int resp_len;
    char *out_body;
    int out_body_len;
} RequestContext;

static RequestContext req_ctx;
static RequestContext *curr_conn = NULL;

typedef struct {
    char *mem;
    uint16_t *free_stack;
    uint32_t top;
} RespPool;

static RespPool resp_pool;

static inline char* acquire_pool(RespPool *p) {
    if (p->top == 0) return malloc(POOL_BUF_SIZE);
    p->top--;
    uint32_t id = p->free_stack[p->top];
    return p->mem + (id * POOL_BUF_SIZE);
}

static inline void release_pool(RespPool *p, char *ptr) {
    if (!ptr) return;
    if (ptr >= p->mem && ptr < p->mem + (POOL_COUNT * POOL_BUF_SIZE)) {
        uint32_t id = (uint32_t)((ptr - p->mem) / POOL_BUF_SIZE);
        p->free_stack[p->top++] = (uint16_t)id;
    } else {
        free(ptr);
    }
}

typedef struct {
    int ring_fd;
    uint32_t id;
    uint32_t slots;
    uint32_t nconn;

    unsigned *sq_khead;
    unsigned *sq_ktail;
    unsigned *sq_kflags;
    unsigned *sq_array;
    struct io_uring_sqe *sqes;
    unsigned sq_mask;
    unsigned sq_entries;
    unsigned sq_head;
    unsigned sq_tail;

    unsigned *cq_khead;
    unsigned *cq_ktail;
    struct io_uring_cqe *cqes;
    unsigned cq_mask;
    unsigned cq_entries;

    struct io_uring_buf_ring *pbuf_ring;
    char *pbuf_mem;
    uint16_t pbuf_tail;

    Conn *conns;
    uint32_t depth;
    uint32_t accepts[K_LISTENER_COUNT];
    bool full;
    bool accepting;
} RingWorker;

static RingWorker rings[K_RING_COUNT];
static int listeners[K_LISTENER_COUNT];
static volatile bool running = true;

static const int8_t b64_table[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['-']=62,['_']=63
};

static int b64url_decode(const char *in, int in_len, unsigned char *out) {
    int o = 0, i = 0;
    while (i + 4 <= in_len) {
        int v0 = b64_table[(unsigned char)in[i]];
        int v1 = b64_table[(unsigned char)in[i+1]];
        int v2 = b64_table[(unsigned char)in[i+2]];
        int v3 = b64_table[(unsigned char)in[i+3]];
        uint32_t val = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
        out[o++] = (val >> 16) & 0xFF;
        out[o++] = (val >> 8) & 0xFF;
        out[o++] = val & 0xFF;
        i += 4;
    }
    if (i < in_len) {
        int rem = in_len - i;
        int v0 = b64_table[(unsigned char)in[i]];
        int v1 = (rem > 1) ? b64_table[(unsigned char)in[i+1]] : 0;
        int v2 = (rem > 2) ? b64_table[(unsigned char)in[i+2]] : 0;
        uint32_t val = (v0 << 18) | (v1 << 12) | (v2 << 6);
        if (rem >= 2) out[o++] = (val >> 16) & 0xFF;
        if (rem == 3) out[o++] = (val >> 8) & 0xFF;
    }
    return o;
}


static HMAC_CTX *base_hmac_ctx = NULL;
static HMAC_CTX *req_hmac_ctx = NULL;

static int verify_jwt(const char *token, int64_t *uid, char *uname, size_t uname_sz) {
    if (!token || !*token) return 0;
    const char *p1 = strchr(token, '.');
    if (!p1) return -1;
    const char *p2 = strchr(p1 + 1, '.');
    if (!p2) return -1;
    int payload_len = p2 - (p1 + 1);
    int sig_len = strlen(p2 + 1);
    if (sig_len != 43) return -1;
    unsigned char header_buf[256];
    int dec_hdr_len = b64url_decode(token, p1 - token, header_buf);
    header_buf[dec_hdr_len] = 0;
    if (strstr((char*)header_buf, "\"none\"")) return -1;
    unsigned char expected_sig[32];
    unsigned int md_len = 0;
    if (base_hmac_ctx && req_hmac_ctx) {
        HMAC_CTX_copy(req_hmac_ctx, base_hmac_ctx);
        HMAC_Update(req_hmac_ctx, (const unsigned char*)token, p2 - token);
        HMAC_Final(req_hmac_ctx, expected_sig, &md_len);
    } else {
        HMAC(EVP_sha256(), jwt_secret, strlen(jwt_secret), (const unsigned char*)token, p2 - token, expected_sig, &md_len);
    }
    unsigned char decoded_sig[40];
    int dec_sig_len = b64url_decode(p2 + 1, sig_len, decoded_sig);
    if (dec_sig_len != 32 || CRYPTO_memcmp(expected_sig, decoded_sig, 32) != 0) return -1;
    unsigned char payload[512];
    int dec_payload_len = b64url_decode(p1 + 1, payload_len, payload);
    payload[dec_payload_len] = 0;
    if (strstr((char*)payload, "\"alg\":\"none\"")) return -1;
    char *sub_pos = strstr((char*)payload, "\"sub\":");
    if (!sub_pos) return -2;
    sub_pos += 6;
    while (*sub_pos == ' ' || *sub_pos == '\"') sub_pos++;
    char *sub_end = sub_pos;
    while (*sub_end >= '0' && *sub_end <= '9') sub_end++;
    if (sub_end == sub_pos) return -2;
    char sub_str[32];
    int slen = sub_end - sub_pos;
    if (slen >= 30) return -2;
    strncpy(sub_str, sub_pos, slen);
    sub_str[slen] = 0;
    *uid = atoll(sub_str);
    if (*uid <= 0) return -2;
    char *uname_pos = strstr((char*)payload, "\"username\":");
    if (!uname_pos) return -2;
    uname_pos += 11;
    while (*uname_pos == ' ' || *uname_pos == '\"') uname_pos++;
    char *uname_end = strchr(uname_pos, '\"');
    if (!uname_end) return -2;
    int ulen = uname_end - uname_pos;
    if (ulen >= (int)uname_sz) ulen = uname_sz - 1;
    strncpy(uname, uname_pos, ulen);
    uname[ulen] = 0;
    if (ulen == 0) return -2;
    char *exp_pos = strstr((char*)payload, "\"exp\":");
    if (!exp_pos) return -1;
    exp_pos += 6;
    while (*exp_pos == ' ') exp_pos++;
    int64_t exp = atoll(exp_pos);
    if (exp <= time(NULL)) return -1;
    return 1;
}


static inline char* write_u64(char *p, uint64_t v) {
    char tmp[24];
    int i = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) *p++ = tmp[--i];
    return p;
}

static inline char* escape_json_str(char *p, const char *src, int len) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (__builtin_expect(c >= 0x20 && c != '\"' && c != '\\', 1)) {
            *p++ = c;
        } else if (c == '\"') {
            *p++ = '\\'; *p++ = '\"';
        } else if (c == '\\') {
            *p++ = '\\'; *p++ = '\\';
        } else if (c == '\n') {
            *p++ = '\\'; *p++ = 'n';
        } else if (c == '\r') {
            *p++ = '\\'; *p++ = 'r';
        } else if (c == '\t') {
            *p++ = '\\'; *p++ = 't';
        } else {
            *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 0xF];
        }
    }
    return p;
}

static int utf8_strlen(const char *s, int len) {
    int count = 0;
    for (int i = 0; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) count++;
    }
    return count;
}

void bf_mmio_sys(int64_t *tape) {
    if (!curr_conn) return;
    int op = tape[7];
    if (op == 1) {
        curr_conn->status_code = 200;
        strcpy(curr_conn->content_type, "application/json");
        curr_conn->out_body_len = sprintf(curr_conn->out_body,
            "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%ld}", (long)(time(NULL) - start_time));
    }
}

void bf_mmio_crypto(int64_t *tape) {
    if (!curr_conn) return;
    if (!curr_conn->has_auth) {
        tape[8] = 0; tape[9] = 1; curr_conn->auth_status = 0;
        return;
    }
    int res = verify_jwt(curr_conn->auth_header, &curr_conn->auth_uid, curr_conn->auth_uname, sizeof(curr_conn->auth_uname));
    curr_conn->auth_status = res;
    if (res == 1) { tape[8] = 1; tape[9] = 0; }
    else if (res == -2) { tape[8] = 0; tape[9] = 2; }
    else { tape[8] = 0; tape[9] = 1; }
}

void bf_mmio_db(int64_t *tape) {
    if (!curr_conn) return;
    int op = tape[16];
    int64_t id = tape[17];
    if (op == 1) {
        sqlite3_reset(st_feed);
        char *p = curr_conn->out_body;
        memcpy(p, "{\"posts\":[", 10); p += 10;
        int count = 0;
        while (sqlite3_step(st_feed) == SQLITE_ROW) {
            if (count > 0) *(p++) = ',';
            int64_t pid = sqlite3_column_int64(st_feed, 0);
            const char *body = (const char*)sqlite3_column_text(st_feed, 1);
            int body_bytes = sqlite3_column_bytes(st_feed, 1);
            const char *created_at = (const char*)sqlite3_column_text(st_feed, 2);
            int created_bytes = sqlite3_column_bytes(st_feed, 2);
            const char *uname = (const char*)sqlite3_column_text(st_feed, 3);
            int user_bytes = sqlite3_column_bytes(st_feed, 3);
            int64_t likes = sqlite3_column_int64(st_feed, 4);

            memcpy(p, "{\"id\":", 6); p += 6;
            p = write_u64(p, (uint64_t)pid);
            memcpy(p, ",\"body\":\"", 9); p += 9;
            p = escape_json_str(p, body, body_bytes);
            memcpy(p, "\",\"created_at\":\"", 16); p += 16;
            memcpy(p, created_at, created_bytes); p += created_bytes;
            memcpy(p, "\",\"author\":\"", 12); p += 12;
            memcpy(p, uname, user_bytes); p += user_bytes;
            memcpy(p, "\",\"like_count\":", 15); p += 15;
            p = write_u64(p, (uint64_t)likes);
            *(p++) = '}';
            count++;
        }
        *(p++) = ']'; *(p++) = '}'; *p = 0;
        curr_conn->out_body_len = p - curr_conn->out_body;
        curr_conn->status_code = 200;
        strcpy(curr_conn->content_type, "application/json");
        return;
    }
    if (op == 2) {
        sqlite3_reset(st_post);
        sqlite3_bind_int64(st_post, 1, id);
        if (sqlite3_step(st_post) == SQLITE_ROW) {
            int64_t pid = sqlite3_column_int64(st_post, 0);
            const char *body = (const char*)sqlite3_column_text(st_post, 1);
            int body_bytes = sqlite3_column_bytes(st_post, 1);
            const char *created_at = (const char*)sqlite3_column_text(st_post, 2);
            int created_bytes = sqlite3_column_bytes(st_post, 2);
            const char *uname = (const char*)sqlite3_column_text(st_post, 3);
            int user_bytes = sqlite3_column_bytes(st_post, 3);
            int64_t likes = sqlite3_column_int64(st_post, 4);
            char *p = curr_conn->out_body;
            memcpy(p, "{\"post\":{\"id\":", 14); p += 14;
            p = write_u64(p, (uint64_t)pid);
            memcpy(p, ",\"body\":\"", 9); p += 9;
            p = escape_json_str(p, body, body_bytes);
            memcpy(p, "\",\"created_at\":\"", 16); p += 16;
            memcpy(p, created_at, created_bytes); p += created_bytes;
            memcpy(p, "\",\"author\":\"", 12); p += 12;
            memcpy(p, uname, user_bytes); p += user_bytes;
            memcpy(p, "\",\"like_count\":", 15); p += 15;
            p = write_u64(p, (uint64_t)likes);
            *(p++) = '}'; *(p++) = '}'; *p = 0;
            curr_conn->out_body_len = p - curr_conn->out_body;
            curr_conn->status_code = 200;
            strcpy(curr_conn->content_type, "application/json");
        } else {
            curr_conn->status_code = 404;
            strcpy(curr_conn->content_type, "application/json");
            curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"post not found\"}");
        }
        sqlite3_reset(st_post);
        return;
    }
    if (op == 3) {
        char *p = curr_conn->body;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p != '{') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"malformed JSON body\"}"); return; }
        p++;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == '}') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"body is required\"}"); return; }
        if (*p != '\"') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"malformed JSON body\"}"); return; }
        char *bpos = strstr(curr_conn->body, "\"body\"");
        if (!bpos) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"body is required\"}"); return; }
        bpos += 6;
        while (*bpos && (*bpos == ' ' || *bpos == '\t' || *bpos == '\r' || *bpos == '\n')) bpos++;
        if (*bpos != ':') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"malformed JSON body\"}"); return; }
        bpos++;
        while (*bpos && (*bpos == ' ' || *bpos == '\t' || *bpos == '\r' || *bpos == '\n')) bpos++;
        if (*bpos != '\"') {
            if (isdigit((unsigned char)*bpos) || *bpos == '-' || *bpos == 't' || *bpos == 'f' || *bpos == 'n' || *bpos == '[' || *bpos == '{') {
                curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"body is required\"}"); return;
            }
            curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"malformed JSON body\"}"); return;
        }
        bpos++;
        char content[2048]; char *cend = content;
        while (*bpos && *bpos != '\"') {
            if (*bpos == '\\') {
                bpos++; if (!*bpos) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"malformed JSON body\"}"); return; }
                if (*bpos == 'n') *cend++ = '\n'; else if (*bpos == 'r') *cend++ = '\r'; else if (*bpos == 't') *cend++ = '\t';
                else if (*bpos == '\"') *cend++ = '\"'; else if (*bpos == '\\') *cend++ = '\\'; else *cend++ = *bpos; bpos++;
            } else { *cend++ = *bpos++; }
        }
        if (*bpos != '\"') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"malformed JSON body\"}"); return; }
        *cend = 0; int clen = cend - content;
        while (clen > 0 && (content[clen-1] == ' ' || content[clen-1] == '\t' || content[clen-1] == '\r' || content[clen-1] == '\n')) content[--clen] = 0;
        char *cstart = content;
        while (*cstart == ' ' || *cstart == '\t' || *cstart == '\r' || *cstart == '\n') { cstart++; clen--; }
        if (clen == 0) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"body is required\"}"); return; }
        int ulen = utf8_strlen(cstart, clen);
        if (ulen > 500) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"body must be at most 500 characters\"}"); return; }
        sqlite3_reset(st_insert);
        sqlite3_bind_int64(st_insert, 1, curr_conn->auth_uid);
        sqlite3_bind_text(st_insert, 2, cstart, clen, SQLITE_TRANSIENT);
        if (sqlite3_step(st_insert) == SQLITE_ROW) {
            int64_t new_id = sqlite3_column_int64(st_insert, 0);
            const char *created_at = (const char*)sqlite3_column_text(st_insert, 1);
            char *p = curr_conn->out_body;
            p += sprintf(p, "{\"post\":{\"id\":%ld,\"body\":\"", (long)new_id);
            p = escape_json_str(p, cstart, clen);
            p += sprintf(p, "\",\"created_at\":\"%s\",\"author\":\"%s\",\"like_count\":0}}", created_at, curr_conn->auth_uname);
            *p = 0; curr_conn->out_body_len = p - curr_conn->out_body;
            curr_conn->status_code = 201; strcpy(curr_conn->content_type, "application/json");
        } else {
            curr_conn->status_code = 500; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"insert failed\"}");
        }
        sqlite3_reset(st_insert);
        return;
    }
    if (op == 4) {
        sqlite3_reset(st_check);
        sqlite3_bind_int64(st_check, 1, id);
        if (sqlite3_step(st_check) != SQLITE_ROW) {
            curr_conn->status_code = 404; strcpy(curr_conn->content_type, "application/json");
            curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"post not found\"}");
            sqlite3_reset(st_check); return;
        }
        sqlite3_reset(st_check);
        sqlite3_reset(st_like);
        sqlite3_bind_int64(st_like, 1, curr_conn->auth_uid);
        sqlite3_bind_int64(st_like, 2, id);
        sqlite3_step(st_like);
        int changes = sqlite3_changes(db);
        sqlite3_reset(st_like);
        if (changes > 0) {
            curr_conn->status_code = 201; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"liked\":true,\"already_liked\":false,\"post_id\":%ld}", (long)id);
        } else {
            curr_conn->status_code = 200; curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"liked\":true,\"already_liked\":true,\"post_id\":%ld}", (long)id);
        }
        strcpy(curr_conn->content_type, "application/json");
        return;
    }
    if (op == 5) {
        sqlite3_reset(st_check);
        sqlite3_bind_int64(st_check, 1, id);
        if (sqlite3_step(st_check) != SQLITE_ROW) {
            curr_conn->status_code = 404; strcpy(curr_conn->content_type, "application/json");
            curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"error\":\"post not found\"}");
            sqlite3_reset(st_check); return;
        }
        sqlite3_reset(st_check);
        sqlite3_reset(st_unlike);
        sqlite3_bind_int64(st_unlike, 1, curr_conn->auth_uid);
        sqlite3_bind_int64(st_unlike, 2, id);
        sqlite3_step(st_unlike);
        sqlite3_reset(st_unlike);
        sqlite3_reset(st_count);
        sqlite3_bind_int64(st_count, 1, id);
        int64_t count = 0;
        if (sqlite3_step(st_count) == SQLITE_ROW) count = sqlite3_column_int64(st_count, 0);
        sqlite3_reset(st_count);
        curr_conn->status_code = 200;
        curr_conn->out_body_len = sprintf(curr_conn->out_body, "{\"liked\":false,\"post_id\":%ld,\"like_count\":%ld}", (long)id, (long)count);
        strcpy(curr_conn->content_type, "application/json");
        return;
    }
}

#define APPEND_STR(ptr, lit) do { \
    memcpy(ptr, lit, sizeof(lit) - 1); \
    ptr += sizeof(lit) - 1; \
} while(0)

static inline char* write_http_header(char *p, int status, int body_len, int keep_alive) {
    if (status == 200) {
        APPEND_STR(p, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
    } else if (status == 201) {
        APPEND_STR(p, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ");
    } else if (status == 400) {
        APPEND_STR(p, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ");
    } else if (status == 401) {
        APPEND_STR(p, "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ");
    } else if (status == 404) {
        APPEND_STR(p, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ");
    } else {
        APPEND_STR(p, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ");
    }
    p = write_u64(p, (uint64_t)body_len);
    if (keep_alive) {
        APPEND_STR(p, "\r\nConnection: keep-alive\r\n\r\n");
    } else {
        APPEND_STR(p, "\r\nConnection: close\r\n\r\n");
    }
    return p;
}

void bf_mmio_net(int64_t *tape) {
    if (!curr_conn) return;
    int resp_type = tape[34];
    if (resp_type != 0) {
        curr_conn->status_code = tape[33];
        char *p = curr_conn->out_body;
        if (resp_type == 1) {
            APPEND_STR(p, "{\"error\":\"not found\"}");
        } else if (resp_type == 2) {
            APPEND_STR(p, "{\"error\":\"invalid post id\"}");
        } else if (resp_type == 3) {
            APPEND_STR(p, "{\"error\":\"missing bearer token\"}");
        } else if (resp_type == 4) {
            APPEND_STR(p, "{\"error\":\"invalid or expired token\"}");
        } else if (resp_type == 5) {
            APPEND_STR(p, "{\"error\":\"invalid token payload\"}");
        } else if (resp_type == 6) {
            APPEND_STR(p, "{\"error\":\"body is required\"}");
        }
        curr_conn->out_body_len = p - curr_conn->out_body;
    }
    
    char hdr[256];
    char *hend = write_http_header(hdr, curr_conn->status_code, curr_conn->out_body_len, curr_conn->keep_alive);
    int hdr_len = hend - hdr;
    
    char *send_start = curr_conn->out_body - hdr_len;
    memcpy(send_start, hdr, hdr_len);
    curr_conn->resp_buf = send_start;
    curr_conn->resp_len = hdr_len + curr_conn->out_body_len;
}

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}
static inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, void *arg, size_t arg_sz) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, arg_sz);
}
static inline int sys_io_uring_register(int fd, unsigned opcode, void *arg, unsigned nr_args) {
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

enum {
    TAG_ACCEPT = 1,
    TAG_RECV = 2,
    TAG_SEND = 3,
    TAG_CLOSE = 4,
};

static inline uint64_t make_ud(uint8_t tag, uint32_t gen, uint32_t slot) {
    return ((uint64_t)tag << 56) | (((uint64_t)gen & 0x00FFFFFF) << 32) | slot;
}
static inline uint8_t ud_tag(uint64_t ud) { return (uint8_t)(ud >> 56); }
static inline uint32_t ud_gen(uint64_t ud) { return (uint32_t)((ud >> 32) & 0x00FFFFFF); }
static inline uint32_t ud_slot(uint64_t ud) { return (uint32_t)(ud & 0xFFFFFFFF); }

static inline struct io_uring_sqe* get_sqe(RingWorker *w) {
    unsigned tail = w->sq_tail;
    unsigned head = __atomic_load_n(w->sq_khead, __ATOMIC_ACQUIRE);
    if (tail - head >= w->sq_entries) return NULL;
    struct io_uring_sqe *sqe = &w->sqes[tail & w->sq_mask];
    memset(sqe, 0, sizeof(*sqe));
    w->sq_array[tail & w->sq_mask] = tail & w->sq_mask;
    w->sq_tail = tail + 1;
    return sqe;
}

static inline uint32_t flush_sq(RingWorker *w) {
    unsigned to_submit = w->sq_tail - w->sq_head;
    if (to_submit > 0) {
        __atomic_store_n(w->sq_ktail, w->sq_tail, __ATOMIC_RELEASE);
        w->sq_head = w->sq_tail;
    }
    return to_submit;
}

static inline void return_pbuf(RingWorker *w, uint16_t bid) {
    struct io_uring_buf *b = &w->pbuf_ring->bufs[w->pbuf_tail & (N_BUFS_PER_RING - 1)];
    b->addr = (uint64_t)(w->pbuf_mem + (bid * BUF_SIZE));
    b->len = BUF_SIZE;
    b->bid = bid;
    b->resv = 0;
    w->pbuf_tail++;
}

static inline void publish_pbufs(RingWorker *w) {
    __atomic_store_n(&w->pbuf_ring->tail, w->pbuf_tail, __ATOMIC_RELEASE);
}

static void fill_accepts(RingWorker *w) {
    uint32_t target = w->depth / K_LISTENER_COUNT;
    for (int li = 0; li < K_LISTENER_COUNT; li++) {
        while (w->accepts[li] < target) {
            struct io_uring_sqe *s = get_sqe(w);
            if (!s) break;
            s->opcode = IORING_OP_ACCEPT;
            s->fd = listeners[li];
            s->file_index = IORING_FILE_INDEX_ALLOC;
            s->user_data = make_ud(TAG_ACCEPT, 0, li);
            w->accepts[li]++;
        }
    }
}

static void arm_recv(RingWorker *w, uint32_t slot, Conn *c) {
    struct io_uring_sqe *s = get_sqe(w);
    if (!s) return;
    s->opcode = IORING_OP_RECV;
    s->fd = slot;
    s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->buf_group = BGID;
    s->user_data = make_ud(TAG_RECV, c->gen, slot);
}

static void arm_send(RingWorker *w, uint32_t slot, Conn *c) {
    struct io_uring_sqe *s = get_sqe(w);
    if (!s) return;
    s->opcode = IORING_OP_SEND;
    s->fd = slot;
    s->addr = (uint64_t)(c->out_buf + c->out_off);
    s->len = c->out_len - c->out_off;
    s->flags = IOSQE_FIXED_FILE;
    s->user_data = make_ud(TAG_SEND, c->gen, slot);
}

static void arm_close(RingWorker *w, uint32_t slot, Conn *c) {
    struct io_uring_sqe *s = get_sqe(w);
    if (!s) return;
    s->opcode = IORING_OP_CLOSE;
    s->file_index = slot + 1;
    s->user_data = make_ud(TAG_CLOSE, c->gen, slot);
    c->flags &= ~FLAG_OPEN;
}

static inline int parse_content_length(const char *headers, int head_len) {
    const char *p = headers;
    const char *end = headers + head_len;
    while (p < end) {
        const char *next_line = memmem(p, end - p, "\r\n", 2);
        if (!next_line) break;
        int line_len = next_line - p;
        if (line_len >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *val = p + 15;
            while (val < next_line && (*val == ' ' || *val == '\t')) val++;
            return atoi(val);
        }
        p = next_line + 2;
    }
    return 0;
}

static void process_http_request(RingWorker *w, uint32_t slot, Conn *c, const char *raw_data, int raw_len) {
    char *end_head = memmem(raw_data, raw_len, "\r\n\r\n", 4);
    if (!end_head) {
        arm_recv(w, slot, c);
        return;
    }
    int head_len = end_head - raw_data + 4;

    memset(&req_ctx, 0, sizeof(req_ctx));
    req_ctx.slot = slot;
    req_ctx.gen = c->gen;
    req_ctx.ring_idx = w->id;
    req_ctx.has_auth = 0;
    req_ctx.auth_status = 0;
    req_ctx.auth_uid = 0;
    req_ctx.auth_uname[0] = 0;
    req_ctx.auth_header[0] = 0;
    req_ctx.status_code = 200;
    req_ctx.content_type[0] = 0;
    req_ctx.out_body_len = 0;
    req_ctx.resp_len = 0;
    req_ctx.method = raw_data[0];

    const char *line_end = memmem(raw_data, head_len, "\r\n", 2);
    if (!line_end) {
        arm_close(w, slot, c);
        return;
    }

    const char *path_start = memchr(raw_data, ' ', line_end - raw_data);
    if (!path_start) {
        arm_close(w, slot, c);
        return;
    }
    path_start++;
    const char *path_end = memchr(path_start, ' ', line_end - path_start);
    if (!path_end) {
        arm_close(w, slot, c);
        return;
    }
    req_ctx.path_len = path_end - path_start;
    if (req_ctx.path_len >= (int)sizeof(req_ctx.path)) req_ctx.path_len = sizeof(req_ctx.path) - 1;
    memcpy(req_ctx.path, path_start, req_ctx.path_len);
    req_ctx.path[req_ctx.path_len] = 0;

    if (memmem(raw_data, line_end - raw_data, "HTTP/1.0", 8)) {
        req_ctx.keep_alive = 0;
    } else {
        req_ctx.keep_alive = 1;
    }

    // Single-pass header scan line-by-line
    const char *cur = line_end + 2;
    while (cur < end_head) {
        const char *next_line = memmem(cur, end_head - cur + 2, "\r\n", 2);
        if (!next_line) break;
        int line_len = next_line - cur;
        if (line_len == 0) break;

        if ((cur[0] == 'A' || cur[0] == 'a') && line_len >= 14 && strncasecmp(cur, "Authorization:", 14) == 0) {
            const char *val = cur + 14;
            while (val < next_line && (*val == ' ' || *val == '\t')) val++;
            if (next_line - val >= 7 && strncasecmp(val, "Bearer ", 7) == 0) {
                val += 7;
                while (val < next_line && (*val == ' ' || *val == '\t')) val++;
                const char *vend = next_line;
                while (vend > val && (*(vend-1) == ' ' || *(vend-1) == '\t')) vend--;
                int tlen = vend - val;
                if (tlen > 0 && tlen < (int)sizeof(req_ctx.auth_header)) {
                    memcpy(req_ctx.auth_header, val, tlen);
                    req_ctx.auth_header[tlen] = 0;
                    req_ctx.has_auth = 1;
                }
            }
        } else if ((cur[0] == 'C' || cur[0] == 'c') && line_len >= 11 && strncasecmp(cur, "Connection:", 11) == 0) {
            const char *val = cur + 11;
            while (val < next_line && (*val == ' ' || *val == '\t')) val++;
            if (next_line - val >= 5 && strncasecmp(val, "close", 5) == 0) {
                req_ctx.keep_alive = 0;
            } else if (next_line - val >= 10 && strncasecmp(val, "keep-alive", 10) == 0) {
                req_ctx.keep_alive = 1;
            }
        }
        cur = next_line + 2;
    }

    req_ctx.body_len = raw_len - head_len;
    if (req_ctx.body_len > 0) {
        if (req_ctx.body_len >= (int)sizeof(req_ctx.body)) req_ctx.body_len = sizeof(req_ctx.body) - 1;
        memcpy(req_ctx.body, end_head + 4, req_ctx.body_len);
        req_ctx.body[req_ctx.body_len] = 0;
    } else {
        req_ctx.body[0] = 0;
    }

    req_ctx.resp_buf = acquire_pool(&resp_pool);
    req_ctx.out_body = req_ctx.resp_buf + 512;

    curr_conn = &req_ctx;
    static int64_t bf_tape[96];
    memset(bf_tape, 0, sizeof(bf_tape));
    bf_tape[0] = req_ctx.method;
    bf_tape[2] = req_ctx.path_len;
    bf_tape[3] = req_ctx.keep_alive;
    bf_tape[4] = req_ctx.has_auth;
    bf_tape[5] = req_ctx.body_len;
    bf_tape[7] = 0;
    if (req_ctx.has_auth) {
        int res = verify_jwt(req_ctx.auth_header, &req_ctx.auth_uid, req_ctx.auth_uname, sizeof(req_ctx.auth_uname));
        req_ctx.auth_status = res;
        if (res == 1) { bf_tape[8] = 1; bf_tape[9] = 0; }
        else if (res == -2) { bf_tape[8] = 0; bf_tape[9] = 2; }
        else { bf_tape[8] = 0; bf_tape[9] = 1; }
    } else {
        bf_tape[8] = 0;
        bf_tape[9] = 1;
        req_ctx.auth_status = 0;
    }
    int copy_len = (req_ctx.path_len < 32) ? req_ctx.path_len : 32;
    for (int i = 0; i < copy_len; i++) bf_tape[64 + i] = req_ctx.path[i];
    bf_tape[64 + copy_len] = 0;

    bf_execute(bf_tape);

    if (bf_tape[7] == 1) bf_mmio_sys(bf_tape);
    if (bf_tape[16] != 0) bf_mmio_db(bf_tape);
    if (bf_tape[32] == 1) bf_mmio_net(bf_tape);

    c->out_buf = req_ctx.resp_buf;
    c->out_len = req_ctx.resp_len;
    c->out_off = 0;
    c->keep_alive = req_ctx.keep_alive;

    arm_send(w, slot, c);
}

static void handle_incoming_data(RingWorker *w, uint32_t slot, Conn *c, const char *data, int data_len) {
    if (c->in_buf != NULL) {
        if (data && data_len > 0) {
            if (c->in_len + data_len > c->in_cap) {
                uint32_t new_cap = c->in_cap * 2;
                if (new_cap < c->in_len + data_len) new_cap = c->in_len + data_len + 1024;
                if (new_cap > 65536) {
                    if (c->in_buf) {
                        free(c->in_buf);
                        c->in_buf = NULL;
                        c->in_len = 0;
                        c->in_cap = 0;
                    }
                    arm_close(w, slot, c);
                    return;
                }
                c->in_buf = realloc(c->in_buf, new_cap);
                c->in_cap = new_cap;
            }
            memcpy(c->in_buf + c->in_len, data, data_len);
            c->in_len += data_len;
        }

        char *end_head = memmem(c->in_buf, c->in_len, "\r\n\r\n", 4);
        if (!end_head) {
            arm_recv(w, slot, c);
            return;
        }
        int head_len = (end_head - c->in_buf) + 4;
        int clen = parse_content_length(c->in_buf, head_len);
        int total_req = head_len + clen;
        if ((int)c->in_len < total_req) {
            arm_recv(w, slot, c);
            return;
        }

        process_http_request(w, slot, c, c->in_buf, total_req);

        int leftover = c->in_len - total_req;
        if (leftover > 0) {
            memmove(c->in_buf, c->in_buf + total_req, leftover);
            c->in_len = leftover;
        } else {
            free(c->in_buf);
            c->in_buf = NULL;
            c->in_len = 0;
            c->in_cap = 0;
        }
        return;
    }

    if (data && data_len > 0) {
        char *end_head = memmem(data, data_len, "\r\n\r\n", 4);
        if (end_head) {
            int head_len = (end_head - data) + 4;
            int clen = parse_content_length(data, head_len);
            int total_req = head_len + clen;
            if (data_len >= total_req) {
                process_http_request(w, slot, c, data, total_req);

                int leftover = data_len - total_req;
                if (leftover > 0) {
                    c->in_cap = (leftover > 2048 ? leftover + 2048 : 4096);
                    c->in_buf = malloc(c->in_cap);
                    memcpy(c->in_buf, data + total_req, leftover);
                    c->in_len = leftover;
                }
                return;
            }
        }

        c->in_cap = (data_len > 2048 ? data_len + 2048 : 4096);
        c->in_buf = malloc(c->in_cap);
        memcpy(c->in_buf, data, data_len);
        c->in_len = data_len;
        arm_recv(w, slot, c);
    }
}

static void on_accept(RingWorker *w, int res, uint32_t listener_idx) {
    if (listener_idx < K_LISTENER_COUNT) {
        w->accepts[listener_idx]--;
    }
    if (res >= 0) {
        uint32_t slot = (uint32_t)res;
        if (slot >= w->slots) return;
        Conn *c = &w->conns[slot];
        c->gen++;
        c->flags = FLAG_OPEN;
        c->ring_idx = (uint8_t)w->id;
        c->keep_alive = 1;
        c->out_buf = NULL;
        c->out_len = 0;
        c->out_off = 0;
        c->in_buf = NULL;
        c->in_len = 0;
        c->in_cap = 0;
        w->nconn++;
        arm_recv(w, slot, c);
    }
}

static void on_recv(RingWorker *w, uint32_t slot, uint32_t gen, int res, uint32_t flags) {
    if (slot >= w->slots) return;
    Conn *c = &w->conns[slot];
    if (!(c->flags & FLAG_OPEN) || c->gen != gen) return;

    if (res > 0) {
        uint16_t bid = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);
        char *data = w->pbuf_mem + (bid * BUF_SIZE);
        handle_incoming_data(w, slot, c, data, res);
        return_pbuf(w, bid);
    } else {
        arm_close(w, slot, c);
    }
}

static void on_send(RingWorker *w, uint32_t slot, uint32_t gen, int res) {
    if (slot >= w->slots) return;
    Conn *c = &w->conns[slot];
    if (!(c->flags & FLAG_OPEN) || c->gen != gen) return;

    if (res <= 0) {
        if (c->out_buf) {
            release_pool(&resp_pool, c->out_buf);
            c->out_buf = NULL;
            c->out_len = 0;
            c->out_off = 0;
        }
        if (c->in_buf) {
            free(c->in_buf);
            c->in_buf = NULL;
            c->in_len = 0;
            c->in_cap = 0;
        }
        arm_close(w, slot, c);
        return;
    }

    c->out_off += res;
    if (c->out_off < c->out_len) {
        arm_send(w, slot, c);
        return;
    }

    if (c->out_buf) {
        release_pool(&resp_pool, c->out_buf);
        c->out_buf = NULL;
        c->out_len = 0;
        c->out_off = 0;
    }

    if (!c->keep_alive) {
        arm_close(w, slot, c);
    } else {
        if (c->in_buf && c->in_len > 0) {
            handle_incoming_data(w, slot, c, NULL, 0);
        } else {
            arm_recv(w, slot, c);
        }
    }
}

static void on_close(RingWorker *w, uint32_t slot, uint32_t gen) {
    if (slot >= w->slots) return;
    Conn *c = &w->conns[slot];
    c->flags = 0;
    c->gen++;
    if (c->out_buf) {
        release_pool(&resp_pool, c->out_buf);
        c->out_buf = NULL;
        c->out_len = 0;
        c->out_off = 0;
    }
    if (c->in_buf) {
        free(c->in_buf);
        c->in_buf = NULL;
        c->in_len = 0;
        c->in_cap = 0;
    }
    if (w->nconn > 0) w->nconn--;
}

static void drain_cqes(RingWorker *w) {
    unsigned head = *w->cq_khead;
    unsigned tail = __atomic_load_n(w->cq_ktail, __ATOMIC_ACQUIRE);
    if (head == tail) return;

    int returned = 0;
    while (head != tail) {
        struct io_uring_cqe *cqe = &w->cqes[head & w->cq_mask];
        uint8_t tag = ud_tag(cqe->user_data);
        uint32_t gen = ud_gen(cqe->user_data);
        uint32_t slot = ud_slot(cqe->user_data);

        switch (tag) {
            case TAG_ACCEPT:
                on_accept(w, cqe->res, slot);
                break;
            case TAG_RECV:
                on_recv(w, slot, gen, cqe->res, cqe->flags);
                if (cqe->flags & IORING_CQE_F_BUFFER) returned++;
                break;
            case TAG_SEND:
                on_send(w, slot, gen, cqe->res);
                break;
            case TAG_CLOSE:
                on_close(w, slot, gen);
                break;
        }
        head++;
    }
    __atomic_store_n(w->cq_khead, head, __ATOMIC_RELEASE);

    if (returned > 0) {
        publish_pbufs(w);
    }
    fill_accepts(w);
}

static inline void enter_with_timeout(int fd, unsigned to_submit, unsigned min_complete, int64_t ns) {
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = ns };
    struct io_uring_getevents_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.ts = (uint64_t)&ts;
    unsigned flags = IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
    sys_io_uring_enter(fd, to_submit, min_complete, flags, &arg, sizeof(arg));
}

static inline void enter_nonblocking(int fd, unsigned to_submit) {
    sys_io_uring_enter(fd, to_submit, 0, IORING_ENTER_GETEVENTS, NULL, 0);
}

static int init_worker(uint32_t id) {
    RingWorker *w = &rings[id];
    w->id = id;
    w->slots = SLOTS_PER_RING;
    w->depth = ACCEPT_DEPTH;
    w->nconn = 0;
    w->full = false;
    w->accepting = true;
    memset(w->accepts, 0, sizeof(w->accepts));

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    p.cq_entries = 16384;
    w->ring_fd = sys_io_uring_setup(2048, &p);
    if (w->ring_fd < 0) {
        perror("sys_io_uring_setup");
        return -1;
    }

    size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    void *sq_ptr = mmap(NULL, sq_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, w->ring_fd, IORING_OFF_SQ_RING);
    w->sq_khead = sq_ptr + p.sq_off.head;
    w->sq_ktail = sq_ptr + p.sq_off.tail;
    w->sq_kflags = sq_ptr + p.sq_off.flags;
    w->sq_array = sq_ptr + p.sq_off.array;
    w->sq_mask = *(unsigned*)(sq_ptr + p.sq_off.ring_mask);
    w->sq_entries = *(unsigned*)(sq_ptr + p.sq_off.ring_entries);
    w->sq_head = 0;
    w->sq_tail = 0;

    size_t sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    w->sqes = mmap(NULL, sqes_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, w->ring_fd, IORING_OFF_SQES);

    size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    void *cq_ptr = mmap(NULL, cq_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, w->ring_fd, IORING_OFF_CQ_RING);
    w->cq_khead = cq_ptr + p.cq_off.head;
    w->cq_ktail = cq_ptr + p.cq_off.tail;
    w->cqes = cq_ptr + p.cq_off.cqes;
    w->cq_mask = *(unsigned*)(cq_ptr + p.cq_off.ring_mask);
    w->cq_entries = *(unsigned*)(cq_ptr + p.cq_off.ring_entries);

    struct io_uring_rsrc_register r = {
        .nr = SLOTS_PER_RING,
        .flags = 0,
        .resv2 = 0,
        .data = 0,
        .tags = 0,
    };
    if (sys_io_uring_register(w->ring_fd, IORING_REGISTER_FILES2, &r, sizeof(r)) != 0) {
        perror("register files");
        return -1;
    }

    size_t ring_bytes = N_BUFS_PER_RING * sizeof(struct io_uring_buf);
    void *rmem = mmap(NULL, ring_bytes, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
    struct io_uring_buf_reg breg;
    memset(&breg, 0, sizeof(breg));
    breg.ring_addr = (uint64_t)rmem;
    breg.ring_entries = N_BUFS_PER_RING;
    breg.bgid = BGID;
    if (sys_io_uring_register(w->ring_fd, IORING_REGISTER_PBUF_RING, &breg, 1) != 0 && errno == EINVAL) {
        breg.resv[0] = 1;
        sys_io_uring_register(w->ring_fd, IORING_REGISTER_PBUF_RING, &breg, 1);
    }
    w->pbuf_ring = rmem;
    w->pbuf_tail = 0;

    size_t bufs_sz = N_BUFS_PER_RING * BUF_SIZE;
    w->pbuf_mem = mmap(NULL, bufs_sz, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    madvise(w->pbuf_mem, bufs_sz, MADV_HUGEPAGE);

    for (int i = 0; i < N_BUFS_PER_RING; i++) {
        return_pbuf(w, i);
    }
    publish_pbufs(w);

    size_t conns_sz = SLOTS_PER_RING * sizeof(Conn);
    w->conns = mmap(NULL, conns_sz, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    madvise(w->conns, conns_sz, MADV_HUGEPAGE);
    memset(w->conns, 0, conns_sz);

    fill_accepts(w);
    return 0;
}

static void sig_handler(int sig) {
    (void)sig;
    exit(0);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    struct rlimit rlm;
    if (getrlimit(RLIMIT_MEMLOCK, &rlm) == 0) {
        rlm.rlim_cur = rlm.rlim_max;
        setrlimit(RLIMIT_MEMLOCK, &rlm);
    }

    start_time = time(NULL);
    const char *port_str = getenv("PORT"); int port = port_str ? atoi(port_str) : 3000;
    const char *db_path = getenv("SQLITE_PATH"); if (!db_path) db_path = "feed.db";
    const char *secret = getenv("JWT_SECRET"); if (!secret) secret = "twelve-dollar-challenge";
    strncpy(jwt_secret, secret, sizeof(jwt_secret)-1);
    base_hmac_ctx = HMAC_CTX_new();
    HMAC_Init_ex(base_hmac_ctx, jwt_secret, strlen(jwt_secret), EVP_sha256(), NULL);
    req_hmac_ctx = HMAC_CTX_new();

    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) return 1;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-32000;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=268435456;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    sqlite3_prepare_v3(db, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20;", -1, SQLITE_PREPARE_PERSISTENT, &st_feed, NULL);
    sqlite3_prepare_v3(db, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?;", -1, SQLITE_PREPARE_PERSISTENT, &st_post, NULL);
    sqlite3_prepare_v3(db, "SELECT 1 FROM posts WHERE id = ?;", -1, SQLITE_PREPARE_PERSISTENT, &st_check, NULL);
    sqlite3_prepare_v3(db, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at;", -1, SQLITE_PREPARE_PERSISTENT, &st_insert, NULL);
    sqlite3_prepare_v3(db, "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?, ?);", -1, SQLITE_PREPARE_PERSISTENT, &st_like, NULL);
    sqlite3_prepare_v3(db, "DELETE FROM likes WHERE user_id = ? AND post_id = ?;", -1, SQLITE_PREPARE_PERSISTENT, &st_unlike, NULL);
    sqlite3_prepare_v3(db, "SELECT count(*) FROM likes WHERE post_id = ?;", -1, SQLITE_PREPARE_PERSISTENT, &st_count, NULL);

    // Initialize RespPool (16384 * 16384 bytes = 256 MB)
    size_t pool_bytes = POOL_COUNT * POOL_BUF_SIZE;
    resp_pool.mem = mmap(NULL, pool_bytes, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    madvise(resp_pool.mem, pool_bytes, MADV_HUGEPAGE);
    resp_pool.free_stack = malloc(POOL_COUNT * sizeof(uint16_t));
    for (uint32_t i = 0; i < POOL_COUNT; i++) {
        resp_pool.free_stack[i] = (uint16_t)i;
    }
    resp_pool.top = POOL_COUNT;

    // Create 8 SO_REUSEPORT listeners
    int opt = 1;
    int rcv = 2048, snd = 2048, clamp = 2048;
    for (int i = 0; i < K_LISTENER_COUNT; i++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
        setsockopt(fd, IPPROTO_TCP, 10, &clamp, sizeof(clamp)); // TCP_WINDOW_CLAMP
        setsockopt(fd, IPPROTO_TCP, 9, &opt, sizeof(opt)); // TCP_DEFER_ACCEPT

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            perror("bind"); return 1;
        }
        if (listen(fd, 65535) != 0) {
            perror("listen"); return 1;
        }
        listeners[i] = fd;
    }

    // Initialize 8 rings (523,776 direct descriptors)
    for (int i = 0; i < K_RING_COUNT; i++) {
        if (init_worker(i) != 0) {
            fprintf(stderr, "Failed to init worker %d\n", i);
            return 1;
        }
    }

    fprintf(stderr, "Brainfuck io_uring Server listening on port %d with 8 rings x %d = %d direct descriptors\n",
        port, SLOTS_PER_RING, K_RING_COUNT * SLOTS_PER_RING);

    uint32_t wait_ri = 0;
    while (running) {
        bool any_cqes = false;
        for (int i = 0; i < K_RING_COUNT; i++) {
            if (*rings[i].cq_khead != __atomic_load_n(rings[i].cq_ktail, __ATOMIC_ACQUIRE)) {
                any_cqes = true;
                break;
            }
        }

        bool any_to_submit = false;
        for (int i = 0; i < K_RING_COUNT; i++) {
            if (rings[i].sq_tail != rings[i].sq_head) {
                any_to_submit = true;
                break;
            }
        }

        bool busy = any_cqes || any_to_submit;
        uint32_t cur_wait = wait_ri % K_RING_COUNT;
        wait_ri++;

        for (int i = K_RING_COUNT - 1; i >= 0; i--) {
            RingWorker *w = &rings[i];
            uint32_t to_submit = flush_sq(w);
            if (i == cur_wait && !busy) {
                enter_with_timeout(w->ring_fd, to_submit, 1, 50000); // 50 us
            } else if (to_submit > 0) {
                enter_nonblocking(w->ring_fd, to_submit);
            }
        }

        for (int i = 0; i < K_RING_COUNT; i++) {
            drain_cqes(&rings[i]);
        }
    }

    return 0;
}
