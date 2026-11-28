/* hphp_modules.h — shared declarations for the optional runtime modules.
 *
 * `hphp_uring.c` (epoll / io_uring / the generic stream server socket path)
 * and `hphp_sqlite.c` (SQLite binding, WAL shim, B-tree cursor binding) are
 * only linked when a program mentions one of their symbols. Both include this
 * header; so does hphp_std.c for the generic pieces (string builders, typed
 * int arrays, request scopes) that live there.
 *
 * Nothing here belongs in hphp_rt.h: that file is the core ABI owned by the
 * memory model work. hphp_ext.h carries the builtin prototypes the generated
 * program needs and is force-included by the compiler.
 */
#ifndef HPHP_MODULES_H
#define HPHP_MODULES_H

#include "hphp_rt.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
typedef SOCKET hp_sock;
#define HP_SOCK_BAD INVALID_SOCKET
#define hp_sock_close(s) closesocket(s)
#define hp_sock_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
typedef int hp_sock;
#define HP_SOCK_BAD (-1)
#define hp_sock_close(s) close(s)
#define hp_sock_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#endif

/* Free a block from hp_alloc(). The core has hp_alloc() but no public free
 * (until reference counting lands); the modules own their own blocks. */
static inline void hp_free(void *p) { if (p) free(p); }

/* Integer -> decimal into [end-24, end); returns the first digit. Same
 * helper the challenge runtime had in its hphp_rt.h. */
static inline char *hp_fmt_i64(char *end, int64_t v) {
    /* two digits per division step */
    static const char d2[201] =
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
        "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = end;
    while (u >= 100) {
        unsigned r = (unsigned)(u % 100);
        u /= 100;
        p -= 2; memcpy(p, d2 + 2 * r, 2);
    }
    if (u >= 10) { p -= 2; memcpy(p, d2 + 2 * u, 2); }
    else *--p = (char)('0' + u);
    if (v < 0) *--p = '-';
    return p;
}

/* Typed int array handle (iarr_*): one allocation, no hval boxing. */
typedef struct hp_iarr_t {
    int64_t *v;
    int64_t cap;
    struct hp_iarr_t *pool_next;
    bool leased;
    int64_t len;
} hp_iarr_t;

/* String builder handle (sb_*). `head` bytes in front of b.data[head..len)
 * are a prepend reserve, see hp_sb_reserve_pooled in hphp_std.c. */
typedef struct { Buf b; size_t head; } HpSb;
HpSb *sb_h(hval v);

/* Two ways a poll result can be collected: a normal PHP array or a leased
 * typed int array (epoll_wait_into / epoll_wait_iarr2). */
typedef struct {
    harr *values;
    hp_iarr_t *ints;
} HpFdList;

static inline bool hp_fdlist_has(const HpFdList *q) {
    return q && (q->values || (q->ints && q->ints->leased));
}

static inline void hp_fdlist_push(HpFdList *q, hp_sock fd) {
    if (!q) return;
    if (q->ints && q->ints->leased) {
        if (q->ints->len >= q->ints->cap) {
            int64_t ncap = q->ints->cap < 256 ? 256 : q->ints->cap * 2;
            if (ncap > (1 << 22)) return;
            int64_t *nv = (int64_t *)hp_alloc((size_t)ncap * sizeof *nv);
            if (!nv) return;
            memcpy(nv, q->ints->v, (size_t)q->ints->len * sizeof *nv);
            hp_free(q->ints->v);
            q->ints->v = nv;
            q->ints->cap = ncap;
        }
        q->ints->v[q->ints->len++] = (int64_t)fd;
    } else if (q->values) {
        hp_arr_push(q->values, hp_of_int((int64_t)fd));
    }
}

/* Typed int arrays are shared by the SQLite B-tree batch reads and the std
 * builtins, so the pool lives here (one per translation unit; the pool only
 * ever reuses released storage within that TU). */
