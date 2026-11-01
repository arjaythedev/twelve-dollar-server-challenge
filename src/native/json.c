/* Fresh SQL rows become one UTF-8 JSON BLOB. No Python row objects or retained results. */
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <stdint.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

typedef struct {
    unsigned char *data;
    sqlite3_uint64 length, capacity;
    int error;
} Buffer;

static int reserve(Buffer *b, sqlite3_uint64 extra) {
    if (b->error) return 0;
    if (extra > 0x7fffffffU || b->length > 0x7fffffffU - extra) {
        b->error = SQLITE_TOOBIG;
        return 0;
    }
    sqlite3_uint64 needed = b->length + extra;
    if (needed <= b->capacity) return 1;
    sqlite3_uint64 capacity = b->capacity ? b->capacity * 2 : 4096;
    if (capacity < needed) capacity = needed;
    unsigned char *data = sqlite3_realloc64(b->data, capacity);
    if (!data) {
        b->error = SQLITE_NOMEM;
        return 0;
    }
    b->data = data;
    b->capacity = capacity;
    return 1;
}

static void append(Buffer *b, const void *data, sqlite3_uint64 size) {
    if (!size) return;
    if (reserve(b, size)) {
        memcpy(b->data + b->length, data, (size_t)size);
        b->length += size;
    }
}

#define LITERAL(b, s) append((b), (s), sizeof(s) - 1)

static void integer(Buffer *b, sqlite3_int64 value) {
    unsigned char digits[21];
    unsigned int n = 0;
    uint64_t v = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    do {
        digits[n++] = (unsigned char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (!reserve(b, n + (value < 0))) return;
    if (value < 0) b->data[b->length++] = '-';
    while (n) b->data[b->length++] = digits[--n];
}

static void string(Buffer *b, sqlite3_value *value) {
    const unsigned char *text = sqlite3_value_text(value);
    int size = sqlite3_value_bytes(value);
    if (!text) {
        b->error = sqlite3_value_type(value) == SQLITE_NULL ? SQLITE_MISMATCH : SQLITE_NOMEM;
        return;
    }
    /* Reserve the worst case once. SSE2 only reads complete blocks inside the string. */
    if (!reserve(b, (sqlite3_uint64)size * 6 + 2)) return;
    unsigned char *out = b->data + b->length;
    *out++ = '"';
    int i = 0;
    while (i < size) {
#if defined(__SSE2__)
        if (size - i >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)(text + i));
            __m128i bad = _mm_or_si128(
                _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')),
                             _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))),
                _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(31)), v));
            unsigned int mask = (unsigned int)_mm_movemask_epi8(bad);
            if (!mask) {
                _mm_storeu_si128((__m128i *)out, v);
                out += 16;
                i += 16;
                continue;
            }
            unsigned int clean = (unsigned int)__builtin_ctz(mask);
            memcpy(out, text + i, clean);
            out += clean;
            i += (int)clean;
        }
#endif
        unsigned char c = text[i++];
        if (c == '"' || c == '\\') {
            *out++ = '\\';
            *out++ = c;
        } else if (c < 32) {
            *out++ = '\\';
            switch (c) {
                case '\b': *out++ = 'b'; break;
                case '\f': *out++ = 'f'; break;
                case '\n': *out++ = 'n'; break;
                case '\r': *out++ = 'r'; break;
                case '\t': *out++ = 't'; break;
                default:
                    *out++ = 'u'; *out++ = '0'; *out++ = '0';
                    *out++ = (unsigned char)"0123456789abcdef"[c >> 4];
                    *out++ = (unsigned char)"0123456789abcdef"[c & 15];
            }
        } else {
            *out++ = c;
        }
    }
    *out++ = '"';
    b->length = (sqlite3_uint64)(out - b->data);
}

static void post(Buffer *b, sqlite3_value **values) {
    LITERAL(b, "{\"id\":"); integer(b, sqlite3_value_int64(values[0]));
    LITERAL(b, ",\"body\":"); string(b, values[1]);
    LITERAL(b, ",\"created_at\":"); string(b, values[2]);
    LITERAL(b, ",\"author\":"); string(b, values[3]);
    LITERAL(b, ",\"like_count\":"); integer(b, sqlite3_value_int64(values[4]));
    LITERAL(b, "}");
}

