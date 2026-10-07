/* HTTP/1.1 transport: request framing split at any point, pipelining, chunked bodies, 100 Continue,
 * connection close semantics, aborted and reset uploads, backpressure against clients that do not read,
 * and the deadline for stalled connections. Cases come from our own checks and from other submissions'
 * suites (PRs 3, 18 and 19 of the contest repository). */
#include "t.h"

static char *T;

static int is_error(const resp *r, int status, const char *msg)
{
    char *want = fmt("{\"error\":\"%s\"}", msg);
    int ok = r->status == status && streq(r->body, want);
    free(want);
    return ok;
}

static long server_rss_kb(void)
{
    char p[64], line[256];
    snprintf(p, sizeof p, "/proc/%d/status", srv_pid);
    FILE *f = fopen(p, "r");
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) kb = atol(line + 6);
    if (f) fclose(f);
    return kb;
}

/* Sends n bytes from a child process, so the parent can hold off reading while the send blocks. */
static pid_t send_in_child(conn *c, const char *s, size_t n)
{
    pid_t pid = fork();
    if (!pid) {
        struct timeval tv = {120, 0};
        setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        csend(c, s, n);
        _exit(0);
    }
    return pid;
}

/* The server closes the connection within ms: reading (and discarding) drains to EOF or a reset. */
static int drained_to_close(conn *c, int ms)
{
    int big = 8 << 20; /* a small receive buffer would take minutes to drain what the server queued */
    setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char b[65536];
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) return 0;
        struct pollfd p = {c->fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) continue;
        ssize_t r = recv(c->fd, b, sizeof b, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno == ECONNRESET)) return 1;
    }
}