#define HP_IARR_POOL_MAX 32
#define HP_IARR_POOL_CAP_MAX 4096
static _Thread_local hp_iarr_t *hp_iarr_pool;
static _Thread_local int hp_iarr_pool_count;

static inline hp_iarr_t *hp_iarr_acquire(int64_t n, bool clear) {
    hp_iarr_t **link = &hp_iarr_pool;
    hp_iarr_t *a = NULL;
    if (n < 1) n = 1;
    while (*link) {
        if ((*link)->cap >= n) {
            a = *link;
            *link = a->pool_next;
            hp_iarr_pool_count--;
            break;
        }
        link = &(*link)->pool_next;
    }
    if (!a) {
        a = (hp_iarr_t *)hp_alloc(sizeof *a);
        a->cap = n;
        a->v = (int64_t *)hp_alloc(sizeof(int64_t) * (size_t)n);
    }
    a->pool_next = NULL;
    a->leased = true;
    a->len = 0;
    if (clear) memset(a->v, 0, sizeof(int64_t) * (size_t)a->cap);
    return a;
}

static inline void hp_iarr_release_ptr(hp_iarr_t *a) {
    if (!a || !a->leased) return;
    a->leased = false;
    if (a->cap <= HP_IARR_POOL_CAP_MAX && hp_iarr_pool_count < HP_IARR_POOL_MAX) {
        a->pool_next = hp_iarr_pool;
        hp_iarr_pool = a;
        hp_iarr_pool_count++;
    } else {
        hp_free(a->v);
        hp_free(a);
    }
}

static inline hp_iarr_t *iarr_of(hval h) { return (hp_iarr_t *)(intptr_t)hp_val_to_int(h); }
static inline int hp_iarr_fast_unchecked_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("HPHP_IARR_FAST");
        /* The explicitly named *_fast builtins assume valid in-range indices.
         * Set HPHP_IARR_FAST=0 to retain bounds checks while diagnosing code. */
        enabled = !(v && v[0] == '0');
    }
    return enabled;
}
static inline int64_t hp_iarr_get_fast_impl(int64_t h, int64_t k) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)h;
    if (!a || !a->leased) return 0;
    if (!hp_iarr_fast_unchecked_enabled() && (k < 0 || k >= a->cap)) return 0;
    return a->v[k];
}
static inline bool hp_iarr_set_fast_impl(int64_t h, int64_t k, int64_t v) {
    hp_iarr_t *a = (hp_iarr_t *)(intptr_t)h;
    if (!a || !a->leased) return true;
    if (!hp_iarr_fast_unchecked_enabled() && (k < 0 || k >= a->cap)) return true;
    a->v[k] = v;
    return true;
}

/* Request scope: a compatibility no-op while the core has no arena (phase 1
 * of the memory plan adds it). Values are plain heap blocks, so persisting is
 * the identity. The builtins still exist so the challenge server keeps its
 * shape and phase 2 can delete the calls. */
void hp_scope_begin(void);
void hp_scope_end(void);
hstr *hp_str_persist(hstr *s);
int64_t hp_live_allocs(void);
int64_t hp_live_bytes(void);

/* Runtime counters the optional modules bump (hphp_uring.c owns the array). */
enum {
    HP_ST_REQ = 0,
    HP_ST_ENTER,
    HP_ST_SQE_SUB,
    HP_ST_SQE_PUSH,
    HP_ST_SQE_PARK,
    HP_ST_CQE,
    HP_ST_CQE_RECV,
    HP_ST_CQE_SEND,
    HP_ST_CQE_POLL,
    HP_ST_ARM_RECV,
    HP_ST_ARM_SEND,
    HP_ST_ARM_POLL,
    HP_ST_ARM_FAIL,
    HP_ST_CLOSE,
    HP_ST_ACCEPT,
    HP_ST_SEND_CHUNK,
    HP_ST_STR_NEW,
    HP_ST_SCOPE_END,
    HP_ST_N
};
extern uint64_t hp_st[HP_ST_N];
hstr *hp_runtime_stats_t(void);

