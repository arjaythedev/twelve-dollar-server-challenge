#ifndef ASCEND_ABI_H
#define ASCEND_ABI_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASC_ABI 0x4153430au
#ifndef ASC_MODULE_ENTRY
#define ASC_MODULE_ENTRY asc_module
#endif
#if !defined(ASC_PORTABLE) && (defined(__x86_64__) || defined(__i386__))
#define ASC_X86 1
#else
#define ASC_X86 0
#endif
#define ASC_HEADER_MAX (1u << 14)
#define ASC_BODY_MAX (1u << 14)
#define ASC_BUFFER_MAX (1u << 22)

typedef struct { const char *p; size_t n; } Slice;
/* 32-bit lengths keep Connection at 64 bytes. */
typedef struct { char *p; uint32_t n, cap; int failed; } Buffer;
static inline int buffer_add(Buffer *b, const void *p, size_t n) {
    if (b->failed || n > ASC_BUFFER_MAX - b->n) { b->failed = 1; return 0; }
    size_t need = b->n + n;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < need) cap <<= 1;
        char *next = realloc(b->p, cap);
        if (!next) { b->failed = 1; return 0; }
        b->p = next; b->cap = (uint32_t)cap;
    }
    if (n) memcpy(b->p + b->n, p, n);
    b->n = (uint32_t)need; return 1;
}
static inline int buffer_text(Buffer *b, const char *s) { return buffer_add(b, s, strlen(s)); }
static inline void buffer_clear(Buffer *b) { b->n = 0; b->failed = 0; }
static inline void buffer_free(Buffer *b) { free(b->p); memset(b, 0, sizeof(*b)); }

enum { ROUTE_NONE, ROUTE_HEALTH, ROUTE_FEED, ROUTE_POST, ROUTE_CREATE, ROUTE_LIKE };
enum { AUTH_OK, AUTH_MISSING, AUTH_INVALID, AUTH_PAYLOAD };
enum { BODY_OK, BODY_MALFORMED, BODY_REQUIRED, BODY_LONG };
enum { STORE_OK, STORE_MISSING, STORE_ERROR };

typedef struct { uint32_t abi; size_t size; const char *name; } ModuleHeader;
typedef struct {
    ModuleHeader h;
    size_t (*special)(const unsigned char *, size_t);
    size_t (*header_end)(const unsigned char *, size_t);
    int (*decimal)(Slice, int64_t *);
    unsigned (*route)(Slice method, Slice path, Slice *id);
    int (*base64url)(Slice, unsigned char *, size_t capacity, size_t *length);
} MathAPI;

typedef struct {
    int64_t id, likes;
    Slice body, created, author;
} Post;
typedef struct {
    char sub[64], username[ASC_HEADER_MAX + 1];
    size_t sub_n, username_n;
    double exp;
    unsigned payload_valid, exp_valid;
} Claims;
typedef struct { char text[2001]; size_t n, characters; } Body;
typedef struct {
    ModuleHeader h;
    int (*body)(Slice, Body *);
    int (*jwt_header)(Slice);
    int (*claims)(Slice, Claims *);
    int (*string)(Buffer *, Slice, const MathAPI *);
    int (*integer)(Buffer *, int64_t);
    int (*post)(Buffer *, const Post *, const MathAPI *);
} CodecAPI;

typedef struct { int64_t id; char username[ASC_HEADER_MAX + 1]; size_t username_n; } User;
typedef struct {
    ModuleHeader h;
    void *(*open)(const char *secret);
    void (*close)(void *);
    unsigned (*verify)(void *, Slice, const CodecAPI *, const MathAPI *, User *);
} AuthAPI;

typedef int (*PostVisitor)(void *, const Post *);
typedef struct { int64_t id; char created[32]; size_t created_n; } Created;
typedef struct {
    ModuleHeader h;
    void *(*open)(const char *path);
    void (*close)(void *);
    int (*health)(void *);
    int (*feed)(void *, PostVisitor, void *);
    int (*post)(void *, int64_t, PostVisitor, void *);
    int (*create)(void *, int64_t, Slice, Created *);
    int (*like)(void *, int64_t, int64_t, int *already);
} StoreAPI;

typedef struct { Slice method, path, authorization, body; int close, expect_continue; } Request;
typedef struct { int status; Buffer body; } Response;
typedef struct Context Context;
typedef struct { ModuleHeader h; void (*handle)(Context *, const Request *, Response *); } AppAPI;

/* This layout must survive module reloads. */
typedef struct {
    int fd;
    uint32_t events;
    Buffer input, output;
    uint32_t sent;
    uint8_t eof, close_after, continue_sent, scheduled;
} Connection;
_Static_assert(sizeof(Connection)==64,"connection state must occupy one 64-byte cache line");
typedef struct {
    int listener, epoll;
    void *internal;
    uint64_t requests, accepted;
} TransportState;
typedef struct {
    ModuleHeader h;
    size_t state_size, connection_size;
    TransportState *(*open)(const char *, const char *);
    int (*step)(TransportState *, Context *, int timeout_ms);
    void (*close)(TransportState *);
} TransportAPI;

struct Context {
    const MathAPI *math;
    const CodecAPI *codec;
    const AuthAPI *auth;
    const StoreAPI *store;
    const AppAPI *app;
    const TransportAPI *transport;
    void *authentication, *database;
    struct timespec started;
};

#endif
