/* Helpers shared by the test programs: a server of our own on a fresh copy of the seed database, raw
 * HTTP/1.1 over sockets, HS256 tokens, and a little JSON reading. No tools beyond libc. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/sha256.h"

#define SECRET "twelve-dollar-challenge"

/* ---------------------------------------------------------------- results */

static int fails, passes;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(ok ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
    if (ok) passes++;
    else fails++;
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    exit(1);
}

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static double now_real(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* malloc'd copy of s repeated n times */
static char *repeat(const char *s, int n)
{
    size_t l = strlen(s);
    char *o = malloc(l * n + 1);
    for (int i = 0; i < n; i++) memcpy(o + l * i, s, l);
    o[l * n] = 0;
    return o;
}

static char *fmt(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    char *s;
    if (vasprintf(&s, f, ap) < 0) abort();
    va_end(ap);
    return s;
}

/* ---------------------------------------------------------------- server */

static int port;
static pid_t srv_pid;
static char srv_dir[256], srv_db[300];
static const char *srv_bin;

static void srv_spawn(void)
{
    char log[300];
    snprintf(log, sizeof log, "%s/server.log", srv_dir);
    srv_pid = fork();
    if (srv_pid < 0) die("fork: %s", strerror(errno));
    if (!srv_pid) {
        int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        char ps[16];
        snprintf(ps, sizeof ps, "%d", port);
        setenv("SQLITE_PATH", srv_db, 1);
        setenv("JWT_SECRET", SECRET, 1);
        setenv("HOST", "127.0.0.1", 1);
        setenv("PORT", ps, 1);
        setenv("TEST_DIR", srv_dir, 1); /* control files for test builds of the server (commit_fault.c) */
        execl(srv_bin, srv_bin, (char *)NULL);
        _exit(127);
    }
}

static int health_ok(void);

static void srv_wait_healthy(void)
{
    for (int i = 0; i < 600; i++) {
        int st;
        if (waitpid(srv_pid, &st, WNOHANG) == srv_pid) die("server exited during startup (log: %s/server.log)", srv_dir);
        if (health_ok()) return;
        sleep_ms(100);
    }
    die("server not healthy after 60 s (log: %s/server.log)", srv_dir);
}

static void srv_stop(void)
{
    if (srv_pid > 0) {
        kill(srv_pid, SIGKILL);
        waitpid(srv_pid, NULL, 0);
        srv_pid = 0;
    }
}

static void srv_cleanup(void)
{
    srv_stop();
    if (!srv_dir[0]) return;
    const char *names[] = {"feed.db", "feed.db-wal", "feed.db-shm", "fail", "hold", "entered", "server.log"};
    size_t keep_log = fails ? 1 : 0; /* after a failure, keep the server's log */
    for (size_t i = 0; i < sizeof names / sizeof *names - keep_log; i++) {
        char p[400];
        snprintf(p, sizeof p, "%s/%s", srv_dir, names[i]);
        unlink(p);
    }
    if (keep_log) printf("server log kept in %s/server.log\n", srv_dir);
    else rmdir(srv_dir);
}

static void copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) die("copy %s -> %s: %s", from, to, strerror(errno));
    char buf[1 << 16];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) die("write %s: %s", to, strerror(errno));
    close(in);
    close(out);
}

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t l = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || getsockname(fd, (struct sockaddr *)&a, &l)) die("free port: %s", strerror(errno));
    close(fd);
    return ntohs(a.sin_port);
}