/* Optional timing diagnostics (HPHP_DIAG=1), shared by both modules. */
static inline int hp_diag_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("HPHP_DIAG");
        enabled = e && strcmp(e, "1") == 0;
    }
    return enabled;
}
static inline uint64_t hp_diag_now_ns(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64() * 1000000ULL;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* hbuf_reserve is part of the challenge runtime's builder API but not of the
 * core header: reserve capacity for a TOTAL of n bytes (plus NUL). */
void hbuf_reserve(Buf *b, size_t n);

/* Response buffer pool (hphp_std.c): shared by the string builder and the
 * transport's send path. */
#define HP_RESP_BUF_POOL_MAX 8192
#define HP_RESP_BUF_POOL_CAP_MAX (16u * 1024u)
#define HP_RESP_BUF_POOL_BYTES_MAX (32u * 1024u * 1024u)
#define HP_RESP_BUF_SMALL_MAX 4096u
char *hp_resp_buf_acquire(size_t *cap);
char *hp_resp_buf_acquire_min(size_t want, size_t *cap);
void hp_resp_buf_release(char *data, size_t cap);

/* Typed fast-path ABI (the challenge codegen called these directly). The
 * normal hval ABI in hphp_std.c uses them internally where it helps. */
int64_t hp_sb_new_t(void);
bool hp_sb_reserve_t(int64_t h, int64_t n);
int64_t hp_sb_data_ptr_t(int64_t h);
bool hp_sb_discard_t(int64_t h);
int64_t hp_sb_len_t(int64_t h);
bool hp_sb_prepend_t(int64_t h, hstr *s);
bool hp_sb_int_t(int64_t h, int64_t n);
bool hp_sb_put_t(int64_t h, hstr *x);
bool hp_sb_bytes_t(int64_t h, int64_t ptr, int64_t n);
bool hp_sb_json_t(int64_t h, int64_t ptr, int64_t len);
bool hp_sb_json_str_t(int64_t h, hstr *s);
hstr *hp_sb_take_t(int64_t h);
int64_t hp_stream_send_sb_t(int64_t fd, int64_t sb);
int64_t hp_int_digits_t(int64_t n);
int64_t hp_iarr_new_t(int64_t n);
int64_t hp_iarr_new_uninit_t(int64_t n);
bool hp_iarr_release_t(int64_t h);
int64_t hp_iarr_get_t(int64_t h, int64_t k);
bool hp_iarr_set_t(int64_t h, int64_t k, int64_t v);
int64_t hp_iarr_get_fast_t(int64_t h, int64_t k);
bool hp_iarr_set_fast_t(int64_t h, int64_t k, int64_t v);
int64_t hp_iarr_data_ptr_t(int64_t h);
int64_t hp_iarr_len_t(int64_t h);
int64_t hp_strlen_t(hstr *s);
int64_t hp_str_to_int_t(hstr *s);
int64_t hp_str_to_int_range_t(hstr *s, int64_t start, int64_t end);
int64_t hp_http_request_bounds_t(hstr *s);
int64_t hp_str_find_range_t(hstr *s, hstr *needle, int64_t start);
bool hp_str_at_eq_t(hstr *s, int64_t start, int64_t len, hstr *lit);
int64_t hp_nofile_limit_t(void);
bool hp_uring_active_t(void);
bool hp_uring_direct_t(void);
int64_t hp_uring_direct_slots_t(void);
bool hp_epoll_mod_t(int64_t fd, int64_t mask);
bool hp_epoll_wait_iarr2_t(int64_t in, int64_t out, int64_t timeout_ms);
int64_t hp_stream_send_nb_t(int64_t fd, hstr *s);
int64_t hp_stream_send2_nb_t(int64_t fd, hstr *a, hstr *b);
hstr *hp_stream_recv_t(int64_t fd, int64_t maxn);
int64_t hp_stream_recv_release_t(int64_t fd);

#endif
