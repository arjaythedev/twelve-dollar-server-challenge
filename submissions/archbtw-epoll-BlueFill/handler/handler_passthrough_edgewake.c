#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>

typedef struct {
    char *method;
    char *path;
    char *body;
    char *headers;
} request_context_t;

typedef struct {
    int status;
    char *content_type;
    char *body;
} response_context_t;

extern void server_create_request_context(request_context_t *ctx, char *data);
extern void server_free_request_context(request_context_t *ctx);
extern void server_set_request_input(const char *data, size_t length);
extern void server_generate_response(response_context_t *resp);
extern void server_append_input(const char *data, size_t length);
extern void server_set_query_handler(void (*handler)(const char *sql, size_t length));

/* Persistent HTTP/1.1 handler for CompilerSparseTape-generated server.c. */
static sqlite3 *handler_db = NULL;
static sqlite3_int64 handler_user_id = 0;
static const char *handler_username = "";
static const char *handler_body = "";
static sqlite3_int64 handler_post_id = 0;
static sqlite3_int64 handler_uptime = 0;
static char *handler_owned_username = NULL;
static sqlite3_stmt *jwt_header_statement = NULL;
static sqlite3_stmt *jwt_payload_statement = NULL;
static sqlite3_stmt *body_validation_statement = NULL;
static bool debug = false;

static const unsigned int batch_max_requests = 64U;
static const int batch_max_age_ms = 20;

static bool current_request_wrote = false;
static bool batch_transaction_open = false;
static bool batch_failed = false;
static unsigned int batch_write_requests = 0U;
static int64_t batch_started_ms = 0;
static int event_loop_epoll_fd = -1;

/* Queries marked with S@ already contain a complete response body. The handler
 * frames the body directly instead of feeding it through the BF echo loop. */
static char *passthrough_storage = NULL;
static const char *passthrough_body = NULL;
static size_t passthrough_length = 0U;
static int passthrough_status = 200;

#define WAL_CHECKPOINT_FRAMES 4000
static int wal_frames = 0;
static int64_t last_checkpoint_ms = 0;

#define QUERY_CACHE_ENTRIES 32U
#define QUERY_CACHE_MAX_SQL_LENGTH 4096U
#define QUERY_CACHE_MAX_SQL_BYTES (64U * 1024U)

typedef struct {
    char *sql;
    size_t length;
    size_t storage;
    sqlite3_stmt *statement;
    uint64_t last_used;
} query_cache_entry_t;

static query_cache_entry_t query_cache[QUERY_CACHE_ENTRIES];
static size_t query_cache_bytes = 0U;
static uint64_t query_cache_clock = 0U;

static void handler_query_cache_evict(size_t index) {
    query_cache_entry_t *entry = &query_cache[index];
    sqlite3_finalize(entry->statement);
    free(entry->sql);
    if (entry->storage <= query_cache_bytes) query_cache_bytes -= entry->storage;
    memset(entry, 0, sizeof(*entry));
}

static void handler_query_cache_clear(void) {
    for (size_t i = 0; i < QUERY_CACHE_ENTRIES; i++) {
        handler_query_cache_evict(i);
    }
    query_cache_bytes = 0U;
    query_cache_clock = 0U;
}

static sqlite3_stmt *handler_query_cache_find(const char *sql, size_t length) {
    if (length > QUERY_CACHE_MAX_SQL_LENGTH) return NULL;
    for (size_t i = 0; i < QUERY_CACHE_ENTRIES; i++) {
        query_cache_entry_t *entry = &query_cache[i];
        if (entry->statement != NULL && entry->length == length
                && memcmp(entry->sql, sql, length) == 0) {
            entry->last_used = ++query_cache_clock;
            return entry->statement;
        }
    }
    return NULL;
}

/* Takes ownership of statement only when it returns 1. */
static int handler_query_cache_store(const char *sql, size_t length, sqlite3_stmt *statement) {
    if (statement == NULL || length > QUERY_CACHE_MAX_SQL_LENGTH
            || length + 1U > QUERY_CACHE_MAX_SQL_BYTES) return 0;

    size_t storage = length + 1U;
    while (query_cache_bytes > QUERY_CACHE_MAX_SQL_BYTES - storage) {
        size_t oldest = QUERY_CACHE_ENTRIES;
        uint64_t oldest_use = UINT64_MAX;
        for (size_t i = 0; i < QUERY_CACHE_ENTRIES; i++) {
            if (query_cache[i].statement != NULL && query_cache[i].last_used < oldest_use) {
                oldest = i;
                oldest_use = query_cache[i].last_used;
            }
        }
        if (oldest == QUERY_CACHE_ENTRIES) return 0;
        handler_query_cache_evict(oldest);
    }

    size_t slot = QUERY_CACHE_ENTRIES;
    for (size_t i = 0; i < QUERY_CACHE_ENTRIES; i++) {
        if (query_cache[i].statement == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == QUERY_CACHE_ENTRIES) {
        uint64_t oldest_use = UINT64_MAX;
        for (size_t i = 0; i < QUERY_CACHE_ENTRIES; i++) {
            if (query_cache[i].last_used < oldest_use) {
                slot = i;
                oldest_use = query_cache[i].last_used;
            }
        }
        handler_query_cache_evict(slot);
    }

    char *copy = malloc(storage);
    if (copy == NULL) return 0;
    memcpy(copy, sql, length);
    copy[length] = '\0';

    query_cache_entry_t *entry = &query_cache[slot];
    entry->sql = copy;
    entry->length = length;
    entry->storage = storage;
    entry->statement = statement;
    entry->last_used = ++query_cache_clock;
    query_cache_bytes += storage;
    return 1;
}

static void handler_release_query_statement(sqlite3_stmt *statement, int cached) {
    if (statement == NULL) return;
    (void) sqlite3_reset(statement);
    (void) sqlite3_clear_bindings(statement);
    if (!cached) sqlite3_finalize(statement);
}

#define MAX_REQUEST_SIZE (64U * 1024U)
#define INITIAL_REQUEST_CAPACITY 1024U
#define INITIAL_OUTPUT_CAPACITY 2048U
#define OUTPUT_KEEP_CAPACITY 4096U

static void debug_trace(const char *prefix, const char *data, size_t length) {
    if (!debug) {
        return;
    }
    fputs(prefix, stderr);
    if (data != NULL && length > 0) {
        fwrite(data, 1, length, stderr);
    }
    if (data == NULL || length == 0 || data[length - 1] != '\n') {
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static void send_input_to_server(const char *data, size_t length) {
    debug_trace("TO  : ", data, length);
    server_append_input(data, length);
}

static void set_server_input(const char *data, size_t length) {
    debug_trace("TO  : ", data, length);
    server_set_request_input(data, length);
}

static char response_code(int status) {
    switch (status) {
        case 201: return '2';
        case 400: return '3';
        case 401: return '4';
        case 404: return '5';
        case 500: return '6';
        case 503: return '7';
        default: return '1';
    }
}

static int status_from_code(char code) {
    switch (code) {
        case '2': return 201;
        case '3': return 400;
        case '4': return 401;
        case '5': return 404;
        case '6': return 500;
        case '7': return 503;
        default: return 200;
    }
}

static void debug_trace_response(int status, const char *content_type,
                                 const char *body, size_t length) {
    if (!debug) {
        return;
    }
    if (content_type != NULL && strcmp(content_type, "application/json") == 0) {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "FROM: R%c", response_code(status));
        debug_trace(prefix, body, length);
    } else {
        debug_trace("FROM: ", body, length);
    }
}

static void handler_append_error(const char *message) {
    (void) message;
    static const char fallback[] = "6{\"error\":\"internal server error\"}\n";
    send_input_to_server(fallback, sizeof(fallback) - 1);
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} output_buffer_t;

static int output_buffer_reserve(output_buffer_t *buffer, size_t additional) {
    if (additional > SIZE_MAX - buffer->length) {
        return 0;
    }
    size_t required = buffer->length + additional;
    if (required <= buffer->capacity) {
        return 1;
    }

    size_t capacity = buffer->capacity != 0 ? buffer->capacity : 256U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }

    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int output_buffer_append(output_buffer_t *buffer, const void *data, size_t length) {
    if (!output_buffer_reserve(buffer, length)) {
        return 0;
    }
    if (length != 0) {
        memcpy(buffer->data + buffer->length, data, length);
        buffer->length += length;
    }
    return 1;
}

static int output_buffer_char(output_buffer_t *buffer, char value) {
    return output_buffer_append(buffer, &value, 1U);
}

static int output_buffer_int64(output_buffer_t *buffer, sqlite3_int64 value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%lld", (long long) value);
    if (length < 0 || (size_t) length >= sizeof(text)) return 0;
    return output_buffer_append(buffer, text, (size_t) length);
}

static int output_buffer_column_json(output_buffer_t *buffer,
                                     sqlite3_stmt *statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return output_buffer_append(buffer, "null", 4U);
    }
    const unsigned char *text = sqlite3_column_text(statement, column);
    int length = sqlite3_column_bytes(statement, column);
    return output_buffer_append(buffer, text, (size_t) length);
}

/* Feed rows already carry JSON-quoted strings, so only framing and integer
 * formatting remain. This avoids building a nested json_object tree for every
 * request while keeping SQLite's exact escaping rules. */
static int handler_build_feed(sqlite3_stmt *statement, output_buffer_t *result) {
    if (!output_buffer_append(result, "{\"posts\":[", 10U)) return SQLITE_NOMEM;

    int rows = 0;
    int rc;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        if (rows > 0 && !output_buffer_char(result, ',')) return SQLITE_NOMEM;
        if (!output_buffer_append(result, "{\"id\":", 6U)
                || !output_buffer_int64(result, sqlite3_column_int64(statement, 0))
                || !output_buffer_append(result, ",\"body\":", 8U)
                || !output_buffer_column_json(result, statement, 1)
                || !output_buffer_append(result, ",\"created_at\":", 14U)
                || !output_buffer_column_json(result, statement, 2)
                || !output_buffer_append(result, ",\"author\":", 10U)
                || !output_buffer_column_json(result, statement, 3)
                || !output_buffer_append(result, ",\"like_count\":", 14U)
                || !output_buffer_int64(result, sqlite3_column_int64(statement, 4))
                || !output_buffer_char(result, '}')) {
            return SQLITE_NOMEM;
        }
        rows++;
    }
    if (rc != SQLITE_DONE) return rc;
    return output_buffer_append(result, "]}", 2U) ? SQLITE_OK : SQLITE_NOMEM;
}