static void framing(void)
{
    resp r = {0};
    static const char *bad[] = {"GET /health HTTP/1.1\r\nX:a\r\r\n\r\n", "GET /health HTTP/1.1\rX\r\n\r\n",
                                "GET /health HTTP/1.1\r\nX:a\nY:b\r\n\r\n", "GARBAGE\r\n\r\n"};
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        conn c = copen();
        csends(&c, bad[i]);
        ok &= cread(&c, &r) && is_error(&r, 400, "bad request") && closed_by_server(&c, 2000);
        cclose(&c);
        ok &= health_ok();
    }
    check(ok, "malformed line endings and request lines -> 400 bad request, closed, server healthy");

    conn c = copen();
    csends(&c, "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n");
    cread(&c, &r);
    char *feed = strdup(r.body ? r.body : "");
    const char *one = "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n";
    csend_split(&c, one, strlen(one), 1, 1);
    ok = cread(&c, &r) && r.status == 200 && streq(r.body, feed);
    check(ok, "request split byte by byte");

    const char *three = "GET /feed HTTP/1.1\r\nHost: x\r\n\r\nGET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\nGET /zzz HTTP/1.1\r\nHost: x\r\n\r\n";
    csends(&c, three);
    int st[3];
    for (int i = 0; i < 3; i++) st[i] = cread(&c, &r) ? r.status : 0;
    check(st[0] == 200 && st[1] == 200 && st[2] == 404, "3 pipelined requests in one write");
    csend(&c, three, 40);
    sleep_ms(50);
    csends(&c, three + 40);
    for (int i = 0; i < 3; i++) st[i] = cread(&c, &r) ? r.status : 0;
    check(st[0] == 200 && st[1] == 200 && st[2] == 404, "pipelined requests split mid-request");

    char *like = fmt("POST /posts/2/like HTTP/1.1\r\nHost: x\r\naUtHoRiZaTiOn: Bearer %s\r\ncontent-length: 7\r\n\r\n{\"x\":1}", T);
    size_t ll = strlen(like);
    csend(&c, like, ll - 4);
    sleep_ms(50);
    csend(&c, like + ll - 4, 4);
    ok = cread(&c, &r) && (r.status == 200 || r.status == 201);
    check(ok, "POST with mixed-case headers and the body split across writes");
    cclose(&c);
    free(like);
    free(feed);

    c = copen();
    csends(&c, "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    ok = cread(&c, &r) && r.status == 200 && has_header(&r, "\r\nConnection: close\r\n") && closed_by_server(&c, 2000);
    check(ok, "Connection: close -> the response says so, then the server closes");
    cclose(&c);
    /* responses to HTTP/1.0 are read as raw bytes */
    c = copen();
    csends(&c, "GET /health HTTP/1.0\r\n\r\n");
    char raw[1024] = {0};
    ssize_t got = 0, k;
    while ((k = recv(c.fd, raw + got, sizeof raw - 1 - got, 0)) > 0) got += k;
    ok = strstr(raw, " 200 ") && strstr(raw, "\r\nConnection: close\r\n") && k == 0;
    check(ok, "HTTP/1.0 without keep-alive: Connection: close, then closed");
    cclose(&c);
    c = copen();
    csends(&c, "GET /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    memset(raw, 0, sizeof raw);
    got = 0;
    while (got < 2 && (k = recv(c.fd, raw + got, sizeof raw - 1 - got, 0)) > 0) {
        got += k;
        char *a = strstr(raw, "keep-alive"), *b = a ? strstr(a + 1, "keep-alive") : NULL;
        if (b && strstr(b, "}")) break;
    }
    char *a = strstr(raw, "\r\nConnection: keep-alive\r\n");
    ok = a && strstr(a + 1, "\r\nConnection: keep-alive\r\n");
    check(ok, "HTTP/1.0 with keep-alive: the header is echoed and the connection reused");
    cclose(&c);

    c = copen();
    char *big = repeat("a", 20000), *line = fmt("GET /%s", big);
    csends(&c, line);
    int got431 = cread(&c, &r); /* a 431 response, or just the close */
    ok = (!got431 || r.status == 431) && (!got431 || closed_by_server(&c, 2000));
    check(ok, "a request head over 16 KB -> closed");
    cclose(&c);
    free(big); free(line);
    free(r.body);
}

static void bodies(void)
{
    resp r = {0};
    /* fragments of one upload, with unrelated requests on other connections in between */
    const char *payload = "{\"body\":\"fragmented \xe2\x9c\x93 body\"}";
    size_t pl = strlen(payload);
    int ok = 1;
    for (int chunked = 0; chunked < 2; chunked++) {
        conn c = copen();
        char *h = chunked ? fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nTransfer-Encoding: chunked\r\n\r\n", T)
                          : fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n", T, pl);
        csends(&c, h);
        size_t cuts[] = {0, 5, 13, pl};
        for (int i = 0; i < 3; i++) {
            size_t n = cuts[i + 1] - cuts[i];
            if (chunked) {
                char *sz = fmt("%zx\r\n", n);
                csends(&c, sz);
                free(sz);
            }
            csend(&c, payload + cuts[i], n);
            if (chunked) csends(&c, "\r\n");
            for (int k = 0; k < 6; k++) ok &= req("GET", "/posts/500000", NULL, NULL, &r) == 200;
        }
        if (chunked) csends(&c, "0\r\n\r\n");
        char *b = NULL;
        ok &= cread(&c, &r) && r.status == 201 && streq(b = jstr(r.body, "body"), "fragmented \xe2\x9c\x93 body");
        char *lq = fmt("POST /posts/%lld/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n", jnum(r.body, "id"), T);
        csends(&c, lq);
        ok &= cread(&c, &r) && r.status == 201;
        free(b); free(h); free(lq);
        cclose(&c);
    }
    check(ok, "upload in fragments (Content-Length and chunked) between other requests, then an empty-body like");

    conn c = copen();
    char *ch = fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "9;test=yes\r\n{\"body\":\"\r\n7\r\nchunk\"}\r\n0\r\nX-Trailer: ok\r\n\r\n", T);
    csend_split(&c, ch, strlen(ch), 13, 1);
    ok = cread(&c, &r) && r.status == 201;
    char *b = jstr(r.body, "body");
    ok &= streq(b, "chunk");
    check(ok, "chunked body with extensions and trailers, sent in 13-byte pieces");
    free(b);
    char *three = fmt("%s%s%sGET /feed HTTP/1.1\r\nHost: x\r\n\r\n", ch, ch, ch);
    csend_split(&c, three, strlen(three), 7, 0);
    ok = 1;
    for (int i = 0; i < 4; i++) ok &= cread(&c, &r) && r.status == (i < 3 ? 201 : 200);
    check(ok, "3 chunked uploads and a read pipelined, sent in 7-byte pieces");
    cclose(&c);
    free(ch); free(three);

    static const char *badchunks[] = {"zz\r\nabc\r\n0\r\n\r\n", "3\r\nabcd\r\n0\r\n\r\n", "3\nabc\r\n0\r\n\r\n"};
    ok = 1;
    for (int i = 0; i < 3; i++) {
        c = copen();
        char *q = fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nTransfer-Encoding: chunked\r\n\r\n%s", T, badchunks[i]);
        csends(&c, q);
        ok &= cread(&c, &r) && r.status == 400;
        cclose(&c);
        free(q);
    }
    check(ok, "malformed chunk sizes and terminators -> 400");

    static const char *framing[] = {"Content-Length: 1\r\nContent-Length: 2\r\n", "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n",
                                    "Transfer-Encoding: gzip\r\n"};
    ok = 1;
    for (int i = 0; i < 3; i++) {
        c = copen();
        char *q = fmt("POST /posts HTTP/1.1\r\nHost: x\r\n%s\r\nxxxxx", framing[i]);
        csends(&c, q);
        ok &= cread(&c, &r) && r.status == 400;
        cclose(&c);
        free(q);
    }
    c = copen();
    char *same = fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 12\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}", T);
    csends(&c, same);
    ok &= cread(&c, &r) && r.status == 201;
    cclose(&c);
    free(same);
    check(ok, "conflicting Content-Length, Content-Length with chunked, other codings -> 400; repeated equal length is fine");

    for (int chunked = 0; chunked < 2; chunked++) {
        const char *data = "{\"body\":\"continue\"}";
        size_t dl = strlen(data);
        c = copen();
        char *q = chunked ? fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n", T)
                          : fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nExpect: 100-continue\r\nContent-Length: %zu\r\n\r\n", T, dl);
        csend(&c, q, 20);
        csends(&c, q + 20);
        char interim[64] = {0};
        size_t got = 0;
        while (got < 25) {
            ssize_t k = recv(c.fd, interim + got, 25 - got, 0);
            if (k <= 0) break;
            got += k;
        }
        ok = !strcmp(interim, "HTTP/1.1 100 Continue\r\n\r\n");
        char *first = chunked ? fmt("3\r\n%.3s\r\n", data) : fmt("%.3s", data);
        char *rest = chunked ? fmt("%zx\r\n%s\r\n0\r\n\r\n", dl - 3, data + 3) : strdup(data + 3);
        csends(&c, first);
        ok &= !readable(&c, 100);
        csends(&c, rest);
        ok &= cread(&c, &r) && r.status == 201;
        check(ok, "Expect: 100-continue (%s): one interim response, then 201", chunked ? "chunked" : "Content-Length");
        cclose(&c);
        free(q); free(first); free(rest);
    }

    c = copen();
    csends(&c, "POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\npartial");
    cclose(&c);
    for (int i = 0; i < 100; i++) {
        c = copen();
        char *q = fmt("POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 1000\r\n\r\n{\"body\":\"unfinished", T);
        csends(&c, q);
        cclose(&c);
        free(q);
    }
    for (int i = 0; i < 100; i++) {
        c = copen();
        struct linger lg = {1, 0};
        setsockopt(c.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        char *q = rq("POST", "/posts", T, "{\"body\":\"reset\"}", 16);
        csends(&c, q);
        cclose(&c); /* RST */
        free(q);
    }
    check(health_ok(), "101 abandoned partial uploads and 100 connections reset after complete uploads");

    /* the token's username is user 1's, so the created post and the feed agree on the author */
    c = copen();
    char *t1 = jwt_user("1", "\"golden_ember_1\"");
    char *pw = fmt("%sGET /feed HTTP/1.1\r\nHost: x\r\n\r\n", rq("POST", "/posts", t1, "{\"body\":\"pipeline-x\"}", 21));
    free(t1);
    csends(&c, pw);
    resp created = {0};
    ok = cread(&c, &created) && created.status == 201 && cread(&c, &r) && r.status == 200;
    char *top = feed_item(r.body, 0), *want = created.body ? strndup(created.body + 8, strlen(created.body) - 9) : NULL;
    ok &= streq(top, want);
    check(ok, "a read pipelined after a write sees it");
    cclose(&c);
    free(pw); free(top); free(want); free(created.body);
    free(r.body);
}

