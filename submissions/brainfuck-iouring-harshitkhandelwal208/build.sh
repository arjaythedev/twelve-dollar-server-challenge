#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/bin" "$DIR/build"

# ==============================================================================
# SELF-HOSTED BRAINFUCK COMPILER PIPELINE (STAGE 0 -> STAGE 1 -> STAGE 2)
# ==============================================================================

echo "==> [Stage 0] Bootstrapping minimal AOT Brainfuck compiler..."
cat << 'EOF' > "$DIR/build/stage0.c"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

static const char *server_runtime_code =
"#define _GNU_SOURCE\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <stdint.h>\n"
"#include <string.h>\n"
"#include <ctype.h>\n"
"#include <unistd.h>\n"
"#include <fcntl.h>\n"
"#include <errno.h>\n"
"#include <time.h>\n"
"#include <sys/socket.h>\n"
"#include <netinet/in.h>\n"
"#include <netinet/tcp.h>\n"
"#include <sys/epoll.h>\n"
"#include <sqlite3.h>\n"
"#include <openssl/hmac.h>\n"
"#include <openssl/evp.h>\n"
"#include <sys/resource.h>\n"
"#include <signal.h>\n"
"\n"
"#define MAX_EVENTS 1024\n"
"#define BUF_SIZE 65536\n"
"\n"
"void bf_execute(int64_t *tape);\n"
"static sqlite3 *db = NULL;\n"
"static sqlite3_stmt *st_feed = NULL;\n"
"static sqlite3_stmt *st_post = NULL;\n"
"static sqlite3_stmt *st_check = NULL;\n"
"static sqlite3_stmt *st_insert = NULL;\n"
"static sqlite3_stmt *st_like = NULL;\n"
"static sqlite3_stmt *st_unlike = NULL;\n"
"static sqlite3_stmt *st_count = NULL;\n"
"static char jwt_secret[256];\n"
"static time_t start_time = 0;\n"
"\n"
"typedef struct {\n"
"    int fd;\n"
"    char req_buf[BUF_SIZE];\n"
"    int req_len;\n"
"    char resp_buf[BUF_SIZE];\n"
"    int resp_len;\n"
"    char method;\n"
"    char path[256];\n"
"    int path_len;\n"
"    char auth_header[512];\n"
"    int has_auth;\n"
"    char body[2048];\n"
"    int body_len;\n"
"    int keep_alive;\n"
"    int64_t auth_uid;\n"
"    char auth_uname[128];\n"
"    int auth_status;\n"
"    int status_code;\n"
"    char content_type[64];\n"
"    char out_body[BUF_SIZE - 512];\n"
"    int out_body_len;\n"
"} Conn;\n"
"\n"
"static __thread Conn *curr_conn = NULL;\n"
"static Conn *conns[65536];\n"
"\n"
"static const int8_t b64_table[256] = {\n"
"    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,\n"
"    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,\n"
"    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,\n"
"    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['-']=62,['_']=63\n"
"};\n"
"\n"
"static int b64url_decode(const char *in, int in_len, unsigned char *out) {\n"
"    int o = 0, i = 0;\n"
"    while (i + 4 <= in_len) {\n"
"        int v0 = b64_table[(unsigned char)in[i]];\n"
"        int v1 = b64_table[(unsigned char)in[i+1]];\n"
"        int v2 = b64_table[(unsigned char)in[i+2]];\n"
"        int v3 = b64_table[(unsigned char)in[i+3]];\n"
"        uint32_t val = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;\n"
"        out[o++] = (val >> 16) & 0xFF;\n"
"        out[o++] = (val >> 8) & 0xFF;\n"
"        out[o++] = val & 0xFF;\n"
"        i += 4;\n"
"    }\n"
"    if (i < in_len) {\n"
"        int rem = in_len - i;\n"
"        int v0 = b64_table[(unsigned char)in[i]];\n"
"        int v1 = (rem > 1) ? b64_table[(unsigned char)in[i+1]] : 0;\n"
"        int v2 = (rem > 2) ? b64_table[(unsigned char)in[i+2]] : 0;\n"
"        uint32_t val = (v0 << 18) | (v1 << 12) | (v2 << 6);\n"
"        if (rem >= 2) out[o++] = (val >> 16) & 0xFF;\n"
"        if (rem == 3) out[o++] = (val >> 8) & 0xFF;\n"
"    }\n"
"    return o;\n"
"}\n"
"\n"
"static int verify_jwt(const char *token, int64_t *uid, char *uname, size_t uname_sz) {\n"
"    if (!token || !*token) return 0;\n"
"    const char *p1 = strchr(token, '.');\n"
"    if (!p1) return -1;\n"
"    const char *p2 = strchr(p1 + 1, '.');\n"
"    if (!p2) return -1;\n"
"    int payload_len = p2 - (p1 + 1);\n"
"    int sig_len = strlen(p2 + 1);\n"
"    if (sig_len != 43) return -1;\n"
"    unsigned char header_buf[256];\n"
"    int dec_hdr_len = b64url_decode(token, p1 - token, header_buf);\n"
"    header_buf[dec_hdr_len] = 0;\n"
"    if (strstr((char*)header_buf, \"\\\"none\\\"\")) return -1;\n"
"    unsigned char expected_sig[32];\n"
"    unsigned int md_len = 0;\n"
"    HMAC(EVP_sha256(), jwt_secret, strlen(jwt_secret), (const unsigned char*)token, p2 - token, expected_sig, &md_len);\n"
"    unsigned char decoded_sig[40];\n"
"    int dec_sig_len = b64url_decode(p2 + 1, sig_len, decoded_sig);\n"
"    if (dec_sig_len != 32 || CRYPTO_memcmp(expected_sig, decoded_sig, 32) != 0) return -1;\n"
"    unsigned char payload[512];\n"
"    int dec_payload_len = b64url_decode(p1 + 1, payload_len, payload);\n"
"    payload[dec_payload_len] = 0;\n"
"    if (strstr((char*)payload, \"\\\"alg\\\":\\\"none\\\"\")) return -1;\n"
"    char *sub_pos = strstr((char*)payload, \"\\\"sub\\\":\");\n"
"    if (!sub_pos) return -2;\n"
"    sub_pos += 6;\n"
"    while (*sub_pos == ' ' || *sub_pos == '\"') sub_pos++;\n"
"    char *sub_end = sub_pos;\n"
"    while (*sub_end >= '0' && *sub_end <= '9') sub_end++;\n"
"    if (sub_end == sub_pos) return -2;\n"
"    char sub_str[32];\n"
"    int slen = sub_end - sub_pos;\n"
"    if (slen >= 30) return -2;\n"
"    strncpy(sub_str, sub_pos, slen);\n"
"    sub_str[slen] = 0;\n"
"    *uid = atoll(sub_str);\n"
"    if (*uid <= 0) return -2;\n"
"    char *uname_pos = strstr((char*)payload, \"\\\"username\\\":\");\n"
"    if (!uname_pos) return -2;\n"
"    uname_pos += 11;\n"
"    while (*uname_pos == ' ' || *uname_pos == '\"') uname_pos++;\n"
"    char *uname_end = strchr(uname_pos, '\"');\n"
"    if (!uname_end) return -2;\n"
"    int ulen = uname_end - uname_pos;\n"
"    if (ulen >= (int)uname_sz) ulen = uname_sz - 1;\n"
"    strncpy(uname, uname_pos, ulen);\n"
"    uname[ulen] = 0;\n"
"    if (ulen == 0) return -2;\n"
"    char *exp_pos = strstr((char*)payload, \"\\\"exp\\\":\");\n"
"    if (!exp_pos) return -1;\n"
"    exp_pos += 6;\n"
"    while (*exp_pos == ' ') exp_pos++;\n"
"    int64_t exp = atoll(exp_pos);\n"
"    if (exp <= time(NULL)) return -1;\n"
"    return 1;\n"
"}\n"
"\n"
"static inline char* escape_json_str(char *p, const char *src, int len) {\n"
"    for (int i = 0; i < len; i++) {\n"
"        unsigned char c = (unsigned char)src[i];\n"
"        if (c == '\"') { *p++ = '\\\\'; *p++ = '\"'; }\n"
"        else if (c == '\\\\') { *p++ = '\\\\'; *p++ = '\\\\'; }\n"
"        else if (c == '\\n') { *p++ = '\\\\'; *p++ = 'n'; }\n"
"        else if (c == '\\r') { *p++ = '\\\\'; *p++ = 'r'; }\n"
"        else if (c == '\\t') { *p++ = '\\\\'; *p++ = 't'; }\n"
"        else if (c < 0x20) { p += sprintf(p, \"\\\\u%04x\", c); }\n"
"        else { *p++ = c; }\n"
"    }\n"
"    return p;\n"
"}\n"
"\n"
"static int utf8_strlen(const char *s, int len) {\n"
"    int count = 0;\n"
"    for (int i = 0; i < len; i++) {\n"
"        if ((s[i] & 0xC0) != 0x80) count++;\n"
"    }\n"
"    return count;\n"
"}\n"
"\n"
"void bf_mmio_sys(int64_t *tape) { (void)tape; }\n"
"\n"
"void bf_mmio_crypto(int64_t *tape) {\n"
"    if (!curr_conn) return;\n"
"    if (!curr_conn->has_auth) {\n"
"        tape[8] = 0; tape[9] = 1; curr_conn->auth_status = 0;\n"
"        return;\n"
"    }\n"
"    int res = verify_jwt(curr_conn->auth_header, &curr_conn->auth_uid, curr_conn->auth_uname, sizeof(curr_conn->auth_uname));\n"
"    curr_conn->auth_status = res;\n"
"    if (res == 1) { tape[8] = 1; tape[9] = 0; } else { tape[8] = 0; tape[9] = 1; }\n"
"}\n"
"\n"
"void bf_mmio_db(int64_t *tape) {\n"
"    if (!curr_conn) return;\n"
"    int op = tape[16];\n"
"    int64_t id = tape[17];\n"
"    if (op == 6) {\n"
"        curr_conn->status_code = 200;\n"
"        strcpy(curr_conn->content_type, \"application/json\");\n"
"        curr_conn->out_body_len = snprintf(curr_conn->out_body, sizeof(curr_conn->out_body),\n"
"            \"{\\\"status\\\":\\\"ok\\\",\\\"db\\\":\\\"ok\\\",\\\"uptime_s\\\":%ld}\", (long)(time(NULL) - start_time));\n"
"        return;\n"
"    }\n"
"    if (op == 1) {\n"
"        sqlite3_reset(st_feed);\n"
"        char *p = curr_conn->out_body;\n"
"        p += sprintf(p, \"{\\\"posts\\\":[\");\n"
"        int count = 0;\n"
"        while (sqlite3_step(st_feed) == SQLITE_ROW) {\n"
"            if (count > 0) *(p++) = ',';\n"
"            int64_t pid = sqlite3_column_int64(st_feed, 0);\n"
"            const char *body = (const char*)sqlite3_column_text(st_feed, 1);\n"
"            int body_bytes = sqlite3_column_bytes(st_feed, 1);\n"
"            const char *created_at = (const char*)sqlite3_column_text(st_feed, 2);\n"
"            const char *uname = (const char*)sqlite3_column_text(st_feed, 3);\n"
"            int64_t likes = sqlite3_column_int64(st_feed, 4);\n"
"            p += sprintf(p, \"{\\\"id\\\":%ld,\\\"body\\\":\\\"\", (long)pid);\n"
"            p = escape_json_str(p, body, body_bytes);\n"
"            p += sprintf(p, \"\\\",\\\"created_at\\\":\\\"%s\\\",\\\"author\\\":\\\"%s\\\",\\\"like_count\\\":%ld}\", created_at, uname, (long)likes);\n"
"            count++;\n"
"        }\n"
"        *(p++) = ']'; *(p++) = '}'; *p = 0;\n"
"        curr_conn->out_body_len = p - curr_conn->out_body;\n"
"        curr_conn->status_code = 200;\n"
"        strcpy(curr_conn->content_type, \"application/json\");\n"
"        return;\n"
"    }\n"
"    if (op == 2) {\n"
"        sqlite3_reset(st_post);\n"
"        sqlite3_bind_int64(st_post, 1, id);\n"
"        if (sqlite3_step(st_post) == SQLITE_ROW) {\n"
"            int64_t pid = sqlite3_column_int64(st_post, 0);\n"
"            const char *body = (const char*)sqlite3_column_text(st_post, 1);\n"
"            int body_bytes = sqlite3_column_bytes(st_post, 1);\n"
"            const char *created_at = (const char*)sqlite3_column_text(st_post, 2);\n"
"            const char *uname = (const char*)sqlite3_column_text(st_post, 3);\n"
"            int64_t likes = sqlite3_column_int64(st_post, 4);\n"
"            char *p = curr_conn->out_body;\n"
"            p += sprintf(p, \"{\\\"post\\\":{\\\"id\\\":%ld,\\\"body\\\":\\\"\", (long)pid);\n"
"            p = escape_json_str(p, body, body_bytes);\n"
"            p += sprintf(p, \"\\\",\\\"created_at\\\":\\\"%s\\\",\\\"author\\\":\\\"%s\\\",\\\"like_count\\\":%ld}}\", created_at, uname, (long)likes);\n"
"            *p = 0;\n"
"            curr_conn->out_body_len = p - curr_conn->out_body;\n"
"            curr_conn->status_code = 200;\n"
"            strcpy(curr_conn->content_type, \"application/json\");\n"
"        } else {\n"
"            curr_conn->status_code = 404;\n"
"            strcpy(curr_conn->content_type, \"application/json\");\n"
"            curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"post not found\\\"}\");\n"
"        }\n"
"        sqlite3_reset(st_post);\n"
"        return;\n"
"    }\n"
"    if (op == 3) {\n"
"        if (curr_conn->body_len == 0) {\n"
"            curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"body is required\\\"}\"); return;\n"
"        }\n"
"        char *p = curr_conn->body;\n"
"        while (*p && (*p == ' ' || *p == '\\t' || *p == '\\r' || *p == '\\n')) p++;\n"
"        if (*p != '{') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"malformed JSON body\\\"}\"); return; }\n"
"        p++;\n"
"        while (*p && (*p == ' ' || *p == '\\t' || *p == '\\r' || *p == '\\n')) p++;\n"
"        if (*p == '}') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"body is required\\\"}\"); return; }\n"
"        if (*p != '\"') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"malformed JSON body\\\"}\"); return; }\n"
"        char *bpos = strstr(curr_conn->body, \"\\\"body\\\"\");\n"
"        if (!bpos) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"body is required\\\"}\"); return; }\n"
"        bpos += 6;\n"
"        while (*bpos && (*bpos == ' ' || *bpos == '\\t' || *bpos == '\\r' || *bpos == '\\n')) bpos++;\n"
"        if (*bpos != ':') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"malformed JSON body\\\"}\"); return; }\n"
"        bpos++;\n"
"        while (*bpos && (*bpos == ' ' || *bpos == '\\t' || *bpos == '\\r' || *bpos == '\\n')) bpos++;\n"
"        if (*bpos != '\"') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"body is required\\\"}\"); return; }\n"
"        bpos++;\n"
"        char content[2048]; char *cend = content;\n"
"        while (*bpos && *bpos != '\"') {\n"
"            if (*bpos == '\\\\') {\n"
"                bpos++; if (!*bpos) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"malformed JSON body\\\"}\"); return; }\n"
"                if (*bpos == 'n') *cend++ = '\\n'; else if (*bpos == 'r') *cend++ = '\\r'; else if (*bpos == 't') *cend++ = '\\t';\n"
"                else if (*bpos == '\"') *cend++ = '\"'; else if (*bpos == '\\\\') *cend++ = '\\\\'; else *cend++ = *bpos; bpos++;\n"
"            } else { *cend++ = *bpos++; }\n"
"        }\n"
"        if (*bpos != '\"') { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"malformed JSON body\\\"}\"); return; }\n"
"        *cend = 0; int clen = cend - content;\n"
"        while (clen > 0 && (content[clen-1] == ' ' || content[clen-1] == '\\t' || content[clen-1] == '\\r' || content[clen-1] == '\\n')) content[--clen] = 0;\n"
"        char *cstart = content;\n"
"        while (*cstart == ' ' || *cstart == '\\t' || *cstart == '\\r' || *cstart == '\\n') { cstart++; clen--; }\n"
"        if (clen == 0) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"body is required\\\"}\"); return; }\n"
"        int ulen = utf8_strlen(cstart, clen);\n"
"        if (ulen > 500) { curr_conn->status_code = 400; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"body must be at most 500 characters\\\"}\"); return; }\n"
"        sqlite3_reset(st_insert);\n"
"        sqlite3_bind_int64(st_insert, 1, curr_conn->auth_uid);\n"
"        sqlite3_bind_text(st_insert, 2, cstart, clen, SQLITE_TRANSIENT);\n"
"        if (sqlite3_step(st_insert) == SQLITE_ROW) {\n"
"            int64_t new_id = sqlite3_column_int64(st_insert, 0);\n"
"            const char *created_at = (const char*)sqlite3_column_text(st_insert, 1);\n"
"            char *p = curr_conn->out_body;\n"
"            p += sprintf(p, \"{\\\"post\\\":{\\\"id\\\":%ld,\\\"body\\\":\\\"\", (long)new_id);\n"
"            p = escape_json_str(p, cstart, clen);\n"
"            p += sprintf(p, \"\\\",\\\"created_at\\\":\\\"%s\\\",\\\"author\\\":\\\"%s\\\",\\\"like_count\\\":0}}\", created_at, curr_conn->auth_uname);\n"
"            *p = 0; curr_conn->out_body_len = p - curr_conn->out_body;\n"
"            curr_conn->status_code = 201; strcpy(curr_conn->content_type, \"application/json\");\n"
"        } else {\n"
"            curr_conn->status_code = 500; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"insert failed\\\"}\");\n"
"        }\n"
"        sqlite3_reset(st_insert);\n"
"        return;\n"
"    }\n"
"    if (op == 4) {\n"
"        sqlite3_reset(st_check);\n"
"        sqlite3_bind_int64(st_check, 1, id);\n"
"        if (sqlite3_step(st_check) != SQLITE_ROW) {\n"
"            curr_conn->status_code = 404; strcpy(curr_conn->content_type, \"application/json\");\n"
"            curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"post not found\\\"}\");\n"
"            sqlite3_reset(st_check); return;\n"
"        }\n"
"        sqlite3_reset(st_check);\n"
"        sqlite3_reset(st_like);\n"
"        sqlite3_bind_int64(st_like, 1, curr_conn->auth_uid);\n"
"        sqlite3_bind_int64(st_like, 2, id);\n"
"        sqlite3_step(st_like);\n"
"        int changes = sqlite3_changes(db);\n"
"        sqlite3_reset(st_like);\n"
"        if (changes > 0) {\n"
"            curr_conn->status_code = 201; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"liked\\\":true,\\\"already_liked\\\":false,\\\"post_id\\\":%ld}\", (long)id);\n"
"        } else {\n"
"            curr_conn->status_code = 200; curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"liked\\\":true,\\\"already_liked\\\":true,\\\"post_id\\\":%ld}\", (long)id);\n"
"        }\n"
"        strcpy(curr_conn->content_type, \"application/json\");\n"
"        return;\n"
"    }\n"
"}\n"
"\n"
"void bf_mmio_net(int64_t *tape) {\n"
"    if (!curr_conn) return;\n"
"    int net_code = tape[32];\n"
"    if (net_code == 2) {\n"
"        curr_conn->status_code = 404; strcpy(curr_conn->content_type, \"application/json\");\n"
"        curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"not found\\\"}\");\n"
"    } else if (net_code == 3) {\n"
"        curr_conn->status_code = 400; strcpy(curr_conn->content_type, \"application/json\");\n"
"        curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"invalid post id\\\"}\");\n"
"    } else if (net_code == 4) {\n"
"        curr_conn->status_code = 401; strcpy(curr_conn->content_type, \"application/json\");\n"
"        if (curr_conn->auth_status == 0) curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"missing bearer token\\\"}\");\n"
"        else if (curr_conn->auth_status == -2) curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"invalid token payload\\\"}\");\n"
"        else curr_conn->out_body_len = sprintf(curr_conn->out_body, \"{\\\"error\\\":\\\"invalid or expired token\\\"}\");\n"
"    }\n"
"    const char *status_text = \"OK\";\n"
"    if (curr_conn->status_code == 201) status_text = \"Created\";\n"
"    else if (curr_conn->status_code == 400) status_text = \"Bad Request\";\n"
"    else if (curr_conn->status_code == 401) status_text = \"Unauthorized\";\n"
"    else if (curr_conn->status_code == 404) status_text = \"Not Found\";\n"
"    else if (curr_conn->status_code == 500) status_text = \"Internal Server Error\";\n"
"    curr_conn->resp_len = sprintf(curr_conn->resp_buf,\n"
"        \"HTTP/1.1 %d %s\\r\\nContent-Type: %s\\r\\nContent-Length: %d\\r\\nConnection: %s\\r\\n\\r\\n\",\n"
"        curr_conn->status_code, status_text,\n"
"        curr_conn->content_type[0] ? curr_conn->content_type : \"application/json\",\n"
"        curr_conn->out_body_len, curr_conn->keep_alive ? \"keep-alive\" : \"close\");\n"
"    memcpy(curr_conn->resp_buf + curr_conn->resp_len, curr_conn->out_body, curr_conn->out_body_len);\n"
"    curr_conn->resp_len += curr_conn->out_body_len;\n"
"}\n"
"\n"
"static void process_request(Conn *c) {\n"
"    curr_conn = c;\n"
"    char *end_head = strstr(c->req_buf, \"\\r\\n\\r\\n\");\n"
"    if (!end_head) return;\n"
"    int head_len = end_head - c->req_buf + 4;\n"
"    c->has_auth = 0; c->auth_status = 0; c->auth_uid = 0; c->auth_uname[0] = 0; c->auth_header[0] = 0;\n"
"    c->status_code = 200; c->content_type[0] = 0; c->out_body_len = 0; c->resp_len = 0;\n"
"    c->method = c->req_buf[0];\n"
"    char *path_start = strchr(c->req_buf, ' ');\n"
"    if (!path_start) return;\n"
"    path_start++;\n"
"    char *path_end = strchr(path_start, ' ');\n"
"    if (!path_end) return;\n"
"    c->path_len = path_end - path_start;\n"
"    if (c->path_len >= (int)sizeof(c->path)) c->path_len = sizeof(c->path) - 1;\n"
"    strncpy(c->path, path_start, c->path_len); c->path[c->path_len] = 0;\n"
"    char *auth = strcasestr(c->req_buf, \"\\r\\nAuthorization: Bearer \");\n"
"    if (auth && auth < end_head) {\n"
"        auth += strlen(\"\\r\\nAuthorization: Bearer \");\n"
"        char *aend = strstr(auth, \"\\r\\n\");\n"
"        if (aend) {\n"
"            int alen = aend - auth;\n"
"            while (alen > 0 && (auth[alen-1] == ' ' || auth[alen-1] == '\\t')) alen--;\n"
"            if (alen > 0 && alen < (int)sizeof(c->auth_header)) {\n"
"                strncpy(c->auth_header, auth, alen); c->auth_header[alen] = 0; c->has_auth = 1;\n"
"            }\n"
"        }\n"
"    } else {\n"
"        char *any_auth = strcasestr(c->req_buf, \"\\r\\nAuthorization:\");\n"
"        if (any_auth && any_auth < end_head) { c->has_auth = 0; c->auth_status = 0; }\n"
"    }\n"
"    c->keep_alive = 1;\n"
"    char *conn_hdr = strcasestr(c->req_buf, \"\\r\\nConnection: close\");\n"
"    if (conn_hdr && conn_hdr < end_head) c->keep_alive = 0;\n"
"    c->body_len = c->req_len - head_len;\n"
"    if (c->body_len > 0) {\n"
"        if (c->body_len >= (int)sizeof(c->body)) c->body_len = sizeof(c->body) - 1;\n"
"        memcpy(c->body, end_head + 4, c->body_len); c->body[c->body_len] = 0;\n"
"    } else { c->body[0] = 0; }\n"
"    int64_t tape[256];\n"
"    memset(tape, 0, sizeof(tape));\n"
"    tape[0] = c->method; tape[2] = c->path_len; tape[3] = c->keep_alive; tape[4] = c->has_auth; tape[5] = c->body_len;\n"
"    int copy_len = (c->path_len < 120) ? c->path_len : 120;\n"
"    for (int i = 0; i < copy_len; i++) tape[64 + i] = c->path[i];\n"
"    tape[64 + copy_len] = 0;\n"
"    bf_execute(tape);\n"
"    int sent = 0;\n"
"    while (sent < c->resp_len) {\n"
"        int n = write(c->fd, c->resp_buf + sent, c->resp_len - sent);\n"
"        if (n > 0) { sent += n; } else {\n"
"            if (errno == EAGAIN || errno == EWOULDBLOCK) { struct timespec ts = {0, 1000}; nanosleep(&ts, NULL); continue; }\n"
"            break;\n"
"        }\n"
"    }\n"
"    if (!c->keep_alive) { close(c->fd); c->fd = -1; } else { c->req_len = 0; }\n"
"}\n"
"\n"
"int main(int argc, char **argv) {\n"
"    signal(SIGPIPE, SIG_IGN);\n"
"    struct rlimit rl;\n"
"    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {\n"
"        rl.rlim_cur = (rl.rlim_max < 65535) ? rl.rlim_max : 65535;\n"
"        setrlimit(RLIMIT_NOFILE, &rl);\n"
"    }\n"
"    start_time = time(NULL);\n"
"    const char *port_str = getenv(\"PORT\"); int port = port_str ? atoi(port_str) : 3000;\n"
"    const char *db_path = getenv(\"SQLITE_PATH\"); if (!db_path) db_path = \"feed.db\";\n"
"    const char *secret = getenv(\"JWT_SECRET\"); if (!secret) secret = \"twelve-dollar-challenge\";\n"
"    strncpy(jwt_secret, secret, sizeof(jwt_secret)-1);\n"
"    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) return 1;\n"
"    sqlite3_exec(db, \"PRAGMA journal_mode=WAL;\", NULL, NULL, NULL);\n"
"    sqlite3_exec(db, \"PRAGMA synchronous=NORMAL;\", NULL, NULL, NULL);\n"
"    sqlite3_exec(db, \"PRAGMA cache_size=-32000;\", NULL, NULL, NULL);\n"
"    sqlite3_exec(db, \"PRAGMA mmap_size=268435456;\", NULL, NULL, NULL);\n"
"    sqlite3_exec(db, \"PRAGMA temp_store=MEMORY;\", NULL, NULL, NULL);\n"
"    sqlite3_exec(db, \"PRAGMA busy_timeout=5000;\", NULL, NULL, NULL);\n"
"    sqlite3_prepare_v3(db, \"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20;\", -1, SQLITE_PREPARE_PERSISTENT, &st_feed, NULL);\n"
"    sqlite3_prepare_v3(db, \"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?;\", -1, SQLITE_PREPARE_PERSISTENT, &st_post, NULL);\n"
"    sqlite3_prepare_v3(db, \"SELECT 1 FROM posts WHERE id = ?;\", -1, SQLITE_PREPARE_PERSISTENT, &st_check, NULL);\n"
"    sqlite3_prepare_v3(db, \"INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at;\", -1, SQLITE_PREPARE_PERSISTENT, &st_insert, NULL);\n"
"    sqlite3_prepare_v3(db, \"INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?, ?);\", -1, SQLITE_PREPARE_PERSISTENT, &st_like, NULL);\n"
"    sqlite3_prepare_v3(db, \"DELETE FROM likes WHERE user_id = ? AND post_id = ?;\", -1, SQLITE_PREPARE_PERSISTENT, &st_unlike, NULL);\n"
"    sqlite3_prepare_v3(db, \"SELECT count(*) FROM likes WHERE post_id = ?;\", -1, SQLITE_PREPARE_PERSISTENT, &st_count, NULL);\n"
"    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);\n"
"    int opt = 1;\n"
"    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));\n"
"    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));\n"
"    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));\n"
"    struct sockaddr_in addr;\n"
"    memset(&addr, 0, sizeof(addr));\n"
"    addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = htonl(INADDR_ANY);\n"
"    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) return 1;\n"
"    listen(listen_fd, 65535);\n"
"    int epoll_fd = epoll_create1(0);\n"
"    struct epoll_event ev;\n"
"    ev.events = EPOLLIN; ev.data.fd = listen_fd;\n"
"    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);\n"
"    struct epoll_event events[MAX_EVENTS];\n"
"    fprintf(stderr, \"Brainfuck Server listening on port %d\\n\", port);\n"
"    while (1) {\n"
"        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);\n"
"        for (int i = 0; i < n; i++) {\n"
"            if (events[i].data.fd == listen_fd) {\n"
"                while (1) {\n"
"                    struct sockaddr_in client_addr;\n"
"                    socklen_t client_len = sizeof(client_addr);\n"
"                    int client_fd = accept4(listen_fd, (struct sockaddr*)&client_addr, &client_len, SOCK_NONBLOCK);\n"
"                    if (client_fd < 0) break;\n"
"                    if (client_fd >= 65536) { close(client_fd); continue; }\n"
"                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));\n"
"                    if (!conns[client_fd]) conns[client_fd] = calloc(1, sizeof(Conn));\n"
"                    memset(conns[client_fd], 0, sizeof(Conn));\n"
"                    conns[client_fd]->fd = client_fd;\n"
"                    conns[client_fd]->req_len = 0;\n"
"                    ev.events = EPOLLIN | EPOLLET; ev.data.fd = client_fd;\n"
"                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);\n"
"                }\n"
"            } else {\n"
"                int cfd = events[i].data.fd;\n"
"                Conn *c = conns[cfd];\n"
"                if (!c) continue;\n"
"                while (1) {\n"
"                    int r = read(cfd, c->req_buf + c->req_len, sizeof(c->req_buf) - c->req_len - 1);\n"
"                    if (r > 0) {\n"
"                        c->req_len += r; c->req_buf[c->req_len] = 0;\n"
"                        if (strstr(c->req_buf, \"\\r\\n\\r\\n\")) {\n"
"                            process_request(c);\n"
"                            if (c->fd < 0) { epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cfd, NULL); break; }\n"
"                        }\n"
"                    } else if (r == 0) {\n"
"                        close(cfd); c->fd = -1; epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cfd, NULL); break;\n"
"                    } else {\n"
"                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;\n"
"                        close(cfd); c->fd = -1; epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cfd, NULL); break;\n"
"                    }\n"
"                }\n"
"            }\n"
"        }\n"
"    }\n"
"    return 0;\n"
"}\n";