static int handler_bind_parameters(sqlite3_stmt *statement) {
    int count = sqlite3_bind_parameter_count(statement);
    int rc = SQLITE_OK;
#define BIND_IF_PRESENT(index, expression) \
    do { \
        if (count >= (index)) { \
            rc = (expression); \
            if (rc != SQLITE_OK) return rc; \
        } \
    } while (0)
    BIND_IF_PRESENT(1, sqlite3_bind_int64(statement, 1, handler_user_id));
    BIND_IF_PRESENT(2, sqlite3_bind_text(statement, 2, handler_username, -1, SQLITE_STATIC));
    BIND_IF_PRESENT(3, sqlite3_bind_text(statement, 3, handler_body, -1, SQLITE_STATIC));
    BIND_IF_PRESENT(4, sqlite3_bind_int64(statement, 4, handler_post_id));
    BIND_IF_PRESENT(5, sqlite3_bind_int64(statement, 5, handler_uptime));
#undef BIND_IF_PRESENT
    return SQLITE_OK;
}

static int64_t current_time_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + (int64_t) (now.tv_nsec / 1000000L);
}

static int handler_wal_hook(void *arg, sqlite3 *db, const char *name, int frames) {
    (void) arg;
    (void) db;
    (void) name;
    wal_frames = frames;
    return SQLITE_OK;
}

static void checkpoint_maybe(void) {
    if (handler_db == NULL || wal_frames < WAL_CHECKPOINT_FRAMES) return;
    int64_t now = current_time_ms();
    if (now - last_checkpoint_ms < 1000) return;
    last_checkpoint_ms = now;
    wal_frames = 0;
    (void) sqlite3_wal_checkpoint_v2(handler_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}

static void flush_batch(void);

static int begin_batch_transaction(void) {
    int rc = sqlite3_exec(handler_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        handler_append_error(sqlite3_errmsg(handler_db));
        return 0;
    }
    batch_transaction_open = true;
    batch_failed = false;
    batch_write_requests = 0U;
    batch_started_ms = current_time_ms();
    return 1;
}

static void handler_execute_query(const char *sql, size_t length) {
    int passthrough = 0;
    int structured = 0;
    if (length > 0U && (sql[0] == '@' || sql[0] == '#')) {
        passthrough = 1;
        structured = sql[0] == '#';
        sql++;
        length--;
    }
    debug_trace(structured ? "FROM: S#" : passthrough ? "FROM: S@" : "FROM: S", sql, length);
    if (handler_db == NULL || sql == NULL || length > (size_t) INT_MAX) {
        handler_append_error("database unavailable or query too large");
        return;
    }

    int cached = 0;
    sqlite3_stmt *statement = handler_query_cache_find(sql, length);
    if (statement != NULL) {
        cached = 1;
    } else {
        int rc = sqlite3_prepare_v2(handler_db, sql, (int) length, &statement, NULL);
        if (rc != SQLITE_OK) {
            handler_append_error(sqlite3_errmsg(handler_db));
            return;
        }
        if (statement == NULL) return;
        cached = handler_query_cache_store(sql, length, statement);
    }

    int readonly = sqlite3_stmt_readonly(statement);
    if (readonly && batch_transaction_open) {
        /* Never serve a read from an uncommitted group-commit transaction. */
        flush_batch();
    }
    if (!readonly) {
        if (!batch_transaction_open && !begin_batch_transaction()) {
            handler_release_query_statement(statement, cached);
            return;
        }
        current_request_wrote = true;
    }

    (void) sqlite3_reset(statement);
    (void) sqlite3_clear_bindings(statement);
    int rc = handler_bind_parameters(statement);
    if (rc != SQLITE_OK) {
        handler_append_error(sqlite3_errmsg(handler_db));
        handler_release_query_statement(statement, cached);
        return;
    }

    if (structured) {
        output_buffer_t result = {0};
        int feed_rc = handler_build_feed(statement, &result);
        if (feed_rc != SQLITE_OK) {
            handler_append_error(feed_rc == SQLITE_NOMEM
                    ? "out of memory" : sqlite3_errmsg(handler_db));
        } else if (!output_buffer_reserve(&result, 1U)) {
            handler_append_error("out of memory");
        } else {
            result.data[result.length] = '\0';
            passthrough_storage = result.data;
            passthrough_body = result.data;
            passthrough_length = result.length;
            passthrough_status = 200;
            result.data = NULL;
            send_input_to_server("\n", 1U);
        }
        free(result.data);
        handler_release_query_statement(statement, cached);
        return;
    }

    int columns = sqlite3_column_count(statement);
    if (columns == 0) {
        rc = sqlite3_step(statement);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            if (!readonly) batch_failed = true;
            handler_append_error(sqlite3_errmsg(handler_db));
        }
        handler_release_query_statement(statement, cached);
        return;
    }

    output_buffer_t result = {0};
    int rows = 0;
    int failed = 0;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        for (int column = 0; column < columns; column++) {
            if (column > 0 && !output_buffer_char(&result, '\t')) {
                failed = 1;
                break;
            }
            const unsigned char *text = sqlite3_column_text(statement, column);
            int text_length = sqlite3_column_bytes(statement, column);
            if (text == NULL) {
                if (!output_buffer_append(&result, "null", 4U)) {
                    failed = 1;
                    break;
                }
            } else if (text_length > 0
                    && !output_buffer_append(&result, text, (size_t) text_length)) {
                failed = 1;
                break;
            }
        }
        if (failed || !output_buffer_char(&result, '\n')) {
            failed = 1;
            break;
        }
        rows++;
    }
    if (!failed && rc != SQLITE_DONE) {
        failed = 1;
    }

    if (failed) {
        if (!readonly) batch_failed = true;
        handler_append_error(rc == SQLITE_DONE ? "out of memory" : sqlite3_errmsg(handler_db));
    } else if (passthrough) {
        if (rows == 0 || result.data == NULL) {
            handler_append_error("empty response");
        } else {
            if (result.length > 0U && result.data[result.length - 1U] == '\n') {
                result.length--;
            }
            if (!output_buffer_reserve(&result, 1U)) {
                handler_append_error("out of memory");
            } else {
                result.data[result.length] = '\0';
                const char *body = result.data;
                size_t body_length = result.length;
                int status = 200;
                if (body_length >= 2U && body[0] >= '1' && body[0] <= '7') {
                    status = status_from_code(body[0]);
                    body++;
                    body_length--;
                }
                if (body_length == 0U) {
                    handler_append_error("empty response");
                } else {
                    passthrough_storage = result.data;
                    passthrough_body = body;
                    passthrough_length = body_length;
                    passthrough_status = status;
                    result.data = NULL;
                    /* The BF echo loop terminates immediately; the body is framed in C. */
                    send_input_to_server("\n", 1U);
                }
            }
        }
    } else if (rows == 0 && !output_buffer_char(&result, '\n')) {
        handler_append_error("out of memory");
    } else if (result.data != NULL) {
        send_input_to_server(result.data, result.length);
    } else {
        /* Empty result with a zero-column allocation still has a defined response. */
        send_input_to_server("\n", 1U);
    }

    free(result.data);
    handler_release_query_statement(statement, cached);
}

