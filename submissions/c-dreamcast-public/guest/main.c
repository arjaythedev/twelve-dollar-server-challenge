/* main.c - an HTTP/1.1 server for the $12 server challenge, running on a Sega Dreamcast.
 *
 * One KallistiOS thread: poll() over the listening socket and up to MAX_CONNS keep-alive
 * connections, parse with fbparse.h, answer with api.c (SQLite through vfs_hostfs.c), and once a
 * second redraw a dashboard of the traffic on the Dreamcast's video output. Connections past
 * MAX_CONNS are accepted and closed at once: 16 MB of RAM is a hard wall, so we refuse politely
 * rather than fall over. */
#include <kos.h>
#include <arch/timer.h>
#include <dc/biosfont.h>
#include <dc/video.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api.h"
#include "hostfs.h"
#include "sqlite3.h"
#define FP_FORCE_SCALAR 1
#include "fbparse.h"

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

#define MAX_CONNS 48
#define MAX_REQ (64 * 1024)
#define PAD 64 /* fbparse.h reads up to 64 bytes past the end */
#define IDLE_MS (10 * 60 * 1000)

typedef struct {
    int fd;
    char *in;
    int inlen, incap;
    buf_t out;
    int outoff;
    int close_after;
    uint64_t last_ms;
} conn_t;

static conn_t conns[MAX_CONNS];
static int nconns;

/* ---- traffic stats for the dashboard ---- */

#define HIST 100
static unsigned hist[HIST][ROUTE_COUNT]; /* requests per second, per route; hist[HIST-1] = this second */
static unsigned long long total, by_route[ROUTE_COUNT], by_class[6], refused;
static char recent[6][48];
static int recent_next;
static unsigned last_us;

static void record(int route, int status, const char *method, int mlen, const char *path, int plen, unsigned us) {
    hist[HIST - 1][route]++;
    total++;
    by_route[route]++;
    by_class[status / 100 < 6 ? status / 100 : 5]++;
    last_us = us;
    snprintf(recent[recent_next], sizeof recent[0], "%3d %.*s %.*s", status, mlen, method,
             plen > 28 ? 28 : plen, path);
    recent_next = (recent_next + 1) % 6;
}

/* ---- dashboard: plain pixels into the 640x480 RGB565 framebuffer ---- */

#define RGB(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
static const uint16_t route_color[ROUTE_COUNT] = {
    RGB(120, 120, 120), RGB(80, 160, 255), RGB(80, 220, 120), RGB(255, 170, 40), RGB(255, 80, 160), RGB(200, 60, 60),
};
static const char *route_name[ROUTE_COUNT] = {"health", "feed", "post", "create", "like", "other"};
#define BG RGB(10, 10, 40)
#define FG RGB(230, 230, 230)
#define ORANGE RGB(255, 120, 0)

