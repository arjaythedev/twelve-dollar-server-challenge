/* pgo/gen.c - a tiny load generator that drives the server build for its
 * PGO training run: the same request mix as bench/load.js (feed, post read,
 * like, create), with tokens signed for the training secret. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "jwt.h"

#define PORT_DEFAULT 18081
#define RUN_SECONDS 10
#define NCONN 48

static char tokens[NCONN][512];
static int token_len[NCONN];

static int read_full_response(int fd, char *buf, size_t cap)
{
    size_t have = 0;
    size_t body_need = 0;
    int header_done = 0;
    for (;;) {
        if (have >= cap - 1) return -1;
        if (!header_done || have < body_need) {
            ssize_t r = recv(fd, buf + have, cap - 1 - have, 0);
            if (r <= 0) return -1;
            have += (size_t)r;
            buf[have] = 0;
        }
        if (!header_done) {
            char *he = strstr(buf, "\r\n\r\n");
            if (!he) continue;
            header_done = 1;
            char *cl = strcasestr(buf, "content-length:");
            if (!cl) return -1;
            body_need = (size_t)(he + 4 - buf) + (size_t)strtoul(cl + 15, NULL, 10);
        }
        if (have >= body_need) return (int)body_need;
    }
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : PORT_DEFAULT;
    int secs = argc > 2 ? atoi(argv[2]) : RUN_SECONDS;

    /* tokens signed like the seed's (uid i+1, username pgo_i), long exp */
    jwt_b64url_init();
    jwt_init("pgo-training-secret", strlen("pgo-training-secret"));
    for (int i = 0; i < NCONN; i++) {
        char payload[256];
        int n = snprintf(payload, sizeof payload,
                         "{\"sub\":\"%d\",\"username\":\"pgo_%d\",\"iat\":0,\"exp\":2000000000}",
                         i + 1, i + 1);
        /* base64url encode the payload by hand */
        static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        char b64[384];
        int bl = 0;
        for (int j = 0; j + 2 < n; j += 3) {
            uint32_t v = (uint32_t)(unsigned char)payload[j] << 16 |
                         (uint32_t)(unsigned char)payload[j + 1] << 8 |
                         (uint32_t)(unsigned char)payload[j + 2];
            b64[bl++] = A[v >> 18 & 63]; b64[bl++] = A[v >> 12 & 63];
            b64[bl++] = A[v >> 6 & 63]; b64[bl++] = A[v & 63];
        }
        int rem = n % 3;
        if (rem == 1) {
            uint32_t v = (uint32_t)(unsigned char)payload[n - 1] << 16;
            b64[bl++] = A[v >> 18 & 63]; b64[bl++] = A[v >> 12 & 63];
        } else if (rem == 2) {
            uint32_t v = (uint32_t)(unsigned char)payload[n - 2] << 16 |
                         (uint32_t)(unsigned char)payload[n - 1] << 8;
            b64[bl++] = A[v >> 18 & 63]; b64[bl++] = A[v >> 12 & 63]; b64[bl++] = A[v >> 6 & 63];
        }
        /* fixed HS256 header */
        const char *hdr = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
        char msg[768], mac[32], sig[64];
        int ml = snprintf(msg, sizeof msg, "%s.%.*s", hdr, bl, b64);
        hmac_sha256_str(msg, (size_t)ml, (uint8_t *)mac);
        int sl = 0;
        for (int j = 0; j < 30; j += 3) {
            uint32_t v = (uint32_t)(uint8_t)mac[j] << 16 | (uint32_t)(uint8_t)mac[j + 1] << 8 | (uint8_t)mac[j + 2];
            sig[sl++] = A[v >> 18 & 63]; sig[sl++] = A[v >> 12 & 63];
            sig[sl++] = A[v >> 6 & 63]; sig[sl++] = A[v & 63];
        }
        uint32_t v = (uint32_t)(uint8_t)mac[30] << 16 | (uint32_t)(uint8_t)mac[31] << 8;
        sig[sl++] = A[v >> 18 & 63]; sig[sl++] = A[v >> 12 & 63]; sig[sl++] = A[v >> 6 & 63];
        token_len[i] = snprintf(tokens[i], sizeof tokens[i], "Bearer %s.%.*s.%.*s",
                               hdr, bl, b64, sl, sig);
    }

    int fds[NCONN];
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(0x7f000001);
    for (int i = 0; i < NCONN; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0 || connect(fds[i], (struct sockaddr *)&addr, sizeof addr)) {
            perror("connect");
            return 1;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    char req[1024], rbuf[16536];
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned seed = 42;
    long nreq = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= secs) break;
        for (int i = 0; i < NCONN; i++) {
            seed = seed * 1103515245 + 12345;
            int pick = (seed >> 16) % 100;

            int n = snprintf(req, sizeof req, "GET /feed HTTP/1.1\r\nHost: t\r\n\r\n");
            if (send_all(fds[i], req, (size_t)n)) return 1;
            if (read_full_response(fds[i], rbuf, sizeof rbuf) < 0) return 1;

            n = snprintf(req, sizeof req, "GET /posts/%d HTTP/1.1\r\nHost: t\r\n\r\n",
                         1 + (seed >> 8) % 4000);
            if (send_all(fds[i], req, (size_t)n)) return 1;
            if (read_full_response(fds[i], rbuf, sizeof rbuf) < 0) return 1;

            if (pick < 30) { /* like */
                n = snprintf(req, sizeof req,
                             "POST /posts/%d/like HTTP/1.1\r\nHost: t\r\nAuthorization: %.*s\r\n"
                             "Content-Length: 0\r\n\r\n",
                             1 + (seed >> 8) % 4000, token_len[i], tokens[i]);
                if (send_all(fds[i], req, (size_t)n)) return 1;
                if (read_full_response(fds[i], rbuf, sizeof rbuf) < 0) return 1;
            }
            if (pick < 6) { /* create */
                char body[128];
                int bl = snprintf(body, sizeof body,
                                  "{\"body\":\"pgo training post %ld says the server database index cache\"}",
                                  nreq);
                n = snprintf(req, sizeof req,
                             "POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: %.*s\r\n"
                             "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                             token_len[i], tokens[i], bl, body);
                if (send_all(fds[i], req, (size_t)n)) return 1;
                if (read_full_response(fds[i], rbuf, sizeof rbuf) < 0) return 1;
            }
            nreq++;
        }
    }
    for (int i = 0; i < NCONN; i++) close(fds[i]);
    printf("gen: %ld request rounds\n", nreq);
    return 0;
}