static const char *handler_find_header(const char *headers, const char *name, size_t *value_length) {
    if (headers == NULL) {
        return NULL;
    }
    size_t name_length = strlen(name);
    const char *line = headers;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t line_length = end == NULL ? strlen(line) : (size_t) (end - line);
        if (line_length > 0 && line[line_length - 1] == '\r') {
            line_length--;
        }
        if (line_length > name_length && strncasecmp(line, name, name_length) == 0
                && line[name_length] == ':') {
            const char *value = line + name_length + 1;
            const char *value_end = line + line_length;
            while (value < value_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
                value_end--;
            }
            *value_length = (size_t) (value_end - value);
            return value;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return NULL;
}

static int base64url_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int base64url_decode_into(const char *text, size_t length,
                                 unsigned char *output, size_t output_capacity,
                                 size_t *out_length) {
    if (text == NULL || output == NULL || out_length == NULL
            || length == SIZE_MAX || output_capacity < length + 1U) {
        return 0;
    }

    size_t data_length = length;
    size_t padding = 0U;
    while (data_length > 0U && text[data_length - 1U] == '=') {
        data_length--;
        padding++;
    }
    if (padding > 2U || data_length % 4U == 1U
            || (padding != 0U && (length % 4U != 0U
                || (padding == 1U && data_length % 4U != 3U)
                || (padding == 2U && data_length % 4U != 2U)))) {
        return 0;
    }

    size_t written = 0U;
    unsigned int accumulator = 0U;
    int bits = 0;
    for (size_t i = 0; i < data_length; i++) {
        int value = base64url_value((unsigned char) text[i]);
        if (value < 0) return 0;
        accumulator = (accumulator << 6) | (unsigned int) value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output[written++] = (unsigned char) ((accumulator >> bits) & 0xFFu);
        }
    }

    /* Reject non-canonical encodings with non-zero unused bits. */
    if (bits > 0 && (accumulator & ((1u << bits) - 1u)) != 0U) return 0;
    output[written] = '\0';
    *out_length = written;
    return 1;
}

