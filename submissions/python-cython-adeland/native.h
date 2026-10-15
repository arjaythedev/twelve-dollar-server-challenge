#ifndef TWELVE_NATIVE_H
#define TWELVE_NATIVE_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/crypto.h>
#include <sqlite3.h>
#include "llhttp.h"
#include "yyjson.h"

#include "native/memory.h"

#define FD_CAP 65536
#define READY_CAP FD_CAP
#define INPUT_CAP 32768
#define WHEEL_SIZE 128
#define IDLE_SECONDS 75

enum ConnectionFlag {
    CF_READY = 1, CF_CLOSE = 2, CF_WRITE = 4, CF_READ = 8,
    CF_RDHUP = 16, CF_PEER_CLOSED = 32,
    CF_EVENTS = CF_WRITE | CF_READ | CF_RDHUP
};
enum Route {
    ROUTE_NOT_FOUND, ROUTE_HEALTH, ROUTE_FEED, ROUTE_POST, ROUTE_CREATE, ROUTE_LIKE
};

typedef struct __attribute__((aligned(64))) Connection {
    int fd; uint32_t flags;
    uint64_t generation;
    Buffer input;
    Output output;
    uint32_t parsed;
    Request *request;
    uint64_t deadline;
    int prev, next, bucket;
    llhttp_t *parser;
} Connection;
_Static_assert(sizeof(Connection) == 128, "aligned connection layout must be two cache lines");
_Static_assert(offsetof(Connection, request) + sizeof(Request*) == 64, "I/O and request fields must fit in the first 64 bytes");
_Static_assert(offsetof(Request, header) <= 64, "request metadata must fit in one cache line");
_Static_assert(sizeof(Request) <= 4096, "request metadata must use the minimum pool class");
typedef struct __attribute__((aligned(64))) Server {
    int epfd, listener;
    uint64_t head, tail, generation, start_ms, tick;
    Pool pool __attribute__((aligned(64)));
    sqlite3 *db;
    sqlite3_stmt *feed, *post, *create, *like, *exists, *health;
    EVP_MAC *mac;
    EVP_MAC_CTX *mac_context;
    Connection *connections;
    llhttp_t *parsers;
    llhttp_settings_t settings;
    yyjson_alc json_allocator;
    int wheel[WHEEL_SIZE];
    char json_arena[524288] __attribute__((aligned(64)));
    char decoded_header[8192], decoded_payload[16384], decoded_signature[64];
    uint64_t ready[READY_CAP] __attribute__((aligned(64)));
} Server;
_Static_assert(offsetof(Server, ready) % 64 == 0, "ready ring must be aligned");
_Static_assert(offsetof(Server, json_arena) % 64 == 0, "JSON scratch must be aligned");
static volatile sig_atomic_t running = 1;
static void stop_server(int signal_number) { (void)signal_number; running = 0; }
static uint64_t mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000; }
static Server *server_new(void) {
    Server *s=NULL;
    if (posix_memalign((void**)&s,64,sizeof(*s))) return NULL;
    memset(s,0,sizeof(*s));
    s->epfd=s->listener=-1;
    if (pool_init(&s->pool)) { free(s); return NULL; }
    return s;
}
static size_t connection_request_offset(void) { return offsetof(Connection,request); }
static int set_events(Server *s, Connection *c, uint32_t events) {
    struct epoll_event event = {0};
    uint32_t previous = ((c->flags & CF_WRITE) ? EPOLLOUT : 0)
                      | ((c->flags & CF_READ) ? EPOLLIN : 0)
                      | ((c->flags & CF_RDHUP) ? EPOLLRDHUP : 0);
    event.events = events | ((c->flags & (CF_CLOSE | CF_PEER_CLOSED)) ? 0 : EPOLLRDHUP);
    if (previous == event.events) return 0;
    event.data.u64 = (c->generation << 32) | (uint32_t)c->fd;
    if (epoll_ctl(s->epfd, EPOLL_CTL_MOD, c->fd, &event)) return -1;
    c->flags = (c->flags & ~CF_EVENTS)
             | ((events & EPOLLOUT) ? CF_WRITE : 0)
             | ((events & EPOLLIN) ? CF_READ : 0)
             | ((event.events & EPOLLRDHUP) ? CF_RDHUP : 0);
    return 0;
}

static int register_fd(Server *s, Connection *c) {
    struct epoll_event event = {0};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.u64 = (c->generation << 32) | (uint32_t)c->fd;
    if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, c->fd, &event)) return -1;
    c->flags |= CF_READ | CF_RDHUP;
    return 0;
}

static int make_listener(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1
        || bind(fd, (struct sockaddr *)&address, sizeof(address))
        || listen(fd, 4096)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int next_client(int listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
        int one = 1, send_size = 16384;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size));
    }
    return fd;
}
static int is_again(void){return errno==EAGAIN||errno==EWOULDBLOCK;}
static int is_interrupted(void){return errno==EINTR;}
static uint64_t event_tag(struct epoll_event *e){return e->data.u64;}
static void set_listener_event(struct epoll_event*e){memset(e,0,sizeof(*e));e->events=EPOLLIN;e->data.u64=UINT64_MAX;}
static void *parser_data(llhttp_t*p){return p->data;}
static void parser_set_data(llhttp_t*p,void*data){p->data=data;}
static Server *active_server;
static void install_signals(void){signal(SIGINT,stop_server);signal(SIGTERM,stop_server);signal(SIGPIPE,SIG_IGN);}
static int crypto_init(Server *s, const char *key, size_t n) {
    s->mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!s->mac) return -1;
    s->mac_context = EVP_MAC_CTX_new(s->mac);
    if (!s->mac_context) return -1;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string("digest", "SHA256", 0),
        OSSL_PARAM_construct_end()
    };
    return EVP_MAC_init(s->mac_context, (const unsigned char *)key, n, params) == 1 ? 0 : -1;
}

static void crypto_destroy(Server *s) {
    EVP_MAC_CTX_free(s->mac_context);
    EVP_MAC_free(s->mac);
}

static int hmac_verify(Server *s, const char *message, size_t n, const char *signature) {
    unsigned char digest[32];
    size_t len = 0;
    if (!EVP_MAC_init(s->mac_context, NULL, 0, NULL)
        || !EVP_MAC_update(s->mac_context, (const unsigned char *)message, n)
        || !EVP_MAC_final(s->mac_context, digest, &len, sizeof(digest))) return -1;
    return len == sizeof(digest) && CRYPTO_memcmp(digest, signature, sizeof(digest)) == 0 ? 0 : -1;
}
#endif