/* Starts SERVER (default ../bin/server) on a fresh copy of SEED (default ../../../seed/feed.db). */
static void srv_start(void)
{
    signal(SIGPIPE, SIG_IGN);
    srv_bin = getenv("SERVER") ? getenv("SERVER") : "../bin/server";
    const char *seed = getenv("SEED") ? getenv("SEED") : "../../../seed/feed.db";
    if (access(srv_bin, X_OK)) die("%s not found: run build.sh first", srv_bin);
    snprintf(srv_dir, sizeof srv_dir, "%s/c-iouring-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(srv_dir)) die("mkdtemp: %s", strerror(errno));
    snprintf(srv_db, sizeof srv_db, "%s/feed.db", srv_dir);
    copy_file(seed, srv_db);
    port = getenv("TEST_PORT") ? atoi(getenv("TEST_PORT")) : free_port();
    atexit(srv_cleanup);
    srv_spawn();
    srv_wait_healthy();
}

/* SIGKILL (no clean shutdown, no checkpoint) and start again on the same database. */
static void srv_crash_restart(void)
{
    srv_stop();
    srv_spawn();
    srv_wait_healthy();
}

/* ---------------------------------------------------------------- sockets */

typedef struct { int fd; char *buf; size_t len, cap; } conn;
typedef struct { int status; char head[8192]; char *body; size_t blen; } resp;

static int tconnect_from(const char *src)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (src) {
        struct sockaddr_in s = {.sin_family = AF_INET};
        inet_pton(AF_INET, src, &s.sin_addr);
        bind(fd, (struct sockaddr *)&s, sizeof s);
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    return fd;
}

static conn copen(void)
{
    conn c = {tconnect_from(NULL), NULL, 0, 0};
    if (c.fd < 0) die("connect to port %d: %s", port, strerror(errno));
    return c;
}

static void cclose(conn *c)
{
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
    c->fd = -1;
    c->buf = NULL;
    c->len = c->cap = 0;
}

static void csend(conn *c, const void *p, size_t n)
{
    const char *s = p;
    while (n) {
        ssize_t w = send(c->fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        s += w;
        n -= w;
    }
}
static void csends(conn *c, const char *s) { csend(c, s, strlen(s)); }

/* Sends n bytes in pieces of `step` bytes with a pause between them. */
static void csend_split(conn *c, const char *s, size_t n, size_t step, int pause_ms)
{
    for (size_t i = 0; i < n; i += step) {
        csend(c, s + i, n - i < step ? n - i : step);
        if (pause_ms) sleep_ms(pause_ms);
    }
}

static int cfill(conn *c)
{
    if (c->cap - c->len < 65536) {
        c->cap = c->cap * 2 + 65536;
        c->buf = realloc(c->buf, c->cap + 1);
    }
    ssize_t r = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
    if (r <= 0) return 0;
    c->len += r;
    return 1;
}

/* Reads one response (status line, headers, Content-Length body). Returns 0 on EOF or timeout. */
static int cread(conn *c, resp *r)
{
    free(r->body);
    r->body = NULL;
    r->status = 0;
    char *he;
    while (!c->buf || !(c->buf[c->len] = 0, he = memmem(c->buf, c->len, "\r\n\r\n", 4)))
        if (!cfill(c)) return 0;
    size_t hl = he + 4 - c->buf;
    size_t cl = 0;
    if (hl >= sizeof r->head) return 0;
    memcpy(r->head, c->buf, hl);
    r->head[hl] = 0;
    if (strncmp(r->head, "HTTP/1.1 ", 9)) return 0;
    r->status = atoi(r->head + 9);
    for (char *l = strstr(r->head, "\r\n"); l && l[2] != '\r'; l = strstr(l + 2, "\r\n"))
        if (!strncasecmp(l + 2, "content-length:", 15)) cl = strtoul(l + 17, NULL, 10);
    while (c->len < hl + cl)
        if (!cfill(c)) return 0;
    r->body = malloc(cl + 1);
    memcpy(r->body, c->buf + hl, cl);
    r->body[cl] = 0;
    r->blen = cl;
    memmove(c->buf, c->buf + hl + cl, c->len - hl - cl);
    c->len -= hl + cl;
    return 1;
}

/* 1 if the server closed the connection (EOF) within ms, with no unread bytes. */
static int closed_by_server(conn *c, int ms)
{
    if (c->len) return 0;
    struct pollfd p = {c->fd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return 0;
    char b;
    return recv(c->fd, &b, 1, MSG_DONTWAIT) == 0;
}

/* 1 if any byte arrives within ms */
static int readable(conn *c, int ms)
{
    if (c->len) return 1;
    struct pollfd p = {c->fd, POLLIN, 0};
    return poll(&p, 1, ms) > 0;
}

static int has_header(const resp *r, const char *line) { return strcasestr(r->head, line) != NULL; }

/* Request text: method, path, optional bearer token, optional body (with Content-Length). */
static char *rq(const char *method, const char *path, const char *tok, const char *body, size_t blen)
{
    char *auth = tok ? fmt("Authorization: Bearer %s\r\n", tok) : strdup("");
    char *h = body ? fmt("%s %s HTTP/1.1\r\nHost: x\r\n%sContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                         method, path, auth, blen)
                   : fmt("%s %s HTTP/1.1\r\nHost: x\r\n%s\r\n", method, path, auth);
    free(auth);
    if (!body) return h;
    size_t hl = strlen(h);
    h = realloc(h, hl + blen + 1);
    memcpy(h + hl, body, blen);
    h[hl + blen] = 0;
    return h;
}

/* One request on a fresh connection, sent by exact length (bodies may contain NUL bytes); fills r and
 * returns its status, 0 on failure. */
static int reqb(const char *method, const char *path, const char *tok, const char *body, size_t blen, resp *r)
{
    conn c = copen();
    char *auth = tok ? fmt("Authorization: Bearer %s\r\n", tok) : strdup("");
    char *h = body ? fmt("%s %s HTTP/1.1\r\nHost: x\r\n%sContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", method, path, auth, blen)
                   : fmt("%s %s HTTP/1.1\r\nHost: x\r\n%s\r\n", method, path, auth);
    csends(&c, h);
    if (body) csend(&c, body, blen);
    free(auth);
    free(h);
    int ok = cread(&c, r);
    cclose(&c);
    return ok ? r->status : 0;
}
#define req(m, p, t, b, r) reqb(m, p, t, b, (b) ? strlen(b) : 0, r)

static int health_ok(void)
{
    int fd = tconnect_from(NULL);
    if (fd < 0) return 0;
    conn c = {fd, NULL, 0, 0};
    resp r = {0};
    csends(&c, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    int ok = cread(&c, &r) && r.status == 200;
    free(r.body);
    cclose(&c);
    return ok;
}

/* ---------------------------------------------------------------- tokens */

static void b64url(const unsigned char *s, size_t n, char *o)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        *o++ = a[s[i] >> 2];
        *o++ = a[((s[i] & 3) << 4) | (s[i + 1] >> 4)];
        *o++ = a[((s[i + 1] & 15) << 2) | (s[i + 2] >> 6)];
        *o++ = a[s[i + 2] & 63];
    }
    if (n - i == 1) {
        *o++ = a[s[i] >> 2];
        *o++ = a[(s[i] & 3) << 4];
    } else if (n - i == 2) {
        *o++ = a[s[i] >> 2];
        *o++ = a[((s[i] & 3) << 4) | (s[i + 1] >> 4)];
        *o++ = a[(s[i + 1] & 15) << 2];
    }
    *o = 0;
}

/* A token for raw header and payload bytes, signed with SECRET (malloc'd). */
static char *jwt_raw(const char *header, const char *payload)
{
    size_t hn = strlen(header), pn = strlen(payload);
    char *t = malloc(hn * 2 + pn * 2 + 64);
    b64url((const unsigned char *)header, hn, t);
    size_t l = strlen(t);
    t[l++] = '.';
    b64url((const unsigned char *)payload, pn, t + l);
    l += strlen(t + l);
    hmac_key k;
    uint8_t mac[32];
    hmac_sha256_key(&k, (const uint8_t *)SECRET, strlen(SECRET));
    hmac_sha256(&k, t, l, mac);
    t[l++] = '.';
    b64url(mac, 32, t + l);
    return t;
}
#define HS256 "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"
static char *jwt(const char *payload) { return jwt_raw(HS256, payload); }

/* The usual valid token: sub as given, a username, exp an hour ahead. */
static char *jwt_user(const char *sub, const char *username_json)
{
    char *p = fmt("{\"sub\":\"%s\",\"username\":%s,\"iat\":%ld,\"exp\":%ld}", sub, username_json, (long)time(NULL), (long)time(NULL) + 3600);
    char *t = jwt(p);
    free(p);
    return t;
}

/* ---------------------------------------------------------------- JSON */

static void put_utf8(char **o, unsigned cp)
{
    unsigned char *p = (unsigned char *)*o;
    if (cp < 0x80) *p++ = cp;
    else if (cp < 0x800) { *p++ = 0xc0 | cp >> 6; *p++ = 0x80 | (cp & 63); }
    else if (cp < 0x10000) { *p++ = 0xe0 | cp >> 12; *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
    else { *p++ = 0xf0 | cp >> 18; *p++ = 0x80 | (cp >> 12 & 63); *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
    *o = (char *)p;
}

/* UTF-8 for one code point (static buffer per call site is fine for tests) */
static char *cp(unsigned c)
{
    char *s = malloc(5), *o = s;
    put_utf8(&o, c);
    *o = 0;
    return s;
}

/* Decodes the JSON string starting after its opening quote into o (NUL-terminated); returns length. */
static size_t jdecode(const char *s, char *o)
{
    char *start = o;
    while (*s && *s != '"') {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        char e = *s++;
        switch (e) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned c = strtoul((char[]){s[0], s[1], s[2], s[3], 0}, NULL, 16);
            s += 4;
            if (c >= 0xd800 && c < 0xdc00 && s[0] == '\\' && s[1] == 'u') {
                unsigned lo = strtoul((char[]){s[2], s[3], s[4], s[5], 0}, NULL, 16);
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                s += 6;
            }
            put_utf8(&o, c);
            break;
        }
        default: *o++ = e;
        }
    }
    *o = 0;
    return o - start;
}

/* The string value of the first "key": in json, decoded (malloc'd), or NULL. */
static char *jstr(const char *json, const char *key)
{
    if (!json) return NULL;
    char *k = fmt("\"%s\":\"", key);
    const char *p = strstr(json, k);
    size_t kl = strlen(k);
    free(k);
    if (!p) return NULL;
    char *o = malloc(strlen(p) + 1);
    jdecode(p + kl, o);
    return o;
}

/* The integer value of the first "key": in json, or LLONG_MIN. */
static long long jnum(const char *json, const char *key)
{
    if (!json) return LLONG_MIN;
    char *k = fmt("\"%s\":", key);
    const char *p = strstr(json, k);
    size_t kl = strlen(k);
    free(k);
    return p ? strtoll(p + kl, NULL, 10) : LLONG_MIN;
}

static int streq(const char *a, const char *b) { return a && b && !strcmp(a, b); }

/* JSON string literal for UTF-8 text: ascii_only escapes everything above 0x7f as \u (surrogate pairs
 * beyond the BMP), otherwise only quote, backslash and control characters are escaped. */
static char *jlit(const char *s, int ascii_only)
{
    char *out = malloc(strlen(s) * 12 + 3), *o = out;
    *o++ = '"';
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        int n = 1;
        if (c >= 0xf0) { c = (c & 7) << 18 | (p[1] & 63) << 12 | (p[2] & 63) << 6 | (p[3] & 63); n = 4; }
        else if (c >= 0xe0) { c = (c & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63); n = 3; }
        else if (c >= 0xc0) { c = (c & 31) << 6 | (p[1] & 63); n = 2; }
        if (c == '"' || c == '\\') { *o++ = '\\'; *o++ = c; }
        else if (c < 0x20 || (ascii_only && c > 0x7e)) {
            if (c >= 0x10000) {
                unsigned v = c - 0x10000;
                o += sprintf(o, "\\u%04x\\u%04x", 0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
            } else {
                o += sprintf(o, "\\u%04x", c);
            }
        } else {
            memcpy(o, p, n);
            o += n;
        }
        p += n;
    }
    *o++ = '"';
    *o = 0;
    return out;
}

/* {"body":<literal>} for UTF-8 text */
static char *body_json(const char *text, int ascii_only)
{
    char *l = jlit(text, ascii_only), *b = fmt("{\"body\":%s}", l);
    free(l);
    return b;
}

/* Number of {"id": objects in a feed body, and the n-th one as a malloc'd copy of its JSON object. */
static int feed_count(const char *body)
{
    int n = 0;
    for (const char *p = body; (p = strstr(p, "{\"id\":")); p++) n++;
    return n;
}
static char *feed_item(const char *body, int i)
{
    const char *p = body;
    for (int k = 0; p && k <= i; k++) p = strstr(k ? p + 1 : p, "{\"id\":");
    if (!p) return NULL;
    /* objects contain no nested braces; strings may contain '}', so skip over strings */
    const char *q = p + 1;
    int instr = 0;
    for (; *q; q++) {
        if (instr) { if (*q == '\\') q++; else if (*q == '"') instr = 0; }
        else if (*q == '"') instr = 1;
        else if (*q == '}') break;
    }
    return strndup(p, q + 1 - p);
}