static int prepare_cached_statements(sqlite3 *db) {
    if (db == NULL) {
        return 0;
    }
    int rc = sqlite3_prepare_v2(db,
            "SELECT json_type(?1, '$.alg'), json_extract(?1, '$.alg')",
            -1, &jwt_header_statement, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(db,
                "SELECT json_type(?1, '$.exp'), json_extract(?1, '$.exp'), "
                "json_type(?1, '$.sub'), json_extract(?1, '$.sub'), "
                "json_type(?1, '$.username'), json_extract(?1, '$.username')",
                -1, &jwt_payload_statement, NULL);
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(db,
                "SELECT json_valid(?1), "
                "CASE WHEN json_valid(?1) THEN json_type(?1, '$.body') END, "
                "CASE WHEN json_valid(?1) THEN json_extract(?1, '$.body') END",
                -1, &body_validation_statement, NULL);
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite statement preparation failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static void finalize_cached_statements(void) {
    handler_query_cache_clear();
    sqlite3_finalize(jwt_header_statement);
    sqlite3_finalize(jwt_payload_statement);
    sqlite3_finalize(body_validation_statement);
    jwt_header_statement = NULL;
    jwt_payload_statement = NULL;
    body_validation_statement = NULL;
}

static int bind_json(sqlite3_stmt *statement, const unsigned char *json, size_t length) {
    if (statement == NULL || json == NULL || length > (size_t) INT_MAX) {
        return SQLITE_MISUSE;
    }
    (void) sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return sqlite3_bind_text(statement, 1, (const char *) json, (int) length, SQLITE_TRANSIENT);
}

static int verify_jwt(const char *headers, int *auth_code,
                      sqlite3_int64 *user_id, char **username) {
    *auth_code = 2;
    *user_id = 0;
    *username = NULL;

    size_t header_length = 0;
    const char *value = handler_find_header(headers, "Authorization", &header_length);
    if (value == NULL || header_length < 7U || strncasecmp(value, "Bearer ", 7U) != 0) {
        return 2;
    }

    const char *token = value + 7;
    const char *token_end = value + header_length;
    while (token < token_end && (*token == ' ' || *token == '\t')) token++;
    while (token_end > token && (token_end[-1] == ' ' || token_end[-1] == '\t')) token_end--;
    if (token >= token_end) {
        *auth_code = 3;
        return 3;
    }

    const char *dot1 = memchr(token, '.', (size_t) (token_end - token));
    const char *dot2 = dot1 == NULL ? NULL
            : memchr(dot1 + 1, '.', (size_t) (token_end - dot1 - 1));
    if (dot1 == NULL || dot2 == NULL) {
        *auth_code = 3;
        return 3;
    }

    size_t header_b64_length = (size_t) (dot1 - token);
    size_t payload_b64_length = (size_t) (dot2 - dot1 - 1);
    size_t signature_b64_length = (size_t) (token_end - dot2 - 1);
    if (header_b64_length == 0U || payload_b64_length == 0U || signature_b64_length == 0U) {
        *auth_code = 3;
        return 3;
    }

    size_t decoded_header_length = 0U;
    size_t decoded_payload_length = 0U;
    size_t signature_length = 0U;
    if (header_b64_length > SIZE_MAX - payload_b64_length
            || header_b64_length + payload_b64_length > SIZE_MAX - signature_b64_length - 3U) {
        *auth_code = 3;
        return 3;
    }
    size_t decoded_capacity = header_b64_length + payload_b64_length
            + signature_b64_length + 3U;
    unsigned char *decoded = malloc(decoded_capacity);
    if (decoded == NULL) {
        *auth_code = 3;
        return 3;
    }
    unsigned char *decoded_header = decoded;
    unsigned char *decoded_payload = decoded_header + header_b64_length + 1U;
    unsigned char *decoded_signature = decoded_payload + payload_b64_length + 1U;
    if (!base64url_decode_into(token, header_b64_length, decoded_header,
                               header_b64_length + 1U, &decoded_header_length)
            || !base64url_decode_into(dot1 + 1, payload_b64_length, decoded_payload,
                                      payload_b64_length + 1U, &decoded_payload_length)
            || !base64url_decode_into(dot2 + 1, signature_b64_length, decoded_signature,
                                      signature_b64_length + 1U, &signature_length)) {
        free(decoded);
        *auth_code = 3;
        return 3;
    }

    if (handler_db == NULL || jwt_header_statement == NULL || jwt_payload_statement == NULL
            || bind_json(jwt_header_statement, decoded_header, decoded_header_length) != SQLITE_OK) {
        free(decoded);
        *auth_code = 3;
        return 3;
    }

    int rc = sqlite3_step(jwt_header_statement);
    int header_ok = 0;
    if (rc == SQLITE_ROW) {
        const unsigned char *type = sqlite3_column_text(jwt_header_statement, 0);
        const unsigned char *algorithm = sqlite3_column_text(jwt_header_statement, 1);
        header_ok = type != NULL && algorithm != NULL
                && strcmp((const char *) type, "text") == 0
                && strcmp((const char *) algorithm, "HS256") == 0;
    }
    (void) sqlite3_reset(jwt_header_statement);
    if (!header_ok) {
        free(decoded);
        *auth_code = 3;
        return 3;
    }
    /* A hard-coded fallback secret makes forged tokens possible when deployment is misconfigured. */
    const char *secret = getenv("JWT_SECRET");
    if (secret == NULL || secret[0] == '\0') {
        free(decoded);
        *auth_code = 3;
        return 3;
    }
    size_t secret_length = strlen(secret);
    if (secret_length > (size_t) INT_MAX || signature_length != 32U) {
        free(decoded);
        *auth_code = 3;
        return 3;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    unsigned char *hmac_result = HMAC(EVP_sha256(), secret, (int) secret_length,
            (const unsigned char *) token, (size_t) (dot2 - token), digest, &digest_length);
    int signature_ok = hmac_result != NULL && digest_length == 32U
            && CRYPTO_memcmp(decoded_signature, digest, 32U) == 0;
    if (!signature_ok) {
        free(decoded);
        *auth_code = 3;
        return 3;
    }

    if (bind_json(jwt_payload_statement, decoded_payload, decoded_payload_length) != SQLITE_OK) {
        free(decoded);
        *auth_code = 3;
        return 3;
    }
    free(decoded);

    rc = sqlite3_step(jwt_payload_statement);
    sqlite3_int64 parsed_id = 0;
    char *name_copy = NULL;
    int claim_status = 3; /* bad/missing exp is an invalid or expired token */
    if (rc == SQLITE_ROW) {
        const unsigned char *exp_type = sqlite3_column_text(jwt_payload_statement, 0);
        double expiration = sqlite3_column_double(jwt_payload_statement, 1);
        const unsigned char *sub_type = sqlite3_column_text(jwt_payload_statement, 2);
        const unsigned char *sub_text = sqlite3_column_text(jwt_payload_statement, 3);
        int sub_length = sqlite3_column_bytes(jwt_payload_statement, 3);
        const unsigned char *name_type = sqlite3_column_text(jwt_payload_statement, 4);
        const unsigned char *name_text = sqlite3_column_text(jwt_payload_statement, 5);
        int name_length = sqlite3_column_bytes(jwt_payload_statement, 5);

        int exp_numeric = exp_type != NULL
                && (strcmp((const char *) exp_type, "integer") == 0
                    || strcmp((const char *) exp_type, "real") == 0);
        if (exp_numeric && isfinite(expiration) && expiration > (double) time(NULL)) {
            int id_ok = sub_type != NULL
                    && strcmp((const char *) sub_type, "text") == 0
                    && sub_text != NULL && sub_length > 0;
            if (id_ok) {
                sqlite3_int64 id = 0;
                for (int i = 0; i < sub_length; i++) {
                    unsigned char c = sub_text[i];
                    if (c < '0' || c > '9') {
                        id_ok = 0;
                        break;
                    }
                    unsigned int digit = (unsigned int) (c - '0');
                    if (id > (INT64_MAX - (sqlite3_int64) digit) / 10) {
                        id_ok = 0;
                        break;
                    }
                    id = id * 10 + (sqlite3_int64) digit;
                }
                if (id <= 0 || (size_t) sub_length != strlen((const char *) sub_text)) {
                    id_ok = 0;
                } else {
                    parsed_id = id;
                }
            }

            int name_ok = name_type != NULL
                    && strcmp((const char *) name_type, "text") == 0
                    && name_text != NULL && name_length >= 0
                    && (size_t) name_length == strlen((const char *) name_text);
            if (!id_ok || !name_ok) {
                claim_status = 4;
            } else {
                name_copy = strndup((const char *) name_text, (size_t) name_length);
                claim_status = name_copy != NULL ? 1 : 4;
            }
        }
    }
    (void) sqlite3_reset(jwt_payload_statement);

    if (claim_status != 1) {
        free(name_copy);
        *auth_code = claim_status;
        return claim_status;
    }

    *auth_code = 1;
    *user_id = parsed_id;
    *username = name_copy;
    return 1;
}

static char *trim_copy(const char *text) {
    while (*text != '\0' && isspace((unsigned char) *text)) {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char) text[length - 1])) {
        length--;
    }
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static size_t utf8_length(const char *text) {
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *) text; *p != '\0'; p++) {
        if ((*p & 0xC0u) != 0x80u) {
            length++;
        }
    }
    return length;
}

static int validate_body(const char *raw_body, char **trimmed_body) {
    *trimmed_body = NULL;
    if (raw_body == NULL || handler_db == NULL || body_validation_statement == NULL) {
        return 1;
    }

    (void) sqlite3_reset(body_validation_statement);
    sqlite3_clear_bindings(body_validation_statement);
    int rc = sqlite3_bind_text(body_validation_statement, 1, raw_body, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        (void) sqlite3_reset(body_validation_statement);
        return 1;
    }
    rc = sqlite3_step(body_validation_statement);
    if (rc != SQLITE_ROW) {
        (void) sqlite3_reset(body_validation_statement);
        return 1;
    }

    int valid = sqlite3_column_int(body_validation_statement, 0);
    const unsigned char *type = sqlite3_column_text(body_validation_statement, 1);
    const unsigned char *value = sqlite3_column_text(body_validation_statement, 2);
    int result = 0;
    if (!valid) {
        result = 1;
    } else if (type == NULL || strcmp((const char *) type, "text") != 0 || value == NULL) {
        result = 2;
    } else {
        char *trimmed = trim_copy((const char *) value);
        if (trimmed == NULL) {
            result = 1;
        } else if (*trimmed == '\0') {
            free(trimmed);
            result = 2;
        } else if (utf8_length(trimmed) > 500U) {
            free(trimmed);
            result = 3;
        } else {
            *trimmed_body = trimmed;
        }
    }

    (void) sqlite3_reset(body_validation_statement);
    return result;
}

static int parse_post_id(const char *text, size_t length, sqlite3_int64 *post_id) {
    if (text == NULL || length == 0U || post_id == NULL) {
        return 0;
    }
    sqlite3_int64 value = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) text[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        unsigned int digit = (unsigned int) (c - '0');
        if (value > (INT64_MAX - (sqlite3_int64) digit) / 10) {
            return 0;
        }
        value = value * 10 + (sqlite3_int64) digit;
    }
    if (value <= 0) {
        return 0;
    }
    *post_id = value;
    return 1;
}

static void create_context(request_context_t *request, int *route, int *auth_code,
                           int *body_code, int *id_code, char **trimmed_body) {
    const char *method = request->method == NULL ? "" : request->method;
    const char *path = request->path == NULL ? "" : request->path;

    *route = 'N';
    *auth_code = 0;
    *body_code = 0;
    *id_code = 0;
    *trimmed_body = NULL;
    handler_user_id = 0;
    handler_username = "";
    handler_body = "";
    handler_post_id = 0;
    handler_owned_username = NULL;

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        *route = 'H';
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/feed") == 0) {
        *route = 'F';
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/posts") == 0) {
        *route = 'C';
    } else if (strncmp(path, "/posts/", 7) == 0) {
        const char *rest = path + 7;
        const char *slash = strchr(rest, '/');
        if (slash == NULL && strcmp(method, "GET") == 0) {
            *route = 'G';
            if (!parse_post_id(rest, strlen(rest), &handler_post_id)) {
                *id_code = 1;
            }
        } else if (slash != NULL && strcmp(slash, "/like") == 0 && strcmp(method, "POST") == 0) {
            *route = 'L';
            if (!parse_post_id(rest, (size_t) (slash - rest), &handler_post_id)) {
                *id_code = 1;
            }
        }
    }

    if (*route == 'N') {
        return;
    }

    if (*route == 'C' || *route == 'L') {
        char *username = NULL;
        verify_jwt(request->headers, auth_code, &handler_user_id, &username);
        if (*auth_code == 1 && username != NULL) {
            handler_username = username;
            handler_owned_username = username;
        }
    }

    if (*route == 'C' && *auth_code == 1) {
        char *trimmed = NULL;
        *body_code = validate_body(request->body, &trimmed);
        if (*body_code == 0) {
            *trimmed_body = trimmed;
            handler_body = trimmed;
        }
    }
}

