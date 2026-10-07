/* The request parser in-process, under AddressSanitizer and UBSan: every request below is fed in two
 * pieces, split at every byte, and must give the same status. Routes are /missing (404, no database
 * needed) or malformed (400, connection closing). */
#define main server_main
#include "../src/server.c"
#undef main

static void reset_conn(void)
{
    buf_free(&conns[0].in);
    buf_free(&conns[0].out);
    memset(&conns[0], 0, sizeof conns[0]);
    conns[0].open = 1;
    ndirty = 0;
}

int main(void)
{
    static const struct { const char *wire; int status; } cases[] = {
        {"GET /missing HTTP/1.1\r\nX:a\r\r\n\r\n", 400}, /* a stray CR once crashed the parser */
        {"GET /missing HTTP/1.1\rX\r\n\r\n", 400},
        {"GET /missing HTTP/1.1\r\r\n\r\n", 400},
        {"GET /missing HTTP/1.1\nX:a\r\n\r\n", 400},
        {"GET /missing HTTP/1.1\r\nX:a\rX\r\n\r\n", 400},
        {"GET /missing HTTP/1.1\r\nX:a\nY:b\r\n\r\n", 400},
        {"GET /missing HTTP/1.1\r\n\r\n", 404},
        {"GET /missing HTTP/1.1\r\nHost: x\r\nX:a\r\n\r\n", 404},
        {"POST /missing HTTP/1.1\r\nContent-Length: 4\r\n\r\n\rX\nY", 404},
        {"POST /missing HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx", 400},
        {"POST /missing HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3;x=1\r\nabc\r\n0\r\nT: y\r\n\r\n", 404},
        {"POST /missing HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcd\r\n0\r\n\r\n", 400},
        {"POST /missing HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n3\r\nabc\r\n0\r\n\r\n", 400},
    };
    int fails = 0, checks = 0;
    conns = calloc(1, sizeof *conns);
    dirty = calloc(1, sizeof *dirty);
    update_clock();
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        size_t n = strlen(cases[i].wire);
        char want[32];
        int wl = snprintf(want, sizeof want, "HTTP/1.1 %d ", cases[i].status);
        int bad = 0;
        for (size_t split = 0; split <= n; split++) {
            reset_conn();
            if (split) on_data(0, cases[i].wire, split);
            if (split < n) on_data(0, cases[i].wire + split, n - split);
            conn_t *c = &conns[0];
            if (c->out.len < (unsigned)wl || memcmp(c->out.p, want, wl) || c->closing != (cases[i].status == 400) || c->in.len) bad++;
            checks++;
        }
        printf(bad ? "  FAIL " : "  ok   ");
        printf("case %zu -> %d at all %zu splits", i + 1, cases[i].status, n + 1);
        if (bad) printf(" (%d wrong)", bad);
        putchar('\n');
        fails += bad != 0;
    }
    reset_conn();
    free(conns);
    free(dirty);
    printf("parser: %d splits, %d cases failed\n", checks, fails);
    return fails != 0;
}
