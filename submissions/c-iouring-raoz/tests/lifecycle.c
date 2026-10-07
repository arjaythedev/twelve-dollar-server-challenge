/* Durability and connection lifetime: acknowledged writes survive SIGKILL, and 15,000 keep-alive
 * connections stay usable after 66 idle seconds (Nginx expects at least 65). CONNECTIONS and IDLE_SECONDS
 * override the defaults. From PRs 18 and 19 of the contest repository. */
#include "t.h"

int main(void)
{
    srv_start();
    char *t = jwt_user("1", "\"golden_ember_1\"");
    resp r = {0};
    int ok = req("POST", "/posts", t, "{\"body\":\"durable before reply\"}", &r) == 201;
    long long id = jnum(r.body, "id");
    char *path = fmt("/posts/%lld", id), *lpath = fmt("/posts/%lld/like", id);
    ok &= req("POST", lpath, t, NULL, &r) == 201;
    srv_crash_restart();
    ok &= req("GET", path, NULL, NULL, &r) == 200 && jnum(r.body, "like_count") == 1;
    char *b = jstr(r.body, "body");
    ok &= streq(b, "durable before reply");
    check(ok, "an acknowledged post and like survive SIGKILL and restart");
    free(b); free(path); free(lpath); free(t);

    int n = getenv("CONNECTIONS") ? atoi(getenv("CONNECTIONS")) : 15000;
    int idle = getenv("IDLE_SECONDS") ? atoi(getenv("IDLE_SECONDS")) : 66;
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < (rlim_t)n + 64) die("need %d file descriptors, have %lu: raise the hard limit (ulimit -Hn)", n + 64, (unsigned long)rl.rlim_cur);

    /* several loopback source addresses, so earlier runs' TIME_WAIT sockets cannot exhaust the ports */
    conn *c = calloc(n, sizeof *c);
    int opened = 0;
    for (int i = 0; i < n; i++) {
        char src[16];
        snprintf(src, sizeof src, "127.0.0.%d", 2 + i % 4);
        c[i] = (conn){tconnect_from(src), NULL, 0, 0};
        if (c[i].fd < 0) break;
        csends(&c[i], "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        if (!cread(&c[i], &r) || r.status != 200 || has_header(&r, "\r\nConnection: close")) { cclose(&c[i]); break; }
        opened++;
    }
    check(opened == n, "%d of %d keep-alive connections open, each answered /health", opened, n);
    sleep(idle);
    int alive = 0;
    for (int i = 0; i < opened; i++) {
        csends(&c[i], "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        alive += cread(&c[i], &r) && r.status == 200 && strstr(r.body, "\"db\":\"ok\"");
    }
    check(alive == n, "%d of %d of the same connections answer again after %d idle seconds", alive, n, idle);
    for (int i = 0; i < opened; i++) cclose(&c[i]);
    free(c);
    free(r.body);
    printf("lifecycle: %d passed, %d failed\n", passes, fails);
    return fails != 0;
}