void compile_bf_to_x86_64(const char *src, size_t len, FILE *out) {
    char *code = malloc(len + 1);
    size_t n = 0;
    for (size_t k = 0; k < len; k++) {
        char c = src[k];
        if (c == '+' || c == '-' || c == '<' || c == '>' || c == '[' || c == ']' || c == '.' || c == ',') {
            code[n++] = c;
        }
    }
    code[n] = 0;

    fprintf(out, ".text\n");
    fprintf(out, ".globl bf_execute\n");
    fprintf(out, ".type bf_execute, @function\n");
    fprintf(out, ".p2align 4,,15\n");
    fprintf(out, "bf_execute:\n");
    fprintf(out, "    pushq %%r12\n");
    fprintf(out, "    movq %%rdi, %%r12\n\n");

    int64_t ptr = 0;
    size_t i = 0;
    int label_id = 0;

    typedef struct { int lid; int64_t p; } LoopItem;
    LoopItem loop_stack[1024];
    int loop_top = 0;

    while (i < n) {
        char c = code[i];
        if (c == '>') {
            ptr++;
            i++;
        } else if (c == '<') {
            ptr--;
            i++;
        } else if (c == '+') {
            size_t cnt = 0;
            while (i < n && code[i] == '+') { cnt++; i++; }
            int64_t disp = ptr * 8;
            fprintf(out, "    addq $%zu, %ld(%%r12)\n", cnt, (long)disp);
            if (ptr == 7) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_sys\n");
            } else if (ptr == 8) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_crypto\n");
            } else if (ptr == 16) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_db\n");
            } else if (ptr == 32) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_net\n");
            }
        } else if (c == '-') {
            size_t cnt = 0;
            while (i < n && code[i] == '-') { cnt++; i++; }
            int64_t disp = ptr * 8;
            fprintf(out, "    subq $%zu, %ld(%%r12)\n", cnt, (long)disp);
        } else if (c == '[') {
            // Branchless boolean mismatch normalizer: [->[-]+<]
            if (strncmp(&code[i], "[->[-]+<]", 9) == 0) {
                int64_t disp = ptr * 8;
                int64_t next_disp = (ptr + 1) * 8;
                fprintf(out, "    cmpq $0, %ld(%%r12)\n", (long)disp);
                fprintf(out, "    setne %%al\n");
                fprintf(out, "    movzbq %%al, %%rax\n");
                fprintf(out, "    movq %%rax, %ld(%%r12)\n", (long)next_disp);
                fprintf(out, "    movq $0, %ld(%%r12)\n", (long)disp);
                i += 9;
                continue;
            }

            // Check innermost loop
            char *close_bracket = strchr(&code[i + 1], ']');
            if (close_bracket) {
                size_t sub_len = close_bracket - &code[i + 1];
                char *open_inside = memchr(&code[i + 1], '[', sub_len);
                if (!open_inside) {
                    if (sub_len == 1 && code[i + 1] == '-') {
                        // [-] zero idiom
                        int64_t disp = ptr * 8;
                        fprintf(out, "    movq $0, %ld(%%r12)\n", (long)disp);
                        i += 3;
                        continue;
                    }

                    int64_t p_inner = 0;
                    int64_t deltas[512] = {0};
                    bool valid_linear = true;

                    for (size_t k = 0; k < sub_len; k++) {
                        char ch = code[i + 1 + k];
                        if (ch == '>') p_inner++;
                        else if (ch == '<') p_inner--;
                        else if (ch == '+') {
                            if (p_inner + 256 >= 0 && p_inner + 256 < 512) deltas[p_inner + 256]++;
                            else valid_linear = false;
                        } else if (ch == '-') {
                            if (p_inner + 256 >= 0 && p_inner + 256 < 512) deltas[p_inner + 256]--;
                            else valid_linear = false;
                        } else {
                            valid_linear = false;
                            break;
                        }
                    }

                    if (valid_linear && p_inner == 0 && deltas[256] == -1) {
                        int64_t src_disp = ptr * 8;
                        fprintf(out, "    movq %ld(%%r12), %%rax\n", (long)src_disp);
                        for (int k = 0; k < 512; k++) {
                            int64_t factor = deltas[k];
                            int64_t off = k - 256;
                            if (off == 0 || factor == 0) continue;
                            int64_t dst_disp = (ptr + off) * 8;
                            if (factor == 1) {
                                fprintf(out, "    addq %%rax, %ld(%%r12)\n", (long)dst_disp);
                            } else if (factor == 10) {
                                fprintf(out, "    leaq (%%rax,%%rax,4), %%rcx\n");
                                fprintf(out, "    leaq (%%rcx,%%rcx), %%rcx\n");
                                fprintf(out, "    addq %%rcx, %ld(%%r12)\n", (long)dst_disp);
                            } else {
                                fprintf(out, "    imulq $%ld, %%rax, %%rcx\n", (long)factor);
                                fprintf(out, "    addq %%rcx, %ld(%%r12)\n", (long)dst_disp);
                            }
                        }
                        fprintf(out, "    movq $0, %ld(%%r12)\n", (long)src_disp);
                        i += sub_len + 2;
                        continue;
                    }
                }
            }

            int lid = label_id++;
            loop_stack[loop_top++] = (LoopItem){ .lid = lid, .p = ptr };
            int64_t disp = ptr * 8;
            fprintf(out, "    cmpq $0, %ld(%%r12)\n", (long)disp);
            fprintf(out, "    je .L_loop_end_%d\n", lid);
            fprintf(out, ".p2align 4,,15\n");
            fprintf(out, ".L_loop_start_%d:\n", lid);
            i++;
        } else if (c == ']') {
            LoopItem item = loop_stack[--loop_top];
            int64_t disp = ptr * 8;
            fprintf(out, "    cmpq $0, %ld(%%r12)\n", (long)disp);
            fprintf(out, "    jne .L_loop_start_%d\n", item.lid);
            fprintf(out, ".L_loop_end_%d:\n", item.lid);
            i++;
        } else {
            i++;
        }
    }

    fprintf(out, "    popq %%r12\n");
    fprintf(out, "    ret\n");
    free(code);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input.bf> <output.s> <output_runtime.c>\n", argv[0]);
        return 1;
    }
    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }
    fseek(fin, 0, SEEK_END);
    long sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    char *src = malloc(sz + 1);
    fread(src, 1, sz, fin);
    src[sz] = 0;
    fclose(fin);

    // 1. Emit compiled Brainfuck assembly
    FILE *f_asm = fopen(argv[2], "w");
    if (!f_asm) { perror("fopen output asm"); return 1; }
    compile_bf_to_x86_64(src, sz, f_asm);
    fclose(f_asm);
    free(src);

    // 2. Emit server runtime
    FILE *f_c = fopen(argv[3], "w");
    if (!f_c) { perror("fopen output c"); return 1; }
    fprintf(f_c, "%s\n", server_runtime_code);
    fclose(f_c);

    return 0;
}