/* Responses larger than one buffer chunk: long, escape-heavy bodies, then many pipelined feeds. */
static void large(void)
{
    resp r = {0};
    char *quote = repeat("\"\\\xc3\xa9\xf0\x9f\x98\x80<\n\t>", 50), *xs = repeat("x", 500), *ctl = repeat("\x01", 500);
    const char *texts[] = {quote, xs, ctl};
    int ok = 1;
    for (int i = 0; i < 21; i++) {
        char *b = body_json(texts[i % 3], 0);
        ok &= req("POST", "/posts", T, b, &r) == 201;
        free(b);
    }
    ok &= req("GET", "/feed", NULL, NULL, &r) == 200 && feed_count(r.body) == 20 && r.blen > 25000;
    for (int i = 0; i < 20 && ok; i++) {
        char *it = feed_item(r.body, i), *b = jstr(it, "body");
        ok &= streq(b, texts[(20 - i) % 3]);
        free(it); free(b);
    }
    check(ok, "a feed of 20 long escaped posts (%zu bytes) round-trips", r.blen);
    char *feed = strdup(r.body ? r.body : "");
    conn c = copen();
    char *q = repeat("GET /feed HTTP/1.1\r\nHost: x\r\n\r\n", 30);
    csends(&c, q);
    ok = 1;
    for (int i = 0; i < 30; i++) ok &= cread(&c, &r) && r.status == 200 && streq(r.body, feed);
    check(ok, "30 pipelined large feeds, all identical");
    cclose(&c);
    free(q); free(feed); free(quote); free(xs); free(ctl);
    free(r.body);
}