static void fill(int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) {
        uint16_t *row = vram_s + (y + j) * 640 + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void text(int x, int y, uint16_t fg, const char *fmt, ...) {
    char s[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    bfont_draw_str_ex(vram_s + y * 640 + x, 640, fg, BG, 16, true, s);
}

static int port;
static char ipstr[20] = "?";
static uint64_t boot_ms;

static void draw(void) {
    uint64_t up = (timer_ms_gettime64() - boot_ms) / 1000;
    fill(0, 0, 640, 480, BG);
    text(20, 12, ORANGE, "DREAMCAST SQL");
    text(200, 12, FG, "SH-4 200MHz / 16MB / SQLite %s", sqlite3_libversion());
    text(20, 44, FG, "%s:%d  up %02u:%02u:%02u  conns %d/%d", ipstr, port, (unsigned)(up / 3600),
         (unsigned)(up / 60 % 60), (unsigned)(up % 60), nconns, MAX_CONNS);

    /* requests per second, last HIST seconds, stacked by route */
    unsigned peak = 1;
    for (int t = 0; t < HIST - 1; t++) {
        unsigned s = 0;
        for (int r = 0; r < ROUTE_COUNT; r++) s += hist[t][r];
        if (s > peak) peak = s;
    }
    const int gx = 20, gy = 84, gh = 180, bw = 6;
    fill(gx - 2, gy - 2, HIST * bw + 4, gh + 4, RGB(40, 40, 80));
    fill(gx, gy, HIST * bw, gh, RGB(0, 0, 20));
    for (int t = 0; t < HIST - 1; t++) { /* the current second is still filling up */
        int y = gy + gh;
        for (int r = 0; r < ROUTE_COUNT; r++) {
            int h = hist[t][r] * gh / peak;
            if (h > y - gy) h = y - gy;
            fill(gx + t * bw, y - h, bw - 1, h, route_color[r]);
            y -= h;
        }
    }
    unsigned now = 0;
    for (int r = 0; r < ROUTE_COUNT; r++) now += hist[HIST - 2][r];
    text(gx + 4, gy + 4, FG, "%u req/s  (peak %u)", now, peak);

    for (int r = 0; r < ROUTE_COUNT; r++) {
        fill(20 + (r % 3) * 200, 278 + (r / 3) * 28, 14, 14, route_color[r]);
        text(40 + (r % 3) * 200, 272 + (r / 3) * 28, FG, "%s %llu", route_name[r], by_route[r]);
    }
    text(20, 336, FG, "total %llu  2xx %llu  4xx %llu  5xx %llu  refused %llu", total, by_class[2], by_class[4],
         by_class[5], refused);
    struct mallinfo mi = mallinfo();
    text(20, 362, FG, "last %u.%03ums  heap free %dKB", last_us / 1000, last_us % 1000, (mi.fordblks) / 1024);
    for (int i = 0; i < 4; i++)
        text(20 + (i % 2) * 310, 400 + (i / 2) * 28, RGB(160, 160, 200), "%s",
             recent[(recent_next + 2 + i) % 6]);
}

static void tick(void) {
    memmove(hist[0], hist[1], sizeof hist - sizeof hist[0]);
    memset(hist[HIST - 1], 0, sizeof hist[0]);
}

/* ---- connections ---- */

static void drop(int i) {
    close(conns[i].fd);
    free(conns[i].in);
    free(conns[i].out.p);
    conns[i] = conns[--nconns];
}

static const char *reason(int s) {
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static void respond(conn_t *c, int status, buf_t *body) {
    char head[160];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n%s\r\n", status,
                     reason(status), body->len, c->close_after ? "Connection: close\r\n" : "");
    buf_t *o = &c->out;
    if (o->len + n + body->len > o->cap) {
        int cap = o->len + n + body->len + 256;
        char *p = realloc(o->p, cap);
        if (!p) { c->close_after = 1; return; }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, head, n);
    memcpy(o->p + o->len + n, body->p, body->len);
    o->len += n + body->len;
}

static buf_t body; /* reused JSON body buffer */

/* Handles every complete request in c->in. Returns -1 if the connection should be dropped now. */
static int serve(conn_t *c) {
    for (;;) {
        const char *method, *path;
        size_t mlen, plen, nh = 32;
        int minor;
        struct fb_header h[32];
        int r = fb_parse_request(c->in, c->inlen, &method, &mlen, &path, &plen, &minor, h, &nh, 0);
        if (r == -2) return c->inlen >= MAX_REQ ? -1 : 0;
        if (r < 0) return -1;
        long clen = 0;
        const char *authz = 0;
        int alen = 0;
        c->close_after = minor == 0;
        for (size_t i = 0; i < nh; i++) {
            if (h[i].name == 0) continue;
            if (h[i].name_len == 14 && strncasecmp(h[i].name, "content-length", 14) == 0)
                clen = strtol(h[i].value, 0, 10);
            else if (h[i].name_len == 13 && strncasecmp(h[i].name, "authorization", 13) == 0)
                authz = h[i].value, alen = h[i].value_len;
            else if (h[i].name_len == 10 && strncasecmp(h[i].name, "connection", 10) == 0)
                c->close_after = h[i].value_len == 5 && strncasecmp(h[i].value, "close", 5) == 0 ? 1
                                 : h[i].value_len == 10 && strncasecmp(h[i].value, "keep-alive", 10) == 0 ? 0
                                 : c->close_after;
        }
        if (clen < 0 || r + clen > MAX_REQ) {
            body.len = 0;
            c->close_after = 1;
            respond(c, 413, &body);
            record(ROUTE_OTHER, 413, method, mlen, path, plen, 0);
            return 0;
        }
        if (c->inlen < r + clen) return 0;

        uint64_t t0 = timer_us_gettime64();
        body.len = 0;
        int route;
        int status = api_handle(method, mlen, path, plen, authz, alen, c->in + r, (int)clen, &body, &route);
        if (body.oom) { body.oom = 0; body.len = 0; status = 500; }
        respond(c, status, &body);
        record(route, status, method, mlen, path, plen, (unsigned)(timer_us_gettime64() - t0));

        int used = r + (int)clen;
        memmove(c->in, c->in + used, c->inlen - used);
        c->inlen -= used;
        if (c->close_after) return 0;
    }
}

static int readable(conn_t *c) {
    if (c->incap - c->inlen - PAD < 1024) {
        int cap = c->incap ? c->incap * 2 : 4096;
        if (cap > MAX_REQ + 4096) cap = MAX_REQ + 4096;
        if (cap == c->incap) return -1;
        char *p = realloc(c->in, cap);
        if (!p) return -1;
        c->in = p;
        c->incap = cap;
    }
    int n = recv(c->fd, c->in + c->inlen, c->incap - c->inlen - PAD, 0);
    if (n <= 0) return -1;
    c->inlen += n;
    memset(c->in + c->inlen, 0, PAD);
    return serve(c);
}

static int writable(conn_t *c) {
    while (c->outoff < c->out.len) {
        int n = send(c->fd, c->out.p + c->outoff, c->out.len - c->outoff, 0);
        if (n <= 0) return n < 0 && errno == EAGAIN ? 0 : -1;
        c->outoff += n;
    }
    c->out.len = c->outoff = 0;
    return c->close_after ? -1 : 0;
}

static void halt(const char *why) {
    hostfs_log(why);
    text(20, 440, RGB(255, 60, 60), "%s", why);
    for (;;) thd_sleep(1000);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vid_set_mode(DM_640x480, PM_RGB565);
    boot_ms = timer_ms_gettime64();
    fill(0, 0, 640, 480, BG);
    text(20, 12, ORANGE, "DREAMCAST SQL  booting...");
    if (!hostfs_present()) halt("no hostfs syscall: run this in the patched Flycast");

    char err[160], pbuf[16];
    port = hostfs_getenv("PORT", pbuf, sizeof pbuf) > 0 ? atoi(pbuf) : 3000;
    if (api_init(err, sizeof err) < 0) halt(err);

    if (net_default_dev == NULL) halt("no network adapter (enable the BBA)");
    for (int i = 0; i < 100 && net_default_dev->ip_addr[0] == 0; i++) thd_sleep(100);
    snprintf(ipstr, sizeof ipstr, "%d.%d.%d.%d", net_default_dev->ip_addr[0], net_default_dev->ip_addr[1],
             net_default_dev->ip_addr[2], net_default_dev->ip_addr[3]);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 64) < 0) halt("listen failed");
    fcntl(ls, F_SETFL, O_NONBLOCK);
    snprintf(err, sizeof err, "listening on %s:%d, SQLite %s", ipstr, port, sqlite3_libversion());
    hostfs_log(err);

    struct pollfd pfd[MAX_CONNS + 1];
    uint64_t next_draw = 0;
    for (;;) {
        uint64_t now = timer_ms_gettime64();
        if (now >= next_draw) {
            if (next_draw) tick();
            draw();
            next_draw = (next_draw ? next_draw : now) + 1000;
            for (int i = nconns - 1; i >= 0; i--)
                if (now - conns[i].last_ms > IDLE_MS) drop(i);
        }
        pfd[0].fd = ls;
        pfd[0].events = POLLIN;
        for (int i = 0; i < nconns; i++) {
            pfd[i + 1].fd = conns[i].fd;
            pfd[i + 1].events = POLLIN | (conns[i].out.len > conns[i].outoff ? POLLOUT : 0);
            pfd[i + 1].revents = 0;
        }
        pfd[0].revents = 0;
        int n = nconns;
        if (poll(pfd, n + 1, 50) <= 0) continue;

        /* walk backwards so drop() (which moves the last conn into the hole) is safe */
        for (int i = n - 1; i >= 0; i--) {
            short ev = pfd[i + 1].revents;
            if (!ev) continue;
            conn_t *c = &conns[i];
            c->last_ms = now;
            int rc = 0;
            if (ev & (POLLIN | POLLHUP | POLLERR)) rc = readable(c);
            if (rc == 0 && c->out.len > c->outoff) rc = writable(c);
            if (rc < 0) drop(i);
        }
        if (pfd[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(ls, NULL, NULL);
                if (fd < 0) break;
                if (nconns == MAX_CONNS) {
                    close(fd);
                    refused++;
                    continue;
                }
                fcntl(fd, F_SETFL, O_NONBLOCK);
                conns[nconns] = (conn_t){.fd = fd, .last_ms = now};
                nconns++;
            }
        }
    }
}
