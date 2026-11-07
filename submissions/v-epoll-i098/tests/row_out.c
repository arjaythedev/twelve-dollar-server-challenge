/* td_put_row must match a plain reference writer for every length and escape position.
 * cc -O2 -Wall -Wextra -Werror -I . tests/row_out.c -o build/row-out-test && build/row-out-test */
#include "row_out.h"
#include <stdio.h>
#include <stdlib.h>

static int reference_string(char *o, const unsigned char *p, int n) {
    int k = 0;
    o[k++] = '"';
    for (int i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c == '\n') { o[k++] = '\\'; o[k++] = 'n'; }
        else if (c == '\r') { o[k++] = '\\'; o[k++] = 'r'; }
        else if (c == '\t') { o[k++] = '\\'; o[k++] = 't'; }
        else if (c == 8) { o[k++] = '\\'; o[k++] = 'b'; }
        else if (c == 12) { o[k++] = '\\'; o[k++] = 'f'; }
        else if (c < 0x20) k += sprintf(o + k, "\\u%04x", c);
        else o[k++] = (char)c;
    }
    o[k++] = '"';
    return k;
}

int main(void) {
    static unsigned char body[300], user[40], created[25] = "2026-10-31T06:00:00.123Z";
    static char want[4096];
    static unsigned char got[4096];
    const unsigned char alphabet[] = {'a', 'z', ' ', '"', '\\', '\n', '\r', '\t', 8, 12, 0, 1, 31, 127, 200};
    srand(7);
    for (int round = 0; round < 200000; round++) {
        int bl = rand() % 300, ul = rand() % 40;
        int dirty = rand() % 4; /* 0: clean text; otherwise this many percent*5 are special */
        for (int i = 0; i < bl; i++) body[i] = (rand() % 20 < dirty) ? alphabet[rand() % sizeof alphabet] : (unsigned char)('a' + i % 26);
        for (int i = 0; i < ul; i++) user[i] = (rand() % 20 < dirty) ? alphabet[rand() % sizeof alphabet] : (unsigned char)('a' + i % 26);
        td_feed_row r = { .id = rand() % 3 ? rand() : 1ll << 40, .likes = rand() % 5 ? rand() % 100 : 0,
                          .body = body, .created = created, .user = user, .body_len = bl, .user_len = ul };
        int k = sprintf(want, "{\"id\":%lld,\"body\":", r.id);
        k += reference_string(want + k, body, bl);
        k += sprintf(want + k, ",\"created_at\":\"%.24s\",\"author\":", created);
        k += reference_string(want + k, user, ul);
        k += sprintf(want + k, ",\"like_count\":%lld}", r.likes);
        unsigned char *end = td_put_row(got, &r);
        if (end - got != k || memcmp(got, want, (size_t)k)) {
            fprintf(stderr, "mismatch at round %d (body %d, user %d)\n", round, bl, ul);
            return 1;
        }
    }
    puts("td_put_row matches the reference writer");
    return 0;
}