/* A client that pipelines without reading: the server stops receiving from it instead of queueing the
 * responses, and everything arrives intact once the client reads. */
static void backpressure(void)
{
    resp r = {0}, post = {0}, feed = {0};
    sleep_ms(100); /* let writes from the reset connections above commit */
    req("GET", "/feed", NULL, NULL, &feed);
    req("GET", "/posts/500000", NULL, NULL, &post);
    const char *paths[] = {"/feed", "/posts/500000", "/health"};
    size_t n = 1800;
    char *all = malloc(n * 48), *o = all;
    for (size_t i = 0; i < n; i++) o += sprintf(o, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", paths[i % 3]);
    conn c = copen();
    int rb = 65536;
    setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    pid_t child = send_in_child(&c, all, o - all);
    sleep_ms(300);
    int ok = 1;
    for (size_t i = 0; i < n && ok; i++) {
        ok &= cread(&c, &r) && r.status == 200;
        if (i % 3 == 0) ok &= streq(r.body, feed.body);
        else if (i % 3 == 1) ok &= streq(r.body, post.body);
        else ok &= strstr(r.body, "\"db\":\"ok\"") != NULL;
    }
    waitpid(child, NULL, 0);
    check(ok, "1800 pipelined feed/post/health requests against a slow reader, all intact");
    cclose(&c);
    free(all);

    /* 20,000 feeds (about 75 MB of responses) from a client that does not read for a second */
    n = 20000;
    all = repeat("GET /feed HTTP/1.1\r\nHost: x\r\n\r\n", n);
    long rss0 = server_rss_kb();
    c = copen();
    setsockopt(c.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    child = send_in_child(&c, all, strlen(all));
    sleep_ms(1500);
    long rss1 = server_rss_kb();
    check(rss1 - rss0 < 16384, "server memory while a client pipelines 20,000 requests without reading: +%ld KB", rss1 - rss0);
    ok = 1;
    for (size_t i = 0; i < n && ok; i++) ok &= cread(&c, &r) && r.status == 200 && streq(r.body, feed.body);
    waitpid(child, NULL, 0);
    check(ok, "then all 20,000 responses arrive in order once it reads");
    cclose(&c);
    free(all);
    free(r.body); free(post.body); free(feed.body);
}

int main(void)
{
    srv_start();
    T = jwt_user("1", "\"transport \xe2\x9c\x93\"");

    /* Stalled-connection deadline (30 s, checked every 5 s): opened first, checked at the end. */
    conn idle = copen(), loris = copen(), deaf = copen();
    resp r = {0};
    csends(&idle, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    cread(&idle, &r);
    csends(&loris, "GET /feed HTTP/1.1\r\nHost:");
    int rb = 4096;
    setsockopt(deaf.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    char *many = repeat("GET /feed HTTP/1.1\r\nHost: x\r\n\r\n", 5000);
    pid_t deaf_child = send_in_child(&deaf, many, strlen(many));
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    framing();
    bodies();
    large();
    backpressure();

    /* drip one more byte into the slow-loris request every few seconds: it still never completes */
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= 42) break;
        csend(&loris, " ", 1);
        sleep_ms(3000);
    }
    check(drained_to_close(&loris, 3000), "a request head dripped in for 40 s is closed by the stall deadline");
    /* it stalls only once the socket buffers are full, so allow one more deadline (30 s) plus sweep (5 s) */
    check(drained_to_close(&deaf, 40000), "a client that pipelines and never reads is closed by the stall deadline");
    kill(deaf_child, SIGKILL);
    waitpid(deaf_child, NULL, 0);
    csends(&idle, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    check(cread(&idle, &r) && r.status == 200, "an idle keep-alive connection is kept over the same 40 s");
    cclose(&idle); cclose(&loris); cclose(&deaf);
    free(many); free(r.body);

    printf("http: %d passed, %d failed\n", passes, fails);
    return fails != 0;
}
