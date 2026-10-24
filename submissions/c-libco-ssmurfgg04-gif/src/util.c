/* util.c - one-page arena for connection buffers + helpers */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "util.h"

/* ---------------------------------------------------------------- arena */

/* Connection buffers come from a private arena of 4 KiB pages, reused
 * newest-first while cache-hot. An idle keep-alive connection therefore
 * holds no heap page at all, and a burst of traffic does not fragment
 * the heap. Bigger needs fall back to malloc. */
#define CHUNK 4096
#define CHUNK_MAX 65536            /* 256 MiB of address space, touched as needed */
#define CHUNK_HOT 128

static __thread char *a_base, *a_next, *a_end;
static __thread char *a_hot[CHUNK_HOT], **a_cold;
static __thread int a_nhot, a_ncold;

static char *chunk_get(void)
{
    if (a_nhot) return a_hot[--a_nhot];
    if (a_ncold) return a_cold[--a_ncold];
    if (!a_base) {
        void *m = mmap(NULL, (size_t)CHUNK * CHUNK_MAX,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        a_cold = malloc(CHUNK_MAX * sizeof *a_cold);
        if (m == MAP_FAILED || !a_cold) abort();
        a_base = a_next = m;
        a_end = a_base + (size_t)CHUNK * CHUNK_MAX;
    }
    if (a_next < a_end) {
        char *p = a_next;
        a_next += CHUNK;
        return p;
    }
    char *p = malloc(CHUNK);
    if (!p) abort();
    return p;
}

static int is_chunk(const char *p)
{
    return p >= a_base && p < a_end;
}

static void chunk_put(char *p)
{
    if (!is_chunk(p)) { free(p); return; }
    if (a_nhot < CHUNK_HOT) { a_hot[a_nhot++] = p; return; }
    madvise(p, CHUNK, MADV_DONTNEED);
    a_cold[a_ncold++] = p;
}

/* ---------------------------------------------------------------- buffers */

void buf_init(buf_t *b) { b->p = NULL; b->len = b->cap = 0; }

int buf_reserve(buf_t *b, size_t extra)
{
    if ((size_t)b->len + extra <= b->cap) return 0;
    if (b->cap == 0 && extra <= CHUNK) {
        b->p = chunk_get();
        b->cap = CHUNK;
        return 0;
    }
    size_t cap = b->cap ? b->cap : CHUNK;
    while (cap < (size_t)b->len + extra) cap *= 2;
    char *np;
    if (b->cap == CHUNK) {
        np = malloc(cap);
        if (np) memcpy(np, b->p, b->len);
        chunk_put(b->p);
    } else {
        np = realloc(b->p, cap);
    }
    if (!np) return -1;
    b->p = np;
    b->cap = (uint32_t)cap;
    return 0;
}

int buf_put(buf_t *b, const void *s, size_t n)
{
    if (n && buf_reserve(b, n)) return -1;
    if (n) memcpy(b->p + b->len, s, n);
    b->len += (uint32_t)n;
    return 0;
}

void buf_free(buf_t *b)
{
    if (b->p) {
        if (b->cap == CHUNK) chunk_put(b->p);
        else free(b->p);
    }
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---------------------------------------------------------------- time */

uint64_t now_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

double now_realtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Microsecond clock for request-path instrumentation. */
long long now_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