static void finish(sqlite3_context *context, Buffer *b) {
    if (b->error) {
        sqlite3_free(b->data);
        if (b->error == SQLITE_NOMEM) sqlite3_result_error_nomem(context);
        else if (b->error == SQLITE_TOOBIG) sqlite3_result_error_toobig(context);
        else sqlite3_result_error(context, "invalid post text", -1);
    } else {
        sqlite3_result_blob64(context, b->data, b->length, sqlite3_free);
    }
    b->data = 0;
}

static void post_json(sqlite3_context *context, int argc, sqlite3_value **values) {
    (void)argc;
    Buffer b = {0};
    LITERAL(&b, "{\"post\":"); post(&b, values); LITERAL(&b, "}");
    finish(context, &b);
}

typedef struct {
    sqlite3_uint64 key, start, end;
    int key_size;
    sqlite3_int64 id;
} FeedRow;

typedef struct {
    Buffer buffer;
    FeedRow rows[20];
    int count;
} Feed;

static void feed_step(sqlite3_context *context, int argc, sqlite3_value **values) {
    (void)argc;
    Feed *feed = sqlite3_aggregate_context(context, sizeof(Feed));
    if (!feed) { sqlite3_result_error_nomem(context); return; }
    Buffer *b = &feed->buffer;
    if (b->error) return;
    if (feed->count == 20) {
        b->error = SQLITE_TOOBIG;
        return;
    }
    FeedRow *row = &feed->rows[feed->count++];
    const unsigned char *key = sqlite3_value_text(values[2]);
    if (!key) { b->error = SQLITE_MISMATCH; return; }
    row->key = b->length;
    row->key_size = sqlite3_value_bytes(values[2]);
    row->id = sqlite3_value_int64(values[0]);
    append(b, key, (sqlite3_uint64)row->key_size);
    row->start = b->length;
    post(b, values);
    row->end = b->length;
}

static int row_before(const Buffer *b, const FeedRow *a, const FeedRow *c) {
    int size = a->key_size < c->key_size ? a->key_size : c->key_size;
    int order = memcmp(b->data + a->key, b->data + c->key, (size_t)size);
    if (order) return order > 0;
    if (a->key_size != c->key_size) return a->key_size > c->key_size;
    return a->id > c->id;
}

static void feed_final(sqlite3_context *context) {
    Feed *feed = sqlite3_aggregate_context(context, 0);
    if (!feed) {
        sqlite3_result_blob(context, "{\"posts\":[]}", 12, SQLITE_STATIC);
        return;
    }
    Buffer *b = &feed->buffer;
    if (b->error) { finish(context, b); return; }
    /* At most 20 rows. Sort metadata, not text or database results from another request. */
    for (int i = 1; i < feed->count; i++) {
        FeedRow row = feed->rows[i];
        int j = i;
        while (j && row_before(b, &row, &feed->rows[j - 1])) {
            feed->rows[j] = feed->rows[j - 1];
            j--;
        }
        feed->rows[j] = row;
    }
    Buffer out = {0};
    reserve(&out, b->length + 12);
    LITERAL(&out, "{\"posts\":[");
    for (int i = 0; i < feed->count; i++) {
        if (i) LITERAL(&out, ",");
        FeedRow *row = &feed->rows[i];
        append(&out, b->data + row->start, row->end - row->start);
    }
    LITERAL(&out, "]}");
    sqlite3_free(b->data);
    b->data = 0;
    finish(context, &out);
}

#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int sqlite3_extension_init(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
    (void)error;
    SQLITE_EXTENSION_INIT2(api);
    int flags = SQLITE_UTF8 | SQLITE_DIRECTONLY;
    int rc = sqlite3_create_function(db, "td_post", 5, flags, 0, post_json, 0, 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_create_function(db, "td_feed", 5, flags, 0, 0, feed_step, feed_final);
    return rc;
}