enum {
    REQUEST_READ_IO = -3,
    REQUEST_READ_TOO_LARGE = -2,
    REQUEST_READ_BAD = -1,
    REQUEST_READ_EOF = 0,
    REQUEST_READ_OK = 1
};

static int parse_expected_request_length(const char *buffer, size_t header_length,
                                        size_t *expected_length) {
    const char *headers_end = buffer + header_length - 4U;
    const char *line = strstr(buffer, "\r\n");
    if (line == NULL || line == buffer || line > headers_end) {
        return REQUEST_READ_BAD;
    }
    line += 2;
    size_t content_length = 0;
    int content_length_seen = 0;

    while (line < headers_end) {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL || line_end > headers_end) {
            return REQUEST_READ_BAD;
        }
        size_t line_length = (size_t) (line_end - line);
        if (line_length == 0U) {
            break;
        }

        const size_t cl_name_length = sizeof("Content-Length") - 1U;
        const size_t te_name_length = sizeof("Transfer-Encoding") - 1U;
        if (line_length >= cl_name_length
                && strncasecmp(line, "Content-Length", cl_name_length) == 0) {
            if (line_length <= cl_name_length || line[cl_name_length] != ':'
                    || content_length_seen) {
                return REQUEST_READ_BAD;
            }
            content_length_seen = 1;
            const char *number = line + cl_name_length + 1U;
            const char *number_end = line_end;
            while (number < number_end && (*number == ' ' || *number == '\t')) number++;
            while (number_end > number && (number_end[-1] == ' ' || number_end[-1] == '\t')) number_end--;
            if (number == number_end) {
                return REQUEST_READ_BAD;
            }
            size_t parsed = 0;
            for (const char *p = number; p < number_end; p++) {
                if (*p < '0' || *p > '9') {
                    return REQUEST_READ_BAD;
                }
                unsigned int digit = (unsigned int) (*p - '0');
                if (parsed > (MAX_REQUEST_SIZE - digit) / 10U) {
                    return REQUEST_READ_TOO_LARGE;
                }
                parsed = parsed * 10U + digit;
            }
            content_length = parsed;
        } else if (line_length >= te_name_length
                && strncasecmp(line, "Transfer-Encoding", te_name_length) == 0) {
            /* Chunked framing is not implemented by the existing backend parser. */
            return REQUEST_READ_BAD;
        }
        line = line_end + 2;
    }

    if (header_length > MAX_REQUEST_SIZE || content_length > MAX_REQUEST_SIZE - header_length) {
        return REQUEST_READ_TOO_LARGE;
    }
    *expected_length = header_length + content_length;
    return REQUEST_READ_OK;
}

static const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

void handler_process(request_context_t *req, response_context_t *resp, sqlite3 *db) {
    (void) req;
    (void) resp;
    (void) db;
}

typedef struct client_connection {
    int fd;
    char *buffer;
    size_t capacity;
    size_t used;
    size_t expected_length;
    size_t header_length;
    int headers_parsed;
    char *out;
    size_t out_capacity;
    size_t out_length;
    size_t out_offset;
    size_t pending_responses;
    int close_pending;
    int epoll_registered;
    int in_lru;
    int in_closing;
    uint64_t last_active_ms;
    struct client_connection *lru_prev;
    struct client_connection *lru_next;
    struct client_connection *closing_next;
} client_connection_t;

typedef struct pending_response {
    struct pending_response *next;
    client_connection_t *client;
    int status;
    char *content_type;
    char *body;
    size_t body_length;
    int keep_alive;
} pending_response_t;

static pending_response_t *pending_responses_head = NULL;
static pending_response_t *pending_responses_tail = NULL;

static client_connection_t *lru_head = NULL;
static client_connection_t *lru_tail = NULL;
static client_connection_t *closing_head = NULL;
static size_t client_count = 0U;
static size_t lru_count = 0U;
static size_t client_limit = 60000U;
static size_t fd_high_water = 57000U;
static size_t fd_low_water = 50000U;
static int listener_paused = 0;
static int listener_fd = -1;
static struct epoll_event listener_event;

static void client_update_epoll(client_connection_t *client);
static void cleanup_closing_clients(void);
static void shed_idle_clients(size_t maximum);
static void update_listener_state(void);

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000U + (uint64_t) (now.tv_nsec / 1000000L);
}

static void lru_remove(client_connection_t *client) {
    if (client == NULL || !client->in_lru) return;
    if (client->lru_prev != NULL) client->lru_prev->lru_next = client->lru_next;
    else lru_head = client->lru_next;
    if (client->lru_next != NULL) client->lru_next->lru_prev = client->lru_prev;
    else lru_tail = client->lru_prev;
    client->lru_prev = NULL;
    client->lru_next = NULL;
    client->in_lru = 0;
    if (lru_count > 0U) lru_count--;
}

static void lru_add_tail(client_connection_t *client) {
    client->lru_prev = lru_tail;
    client->lru_next = NULL;
    if (lru_tail != NULL) lru_tail->lru_next = client;
    else lru_head = client;
    lru_tail = client;
    client->in_lru = 1;
    client->last_active_ms = monotonic_ms();
    lru_count++;
}

static void lru_touch(client_connection_t *client) {
    if (client == NULL || !client->in_lru) return;
    client->last_active_ms = monotonic_ms();
    if (lru_tail == client) return;
    lru_remove(client);
    lru_add_tail(client);
}

static void mark_client_closing(client_connection_t *client) {
    if (client == NULL || client->close_pending) return;
    client->close_pending = 1;
    lru_remove(client);
    if (!client->in_closing) {
        client->closing_next = closing_head;
        closing_head = client;
        client->in_closing = 1;
    }
    client_update_epoll(client);
}