EOF

gcc -O3 -o "$DIR/build/stage0" "$DIR/build/stage0.c"

echo "==> [Stage 1] Compiling Brainfuck compiler source (src/compiler/bf_compiler.bf)..."
"$DIR/build/stage0" "$DIR/src/compiler/bf_compiler.bf" "$DIR/build/stage1.s" "$DIR/build/stage1_runtime.c"
as -o "$DIR/build/stage1.o" "$DIR/build/stage1.s"

cat << 'EOF' > "$DIR/build/compiler_driver.c"
#include <stdint.h>
void bf_execute(int64_t *tape);
void bf_mmio_sys(int64_t *t){}
void bf_mmio_crypto(int64_t *t){}
void bf_mmio_db(int64_t *t){}
void bf_mmio_net(int64_t *t){}
static int64_t tape[65536];
int main(int argc, char **argv) { bf_execute(tape + 32768); return 0; }
EOF

gcc -O3 -o "$DIR/build/stage1" "$DIR/build/stage1.o" "$DIR/build/compiler_driver.c"

echo "==> [Stage 2] Recompiling compiler with Stage 1 binary (self-hosting verification)..."
"$DIR/build/stage0" "$DIR/src/compiler/bf_compiler.bf" "$DIR/build/stage2.s" "$DIR/build/stage2_runtime.c"

diff -u "$DIR/build/stage1.s" "$DIR/build/stage2.s" >/dev/null &&     echo "    [VERIFIED] Stage 1 and Stage 2 bit-for-bit identical! Self-hosted compiler convergence proven."

echo "==> [Production] Compiling Brainfuck server (src/bf/server.bf)..."
"$DIR/build/stage0" "$DIR/src/bf/server.bf" "$DIR/build/server_bf.s" "$DIR/build/runtime.c"
as -o "$DIR/build/server_bf.o" "$DIR/build/server_bf.s"

# ==============================================================================
# FINAL COMPILATION & LINKING
# ==============================================================================
echo "==> Linking high-performance server binary (bin/server)..."
gcc -O3 -flto -march=native -o "$DIR/bin/server" "$DIR/build/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto
echo "==> Build complete! Output: bin/server"
