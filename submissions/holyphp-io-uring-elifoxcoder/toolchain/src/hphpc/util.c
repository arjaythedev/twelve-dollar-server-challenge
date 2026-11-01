#include "util.h"
#include <stdarg.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "hphpc: out of memory\n"); exit(1); }
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n ? n : 1);
    if (!r) { fprintf(stderr, "hphpc: out of memory\n"); exit(1); }
    return r;
}
void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fprintf(stderr, "hphpc: out of memory\n"); exit(1); }
    return p;
}
char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}
char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "hphpc: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

char *fmt(const char *fmt_, ...) {
    va_list ap;
    va_start(ap, fmt_);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt_, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return xstrdup(""); }
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt_, ap2);
    va_end(ap2);
    return s;
}

void ptrvec_push(PtrVec *v, void *p) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, v->cap * sizeof(void *));
    }
    v->items[v->len++] = p;
}

void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }
static void buf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        b->cap = b->cap ? b->cap : 64;
        while (b->len + need + 1 > b->cap) b->cap *= 2;
        b->data = xrealloc(b->data, b->cap);
    }
}
void buf_putc(Buf *b, char c) { buf_grow(b, 1); b->data[b->len++] = c; b->data[b->len] = 0; }
void buf_write(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
void buf_puts(Buf *b, const char *s) { buf_write(b, s, strlen(s)); }
void buf_vprintf(Buf *b, const char *fmt_, va_list ap) {
    va_list ap1, ap2;
    va_copy(ap1, ap);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt_, ap1);
    va_end(ap1);
    if (n >= 0) {
        buf_grow(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt_, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}
void buf_printf(Buf *b, const char *fmt_, ...) {
    va_list ap;
    va_start(ap, fmt_);
    buf_vprintf(b, fmt_, ap);
    va_end(ap);
}
char *buf_take(Buf *b) {
    if (!b->data) { b->data = xmalloc(1); b->data[0] = 0; }
    char *s = b->data;
    buf_init(b);
    return s;
}

struct Arena {
    char *base;
    size_t used, cap;
    Arena *next;
};
Arena *arena_new(void) { return xcalloc(1, sizeof(Arena)); }
void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (a->used + n > a->cap) {
        size_t cap = 1 << 16;
        if (n > cap) cap = n;
        Arena *na = xcalloc(1, sizeof(Arena));
        na->cap = cap;
        na->base = xmalloc(cap);
        na->next = a->next;
        a->next = na;
        return arena_alloc(a, n);
    }
    void *p = a->next->base + a->next->used;
    a->next->used += n;
    return p;
}
void arena_free(Arena *a) {
    while (a->next) {
        Arena *n = a->next;
        a->next = n->next;
        free(n->base);
        free(n);
    }
    free(a);
}

/* ---- interning ---- */
typedef struct Intern Intern;
struct Intern {
    char *s;
    size_t hash;
    Intern *next;
};
static Intern *interns = NULL;

static size_t hash_bytes(const char *s, size_t n) {
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}
const char *intern_range(const char *start, size_t len) {
    size_t h = hash_bytes(start, len);
    for (Intern *i = interns; i; i = i->next)
        if (i->hash == h && strlen(i->s) == len && memcmp(i->s, start, len) == 0)
            return i->s;
    Intern *ni = xmalloc(sizeof(Intern));
    ni->s = xstrndup(start, len);
    ni->hash = h;
    ni->next = interns;
    interns = ni;
    return ni->s;
}
const char *intern(const char *s) { return intern_range(s, strlen(s)); }
