/* pkgnet.c — minimal HTTP/1.1 client (no TLS, no deps).
 *
 * Enough for a package registry: GET / POST with small request bodies and
 * binary-safe response reading (Content-Length or connection-close).
 */
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1    /* getaddrinfo, struct addrinfo on glibc */
#endif
#include "pkgnet.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define hp_close(s) closesocket(s)
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#define hp_close(s) close(s)
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

static bool net_init(void) {
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    return true;
#endif
}

typedef struct { char host[256]; char port[16]; char path[1024]; } UrlParts;

static bool url_parse(const char *url, UrlParts *u) {
    memset(u, 0, sizeof *u);
    if (strncmp(url, "http://", 7) != 0) return false;
    url += 7;
    const char *slash = strchr(url, '/');
    const char *colon = strchr(url, ':');
    size_t hl;
    if (slash && colon && colon < slash) hl = (size_t)(colon - url);
    else if (slash) hl = (size_t)(slash - url);
    else hl = strlen(url);
    if (hl >= sizeof u->host) return false;
    memcpy(u->host, url, hl);
    u->host[hl] = 0;
    snprintf(u->port, sizeof u->port, "80");
    if (colon && (!slash || colon < slash)) {
        size_t pl = (slash ? (size_t)(slash - colon - 1) : strlen(colon + 1));
        if (pl >= sizeof u->port) pl = sizeof u->port - 1;
        memcpy(u->port, colon + 1, pl);
        u->port[pl] = 0;
    }
    snprintf(u->path, sizeof u->path, "%s", slash ? slash : "/");
    return u->host[0] != 0;
}

static SOCKET tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    /* try every resolved address: "localhost" often yields ::1 first, and a
     * server bound to IPv4-only 0.0.0.0 rejects that — the next entry wins */
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
        hp_close(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

/* read from socket until closed; grow buffer */
static char *recv_all(SOCKET s, size_t *out_len) {
    size_t cap = 8192, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
        int r = (int)recv(s, buf + len, (int)(cap - len - 1), 0);
        if (r <= 0) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static char *http_request(const char *method, const char *url, const char *ctype,
                          const char *body, size_t body_len, int *status) {
    *status = 0;
    UrlParts u;
    if (!url_parse(url, &u)) { fprintf(stderr, "hphp: only http:// registry URLs supported (%s)\n", url); return NULL; }
    if (!net_init()) return NULL;
    SOCKET s = tcp_connect(u.host, u.port);
    if (s == INVALID_SOCKET) return NULL;

    const char *len_hdr = body ? "" : "Content-Length: 0\r\n";
    char hdr[2048];
    if (body)
        snprintf(hdr, sizeof hdr,
                 "%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n"
                 "Content-Type: %s\r\nContent-Length: %zu\r\nUser-Agent: hphp/1.0\r\n\r\n",
                 method, u.path, u.host, u.port, ctype ? ctype : "application/octet-stream", body_len);
    else
        snprintf(hdr, sizeof hdr,
                 "%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n%s"
                 "User-Agent: hphp/1.0\r\nAccept: */*\r\n\r\n",
                 method, u.path, u.host, u.port, len_hdr);

    if (send(s, hdr, (int)strlen(hdr), 0) == SOCKET_ERROR ||
        (body_len && send(s, body, (int)body_len, 0) == SOCKET_ERROR)) {
        hp_close(s);
        return NULL;
    }

    size_t raw_len;
    char *raw = recv_all(s, &raw_len);
    hp_close(s);
    if (!raw || raw_len == 0) { free(raw); return NULL; }

    /* status line: HTTP/1.x NNN ... */
    if (strncmp(raw, "HTTP/", 5) != 0) { free(raw); return NULL; }
    *status = atoi(raw + 9);

    /* split headers/body */
    char *body_start = strstr(raw, "\r\n\r\n");
    size_t skip = 4;
    if (!body_start) { body_start = strstr(raw, "\n\n"); skip = 2; }
    if (!body_start) { free(raw); return NULL; }
    body_start += skip;

    /* honor Content-Length if smaller than what arrived */
    size_t total = raw_len - (size_t)(body_start - raw);
    char *cl = strstr(raw, "ontent-Length:");            /* case-insensitive-ish */
    if (!cl) cl = strstr(raw, "ONTENT-LENGTH:");
    if (cl && cl < body_start) {
        size_t want = (size_t)strtoull(cl + 15, NULL, 10);
        if (want < total) total = want;
    }
    char *out = xmalloc(total + 1);
    memcpy(out, body_start, total);
    out[total] = 0;
    free(raw);
    return out;
}

char *pkgnet_get(const char *url, size_t *out_len, int *status) {
    size_t n;
    char *b = http_request("GET", url, NULL, NULL, 0, status);
    if (b && out_len) { n = strlen(b); *out_len = n; }
    return b;
}

char *pkgnet_post(const char *url, const char *ctype,
                  const char *body, size_t len, int *status) {
    return http_request("POST", url, ctype, body, len, status);
}