static void client_update_epoll(client_connection_t *client) {
    if (event_loop_epoll_fd < 0) return;
    uint32_t events = 0U;
    if (!client->close_pending) events |= EPOLLIN | EPOLLRDHUP;
    if (client->out_length > client->out_offset) events |= EPOLLOUT;

    if (events == 0U) {
        if (client->epoll_registered) {
            (void) epoll_ctl(event_loop_epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
            client->epoll_registered = 0;
        }
        return;
    }

    /* Read and write handlers drain until EAGAIN, so edge-triggered events are safe. */
    events |= EPOLLET;
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.ptr = client;
    if (client->epoll_registered) {
        (void) epoll_ctl(event_loop_epoll_fd, EPOLL_CTL_MOD, client->fd, &event);
    } else if (epoll_ctl(event_loop_epoll_fd, EPOLL_CTL_ADD, client->fd, &event) == 0) {
        client->epoll_registered = 1;
    }
}

#define MAX_CLIENT_OUTPUT (1024U * 1024U)

static void client_discard_output(client_connection_t *client) {
    client->out_offset = 0U;
    client->out_length = 0U;
    if (client->out_capacity > OUTPUT_KEEP_CAPACITY) {
        free(client->out);
        client->out = NULL;
        client->out_capacity = 0U;
    }
    client_update_epoll(client);
}

static int client_out_reserve(client_connection_t *client, size_t additional) {
    if (additional > MAX_CLIENT_OUTPUT - client->out_length) return 0;
    size_t required = client->out_length + additional;
    if (required <= client->out_capacity) return 1;

    size_t capacity = client->out_capacity != 0U
            ? client->out_capacity : INITIAL_OUTPUT_CAPACITY;
    while (capacity < required) {
        if (capacity > MAX_CLIENT_OUTPUT / 2U) {
            capacity = MAX_CLIENT_OUTPUT;
            break;
        }
        capacity *= 2U;
    }
    char *next = realloc(client->out, capacity);
    if (next == NULL) return 0;
    client->out = next;
    client->out_capacity = capacity;
    return 1;
}

static int client_out_append(client_connection_t *client,
                             const void *data, size_t length) {
    if (client->out_offset == client->out_length) {
        client->out_offset = 0U;
        client->out_length = 0U;
        if (client->out_capacity > OUTPUT_KEEP_CAPACITY) {
            free(client->out);
            client->out = NULL;
            client->out_capacity = 0U;
        }
    }
    if (!client_out_reserve(client, length)) return 0;
    if (length != 0U) {
        memcpy(client->out + client->out_length, data, length);
        client->out_length += length;
    }
    return 1;
}

static void client_flush(client_connection_t *client) {
    while (client->out_offset < client->out_length) {
        ssize_t written = send(client->fd,
                               client->out + client->out_offset,
                               client->out_length - client->out_offset,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written > 0) {
            client->out_offset += (size_t) written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        client_discard_output(client);
        mark_client_closing(client);
        return;
    }

    if (client->out_offset == client->out_length) {
        client->out_offset = 0U;
        client->out_length = 0U;
        if (client->out_capacity > OUTPUT_KEEP_CAPACITY) {
            free(client->out);
            client->out = NULL;
            client->out_capacity = 0U;
        }
    }
    client_update_epoll(client);
}

static int client_frame_response(client_connection_t *client, int status,
                                 const char *content_type, const char *body,
                                 size_t body_length, int keep_alive) {
    if (content_type == NULL) content_type = "text/plain";
    if (body == NULL) {
        body = "";
        body_length = 0U;
    }

    char header[512];
    const char *connection_value = keep_alive ? "keep-alive" : "close";
    int header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n",
            status, status_text(status), content_type, body_length, connection_value);
    if (header_length < 0 || (size_t) header_length >= sizeof(header)) {
        static const char fallback_header[] =
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 21\r\n"
                "Connection: close\r\n\r\n";
        static const char fallback_body[] = "internal server error";
        keep_alive = 0;
        header_length = (int) (sizeof(fallback_header) - 1U);
        if (!client_out_append(client, fallback_header, (size_t) header_length)
                || !client_out_append(client, fallback_body, sizeof(fallback_body) - 1U)) {
            client_discard_output(client);
            mark_client_closing(client);
            return 0;
        }
    } else if (!client_out_append(client, header, (size_t) header_length)
            || !client_out_append(client, body, body_length)) {
        client_discard_output(client);
        mark_client_closing(client);
        return 0;
    }

    if (!keep_alive) mark_client_closing(client);
    client_flush(client);
    return 1;
}

static int enqueue_response(client_connection_t *client, int status,
                            const char *content_type, const char *body,
                            size_t body_length, int keep_alive) {
    pending_response_t *entry = calloc(1U, sizeof(*entry));
    if (entry == NULL) return 0;

    if (content_type == NULL) content_type = "text/plain";
    if (body == NULL) body = "";
    entry->content_type = strdup(content_type);
    entry->body = malloc(body_length + 1U);
    if (entry->content_type == NULL || entry->body == NULL) {
        free(entry->content_type);
        free(entry->body);
        free(entry);
        return 0;
    }
    memcpy(entry->body, body, body_length);
    entry->body[body_length] = '\0';
    entry->body_length = body_length;
    entry->client = client;
    entry->status = status;
    entry->keep_alive = keep_alive;

    if (pending_responses_tail != NULL) {
        pending_responses_tail->next = entry;
    } else {
        pending_responses_head = entry;
    }
    pending_responses_tail = entry;
    client->pending_responses++;
    return 1;
}

static void send_pending_responses(int transaction_failed) {
    pending_response_t *entry = pending_responses_head;
    pending_responses_head = NULL;
    pending_responses_tail = NULL;

    while (entry != NULL) {
        pending_response_t *next = entry->next;
        client_connection_t *client = entry->client;
        int status = transaction_failed ? 500 : entry->status;
        const char *content_type = transaction_failed
                ? "text/plain; charset=utf-8" : entry->content_type;
        const char *body = transaction_failed ? "transaction failed\n" : entry->body;
        size_t body_length = transaction_failed
                ? sizeof("transaction failed\n") - 1U : entry->body_length;

        int queue_rc = client_frame_response(client, status, content_type,
                                             body, body_length, entry->keep_alive);
        if (client->pending_responses > 0U) client->pending_responses--;
        if (queue_rc == 0) mark_client_closing(client);

        free(entry->content_type);
        free(entry->body);
        free(entry);
        entry = next;
    }
}

static void flush_batch(void) {
    if (!batch_transaction_open) {
        if (pending_responses_head != NULL) send_pending_responses(0);
        return;
    }

    int failed = batch_failed;
    if (failed) {
        (void) sqlite3_exec(handler_db, "ROLLBACK", NULL, NULL, NULL);
    } else if (sqlite3_exec(handler_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        failed = 1;
        (void) sqlite3_exec(handler_db, "ROLLBACK", NULL, NULL, NULL);
    }

    batch_transaction_open = false;
    batch_failed = false;
    batch_write_requests = 0U;
    batch_started_ms = 0;
    send_pending_responses(failed);
}

static int batch_timeout_ms(void) {
    if (!batch_transaction_open) return -1;
    int64_t elapsed = current_time_ms() - batch_started_ms;
    if (elapsed >= batch_max_age_ms) return 0;
    if (elapsed < 0) elapsed = 0;
    return (int) (batch_max_age_ms - elapsed);
}

static void send_or_queue_error(client_connection_t *client, int status,
                                const char *message) {
    size_t message_length = strlen(message);
    if (client->pending_responses == 0U) {
        (void) client_frame_response(client, status, "text/plain; charset=utf-8",
                                     message, message_length, 0);
        return;
    }
    if (!enqueue_response(client, status, "text/plain; charset=utf-8",
                          message, message_length, 0)) {
        mark_client_closing(client);
    }
}

static void cleanup_closing_clients(void) {
    client_connection_t **link = &closing_head;
    while (*link != NULL) {
        client_connection_t *client = *link;
        if (client->pending_responses == 0U && client->out_offset == client->out_length) {
            *link = client->closing_next;
            client->in_closing = 0;
            if (client->epoll_registered && event_loop_epoll_fd >= 0) {
                (void) epoll_ctl(event_loop_epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
            }
            close(client->fd);
            free(client->buffer);
            free(client->out);
            free(client);
            if (client_count > 0U) client_count--;
        } else {
            link = &client->closing_next;
        }
    }
}

static void shed_idle_clients(size_t maximum) {
    size_t closing = client_count - lru_count;
    size_t active_target = maximum > closing ? maximum - closing : 0U;
    while (lru_count > active_target && lru_head != NULL) {
        client_connection_t *victim = lru_head;
        if (victim->pending_responses != 0U
                || victim->out_offset != victim->out_length) {
            /* Busy clients are rare; do not scan the whole LRU under load. */
            break;
        }
        mark_client_closing(victim);
    }
}

static void update_listener_state(void) {
    if (event_loop_epoll_fd < 0 || listener_fd < 0) return;
    if (!listener_paused && client_count >= fd_high_water) {
        if (epoll_ctl(event_loop_epoll_fd, EPOLL_CTL_DEL, listener_fd, NULL) == 0) {
            listener_paused = 1;
        }
    } else if (listener_paused && client_count <= fd_low_water) {
        if (epoll_ctl(event_loop_epoll_fd, EPOLL_CTL_ADD, listener_fd, &listener_event) == 0) {
            listener_paused = 0;
        }
    }
}

static int has_header_token(const char *value, size_t length, const char *wanted) {
    size_t wanted_length = strlen(wanted);
    size_t i = 0U;
    while (i < length) {
        while (i < length && (value[i] == ',' || value[i] == ' ' || value[i] == '\t')) i++;
        size_t begin = i;
        while (i < length && value[i] != ',') i++;
        size_t end = i;
        while (end > begin && (value[end - 1U] == ' ' || value[end - 1U] == '\t')) end--;
        if (end - begin == wanted_length
                && strncasecmp(value + begin, wanted, wanted_length) == 0) return 1;
    }
    return 0;
}

static const char *find_request_header(const char *request, size_t request_length,
                                       const char *wanted, size_t *value_length) {
    const char *request_end = request + request_length;
    const char *line_end = strstr(request, "\r\n");
    if (line_end == NULL || line_end + 2 > request_end) return NULL;
    const char *line = line_end + 2;
    const char *headers_end = strstr(line, "\r\n\r\n");
    if (headers_end == NULL || headers_end > request_end) return NULL;
    size_t wanted_length = strlen(wanted);
    while (line < headers_end) {
        const char *end = strstr(line, "\r\n");
        if (end == NULL || end > headers_end) return NULL;
        size_t length = (size_t) (end - line);
        if (length > wanted_length && strncasecmp(line, wanted, wanted_length) == 0
                && line[wanted_length] == ':') {
            const char *value = line + wanted_length + 1U;
            const char *value_end = end;
            while (value < value_end && (*value == ' ' || *value == '\t')) value++;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) value_end--;
            *value_length = (size_t) (value_end - value);
            return value;
        }
        line = end + 2;
    }
    return NULL;
}

static int request_wants_keep_alive(const char *raw_request, size_t request_length) {
    const char *request_end = raw_request + request_length;
    const char *line_end = strstr(raw_request, "\r\n");
    if (line_end == NULL || line_end > request_end) return 0;
    int http_11 = line_end - raw_request >= 8
            && memcmp(line_end - 8, "HTTP/1.1", 8U) == 0;
    int http_10 = line_end - raw_request >= 8
            && memcmp(line_end - 8, "HTTP/1.0", 8U) == 0;
    size_t connection_length = 0U;
    const char *connection = find_request_header(raw_request, request_length,
                                                  "Connection", &connection_length);
    int closes = connection != NULL
            && has_header_token(connection, connection_length, "close");
    int explicitly_keeps = connection != NULL
            && has_header_token(connection, connection_length, "keep-alive");
    if (closes) return 0;
    if (http_11) return 1;
    return http_10 && explicitly_keeps;
}

/* Processes one fully framed request. The generated interpreter and handler state
 * are intentionally single-threaded, while epoll multiplexes thousands of idle sockets. */
static int process_one_request(client_connection_t *client, char *buffer,
                               size_t request_length, time_t started_at) {
    if (debug) {
        size_t request_line_length = strcspn(buffer, "\r\n");
        fprintf(stderr, "request \"%.*s\"\n", (int) request_line_length, buffer);
        fflush(stderr);
    }

    free(passthrough_storage);
    passthrough_storage = NULL;
    passthrough_body = NULL;
    passthrough_length = 0U;
    passthrough_status = 200;

    int keep_alive = request_wants_keep_alive(buffer, request_length);
    request_context_t req = {0};
    server_create_request_context(&req, buffer);

    int route = 'N';
    int auth_code = 0;
    int body_code = 0;
    int id_code = 0;
    char *trimmed_body = NULL;
    create_context(&req, &route, &auth_code, &body_code, &id_code, &trimmed_body);

    handler_uptime = (sqlite3_int64) (time(NULL) - started_at);
    char context[5] = {
            (char) route,
            (char) ('0' + auth_code),
            (char) ('0' + body_code),
            (char) ('0' + id_code),
            '\n'
    };
    set_server_input(context, sizeof(context));

    current_request_wrote = false;
    response_context_t resp = {500, "text/plain; charset=utf-8", "internal server error"};
    server_generate_response(&resp);
    handler_process(&req, &resp, handler_db);

    const char *body = resp.body != NULL ? resp.body : "";
    size_t body_length = strlen(body);
    if (passthrough_body != NULL) {
        body = passthrough_body;
        body_length = passthrough_length;
        resp.status = passthrough_status;
        resp.content_type = "application/json";
    }
    debug_trace_response(resp.status, resp.content_type, body, body_length);

    int wrote = current_request_wrote;
    if (wrote) batch_write_requests++;

    int must_queue = wrote || client->pending_responses > 0U;
    int queue_failed = 0;
    if (must_queue) {
        if (!enqueue_response(client, resp.status, resp.content_type,
                              body, body_length, keep_alive)) {
            mark_client_closing(client);
            queue_failed = 1;
        }
    } else if (!client_frame_response(client, resp.status, resp.content_type,
                                      body, body_length, keep_alive)) {
        queue_failed = 1;
    }
    if (!keep_alive) mark_client_closing(client);

    if (wrote && (batch_write_requests >= batch_max_requests
            || current_time_ms() - batch_started_ms >= batch_max_age_ms)) {
        flush_batch();
    }

    free(handler_owned_username);
    handler_owned_username = NULL;
    free(trimmed_body);
    server_free_request_context(&req);
    free(passthrough_storage);
    passthrough_storage = NULL;
    passthrough_body = NULL;
    passthrough_length = 0U;
    (void) request_length;
    return !queue_failed && keep_alive;
}

static int client_reserve(client_connection_t *client, size_t needed) {
    if (needed <= client->capacity) return 1;
    if (needed > MAX_REQUEST_SIZE + 1U) return 0;
    size_t capacity = client->capacity == 0U ? INITIAL_REQUEST_CAPACITY : client->capacity;
    while (capacity < needed) {
        if (capacity > (MAX_REQUEST_SIZE + 1U) / 2U) {
            capacity = MAX_REQUEST_SIZE + 1U;
            break;
        }
        capacity *= 2U;
    }
    char *next = realloc(client->buffer, capacity);
    if (next == NULL) return 0;
    client->buffer = next;
    client->capacity = capacity;
    return 1;
}

/* Returns 1 to keep the socket, 0 to close it. */
static int process_buffered_requests(client_connection_t *client, time_t started_at) {
    for (;;) {
        if (client->used == 0U) return 1;
        client->buffer[client->used] = '\0';

        if (!client->headers_parsed) {
            char *header_end = strstr(client->buffer, "\r\n\r\n");
            if (header_end == NULL) {
                if (client->used >= MAX_REQUEST_SIZE) {
                    send_or_queue_error(client, 413, "request too large\n");
                    return 0;
                }
                return 1;
            }
            client->header_length = (size_t) (header_end - client->buffer) + 4U;
            int parsed = parse_expected_request_length(client->buffer,
                                                        client->header_length,
                                                        &client->expected_length);
            if (parsed != REQUEST_READ_OK) {
                if (parsed == REQUEST_READ_TOO_LARGE)
                    send_or_queue_error(client, 413, "request too large\n");
                else
                    send_or_queue_error(client, 400, "malformed or incomplete request\n");
                return 0;
            }
            client->headers_parsed = 1;
        }

        if (client->used < client->expected_length) return 1;
        size_t request_length = client->expected_length;
        char next_byte = client->buffer[request_length];
        client->buffer[request_length] = '\0';
        int keep_alive = process_one_request(client, client->buffer,
                                             request_length, started_at);
        client->buffer[request_length] = next_byte;
        if (!keep_alive) return 0;

        size_t remaining = client->used - request_length;
        if (remaining > 0U) memmove(client->buffer, client->buffer + request_length, remaining);
        client->used = remaining;
        client->buffer[client->used] = '\0';
        client->headers_parsed = 0;
        client->expected_length = 0U;
        client->header_length = 0U;
        /* Continue immediately if the client pipelined another request. */
    }
}

/* Returns 1 while the connection stays open; 0 means it must be closed. */
static int read_ready_connection(client_connection_t *client, time_t started_at) {
    for (;;) {
        if (!client_reserve(client, client->used + 2U)) {
            send_or_queue_error(client, 413, "request too large\n");
            return 0;
        }
        ssize_t received = recv(client->fd, client->buffer + client->used,
                                client->capacity - client->used - 1U, MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            if (debug) fprintf(stderr, "recv failed: %s\n", strerror(errno));
            return 0;
        }
        if (received == 0) {
            if (client->used > 0U) {
                if (process_buffered_requests(client, started_at) && client->used > 0U)
                    send_or_queue_error(client, 400, "malformed or incomplete request\n");
            }
            return 0;
        }

        client->used += (size_t) received;
        client->buffer[client->used] = '\0';
        if (!process_buffered_requests(client, started_at)) return 0;
        /* The socket remains nonblocking only for recv(MSG_DONTWAIT); drain ready bytes. */
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;

    time_t started_at = time(NULL);
    const char *debug_value = getenv("DEBUG");
    debug = debug_value != NULL && debug_value[0] != '\0'
            && strcmp(debug_value, "0") != 0
            && strcasecmp(debug_value, "false") != 0;

    sqlite3 *db = NULL;
    const char *db_path = getenv("SQLITE_PATH");
    if (db_path == NULL || db_path[0] == '\0') {
        fprintf(stderr, "SQLITE_PATH is required\n");
        return 1;
    }
    int db_rc = sqlite3_open_v2(db_path, &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
    if (db_rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open SQLite database '%s': %s\n", db_path,
                db != NULL ? sqlite3_errmsg(db) : "out of memory");
        if (db != NULL) sqlite3_close(db);
        return 1;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, 5000);
    (void) sqlite3_exec(db,
            "PRAGMA journal_mode=WAL;"
            "PRAGMA synchronous=NORMAL;"
            "PRAGMA cache_size=-16384;"
            "PRAGMA temp_store=MEMORY;"
            "PRAGMA wal_autocheckpoint=0;",
            NULL, NULL, NULL);
    (void) sqlite3_wal_hook(db, handler_wal_hook, NULL);
    if (!prepare_cached_statements(db)) {
        finalize_cached_statements();
        sqlite3_close(db);
        return 1;
    }
    handler_db = db;
    server_set_query_handler(handler_execute_query);

    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0 && rlim.rlim_cur != RLIM_INFINITY) {
        size_t limit = (size_t) rlim.rlim_cur;
        if (limit > 256U) limit -= 128U;
        if (limit < 128U) limit = 128U;
        client_limit = limit;
        fd_high_water = limit > 4096U ? limit - 1024U : (limit * 3U) / 4U;
        fd_low_water = limit > 4096U ? limit - 8192U : (limit * 2U) / 3U;
        if (fd_low_water > fd_high_water) fd_low_water = fd_high_water;
    }

    const char *host = getenv("HOST");
    const char *port_str = getenv("PORT");
    if (host == NULL || host[0] == '\0') host = "127.0.0.1";
    if (port_str == NULL || port_str[0] == '\0') port_str = "3000";
    errno = 0;
    char *port_end = NULL;
    long parsed_port = strtol(port_str, &port_end, 10);
    if (errno != 0 || port_end == port_str || *port_end != '\0'
            || parsed_port < 1 || parsed_port > 65535) {
        fprintf(stderr, "Invalid PORT: %s\n", port_str);
        finalize_cached_statements(); sqlite3_close(db); return 1;
    }
    struct in_addr listen_address;
    if (inet_pton(AF_INET, host, &listen_address) != 1) {
        fprintf(stderr, "Invalid IPv4 HOST: %s\n", host);
        finalize_cached_statements(); sqlite3_close(db); return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket"); finalize_cached_statements(); sqlite3_close(db); return 1;
    }
    int opt = 1;
    (void) setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) parsed_port);
    addr.sin_addr = listen_address;
    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); finalize_cached_statements(); sqlite3_close(db); return 1;
    }
    if (listen(server_fd, 4096) < 0) {
        perror("listen"); close(server_fd); finalize_cached_statements(); sqlite3_close(db); return 1;
    }
    if (!set_nonblocking(server_fd)) {
        perror("fcntl(O_NONBLOCK)"); close(server_fd); finalize_cached_statements(); sqlite3_close(db); return 1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1"); close(server_fd); finalize_cached_statements(); sqlite3_close(db); return 1;
    }
    event_loop_epoll_fd = epoll_fd;
    listener_fd = server_fd;
    static char listener_tag;
    memset(&listener_event, 0, sizeof(listener_event));
    listener_event.events = EPOLLIN | EPOLLET;
    listener_event.data.ptr = &listener_tag;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &listener_event) < 0) {
        perror("epoll_ctl(listener)"); close(epoll_fd); close(server_fd);
        finalize_cached_statements(); sqlite3_close(db); return 1;
    }

    struct epoll_event events[512];
    for (;;) {
        int timeout = batch_timeout_ms();
        if (listener_paused && (timeout < 0 || timeout > 20)) timeout = 20;
        int count = epoll_wait(epoll_fd, events, (int) (sizeof(events) / sizeof(events[0])),
                               timeout);
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        if (batch_transaction_open
                && current_time_ms() - batch_started_ms >= batch_max_age_ms) {
            flush_batch();
        }
        for (int i = 0; i < count; i++) {
            if (events[i].data.ptr == &listener_tag) {
                for (;;) {
                    struct sockaddr_in peer;
                    socklen_t peer_length = sizeof(peer);
                    int client_fd = accept4(server_fd, (struct sockaddr *) &peer, &peer_length,
                                            SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EMFILE || errno == ENFILE) {
                            if (!listener_paused) {
                                (void) epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, NULL);
                                listener_paused = 1;
                            }
                            shed_idle_clients(fd_low_water);
                            update_listener_state();
                        }
                        if (debug) perror("accept4");
                        break;
                    }

                    if (client_count >= client_limit) {
                        close(client_fd);
                        shed_idle_clients(fd_low_water);
                        update_listener_state();
                        if (client_count >= client_limit) break;
                        continue;
                    }

                    int nodelay = 1;
                    (void) setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                                      &nodelay, sizeof(nodelay));

                    client_connection_t *client = calloc(1U, sizeof(*client));
                    if (client == NULL) {
                        close(client_fd);
                        continue;
                    }
                    client->fd = client_fd;
                    client_update_epoll(client);
                    if (!client->epoll_registered) {
                        close(client_fd);
                        free(client);
                        continue;
                    }
                    client_count++;
                    lru_add_tail(client);

                    if (debug) {
                        char ip[INET_ADDRSTRLEN] = "unknown";
                        (void) inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                        fprintf(stderr, "accepted %s:%u\n", ip, (unsigned int) ntohs(peer.sin_port));
                    }

                    if (client_count >= fd_high_water) {
                        shed_idle_clients(fd_low_water);
                        update_listener_state();
                        if (client_count >= fd_high_water) break;
                    }
                }
                update_listener_state();
                continue;
            }

            client_connection_t *client = (client_connection_t *) events[i].data.ptr;
            if (client->close_pending) {
                if (client->out_offset < client->out_length) client_flush(client);
                continue;
            }

            uint32_t flags = events[i].events;
            if ((flags & (EPOLLIN | EPOLLOUT)) != 0U) lru_touch(client);
            if ((flags & EPOLLOUT) != 0U) client_flush(client);
            if (!client->close_pending && (flags & (EPOLLIN | EPOLLRDHUP)) != 0U) {
                int keep = read_ready_connection(client, started_at);
                if (!keep) mark_client_closing(client);
            }
            if ((flags & (EPOLLHUP | EPOLLERR)) != 0U) mark_client_closing(client);
        }
        if (batch_transaction_open
                && current_time_ms() - batch_started_ms >= batch_max_age_ms) {
            flush_batch();
        }
        checkpoint_maybe();
        cleanup_closing_clients();
        update_listener_state();
    }

    close(epoll_fd);
    close(server_fd);
    finalize_cached_statements();
    sqlite3_close(db);
    return 1;
}
