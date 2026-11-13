/* From the submission directory, compile with the same -march as the server:
 * cc -O2 -march=native -I. tests/render-guard.c buffer.c -o /tmp/render-guard
 * /tmp/render-guard
 */
#define _GNU_SOURCE
#include "challenge.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void literal(Buf *buffer, const char *s) {
    assert(buf_append(buffer, s, strlen(s)) == 0);
}

static void decimal(Buf *buffer, int64_t value) {
    char s[32];
    int length = snprintf(s, sizeof(s), "%" PRId64, value);
    assert(length > 0 && (size_t)length < sizeof(s));
    assert(buf_append(buffer, s, (size_t)length) == 0);
}

int main(void) {
    const int64_t numbers[] = {INT64_MIN, -100000000, -10000, -99, -1, 0,
                              9, 10, 99, 100, 9999, 10000, 99999999, INT64_MAX};
    const char escapes[] = {'\0', '\b', '\f', '\n', '\r', '\t', '"', '\\', 31};
    const char *properties[] = {",\"body\":", ",\"created_at\":", ",\"author\":"};
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *mapping = mmap(NULL, 7 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    for (size_t i = 0; i < 3; ++i)
        assert(mprotect(mapping + (2 * i + 1) * page, page, PROT_READ | PROT_WRITE) == 0);

    Buf expected = {0}, actual = {0};
    unsigned cases = 0;
    for (size_t trial = 0; trial < 4 * page; ++trial) {
        char *fields[3];
        size_t lengths[3];
        int64_t id = numbers[trial % (sizeof(numbers) / sizeof(*numbers))];
        int64_t likes = numbers[(trial / 7) % (sizeof(numbers) / sizeof(*numbers))];
        expected.len = actual.len = 0;
        literal(&expected, "prefix:");
        literal(&actual, "prefix:");
        literal(&expected, "{\"id\":");
        decimal(&expected, id);
        for (size_t i = 0; i < 3; ++i) {
            size_t n = (trial + 17 * i) % (page + 1);
            char *field = mapping + (2 * i + 1) * page;
            if ((trial / 4 + i) % 2) field += page - n;
            fields[i] = field;
            lengths[i] = n;
            if (n) {
                memset(field, 'a' + (int)i, n);
                if (n >= 2 && trial % 11 == 0) memcpy(field + n - 2, "\xc3\xa9", 2);
                if ((trial + i) % 4 == 0)
                    field[(trial * 7 + i) % n] = escapes[trial % sizeof(escapes)];
            }
            literal(&expected, properties[i]);
            assert(buf_json_string(&expected, field, n) == 0);
        }
        literal(&expected, ",\"like_count\":");
        decimal(&expected, likes);
        literal(&expected, "}");
        assert(buf_post_row(&actual, id, fields[0], lengths[0], fields[1], lengths[1],
                            fields[2], lengths[2], likes) == 0);
        assert(actual.len == expected.len && memcmp(actual.data, expected.data, expected.len) == 0);
        ++cases;
    }
    printf("%u guarded rows matched the general JSON encoder and snprintf\n", cases);
    free(actual.data);
    free(expected.data);
    assert(munmap(mapping, 7 * page) == 0);
    return 0;
}
