/* hphp_rt.c — HolyPHP runtime: values, strings, arrays (COW), exceptions.
 *
 * Memory model:
 *   - strings, arrays, closures and generated objects use reference counts
 *   - arrays use copy-on-write through owner-aware mutation helpers
 *   - generated code and runtime containers release their owned values
 *   - raw pointers only exist inside unsafe blocks; runtime helpers used
 *     there map directly onto C (malloc/free/memcpy/memset)
 *
 * POSIX: _DEFAULT_SOURCE must be defined before any system include (glibc
 * hides POSIX symbols under strict -std=c11 otherwise).
 */
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif

/* thread-local interpreter state (see hphp_thr.c): every worker thread
 * runs its own try-stack, exception slot and closure environment
 * (HP_THREAD_QUAL comes from hphp_rt.h) */
#include "hphp_rt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ---------------- allocation ---------------- */
void *hp_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "Fatal error: Allowed memory size exhausted\n");
        exit(255);
    }
    return p;
}

/* Trial-deletion collector for identity-bearing objects, arrays and
 * closures. Each participant carries a node pointer in its header; tracing
 * callbacks expose only strong edges. */
struct hp_gc_node {
    void *ptr;
    uint32_t *rc;
    hp_gc_node **slot;
    void (*trace)(void *);
    void (*clear)(void *);
    void (*destroy)(void *);
    int64_t trial;
    bool marked;
    uint64_t epoch;
    struct hp_gc_node *prev;
    struct hp_gc_node *next;
};

typedef struct hp_region_block {
    struct hp_region_block *next;
    size_t used, cap;
    max_align_t align;
    unsigned char data[];
} hp_region_block;

void hp_region_begin(hp_region *region) {
    if (region) region->blocks = region->current = region->tail =
        region->active_tail = NULL;
}

void hp_region_reset(hp_region *region) {
    if (!region) return;
    hp_region_block *active = (hp_region_block *)region->active_tail;
    if (!active) { region->current = region->blocks; return; }
    for (hp_region_block *block = (hp_region_block *)region->blocks;
         block; block = block->next) {
        block->used = 0;
        if (block == active) break;
    }
    region->current = region->blocks;
    region->active_tail = NULL;
}

static void *hp_region_alloc(hp_region *region, size_t size) {
    if (!region) return NULL;
    size_t align = _Alignof(max_align_t);
    size = (size + align - 1) & ~(align - 1);
    hp_region_block *block = (hp_region_block *)region->current;
    while (block && block->cap - block->used < size) block = block->next;
    if (!block) {
        size_t cap = size > 4096 ? size : 4096;
        hp_region_block *fresh = (hp_region_block *)malloc(
            offsetof(hp_region_block, data) + cap);
        if (!fresh) { fprintf(stderr, "Fatal error: out of memory\n"); exit(255); }
        fresh->next = NULL;
        fresh->used = 0;
        fresh->cap = cap;
        if (region->tail) ((hp_region_block *)region->tail)->next = fresh;
        else region->blocks = fresh;
        region->tail = fresh;
        block = fresh;
    }
    region->current = block;
    region->active_tail = block;
    void *out = block->data + block->used;
    block->used += size;
    return out;
}

void hp_region_end(hp_region *region) {
    if (!region) return;
    hp_region_block *block = (hp_region_block *)region->blocks;
    while (block) {
        hp_region_block *next = block->next;
        free(block);
        block = next;
    }
    region->blocks = region->current = region->tail = region->active_tail = NULL;
}

hstr *hp_region_str_copy(hp_region *region, const hstr *source) {
    size_t len = source ? source->len : 0;
    hstr *copy = (hstr *)hp_region_alloc(region, sizeof(*copy) + len + 1);
    if (!copy) return hp_null_str();
    copy->len = len;
    copy->cap = len + 1;
    copy->hash = 0;
    copy->rc = UINT32_MAX; /* region lifetime, never individual RC release */
    if (len) memcpy(copy->data, source->data, len);
    copy->data[len] = 0;
    return copy;
}
static hp_gc_node *hp_gc_nodes;
static size_t hp_gc_node_count;
#ifdef HP_RUNTIME_NO_THREADS
static uint16_t hp_gc_release_count;
#else
static HP_THREAD_QUAL uint16_t hp_gc_release_count;
#endif
static bool hp_gc_collecting;
#ifdef HP_RUNTIME_NO_THREADS
static unsigned hp_gc_drop_depth;
#else
static HP_THREAD_QUAL unsigned hp_gc_drop_depth;
#endif
static uint64_t hp_gc_epoch;
static hp_gc_node **hp_gc_work;
static size_t hp_gc_work_len, hp_gc_work_cap;
static enum { HP_GC_IDLE, HP_GC_GATHER, HP_GC_SUBTRACT, HP_GC_MARK } hp_gc_mode;
static bool hp_gc_oom;
#ifdef HP_RUNTIME_NO_THREADS
void hp_runtime_thread_starting(void) { }
static inline void hp_gc_lock(void) { }
static inline void hp_gc_unlock(void) { }
static inline uint32_t hp_rc_retain(uint32_t *rc) {
    uint32_t old = *rc;
    if (old != UINT32_MAX) *rc = old + 1;
    return old;
}
static inline uint32_t hp_rc_release(uint32_t *rc) {
    uint32_t old = *rc;
    if (old != 0 && old != UINT32_MAX) *rc = old - 1;
    return old;
}
static inline uint32_t hp_rc_count(uint32_t *rc) { return *rc; }
#else
static pthread_mutex_t hp_gc_mu;
static pthread_once_t hp_gc_mu_once = PTHREAD_ONCE_INIT;
static bool hp_threads_started;

void hp_runtime_thread_starting(void) {
    hp_threads_started = true;
}

static void hp_gc_mu_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&hp_gc_mu, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void hp_gc_lock(void) {
    if (!hp_threads_started) return;
    pthread_once(&hp_gc_mu_once, hp_gc_mu_init);
    pthread_mutex_lock(&hp_gc_mu);
}

static void hp_gc_unlock(void) {
    if (hp_threads_started) pthread_mutex_unlock(&hp_gc_mu);
}

static uint32_t hp_rc_retain(uint32_t *rc) {
    if (!hp_threads_started) {
        uint32_t old = *rc;
        if (old != UINT32_MAX) *rc = old + 1;
        return old;
    }
    uint32_t old = __atomic_load_n(rc, __ATOMIC_RELAXED);
    while (old != UINT32_MAX &&
           !__atomic_compare_exchange_n(rc, &old, old + 1, true,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
    return old;
}

static uint32_t hp_rc_release(uint32_t *rc) {
    if (!hp_threads_started) {
        uint32_t old = *rc;
        if (old != 0 && old != UINT32_MAX) *rc = old - 1;
        return old;
    }
    uint32_t old = __atomic_load_n(rc, __ATOMIC_RELAXED);
    while (old != 0 && old != UINT32_MAX &&
           !__atomic_compare_exchange_n(rc, &old, old - 1, true,
               __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) { }
    return old;
}

static inline uint32_t hp_rc_count(uint32_t *rc) {
    return hp_threads_started ? __atomic_load_n(rc, __ATOMIC_RELAXED) : *rc;
}
#endif

static bool hp_gc_work_push(hp_gc_node *node) {
    if (hp_gc_work_len == hp_gc_work_cap) {
        size_t cap = hp_gc_work_cap ? hp_gc_work_cap * 2 : 64;
        hp_gc_node **items = (hp_gc_node **)realloc(hp_gc_work, cap * sizeof(*items));
        if (!items) return false;
        hp_gc_work = items;
        hp_gc_work_cap = cap;
    }
    hp_gc_work[hp_gc_work_len++] = node;
    return true;
}

static void hp_gc_unregister(hp_gc_node **slot) {
    hp_gc_lock();
    if (!slot || !*slot) { hp_gc_unlock(); return; }
    hp_gc_node *node = *slot;
    if (node->prev) node->prev->next = node->next;
    else if (hp_gc_nodes == node) hp_gc_nodes = node->next;
    if (node->next) node->next->prev = node->prev;
    if (hp_gc_node_count) hp_gc_node_count--;
    *slot = NULL;
    free(node);
    hp_gc_unlock();
}

void hp_gc_register(void *ptr, uint32_t *rc, hp_gc_node **slot,
                    void (*trace)(void *), void (*clear)(void *),
                    void (*destroy)(void *)) {
    hp_gc_lock();
    if (!ptr || !rc || !slot || *slot || hp_gc_collecting) {
        hp_gc_unlock();
        return;
    }
    hp_gc_node *node = (hp_gc_node *)calloc(1, sizeof(*node));
    if (!node) { hp_gc_unlock(); return; } /* RC still works; cycles may leak under OOM. */
    node->ptr = ptr;
    node->rc = rc;
    node->slot = slot;
    node->trace = trace;
    node->clear = clear;
    node->destroy = destroy;
    node->next = hp_gc_nodes;
    if (hp_gc_nodes) hp_gc_nodes->prev = node;
    hp_gc_nodes = node;
    *slot = node;
    hp_gc_node_count++;
    hp_gc_unlock();
}

void hp_gc_visit(hval value) {
    hp_gc_node *node = NULL;
    if (value.tag == HV_ARR && value.u.a) node = value.u.a->gc;
    else if (value.tag == HV_CLO && value.u.c) node = value.u.c->gc;
    else if (value.tag == HV_OBJ && value.u.p) node = ((hobj *)value.u.p)->gc;
    if (!node) return;
    if (hp_gc_mode == HP_GC_GATHER) {
        if (node->epoch == hp_gc_epoch) return;
        node->epoch = hp_gc_epoch;
        if (!hp_gc_work_push(node)) { hp_gc_oom = true; return; }
        if (node->trace) node->trace(node->ptr);
        return;
    }
    if (hp_gc_mode == HP_GC_SUBTRACT) {
        if (node->epoch != hp_gc_epoch) return;
        if (node->trial > 0) node->trial--;
        return;
    }
    if (hp_gc_mode != HP_GC_MARK || node->epoch != hp_gc_epoch) return;
    if (node->marked) return;
    node->marked = true;
    if (node->trace) node->trace(node->ptr);
}

static void hp_gc_trace_array(void *raw) {
    harr *a = (harr *)raw;
    for (size_t i = 0; i < a->len; i++) {
        hp_gc_visit(a->vals[i]);
        if (a->is_map && a->keys) hp_gc_visit(a->keys[i]);
    }
}

static void hp_gc_trace_closure(void *raw) {
    hclosure *c = (hclosure *)raw;
    if (c->trace_env) c->trace_env((char *)c + sizeof(*c));
}

static void hp_gc_destroy_array(void *raw) { hp_arr_free((harr *)raw); }
static void hp_gc_destroy_closure(void *raw) { hpclosure_free((hclosure *)raw); }
static void hp_gc_clear_array(void *raw) {
    harr *a = (harr *)raw;
    for (size_t i = 0; i < a->len; i++) {
        hp_val_rc_dec(a->vals[i]);
        if (a->is_map && a->keys) hp_val_rc_dec(a->keys[i]);
    }
    a->len = 0;
}
static void hp_gc_clear_closure(void *raw) {
    hclosure *c = (hclosure *)raw;
    if (c->drop_env) c->drop_env((char *)c + sizeof(*c));
    c->drop_env = NULL;
}

static void hp_gc_collect_candidate(hp_gc_node *root) {
    hp_gc_lock();
    if (!root || hp_gc_collecting || hp_gc_drop_depth || hp_gc_node_count == 0) {
        hp_gc_unlock();
        return;
    }
    /* Follow only the graph reachable from the released candidate. This
     * avoids rescanning unrelated live DOM/object graphs on every cycle pass. */
    hp_gc_collecting = true;
    if (++hp_gc_epoch == 0) {
        for (hp_gc_node *it = hp_gc_nodes; it; it = it->next) it->epoch = 0;
        hp_gc_epoch = 1;
    }
    hp_gc_work_len = 0;
    hp_gc_oom = false;
    hp_gc_mode = HP_GC_GATHER;
    if (root->slot && *root->slot == root) {
        if (root->ptr == NULL) { hp_gc_mode = HP_GC_IDLE; hp_gc_collecting = false; hp_gc_unlock(); return; }
        /* The root payload is not uniform; seed the traversal from its trace
         * callback so the candidate itself is included without a fake hval. */
        root->epoch = hp_gc_epoch;
        if (!hp_gc_work_push(root)) hp_gc_oom = true;
        if (root->trace) root->trace(root->ptr);
    }
    if (hp_gc_oom) {
        hp_gc_mode = HP_GC_IDLE;
        hp_gc_collecting = false;
        hp_gc_unlock();
        return;
    }
    for (size_t i = 0; i < hp_gc_work_len; i++) {
        hp_gc_node *node = hp_gc_work[i];
        node->trial = *node->rc == UINT32_MAX ? INT64_MAX : (int64_t)*node->rc;
        node->marked = false;
    }
    hp_gc_mode = HP_GC_SUBTRACT;
    for (size_t i = 0; i < hp_gc_work_len; i++)
        if (hp_gc_work[i]->trace) hp_gc_work[i]->trace(hp_gc_work[i]->ptr);
    hp_gc_mode = HP_GC_MARK;
    for (size_t i = 0; i < hp_gc_work_len; i++) {
        hp_gc_node *node = hp_gc_work[i];
        if (node->trial > 0 || *node->rc == UINT32_MAX) {
            node->marked = true;
            if (node->trace) node->trace(node->ptr);
        }
    }
    hp_gc_mode = HP_GC_IDLE;
    hp_gc_node **dead = (hp_gc_node **)malloc(hp_gc_work_len * sizeof(*dead));
    size_t ndead = 0;
    if (dead) {
        for (size_t i = 0; i < hp_gc_work_len; i++)
            if (!hp_gc_work[i]->marked) dead[ndead++] = hp_gc_work[i];
        /* Detach every unreachable node and zero its count before any drop
         * callback runs, so internal edges are released exactly once. */
        for (size_t i = 0; i < ndead; i++) {
            hp_gc_node *node = dead[i];
            if (node->prev) node->prev->next = node->next;
            else if (hp_gc_nodes == node) hp_gc_nodes = node->next;
            if (node->next) node->next->prev = node->prev;
            if (hp_gc_node_count) hp_gc_node_count--;
            if (node->slot) *node->slot = NULL;
            *node->rc = 0;
        }
        hp_gc_drop_depth++;
        /* First break every dead node's outgoing edges while all dead payloads
         * are still allocated. Cycles can point in any order, so freeing one
         * member before clearing the others would make their drops dereference
         * freed siblings. */
        for (size_t i = 0; i < ndead; i++)
            if (dead[i]->clear) dead[i]->clear(dead[i]->ptr);
        for (size_t i = 0; i < ndead; i++) {
            if (dead[i]->destroy) dead[i]->destroy(dead[i]->ptr);
            free(dead[i]);
        }
        hp_gc_drop_depth--;
        free(dead);
    }
    hp_gc_collecting = false;
    hp_gc_unlock();
}

static inline void hp_gc_candidate(hp_gc_node *root) {
    if (!root) return;
    if (++hp_gc_release_count & 1023u) return;
    hp_gc_collect_candidate(root);
}

void hp_gc_collect_now(void *ptr) {
    if (!ptr) return;
    hp_gc_node *node = ((hobj *)ptr)->gc;
    hp_gc_collect_candidate(node);
}

/* Generic strong object handles use the common generated-object header. */
typedef struct { uint32_t rc; } hp_rc_header;

void hp_rc_inc(void *p) {
    if (!p) return;
    hp_gc_lock();
    hp_rc_header *h = (hp_rc_header *)p;
    hp_rc_retain(&h->rc);
    hp_gc_unlock();
}

void hp_rc_dec(void *p) {
    if (!p) return;
    hp_gc_lock();
    hobj *h = (hobj *)p;
    uint32_t old = hp_rc_release(&h->rc);
    if (old == 0 || old == UINT32_MAX) { hp_gc_unlock(); return; }
    if (old == 1) {
        hp_gc_unregister(&h->gc);
        hp_gc_drop_depth++;
        hp_weak_invalidate(h);
        if (h->drop) h->drop(p);
        else free(p);
        hp_gc_drop_depth--;
    } else {
        hp_gc_candidate(h->gc);
    }
    hp_gc_unlock();
}

#define HP_VAL_TMP 48
static char *hp_int_digits(int64_t v, char *end);
static const char *hp_val_bytes(hval v, char *tmp, size_t *len);

/* last byte the current echo statement wrote (0 = nothing yet) */
static _Thread_local int hp_echo_last = 0;

void hp_echo(hval v) {
    char tmp[HP_VAL_TMP];
    size_t n;
    const char *p = hp_val_bytes(v, tmp, &n);
    fwrite(p, 1, n, stdout);
    if (n) hp_echo_last = (unsigned char)p[n - 1];
}

/* An echo statement ends its line - unless what it printed already ends
 * with "\n" (PHP-style `echo "done\n";`), so ported code prints one line,
 * not two. `echo "";` still prints an empty line. */
void hp_echo_newline(void) {
    if (hp_echo_last != '\n') fputc('\n', stdout);
    hp_echo_last = 0;
}

/* ---------------- string builder ---------------- */
void hbuf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }

static void hbuf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < b->len + need + 1) ncap *= 2;
    b->data = realloc(b->data, ncap);
    if (!b->data) { fprintf(stderr, "Fatal error: out of memory\n"); exit(255); }
    b->cap = ncap;
}

void hbuf_putc(Buf *b, char c) { hbuf_grow(b, 1); b->data[b->len++] = c; b->data[b->len] = 0; }

/* hbuf_write itself is inline in hphp_rt.h; this is its growth path. */
void hbuf_grow_slow(Buf *b, size_t need) { hbuf_grow(b, need); }

void hbuf_puts(Buf *b, const char *s) { hbuf_write(b, s, strlen(s)); }

void hbuf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        hbuf_grow(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

char *hbuf_take(Buf *b) {
    if (!b->data) { b->data = malloc(1); b->data[0] = 0; }
    char *out = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return out;
}

/* ---------------- strings ---------------- */
hstr *hp_str_new(const char *bytes, size_t len) {
    size_t cap = len + 1;
    /* every header field and the terminator are written below, so the
     * zeroing of hp_alloc (calloc) would be wasted work */
    hstr *s = malloc(sizeof(hstr) + cap);
    if (!s) {
        fprintf(stderr, "Fatal error: Allowed memory size exhausted\n");
        exit(255);
    }
    s->len = len;
    s->cap = cap;
    s->hash = 0;
    s->rc = 1;
    if (bytes && len) memcpy(s->data, bytes, len);
    s->data[len] = 0;
    return s;
}

/* Interned literals: an open-addressing hash set keyed by the bytes. It used
 * to be a linked list searched with strcmp, so every runtime hp_str_lit("")
 * (empty results, null-to-string, ...) scanned all of a program's literals. */
static hstr **hp_lit_tab;
static size_t hp_lit_cap, hp_lit_len;
static atomic_flag hp_literals_lock = ATOMIC_FLAG_INIT;

static uint64_t hp_lit_hash(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
    return h;
}

static void hp_lit_grow(void) {
    size_t ncap = hp_lit_cap ? hp_lit_cap * 2 : 1024;
    hstr **nt = calloc(ncap, sizeof *nt);
    if (!nt) { fputs("hphp: out of memory\n", stderr); exit(1); }
    for (size_t i = 0; i < hp_lit_cap; i++) {
        hstr *v = hp_lit_tab[i];
        if (!v) continue;
        size_t j = (size_t)hp_lit_hash(v->data, v->len) & (ncap - 1);
        while (nt[j]) j = (j + 1) & (ncap - 1);
        nt[j] = v;
    }
    free(hp_lit_tab);
    hp_lit_tab = nt;
    hp_lit_cap = ncap;
}

hstr *hp_str_lit(const char *cstr) {
    if (!cstr) cstr = "";
    size_t n = strlen(cstr);
    uint64_t h = hp_lit_hash(cstr, n);
    while (atomic_flag_test_and_set_explicit(&hp_literals_lock, memory_order_acquire)) { }
    if (hp_lit_len * 2 >= hp_lit_cap) hp_lit_grow();
    size_t j = (size_t)h & (hp_lit_cap - 1);
    for (;;) {
        hstr *v = hp_lit_tab[j];
        if (!v) break;
        if (v->len == n && memcmp(v->data, cstr, n) == 0) {
            atomic_flag_clear_explicit(&hp_literals_lock, memory_order_release);
            return v;
        }
        j = (j + 1) & (hp_lit_cap - 1);
    }
    hstr *v = hp_str_new(cstr, n);
    v->rc = UINT32_MAX; /* literal pool owns these for process lifetime */
    hp_lit_tab[j] = v;
    hp_lit_len++;
    atomic_flag_clear_explicit(&hp_literals_lock, memory_order_release);
    return v;
}

#if defined(__GNUC__)
__attribute__((noinline, cold))
#endif
hstr *hp_str_lit_site_slow(_Atomic(hstr *) *slot, const char *cstr) {
    hstr *created = hp_str_lit(cstr);
    hstr *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(slot, &expected, created,
            memory_order_release, memory_order_acquire)) return created;
    return expected;
}

hstr *hp_str_cstr(const char *cstr) {
    if (!cstr) cstr = "";
    return hp_str_new(cstr, strlen(cstr));
}

hstr *hp_str_copy(hstr *s) {
    return hp_str_new(s ? s->data : "", s ? s->len : 0);
}

void hp_str_free(hstr *s) {
    /* Literal-pool and persisted strings use UINT32_MAX as an immortal RC.
     * Several helpers build temporary replacement patterns with hp_str_lit
     * and pass them through this low-level disposer. */
    if (s && s->rc != UINT32_MAX) free(s);
}

uint32_t hp_str_hash(hstr *s) {
    if (!s) return 0;
    uint32_t cached = atomic_load_explicit(&s->hash, memory_order_relaxed);
    if (cached) return cached;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s->len; i++) {
        h ^= (unsigned char)s->data[i];
        h *= 16777619u;
    }
    if (!h) h = 1;
    unsigned expected = 0;
    atomic_compare_exchange_strong_explicit(&s->hash, &expected, h,
        memory_order_relaxed, memory_order_relaxed);
    return atomic_load_explicit(&s->hash, memory_order_relaxed);
}

int hp_argc_global = 0;

harr *hp_argv_from_c(int argc, char **argv) {
    harr *out = hp_arr_new();
    for (int i = 0; i < argc; i++)
        hp_arr_push(out, hp_of_str(hp_str_new(argv[i] ? argv[i] : "",
                                              strlen(argv[i] ? argv[i] : ""))));
    hp_argc_global = argc;
    return out;
}

int64_t hp_rand(void) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = true; }
    return (int64_t)rand();
}

int64_t hp_rand_range(int64_t min, int64_t max) {
    if (max < min) { int64_t t = min; min = max; max = t; }
    static bool seeded2 = false;
    if (!seeded2) { srand((unsigned)time(NULL)); seeded2 = true; }
    if (max == min) return min;
    return min + (int64_t)rand() % (max - min + 1);
}

double hp_mt_rand(void) {
    static bool seeded3 = false;
    if (!seeded3) { srand((unsigned)time(NULL)); seeded3 = true; }
    return (double)rand() / (double)RAND_MAX;
}

bool hp_str_eq(hstr *a, hstr *b) {
    if (a == b) return true;
    if (!a || !b || a->len != b->len) return false;
    return memcmp(a->data, b->data, a->len) == 0;
}

int hp_str_cmp(hstr *a, hstr *b) {
    if (!a) a = hp_str_lit("");
    if (!b) b = hp_str_lit("");
    size_t n = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->data, b->data, n);
    if (r) return r;
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

hstr *hp_str_concat2(hstr *a, hstr *b) {
    if (!a) a = hp_str_lit("");
    if (!b) b = hp_str_lit("");
    size_t len = a->len + b->len;
    /* Builder optimization: if the left string is uniquely owned, has
     * spare capacity and enough room for the right side, append in place.
     * This makes "$s .= "x";" loops amortized O(n) like PHP/rope builds. */
    if (hp_rc_count(&a->rc) == 1 && a->cap > a->len && len < a->cap) {
        memcpy(a->data + a->len, b->data, b->len);
        a->len = len;
        a->data[len] = 0;
        a->hash = 0;
        return a;
    }
    /* grow geometrically so repeated .= reuses the in-place path */
    size_t cap = len + 1;
    if (hp_rc_count(&a->rc) == 1) {
        cap = len * 2 + 16;
    }
    hstr *s = hp_alloc(sizeof(hstr) + cap);
    s->len = len;
    s->cap = cap;
    s->hash = 0;
    s->rc = 1;
    memcpy(s->data, a->data, a->len);
    memcpy(s->data + a->len, b->data, b->len);
    s->data[len] = 0;
    return s;
}

/* hval-level concat: PHP '.' operator on two values of any kind. */
hstr *hp_str_concat_v(hval a, hval b) {
    return hp_str_join_v(a, b);
}

/* PHP `.` on two values: the result is a NEW string.
 *
 * hp_str_concat2 appends in place when its left operand is uniquely owned,
 * which is the right thing for a builder nobody else can see - but a
 * variable holding the left operand is exactly the case where "uniquely
 * owned" is a lie, because the variable is still there afterwards. Codegen
 * emits this one for `.`, so `$b = $a . "x"` leaves $a alone; `$a .= "x"`
 * goes through hp_str_append_v, where overwriting $a is the point. */
/* The bytes of v as PHP would print them, for concatenation. Numbers are
 * formatted into tmp (HP_VAL_TMP bytes) instead of a new string: the string
 * hp_val_to_str makes for an int or float was never released by the concat
 * helpers, so every `"x" . $n` leaked one allocation. */
static const char *hp_val_bytes(hval v, char *tmp, size_t *len) {
    switch (v.tag) {
    case HV_INT: {
        char *p = hp_int_digits(v.u.i, tmp + HP_VAL_TMP);
        *len = (size_t)(tmp + HP_VAL_TMP - p);
        return p;
    }
    case HV_FLOAT: {
        double f = v.u.f;
        int n;
        if (f == (double)(long long)f && fabs(f) < 1e15)
            n = snprintf(tmp, HP_VAL_TMP, "%lld", (long long)f);
        else
            n = snprintf(tmp, HP_VAL_TMP, "%.14g", f);
        *len = n > 0 ? (size_t)n : 0;
        return tmp;
    }
    case HV_BOOL:
        *len = v.u.b ? 1 : 0;
        return "1";
    default: {
        hstr *s = hp_val_to_str(v);   /* an existing string or a literal */
        if (!s) { *len = 0; return ""; }
        *len = s->len;
        return s->data;
    }
    }
}

hstr *hp_str_join_v(hval a, hval b) {
    char lt[HP_VAL_TMP], rt[HP_VAL_TMP];
    size_t ln, rn;
    const char *l = hp_val_bytes(a, lt, &ln);
    const char *r = hp_val_bytes(b, rt, &rn);
    size_t len = ln + rn;
    hstr *s = malloc(sizeof(hstr) + len + 1);
    if (!s) {
        fprintf(stderr, "Fatal error: Allowed memory size exhausted\n");
        exit(255);
    }
    s->len = len;
    s->cap = len + 1;
    s->hash = 0;
    s->rc = 1;
    memcpy(s->data, l, ln);
    memcpy(s->data + ln, r, rn);
    s->data[len] = 0;
    return s;
}

/* `$s .= x` writes a new string into the target slot. Runtime string
 * reference counts are not maintained yet, so mutating the old buffer in
 * place could also change another variable that still points to it. */
hstr *hp_str_append_v(hval a, hval b) {
    return hp_str_join_v(a, b);
}/* Concat over an hval array — avoids struct varargs (Win64 ABI pitfalls). */
hstr *hp_str_concat_n(int n, const hval *vals) {
    if (n <= 0 || !vals) return hp_str_lit("");
    /* one block: the part pointers, their lengths, and number scratch */
    size_t each = sizeof(const char *) + sizeof(size_t) + HP_VAL_TMP;
    char *blk = malloc((size_t)n * each);
    if (!blk) return hp_str_lit("");
    const char **ptr = (const char **)(void *)blk;
    size_t *lens = (size_t *)(void *)(blk + (size_t)n * sizeof(const char *));
    char *tmp = blk + (size_t)n * (sizeof(const char *) + sizeof(size_t));
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        ptr[i] = hp_val_bytes(vals[i], tmp + (size_t)i * HP_VAL_TMP, &lens[i]);
        if (lens[i] > SIZE_MAX - len) {
            free(blk);
            return hp_str_lit("");
        }
        len += lens[i];
    }
    hstr *s = malloc(sizeof(hstr) + len + 1);
    if (!s) {
        fprintf(stderr, "Fatal error: Allowed memory size exhausted\n");
        exit(255);
    }
    s->len = len;
    s->cap = len + 1;
    s->hash = 0;
    s->rc = 1;
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        memcpy(s->data + off, ptr[i], lens[i]);
        off += lens[i];
    }
    s->data[len] = 0;
    free(blk);
    return s;
}

static int64_t norm_index(int64_t i, size_t len) {
    if (i < 0) i += (int64_t)len;
    if (i < 0) return 0;
    if ((size_t)i > len) return (int64_t)len;
    return i;
}

hstr *hp_str_slice(hstr *s, int64_t start, int64_t end) {
    if (!s) return hp_str_lit("");
    start = norm_index(start, s->len);
    if (end < 0) end += (int64_t)s->len;
    if (end < 0 || (size_t)end > s->len) end = (int64_t)s->len;
    if (end <= start) return hp_str_lit("");
    return hp_str_new(s->data + start, (size_t)(end - start));
}

/* One shared, never-freed string per byte value: `$s[$i]` in a parser loop
 * then allocates nothing. Immortal (rc UINT32_MAX), so callers may release
 * it like any owned result. */
static _Atomic(hstr *) hp_byte_strs[256];
hstr *hp_str_char_at(hstr *s, int64_t idx) {
    if (!s || idx < 0 || (uint64_t)idx >= (uint64_t)s->len)
        hp_throw_str("String index out of bounds");
    unsigned char c = (unsigned char)s->data[idx];
    hstr *b = atomic_load_explicit(&hp_byte_strs[c], memory_order_acquire);
    if (b) return b;
    b = hp_str_new(s->data + idx, 1);
    b->rc = UINT32_MAX;
    hstr *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&hp_byte_strs[c], &expected, b,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        free(b);            /* another thread installed it first */
        b = expected;
    }
    return b;
}

int64_t hp_str_len(hstr *s) { return s ? (int64_t)s->len : 0; }

int64_t hp_str_index(hstr *hay, hstr *needle) {
    if (!hay || !needle || needle->len > hay->len) return -1;
    if (needle->len == 0) return 0;
    for (size_t i = 0; i + needle->len <= hay->len; i++) {
        if (memcmp(hay->data + i, needle->data, needle->len) == 0)
            return (int64_t)i;
    }
    return -1;
}

/* Numbers become fresh strings. They used to go through hp_str_lit, which
 * interned every distinct value for the process lifetime in a linked list
 * searched with strcmp: O(distinct numbers) per conversion and unbounded
 * memory (a server turning 280k user ids into strings spent most of its CPU
 * there). Small non-negative ints share immortal strings. */
#define HP_SMALL_INT_STRS 256
static _Atomic(hstr *) hp_small_int_strs[HP_SMALL_INT_STRS];

/* Decimal digits of v, written backwards from end; returns the start. No
 * snprintf: this runs for every number concatenated into a string. */
static char *hp_int_digits(int64_t v, char *end) {
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    char *p = end;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--p = '-';
    return p;
}

hstr *hp_str_from_int(int64_t v) {
    if (v >= 0 && v < HP_SMALL_INT_STRS) {
        hstr *s = atomic_load_explicit(&hp_small_int_strs[v], memory_order_acquire);
        if (s) return s;
        char sbuf[8];
        char *sp = hp_int_digits(v, sbuf + sizeof sbuf - 1);
        sbuf[sizeof sbuf - 1] = 0;
        s = hp_str_lit(sp);
        atomic_store_explicit(&hp_small_int_strs[v], s, memory_order_release);
        return s;
    }
    char buf[24];
    char *p = hp_int_digits(v, buf + sizeof buf);
    return hp_str_new(p, (size_t)(buf + sizeof buf - p));
}

hstr *hp_str_from_double(double v) {
    char buf[64];
    if (v == (double)(long long)v && fabs(v) < 1e15)
        snprintf(buf, sizeof buf, "%lld", (long long)v);
    else
        snprintf(buf, sizeof buf, "%.14g", v);
    return hp_str_new(buf, strlen(buf));
}

hstr *hp_str_from_bool(bool b) { return hp_str_lit(b ? "1" : ""); }

int64_t hp_str_to_int(hstr *s) {
    if (!s) return 0;
    return (int64_t)strtoll(s->data, NULL, 10);
}

/* ---------- i128 ---------- */
hstr *hp_i128_to_str(hp_i128 v) {
    char buf[48];
    char *p = buf + sizeof buf;
    hp_u128 u;
    bool neg = v < 0;
    /* -(v + 1) + 1 is safe even for the minimum value, whose negation is
     * not representable */
    u = neg ? (hp_u128)(-(v + 1)) + 1u : (hp_u128)v;
    do {
        *--p = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u);
    if (neg) *--p = '-';
    return hp_str_new(p, (size_t)(buf + sizeof buf - p));
}

hp_i128 hp_str_to_i128(hstr *s) {
    if (!s) return 0;
    const char *p = s->data;
    size_t n = s->len, i = 0;
    bool neg = false;
    hp_u128 u = 0;
    while (i < n && isspace((unsigned char)p[i])) i++;
    if (i < n && (p[i] == '+' || p[i] == '-')) {
        neg = p[i] == '-';
        i++;
    }
    for (; i < n && isdigit((unsigned char)p[i]); i++)
        u = u * 10u + (hp_u128)(p[i] - '0');
    return hp_i128_from_u(neg ? (hp_u128)0 - u : u);
}

hval hp_of_i128_str(hp_i128 v) {
    return hp_of_str(hp_i128_to_str(v));
}

hp_i128 hp_i128_pow(hp_i128 a, hp_i128 b) {
    if (b < 0) return 0;   /* integer pow truncates a negative exponent to 0 */
    hp_u128 r = 1, base = (hp_u128)a;
    while (b) {
        if (b & 1) r *= base;
        base *= base;
        b >>= 1;
    }
    return hp_i128_from_u(r);
}

hval hp_i128_var_dump(hp_i128 v) {
    hstr *s = hp_i128_to_str(v);
    printf("int(%.*s)\n", (int)s->len, s->data);
    hp_str_free(s);
    return hp_of_int(0);
}

hstr *hp_i128_print_r(hp_i128 v) {
    hstr *s = hp_i128_to_str(v);
    fwrite(s->data, 1, s->len, stdout);
    return s;
}

double hp_str_to_double(hstr *s) {
    if (!s) return 0;
    return strtod(s->data, NULL);
}

/* PHP's is_numeric(): optional surrounding whitespace, optional sign, digits
 * with an optional decimal point and exponent. Used to decide whether '+' on
 * a string is arithmetic ("1" + "2" = 3) or our friendly concatenation. */
bool hp_str_is_numeric(hstr *s) {
    if (!s || s->len == 0) return false;
    const char *p = s->data;
    size_t n = s->len, i = 0;
    while (i < n && isspace((unsigned char)p[i])) i++;
    if (i < n && (p[i] == '+' || p[i] == '-')) i++;
    bool digits = false;
    while (i < n && isdigit((unsigned char)p[i])) { i++; digits = true; }
    if (i < n && p[i] == '.') {
        i++;
        while (i < n && isdigit((unsigned char)p[i])) { i++; digits = true; }
    }
    if (!digits) return false;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        size_t save = i;
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) i++;
        bool exp_digits = false;
        while (i < n && isdigit((unsigned char)p[i])) { i++; exp_digits = true; }
        if (!exp_digits) i = save;
    }
    while (i < n && isspace((unsigned char)p[i])) i++;
    return i == n;
}

hstr *hp_str_toupper(hstr *s) {
    if (!s) return hp_str_lit("");
    hstr *r = hp_str_copy(s);
    for (size_t i = 0; i < r->len; i++) r->data[i] = (char)toupper((unsigned char)r->data[i]);
    return r;
}

hstr *hp_str_tolower(hstr *s) {
    if (!s) return hp_str_lit("");
    hstr *r = hp_str_copy(s);
    for (size_t i = 0; i < r->len; i++) r->data[i] = (char)tolower((unsigned char)r->data[i]);
    return r;
}

hstr *hp_str_repeat(hstr *s, int64_t n) {
    if (!s || n <= 0) return hp_str_lit("");
    size_t len = s->len * (size_t)n;
    hstr *r = hp_alloc(sizeof(hstr) + len + 1);
    r->len = len;
    r->cap = len + 1;
    r->rc = 1;
    for (int64_t i = 0; i < n; i++) memcpy(r->data + i * s->len, s->data, s->len);
    r->data[len] = 0;
    return r;
}

hstr *hp_str_replace(hstr *subject, hstr *search, hstr *replace) {
    if (!subject) return hp_str_lit("");
    if (!search || search->len == 0) return hp_str_copy(subject);
    if (!replace) replace = hp_str_lit("");
    hstr *out = hp_str_lit("");
    size_t i = 0;
    while (i < subject->len) {
        if (i + search->len <= subject->len &&
            memcmp(subject->data + i, search->data, search->len) == 0) {
            out = hp_str_concat2(out, replace);
            i += search->len;
        } else {
            out = hp_str_concat2(out, hp_str_new(subject->data + i, 1));
            i++;
        }
    }
    return out;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0' || c == '\v' || c == '\f';
}

hstr *hp_str_trim(hstr *s) {
    if (!s) return hp_str_lit("");
    size_t a = 0, b = s->len;
    while (a < b && is_ws(s->data[a])) a++;
    while (b > a && is_ws(s->data[b - 1])) b--;
    return hp_str_new(s->data + a, b - a);
}

hstr *hp_str_reverse(hstr *s) {
    if (!s) return hp_str_lit("");
    hstr *r = hp_str_copy(s);
    for (size_t i = 0; i < r->len / 2; i++) {
        char t = r->data[i];
        r->data[i] = r->data[r->len - 1 - i];
        r->data[r->len - 1 - i] = t;
    }
    return r;
}

/* ---------------- values ---------------- */
hval hp_null = { HV_NULL, { 0 } };


hstr *hp_null_str(void) {
    static hstr *ns = NULL;
    if (!ns) ns = hp_str_lit("");
    return ns;
}

harr *hp_null_arr(void) {
    static harr *na = NULL;
    if (!na) na = hp_arr_new();
    return na;
}

HKind hp_tag(hval v) { return v.tag; }

const char *hp_kind_name(HKind k) {
    switch (k) {
    case HV_NULL: return "null";
    case HV_INT: return "integer";
    case HV_FLOAT: return "double";
    case HV_BOOL: return "boolean";
    case HV_STR: return "string";
    case HV_ARR: return "array";
    case HV_OBJ: return "object";
    case HV_CLO: return "closure";
    default: return "unknown";
    }
}

hstr *hp_val_to_str(hval v) {
    switch (v.tag) {
    case HV_NULL: return hp_str_lit("");
    case HV_INT: return hp_str_from_int(v.u.i);
    case HV_FLOAT: return hp_str_from_double(v.u.f);
    case HV_BOOL: return hp_str_from_bool(v.u.b);
    case HV_STR: return v.u.s ? v.u.s : hp_null_str();
    case HV_ARR: return hp_str_lit("Array");
    case HV_OBJ: return hp_str_lit("Object");
    case HV_CLO: return hp_str_lit("Closure");
    default: return hp_str_lit("");
    }
}

int64_t hp_val_to_int_slow(hval v) {
    switch (v.tag) {
    case HV_INT: return v.u.i;
    case HV_FLOAT: return (int64_t)v.u.f;
    case HV_BOOL: return v.u.b ? 1 : 0;
    case HV_STR: return hp_str_to_int(v.u.s);
    case HV_NULL: return 0;
    default: return 1;
    }
}

double hp_val_to_float(hval v) {
    switch (v.tag) {
    case HV_INT: return (double)v.u.i;
    case HV_FLOAT: return v.u.f;
    case HV_BOOL: return v.u.b ? 1.0 : 0.0;
    case HV_STR: return hp_str_to_double(v.u.s);
    case HV_NULL: return 0.0;
    default: return 0.0;
    }
}

bool hp_val_to_bool(hval v) {
    switch (v.tag) {
    case HV_NULL: return false;
    case HV_INT: return v.u.i != 0;
    case HV_FLOAT: return v.u.f != 0.0;
    case HV_BOOL: return v.u.b;
    /* PHP's "falsy strings" are "" and the single character "0" - and only
     * those. A non-empty "0"-led string like "00" or "0.0" is truthy. */
    case HV_STR:
        return v.u.s && !(v.u.s->len == 0 ||
                          (v.u.s->len == 1 && v.u.s->data[0] == '0'));
    case HV_ARR: return v.u.a && v.u.a->len > 0;
    /* A property declared with a class type but defaulted to null --
       `public Button $tab = null;` -- is an object slot holding a NULL
       pointer, not a real object. Reading it as unconditionally truthy
       made every `if ($o->f == null)` guard lie, so callers went on to
       dereference the null. An absent object is falsy, as in PHP. */
    case HV_OBJ:
    case HV_CLO: return v.u.p != NULL;
    default: return true;
    }
}

static int64_t hp_key_index(harr *a, hval key);
static bool hp_val_eq_r(hval a, hval b, int depth);

/* PHP ==: arrays are equal when they hold the same key/value pairs, in any
 * order, with loosely equal values. */
static bool hp_arr_loose_eq(harr *x, harr *y, int depth) {
    if (x == y) return true;
    size_t xn = x ? x->len : 0, yn = y ? y->len : 0;
    if (xn != yn) return false;
    if (xn == 0) return true;
    if (depth > 64) return false;            /* cycles: give up conservatively */
    for (size_t i = 0; i < xn; i++) {
        hval key = x->keys ? x->keys[i] : hp_of_int((int64_t)i);
        int64_t j = (!x->keys && !y->keys) ? (int64_t)i : hp_key_index(y, key);
        if (j < 0 || !hp_val_eq_r(x->vals[i], y->vals[j], depth + 1)) return false;
    }
    return true;
}

bool hp_val_eq(hval a, hval b) { return hp_val_eq_r(a, b, 0); }

static bool hp_val_eq_r(hval a, hval b, int depth) {
    if (a.tag == HV_STR && b.tag == HV_STR) return hp_str_eq(a.u.s, b.u.s);
    if (a.tag == HV_ARR && b.tag == HV_ARR) return hp_arr_loose_eq(a.u.a, b.u.a, depth);
    if (a.tag == HV_ARR || b.tag == HV_ARR) {
        /* an array equals only another array, or null/false when empty */
        hval other = a.tag == HV_ARR ? b : a;
        harr *arr = a.tag == HV_ARR ? a.u.a : b.u.a;
        if (other.tag == HV_NULL || other.tag == HV_BOOL)
            return hp_val_to_bool(other) == (arr && arr->len > 0);
        return false;
    }
    if (a.tag == HV_NULL && b.tag == HV_NULL) return true;
    if (a.tag == HV_NULL || b.tag == HV_NULL)
        return hp_val_to_bool(a) == false && hp_val_to_bool(b) == false;
    if (a.tag == HV_BOOL || b.tag == HV_BOOL) return hp_val_to_bool(a) == hp_val_to_bool(b);
    if (a.tag == HV_FLOAT || b.tag == HV_FLOAT || a.tag == HV_INT || b.tag == HV_INT)
        return hp_val_to_float(a) == hp_val_to_float(b);
    return false;
}

int64_t hp_val_cmp(hval a, hval b) {
    if (a.tag == HV_STR && b.tag == HV_STR) {
        int r = hp_str_cmp(a.u.s, b.u.s);
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    }
    double x = hp_val_to_float(a), y = hp_val_to_float(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* ---------------- arrays: COW ---------------- */
harr *hp_arr_new(void) {
    harr *a = hp_alloc(sizeof(harr));
    a->rc = 1;
    a->next_index = 0;
    hp_gc_register(a, &a->rc, &a->gc, hp_gc_trace_array,
                   hp_gc_clear_array, hp_gc_destroy_array);
    return a;
}

harr *hp_region_arr_scalars(hp_region *region, size_t len, const hval *values) {
    harr *a = (harr *)hp_region_alloc(region, sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    a->len = a->cap = len;
    a->rc = 1;
    a->next_index = (int64_t)len;
    if (len) {
        a->vals = (hval *)hp_region_alloc(region, len * sizeof(*a->vals));
        if (!a->vals) return NULL;
        memcpy(a->vals, values, len * sizeof(*a->vals));
    }
    return a;
}

static void hp_arr_grow(harr *a, size_t need) {
    if (need <= a->cap) return;
    size_t cap = a->cap ? a->cap : 8;
    while (cap < need) cap *= 2;
    a->vals = realloc(a->vals, cap * sizeof(hval));
    if (a->is_map) {
        a->keys = realloc(a->keys, cap * sizeof(hval));
        for (size_t i = a->cap; i < cap; i++) {
            a->keys[i] = hp_null;
        }
    }
    a->cap = cap;
}

static void hp_arr_index_rebuild(harr *a, uint32_t icap);
static void hp_arr_index_del(harr *a, size_t valslot);
static uint32_t hp_arr_index_cap_for(size_t len);

harr *hp_arr_clone(harr *a) {
    harr *r = hp_arr_new();
    r->is_map = a->is_map;
    r->next_index = a->next_index;
    hp_arr_grow(r, a->len);
    for (size_t i = 0; i < a->len; i++) {
        /* deep clone: nested arrays get value semantics too */
        if (a->vals[i].tag == HV_ARR && a->vals[i].u.a)
            r->vals[i] = hp_of_arr(hp_arr_clone(a->vals[i].u.a));
        else {
            r->vals[i] = a->vals[i];
            hp_val_rc_inc(r->vals[i]);
        }
        if (r->is_map) {
            r->keys[i] = a->keys[i];
            hp_val_rc_inc(r->keys[i]);
        }
    }
    r->len = a->len;
    /* a cloned map keeps O(1) lookups (it used to start without an index,
     * so every lookup scanned all entries until the next insert) */
    if (r->is_map && r->len >= 12) hp_arr_index_rebuild(r, hp_arr_index_cap_for(r->len));
    return r;
}

/* value-assignment helper: clone arrays, pass everything else through.
 * Strings are immutable so sharing them is safe. */
hval hp_val_clone_v(hval v) {
    if (v.tag == HV_ARR && v.u.a) return hp_of_arr(hp_arr_clone(v.u.a));
    return v;
}

harr *hp_arr_of(int n, const hval *vals) {
    harr *a = hp_arr_new();
    for (int i = 0; i < n; i++)
        hp_arr_push(a, vals[i]);
    return a;
}

harr *hp_arr_of_take(int n, const hval *vals) {
    harr *a = hp_arr_new();
    for (int i = 0; i < n; i++) hp_arr_push_take(a, vals[i]);
    return a;
}

harr *hp_map_of(int n, const hval *kvs) {
    harr *a = hp_arr_new();
    a->is_map = true;
    for (int i = 0; i < n / 2; i++)
        hp_arr_set(a, kvs[2 * i], kvs[2 * i + 1]);
    return a;
}

harr *hp_map_of_take(int n, const hval *kvs) {
    harr *a = hp_arr_new();
    a->is_map = true;
    for (int i = 0; i < n / 2; i++) {
        hp_arr_set(a, kvs[2 * i], kvs[2 * i + 1]);
        hp_val_rc_dec(kvs[2 * i]);
        hp_val_rc_dec(kvs[2 * i + 1]);
    }
    return a;
}

static void hp_arr_index_add(harr *a, size_t valslot);

/* A list that became a map (a string or out-of-order key was set, or a
 * builtin flagged it) needs a key per existing element: its position.
 * Without this the old elements' keys were uninitialised memory, so
 * `$a = [1]; $a["k"] = 2; $a[0]` found nothing. */
static void hp_arr_ensure_keys(harr *a) {
    if (!a->is_map || a->keys) return;
    size_t cap = a->cap ? a->cap : 8;
    a->keys = malloc(cap * sizeof(hval));
    for (size_t i = 0; i < cap; i++)
        a->keys[i] = i < a->len ? hp_of_int((int64_t)i) : hp_null;
}

void hp_arr_push(harr *a, hval v) {
    /* Low-level mutation: callers without an owner slot must pass writable
     * storage. COW belongs in hp_arr_push_v/owner helpers, which can return
     * or install the detached pointer. */
    if (!a) return;
    hp_arr_ensure_keys(a);
    hp_arr_grow(a, a->len + 1);
    hp_val_rc_inc(v);
    a->vals[a->len] = v;
    if (a->is_map) a->keys[a->len] = hp_of_int(a->next_index);
    a->len++;
    a->next_index++;
    if (a->is_map && a->index) hp_arr_index_add(a, a->len - 1);   /* keep lookups exact */
}

void hp_arr_push_take(harr *a, hval v) {
    if (!a) return;
    hp_arr_ensure_keys(a);
    hp_arr_grow(a, a->len + 1);
    a->vals[a->len] = v;
    if (a->is_map) a->keys[a->len] = hp_of_int(a->next_index);
    a->len++;
    a->next_index++;
    if (a->is_map && a->index) hp_arr_index_add(a, a->len - 1);
}

/* PHP `$a[] = v`: push and return the array (hval wrapper) so the
 * expression also works as a value. */
hval hp_arr_push_v(harr *a, hval v) {
    if (!a) a = hp_arr_new();
    a = hp_arr_cow(a);      /* COW: mutate the writable copy, not a shared one */
    hp_arr_push(a, v);
    return hp_of_arr(a);
}

hval hp_arr_push_v_take(harr *a, hval v) {
    hval result = hp_arr_push_v(a, v);
    hp_val_rc_dec(v);
    return result;
}

/* PHP `$a[] = v` where the container is a runtime hval (boxed by-ref
 * capture / untyped slot): push and return the container so the caller
 * can store it back (the push may have cloned the array under COW). */
harr *hp_val_to_arr(hval v) {
    if (v.tag == HV_ARR && v.u.a) return v.u.a;
    return hp_arr_new();   /* PHP cast semantics: null/scalar -> [] */
}

hval hp_arr_push_vv(hval container, hval v) {
    if (container.tag != HV_ARR || !container.u.a) {
        /* PHP auto-vivification: pushing into null creates the array */
        if (container.tag == HV_NULL) {
            harr *na = hp_arr_new();
            hp_arr_push(na, v);
            return hp_of_arr(na);
        }
        return container;
    }
    harr *a = hp_arr_cow(container.u.a);
    hp_arr_push(a, v);
    return hp_of_arr(a);
}

hval hp_arr_push_vv_take(hval container, hval v) {
    hval result = hp_arr_push_vv(container, v);
    hp_val_rc_dec(v);
    return result;
}

hval hp_arr_pop(harr *a) {
    if (!a || a->len == 0) return hp_null;
    a = hp_arr_cow(a);
    if (a->is_map && a->index) hp_arr_index_del(a, a->len - 1);
    hval v = a->vals[--a->len];
    a->vals[a->len] = hp_null; /* transfer the entry owner to the result */
    if (a->is_map) { hp_val_rc_dec(a->keys[a->len]); a->keys[a->len] = hp_null; }
    return v;
}

hval hp_arr_shift(harr *a) {
    if (!a || a->len == 0) return hp_null;
    a = hp_arr_cow(a);
    hval v = a->vals[0];
    a->vals[0] = hp_null; /* transfer; shifted owners are not duplicated */
    if (a->is_map) hp_val_rc_dec(a->keys[0]);
    memmove(a->vals, a->vals + 1, (a->len - 1) * sizeof(hval));
    if (a->is_map) memmove(a->keys, a->keys + 1, (a->len - 1) * sizeof(hval));
    a->len--;
    if (a->is_map) a->keys[a->len] = hp_null;
    /* every slot moved: re-index (the old index pointed one slot off) */
    if (a->is_map && a->index) hp_arr_index_rebuild(a, hp_arr_index_cap_for(a->len));
    return v;
}

void hp_arr_unshift(harr *a, hval v) {
    a = hp_arr_cow(a);
    hp_arr_grow(a, a->len + 1);
    memmove(a->vals + 1, a->vals, a->len * sizeof(hval));
    if (a->is_map) memmove(a->keys + 1, a->keys, a->len * sizeof(hval));
    hp_val_rc_inc(v);
    a->vals[0] = v;
    if (a->is_map) a->keys[0] = hp_of_int(a->next_index++);
    a->len++;
    if (a->is_map && a->index) hp_arr_index_rebuild(a, hp_arr_index_cap_for(a->len));
}

/* strict array-key comparison (PHP array keys): only int==int and
 * str==str match. The loose hp_val_eq would accept 0 == "a" because the
 * numeric conversion of "a" is 0 — that corrupted mixed-key maps. */
static bool hp_key_eq(hval a, hval b) {
    if (a.tag == HV_INT && b.tag == HV_INT) return a.u.i == b.u.i;
    if (a.tag == HV_STR && b.tag == HV_STR) return hp_str_eq(a.u.s, b.u.s);
    return false;
}

static int64_t hp_key_index(harr *a, hval key) {
    if (a->is_map) {
        /* fast path: open-addressing hash index (int and string keys).
         * String keys are the common PHP pattern ($m["name"] => x), so they
         * get the same O(1) treatment as int keys via FNV-1a (hp_str_hash
         * caches the digest in the string itself). */
        if (a->index && key.tag == HV_INT) {
            uint32_t mask = a->icap - 1;
            uint32_t h = ((uint64_t)key.u.i * 0x9E3779B97F4A7C15ULL) >> 32;
            uint32_t slot = h & mask;
            for (;;) {
                uint32_t e = a->index[slot];
                if (e == 0) return -1;
                if (e != 0xFFFFFFFFu) {
                    hval k = a->keys[e - 1];
                    if (k.tag == HV_INT && k.u.i == key.u.i) return (int64_t)(e - 1);
                }
                slot = (slot + 1) & mask;
            }
        }
        if (a->index && key.tag == HV_STR && key.u.s) {
            uint32_t mask = a->icap - 1;
            uint32_t h = hp_str_hash(key.u.s);
            if (!h) h = 1;   /* the index stores hash 0 as 1 (hp_hash_str) */
            uint32_t slot = h & mask;
            for (;;) {
                uint32_t e = a->index[slot];
                if (e == 0) return -1;
                if (e != 0xFFFFFFFFu) {
                    hval k = a->keys[e - 1];
                    if (k.tag == HV_STR && k.u.s && hp_str_eq(k.u.s, key.u.s))
                        return (int64_t)(e - 1);
                }
                slot = (slot + 1) & mask;
            }
        }
        for (size_t i = 0; i < a->len; i++)
            if (hp_key_eq(a->keys[i], key)) return (int64_t)i;
        return -1;
    }
    if (key.tag == HV_STR) return -1; /* string index on list */
    int64_t i = hp_val_to_int(key);
    if (i < 0 || (size_t)i >= a->len) return -1;
    return i;
}

/* ---- incremental hash index maintenance (int-key maps) ----
 * rebuild_flag(): decide whether to (re)build; called after inserts.
 * Cost amortized O(1) per insert, like PHP's ordered hash tables. */
static void hp_arr_index_add(harr *a, size_t valslot);
static void hp_arr_index_del(harr *a, size_t valslot);
static void hp_arr_index_rebuild(harr *a, uint32_t icap);

static uint32_t hp_hash_int(int64_t k) {
    uint64_t x = (uint64_t)k * 0x9E3779B97F4A7C15ULL;
    return (uint32_t)(x >> 32);
}

/* string-key hash: FNV-1a (cached in the string), forced non-zero so 0 can
 * stay the "empty slot" marker in the index table */
static uint32_t hp_hash_str(hstr *s) {
    uint32_t h = hp_str_hash(s);
    return h ? h : 1;
}

static void hp_arr_index_rebuild(harr *a, uint32_t icap) {
    free(a->index);
    a->index = calloc(icap, sizeof(uint32_t));
    a->icap = icap;
    /* Inserts AND deletes (tombstones) spend this budget, so live entries
     * plus tombstones stay below icap: a probe for a missing key always
     * reaches an empty slot. (Deletes used to be free, so unset-heavy maps
     * filled the table with tombstones and lookups spun forever.) */
    a->ibudget = a->len < icap ? (uint32_t)((icap - a->len) / 2) : 0;
    for (size_t i = 0; i < a->len; i++) {
        hval k = a->keys[i];
        if (k.tag != HV_INT && k.tag != HV_STR) continue;
        uint32_t slot = (k.tag == HV_INT ? hp_hash_int(k.u.i)
                                         : hp_hash_str(k.u.s)) & (icap - 1);
        while (a->index[slot] != 0 && a->index[slot] != 0xFFFFFFFFu)
            slot = (slot + 1) & (icap - 1);
        a->index[slot] = (uint32_t)i + 1;
    }
}

/* Index capacity for `len` live entries: a power of two with load <= 0.5. */
static uint32_t hp_arr_index_cap_for(size_t len) {
    uint32_t cap = 32;
    while ((size_t)cap < len * 2 + 2) cap *= 2;
    return cap;
}

static void hp_arr_index_add(harr *a, size_t valslot) {
    if (a->keys[valslot].tag != HV_INT && a->keys[valslot].tag != HV_STR)
        return;
    if (!a->index) {
        if (a->len < 12) return;          /* too small to be worth it */
        hp_arr_index_rebuild(a, hp_arr_index_cap_for(a->len));
        return;                           /* the rebuild indexed valslot */
    }
    if (a->ibudget == 0) {
        /* grow to keep load factor low, or just rebuild to clear tombstones;
         * the rebuild indexes every live slot, valslot included */
        hp_arr_index_rebuild(a, hp_arr_index_cap_for(a->len));
        return;
    }
    uint32_t slot = (a->keys[valslot].tag == HV_INT
                         ? hp_hash_int(a->keys[valslot].u.i)
                         : hp_hash_str(a->keys[valslot].u.s)) & (a->icap - 1);
    while (a->index[slot] != 0 && a->index[slot] != 0xFFFFFFFFu)
        slot = (slot + 1) & (a->icap - 1);
    a->index[slot] = (uint32_t)valslot + 1;
    a->ibudget--;
}

static void hp_arr_index_del(harr *a, size_t valslot) {
    if (!a->index ||
        (a->keys[valslot].tag != HV_INT && a->keys[valslot].tag != HV_STR))
        return;
    uint32_t mask = a->icap - 1;
    uint32_t slot = (a->keys[valslot].tag == HV_INT
                         ? hp_hash_int(a->keys[valslot].u.i)
                         : hp_hash_str(a->keys[valslot].u.s)) & mask;
    for (;;) {
        uint32_t e = a->index[slot];
        if (e == 0) return;
        if (e == (uint32_t)valslot + 1) {
            a->index[slot] = 0xFFFFFFFFu;   /* tombstone */
            if (a->ibudget) a->ibudget--;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

/* shiftless deletion: swap-with-last keeps O(1) unset and preserves the
 * hash index without slot shifting (iteration order for maps is therefore
 * not insertion-ordered after unset — documented tradeoff). */
void hp_arr_unset(harr *a, hval key) {
    /* Raw mutator; callers with a variable slot must use an owner helper. */
    if (!a) return;
    int64_t i = hp_key_index(a, key);
    if (i < 0) return;
    if (!a->is_map) {
        /* PHP: unset($list[2]) leaves keys 0,1,3,4 (and the next push still
         * gets the old next index). The swap below would renumber a list, so
         * give it explicit keys first. */
        a->is_map = true;
        hp_arr_ensure_keys(a);
        if (a->len >= 12) hp_arr_index_rebuild(a, hp_arr_index_cap_for(a->len));
    }
    size_t last = a->len - 1;
    hp_val_rc_dec(a->vals[i]);
    if (a->is_map) hp_val_rc_dec(a->keys[i]);
    if ((size_t)i != last) {
        if (a->is_map && a->keys) {
            hp_arr_index_del(a, (size_t)i);
            hp_arr_index_del(a, last);
            a->keys[i] = a->keys[last];
        }
        a->vals[i] = a->vals[last];
        a->vals[last] = hp_null; /* move the last entry into the removed slot */
        if (a->is_map) a->keys[last] = hp_null;
        if (a->is_map && a->index) hp_arr_index_add(a, (size_t)i);
    } else if (a->is_map) {
        hp_arr_index_del(a, (size_t)i);
    }
    a->len = last;
    if (a->index && a->ibudget == 0)
        hp_arr_index_rebuild(a, hp_arr_index_cap_for(a->len));
}

hval hp_arr_unset_owner(harr **owner, hval key) {
    if (owner && *owner) {
        *owner = hp_arr_cow(*owner);
        hp_arr_unset(*owner, key);
    }
    return hp_null;
}

hval hp_arr_unset_hval_owner(hval *owner, hval key) {
    if (owner && owner->tag == HV_ARR && owner->u.a) {
        owner->u.a = hp_arr_cow(owner->u.a);
        hp_arr_unset(owner->u.a, key);
    }
    return hp_null;
}

hval hp_arr_get(harr *a, hval idx) {
    if (!a) return hp_null;
    int64_t i = hp_key_index(a, idx);
    if (i < 0) return hp_null; /* PHP notice + null; we return null */
    return a->vals[i];
}

hval hp_arr_get_unchecked(harr *a, int64_t idx) {
    /* This entry point is only emitted for the compiler's canonical
     * `for ($i = 0; $i < count($a); $i++)` proof with a stable array. */
    /* Maps can have holes or string keys, so retain normal lookup semantics. */
    if (a->is_map) return hp_arr_get(a, hp_of_int(idx));
    return a->vals[idx];
}

void *hp_move_ptr(void *slot) {
    void *value = NULL;
    if (!slot) return NULL;
    memcpy(&value, slot, sizeof(value));
    memset(slot, 0, sizeof(value));
    return value;
}

hval hp_move_hval(hval *slot) {
    if (!slot) return hp_null;
    hval value = *slot;
    *slot = hp_null;
    return value;
}

/* Linear element extraction: the compiler only permits this for collections
 * whose element type is statically linear, so there must be no collection
 * aliases. Refuse a violated invariant rather than silently copying a value. */
hval hp_arr_take(harr *a, hval idx) {
    if (!a) return hp_null;
    if (!a->is_map) {
        if (idx.tag != HV_INT) hp_throw_str("Array index must be an integer");
        if (idx.u.i < 0 || (uint64_t)idx.u.i >= (uint64_t)a->len)
            hp_throw_str("Array index out of bounds");
    }
    if (hp_rc_count(&a->rc) != 1) {
        hp_throw(hp_of_str(hp_str_lit("cannot move an element from a shared collection")));
        return hp_null;
    }
    int64_t i = hp_key_index(a, idx);
    if (i < 0) return hp_null;
    hval value = a->vals[i];
    size_t last = a->len - 1;
    if (a->is_map) {
        hp_arr_index_del(a, (size_t)i);
        if ((size_t)i != last) hp_arr_index_del(a, last);
        hp_val_rc_dec(a->keys[i]);
    }
    if ((size_t)i != last) {
        a->vals[i] = a->vals[last];
        if (a->is_map) a->keys[i] = a->keys[last];
    }
    a->vals[last] = hp_null;
    if (a->is_map) a->keys[last] = hp_null;
    a->len = last;
    if (a->is_map && (size_t)i != last) hp_arr_index_add(a, (size_t)i);
    return value;
}

/* PHP-style indexing on any value: arrays/maps index normally, strings
 * return a 1-char string, null indexes to null (like PHP 8 null["]). */
hval hp_arr_get_v(hval container, hval idx) {
    if (container.tag == HV_ARR) {
        harr *a = container.u.a;
        if (a && !a->is_map) {
            if (idx.tag != HV_INT) hp_throw_str("Array index must be an integer");
            if (idx.u.i < 0 || (uint64_t)idx.u.i >= (uint64_t)a->len)
                hp_throw_str("Array index out of bounds");
        }
        return hp_arr_get(a, idx);
    }
    if (container.tag == HV_STR) {
        int64_t i = hp_val_to_int(idx);
        hstr *s = container.u.s;
        if (i < 0 || !s || (uint64_t)i >= (uint64_t)s->len)
            hp_throw_str("String index out of bounds");
        return hp_of_str(hp_str_new(s->data + i, 1));
    }
    return hp_null;
}

/* element write on a value container: only arrays are mutable in place;
 * the compiler only emits this for array-typed hvals. */
hval hp_arr_set_v(hval container, hval idx, hval v) {
    if (container.tag != HV_ARR || !container.u.a) return v;
    hp_arr_set(container.u.a, idx, v);
    return v;
}

hval hp_arr_set_take(harr *a, hval idx, hval v) {
    hp_arr_set(a, idx, v);
    hp_val_rc_dec(v);
    return v;
}

hval hp_arr_set_v_take(hval container, hval idx, hval v) {
    hp_arr_set_v(container, idx, v);
    hp_val_rc_dec(v);
    return v;
}

hval hp_arr_set_owner(harr **owner, hval idx, hval v) {
    if (!owner) return v;
    harr *a = hp_arr_cow(*owner);
    *owner = a;
    hp_arr_set(a, idx, v);
    return v;
}

hval hp_arr_set_hval_owner(hval *owner, hval idx, hval v) {
    if (!owner) return v;
    if (owner->tag != HV_ARR || !owner->u.a) {
        if (owner->tag != HV_NULL) return v;
        *owner = hp_of_arr(hp_arr_new());
    }
    harr *a = hp_arr_cow(owner->u.a);
    owner->u.a = a;
    hp_arr_set(a, idx, v);
    return v;
}

hval hp_arr_set_owner_take(harr **owner, hval idx, hval v) {
    hp_arr_set_owner(owner, idx, v);
    hp_val_rc_dec(v);
    return v;
}

hval hp_arr_set_hval_owner_take(hval *owner, hval idx, hval v) {
    hp_arr_set_hval_owner(owner, idx, v);
    hp_val_rc_dec(v);
    return v;
}

/* Update a nested array path while preserving value semantics at every
 * level. Each array on the path is detached before mutation. */
static void hp_arr_path_set(harr *a, const hval *path, size_t n, hval v) {
    if (!a || !n) return;
    if (n == 1) { hp_arr_set(a, path[0], v); return; }
    int64_t i = hp_key_index(a, path[0]);
    if (i < 0) {
        harr *created = hp_arr_new();
        hp_arr_set(a, path[0], hp_of_arr(created));
        hp_rc_dec(created); /* hp_arr_set retained the value for the entry */
        i = hp_key_index(a, path[0]);
    }
    hval childv = a->vals[i];
    if (childv.tag != HV_ARR || !childv.u.a) {
        hp_val_rc_dec(childv);
        childv = hp_of_arr(hp_arr_new());
    }
    harr *child = hp_arr_cow(childv.u.a);
    childv.u.a = child;
    /* hp_arr_cow consumes the parent slot's old reference if it clones; the
     * returned reference is transferred directly into that slot. */
    a->vals[i] = childv;
    hp_arr_path_set(child, path + 1, n - 1, v);
}

hval hp_arr_path_set_owner_take(harr **owner, const hval *path, size_t n, hval v) {
    if (owner && n) {
        *owner = hp_arr_cow(*owner);
        hp_arr_path_set(*owner, path, n, v);
    }
    hp_val_rc_dec(v);
    return v;
}

void hp_arr_push_owner(harr **owner, hval v) {
    if (!owner) return;
    harr *a = hp_arr_cow(*owner);
    *owner = a;
    hp_arr_push(a, v);
}

void hp_arr_push_hval_owner(hval *owner, hval v) {
    if (!owner) return;
    if (owner->tag != HV_ARR || !owner->u.a) {
        if (owner->tag != HV_NULL) return;
        *owner = hp_of_arr(hp_arr_new());
    }
    harr *a = hp_arr_cow(owner->u.a);
    owner->u.a = a;
    hp_arr_push(a, v);
}

hval hp_arr_push_owner_take(harr **owner, hval v) {
    hp_arr_push_owner(owner, v);
    hp_val_rc_dec(v);
    return v;
}

hval hp_arr_push_hval_owner_take(hval *owner, hval v) {
    hp_arr_push_hval_owner(owner, v);
    hp_val_rc_dec(v);
    return v;
}

void hp_arr_set(harr *a, hval idx, hval v) {
    /* Raw mutator. Owner-aware code must detach before calling this. */
    if (!a) return;
    if (idx.tag == HV_NULL) {
        hp_arr_push(a, v);
        return;
    }
    int64_t i = hp_key_index(a, idx);
    if (i >= 0) {
        hp_val_rc_inc(v);
        hp_val_rc_dec(a->vals[i]);
        a->vals[i] = v;
        return;
    }
    /* PHP semantics: arrays remember their keys. A string key or an int key
     * that does not continue the implicit 0..n sequence turns the array
     * into a keyed map ($m[1] = x keeps key 1; foreach yields it). */
    if (!a->is_map) {
        if (idx.tag == HV_STR)
            a->is_map = true;
        else if (idx.tag == HV_INT && idx.u.i != a->next_index)
            a->is_map = true;
    }
    hp_arr_ensure_keys(a);   /* a list turning into a map keeps its positions as keys */
    hp_arr_grow(a, a->len + 1);
    if (a->is_map) {
        a->keys = realloc(a->keys, a->cap * sizeof(hval));
        hp_val_rc_inc(idx);
        a->keys[a->len] = idx;
    }
    size_t newslot = a->len;
    hp_val_rc_inc(v);
    a->vals[a->len] = v;
    a->len++;
    if (a->is_map) hp_arr_index_add(a, newslot);
    if (idx.tag == HV_INT && idx.u.i >= a->next_index)
        a->next_index = idx.u.i + 1;
}

bool hp_arr_has(harr *a, hval key) {
    if (!a) return false;
    return hp_key_index(a, key) >= 0;
}

int64_t hp_arr_len(harr *a) { return a ? (int64_t)a->len : 0; }

harr *hp_arr_slice(harr *a, int64_t start, int64_t end) {
    harr *r = hp_arr_new();
    if (!a) return r;
    start = norm_index(start, a->len);
    if (end < 0) end += (int64_t)a->len;
    if ((size_t)end > a->len) end = (int64_t)a->len;
    for (int64_t i = start; i < end; i++)
        hp_arr_push(r, a->vals[i]);
    return r;
}

harr *hp_arr_keys(harr *a) {
    harr *r = hp_arr_new();
    if (!a) return r;
    for (size_t i = 0; i < a->len; i++)
        hp_arr_push(r, a->is_map ? a->keys[i] : hp_of_int((int64_t)i));
    return r;
}

harr *hp_arr_values(harr *a) {
    harr *r = hp_arr_new();
    if (!a) return r;
    for (size_t i = 0; i < a->len; i++)
        hp_arr_push(r, a->vals[i]);
    return r;
}

harr *hp_arr_merge(harr *a, harr *b) {
    harr *r = hp_arr_clone(a ? a : hp_null_arr());
    if (!b) return r;
    for (size_t i = 0; i < b->len; i++) {
        if (b->is_map && b->keys[i].tag == HV_STR)
            hp_arr_set(r, b->keys[i], b->vals[i]);
        else
            hp_arr_push(r, b->vals[i]);
    }
    return r;
}

harr *hp_arr_reverse(harr *a) {
    harr *r = hp_arr_new();
    if (!a) return r;
    for (size_t i = a->len; i > 0; i--)
        hp_arr_push(r, a->vals[i - 1]);
    return r;
}

bool hp_arr_in(harr *a, hval v) {
    if (!a) return false;
    for (size_t i = 0; i < a->len; i++)
        if (hp_val_eq(a->vals[i], v)) return true;
    return false;
}

static int hp_sort_cmp(const void *pa, const void *pb) {
    hval a = *(const hval *)pa, b = *(const hval *)pb;
    return (int)hp_val_cmp(a, b);
}

harr *hp_arr_sort(harr *a) {
    harr *r = hp_arr_clone(a ? a : hp_null_arr());
    qsort(r->vals, r->len, sizeof(hval), hp_sort_cmp);
    return r;
}

harr *hp_arr_rsort(harr *a) {
    harr *r = hp_arr_sort(a);
    hp_arr_reverse(r);
    return r;
}

harr *hp_map_sort_by_key(harr *a) {
    harr *r = hp_arr_clone(a ? a : hp_null_arr());
    /* simple insertion sort by key */
    for (size_t i = 1; i < r->len; i++) {
        hval kv = r->keys[i], vv = r->vals[i];
        size_t j = i;
        while (j > 0 && hp_val_cmp(r->keys[j - 1], kv) > 0) {
            r->keys[j] = r->keys[j - 1];
            r->vals[j] = r->vals[j - 1];
            j--;
        }
        r->keys[j] = kv;
        r->vals[j] = vv;
    }
    return r;
}

hval hp_arr_sum(harr *a) {
    if (!a) return hp_of_int(0);
    bool anyf = false;
    double sum = 0;
    int64_t isum = 0;
    for (size_t i = 0; i < a->len; i++) {
        if (a->vals[i].tag == HV_FLOAT) anyf = true;
        sum += hp_val_to_float(a->vals[i]);
        int64_t n = hp_val_to_int(a->vals[i]);
        if (!anyf && ((n > 0 && isum > INT64_MAX - n) ||
                      (n < 0 && isum < INT64_MIN - n)))
            anyf = true;
        if (!anyf) isum += n;
    }
    return anyf ? hp_of_float(sum) : hp_of_int(isum);
}

harr *hp_arr_unique(harr *a) {
    harr *r = hp_arr_new();
    if (!a) return r;
    for (size_t i = 0; i < a->len; i++)
        if (!hp_arr_in(r, a->vals[i])) hp_arr_push(r, a->vals[i]);
    return r;
}

harr *hp_range(int64_t lo, int64_t hi) {
    harr *a = hp_arr_new();
    if (lo <= hi)
        for (int64_t i = lo; i <= hi; i++) hp_arr_push(a, hp_of_int(i));
    else
        for (int64_t i = lo; i >= hi; i--) hp_arr_push(a, hp_of_int(i));
    return a;
}

void hp_arr_free(harr *a) {
    if (!a) return;
    hp_gc_unregister(&a->gc);
    hp_gc_drop_depth++;
    for (size_t i = 0; i < a->len; i++) {
        hp_val_rc_dec(a->vals[i]);
        if (a->is_map && a->keys) hp_val_rc_dec(a->keys[i]);
    }
    free(a->vals);
    free(a->keys);
    free(a->index);
    free(a);
    hp_gc_drop_depth--;
}

static inline void hp_arr_release_cow_owner(harr *a) {
    hp_gc_lock();
    uint32_t old = hp_rc_release(&a->rc);
    if (old == 1) hp_arr_free(a);
    else if (old != 0 && old != UINT32_MAX) hp_gc_candidate(a->gc);
    hp_gc_unlock();
}

/* COW: the runtime shares arrays on assignment; writes go through
 * hp_arr_cow which clones when more than one owner exists. Ownership is
 * tracked by the compiler: values assigned by-value share, explicit
 * hp_arr_cow marks the write. In generated code, every mutating call
 * routes through hp_arr_cow first. */
harr *hp_arr_cow(harr *a) {
    if (!a) return hp_arr_new();
    if (hp_rc_count(&a->rc) > 1) {
        harr *r = hp_arr_clone(a);
        r->rc = 1;
        hp_arr_release_cow_owner(a);
        return r;
    }
    return a;
}

/* ---------------- foreach ---------------- */
hp_foreach_ctx hp_arr_iter(harr *a) {
    hp_foreach_ctx c = { .a = a, .pos = 0, .i = 0, .is_str = false };
    return c;
}
hp_foreach_ctx hp_map_iter(harr *a) { return hp_arr_iter(a); }

/* foreach over a dynamically typed value (untyped property, mixed param):
 * arrays/strings iterate, anything else is a catchable error, like PHP. */
hp_foreach_ctx hp_val_iter(hval v) {
    switch (v.tag) {
    case HV_ARR: return hp_arr_iter(v.u.a);
    case HV_STR: return hp_str_iter(v.u.s);
    case HV_NULL: return hp_arr_iter(hp_null_arr());
    default: {
        hstr *msg = hp_str_concat2(hp_str_lit("foreach: cannot iterate "),
                                   hp_str_lit(hp_kind_name(v.tag)));
        hp_throw(hp_of_str(msg));
        return hp_arr_iter(hp_null_arr());
    }
    }
}
hp_foreach_ctx hp_str_iter(hstr *s) {
    hp_foreach_ctx c = { .a = NULL, .s = s, .pos = 0, .i = 0, .is_str = true };
    return c;
}
hval hpv_tmp = { HV_NULL, { 0 } };

bool hp_foreach_next(hp_foreach_ctx *ctx, hval *out) {
    if (ctx->is_str) {
        if (!ctx->s || ctx->pos >= ctx->s->len) return false;
        *out = hp_of_str(hp_str_new(ctx->s->data + ctx->pos, 1));
        ctx->k = hp_of_int((int64_t)ctx->pos);
        ctx->pos++;
        ctx->i = (int64_t)ctx->pos;
        return true;
    }
    if (!ctx->a || ctx->pos >= ctx->a->len) return false;
    *out = ctx->a->vals[ctx->pos];
    ctx->k = ctx->a->is_map ? ctx->a->keys[ctx->pos] : hp_of_int((int64_t)ctx->pos);
    ctx->i = (int64_t)ctx->pos;
    ctx->pos++;
    return true;
}

/* ---------------- dynamic operators ---------------- */
static bool hp_num_like(hval v); /* defined below */

hval hp_add(hval a, hval b) {
    if (a.tag == HV_ARR && b.tag == HV_ARR) return hp_of_arr(hp_arr_merge(a.u.a, b.u.a));
    if (a.tag == HV_STR || b.tag == HV_STR) {
        /* PHP numeric-string arithmetic: "1" + "2" is 3, "1.5" + 1 is 2.5.
         * A non-numeric string has no numeric meaning, so we stay friendly
         * and concatenate ('.' is the real concatenation operator). */
        if (hp_num_like(a) && hp_num_like(b)) {
            double da = hp_val_to_float(a), db = hp_val_to_float(b);
            if (a.tag != HV_FLOAT && b.tag != HV_FLOAT &&
                da == (double)(int64_t)da && db == (double)(int64_t)db)
                return hp_of_int(hp_i64_add_wrap((int64_t)da, (int64_t)db));
            return hp_of_float(da + db);
        }
        return hp_of_str(hp_str_join_v(a, b));
    }
    if (a.tag == HV_FLOAT || b.tag == HV_FLOAT)
        return hp_of_float(hp_val_to_float(a) + hp_val_to_float(b));
    return hp_of_int(hp_i64_add_wrap(hp_val_to_int(a), hp_val_to_int(b)));
}

hval hp_sub(hval a, hval b) {
    if (a.tag == HV_FLOAT || b.tag == HV_FLOAT)
        return hp_of_float(hp_val_to_float(a) - hp_val_to_float(b));
    return hp_of_int(hp_i64_sub_wrap(hp_val_to_int(a), hp_val_to_int(b)));
}

hval hp_mul(hval a, hval b) {
    if (a.tag == HV_FLOAT || b.tag == HV_FLOAT)
        return hp_of_float(hp_val_to_float(a) * hp_val_to_float(b));
    return hp_of_int(hp_i64_mul_wrap(hp_val_to_int(a), hp_val_to_int(b)));
}

hval hp_divv(hval a, hval b) {
    double d = hp_val_to_float(b);
    if (d == 0.0) hp_throw_str("Division by zero");
    return hp_of_float(hp_val_to_float(a) / d);
}

bool hp_streq(hval a, hval b) { return hp_val_eq(a, b); }

/* === : identity. Both the tag and the value must match — no bool/int/str
 * coercion. Two different tags are never equal (PHP 8 semantics). */
static bool hp_ident_eq_r(hval a, hval b, int depth);

bool hp_ident_eq(hval a, hval b) { return hp_ident_eq_r(a, b, 0); }

static bool hp_ident_eq_r(hval a, hval b, int depth) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
    case HV_NULL: return true;
    case HV_BOOL: return a.u.b == b.u.b;
    case HV_INT: return a.u.i == b.u.i;
    case HV_FLOAT: return a.u.f == b.u.f;
    case HV_STR: return hp_str_eq(a.u.s, b.u.s);
    case HV_ARR:
        /* PHP 8 ===: same key/value pairs, same order, same types */
        if (a.u.a == b.u.a) return true;
        if (!a.u.a || !b.u.a) return false;
        if (a.u.a->len != b.u.a->len) return false;
        if (a.u.a->is_map != b.u.a->is_map) return false;
        if (depth > 64) return false;   /* cycles: give up conservatively */
        for (size_t i = 0; i < a.u.a->len; i++) {
            if (a.u.a->keys && b.u.a->keys) {
                if (!hp_ident_eq_r(a.u.a->keys[i], b.u.a->keys[i], depth + 1))
                    return false;
            } else if (a.u.a->keys || b.u.a->keys) {
                return false;
            }
            if (!hp_ident_eq_r(a.u.a->vals[i], b.u.a->vals[i], depth + 1))
                return false;
        }
        return true;
    default: return a.u.p == b.u.p;       /* objects/closures by identity */
    }
}
static bool hp_num_like(hval v) {
    return v.tag == HV_INT || v.tag == HV_FLOAT || v.tag == HV_BOOL ||
           (v.tag == HV_STR && hp_str_is_numeric(v.u.s));
}

bool hp_lt(hval a, hval b) { return hp_val_cmp(a, b) < 0; }
bool hp_gt(hval a, hval b) { return hp_val_cmp(a, b) > 0; }
bool hp_le(hval a, hval b) { return hp_val_cmp(a, b) <= 0; }
bool hp_ge(hval a, hval b) { return hp_val_cmp(a, b) >= 0; }
int64_t hp_cmp(hval a, hval b) { return hp_val_cmp(a, b); }

int64_t hp_mod(int64_t a, int64_t b) {
    if (b == 0) hp_throw_str("Modulo by zero");
    if (a == INT64_MIN && b == -1) return 0;
    return a % b;
}

int64_t hp_pow_i(int64_t a, int64_t b) {
    if (b < 0) return 0; /* PHP: 2 ** -1 is float; ints truncate to 0 */
    uint64_t r = 1, base = (uint64_t)a;
    while (b) {
        if (b & 1) r *= base;
        base *= base;
        b >>= 1;
    }
    return hp_i64_from_u(r);
}

hval hp_powv(hval a, hval b) {
    if (a.tag == HV_INT && b.tag == HV_INT && b.u.i >= 0) {
        uint64_t r = 1, base = (uint64_t)a.u.i;
        int64_t e = b.u.i;
        while (e) {
            if (e & 1) r *= base;
            base *= base;
            e >>= 1;
        }
        return hp_of_int(hp_i64_from_u(r));
    }
    return hp_of_float(pow(hp_val_to_float(a), hp_val_to_float(b)));
}

hval hp_nullcoal(hval a, hval b) {
    if (a.tag == HV_NULL) return b;
    return a;
}

/* match ($x) { "a" => ... } compares with ===, the way PHP 8 does: match is
 * strict, only `switch`/`==` is loose. So `match(0.0) { 0 => ... }` takes the
 * default arm, and a pattern can only ever be taken by a value of its own
 * type - which is what lets the checker narrow the subject inside an arm. */
hval hp_match_any(hval subj, hval *pats, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (hp_ident_eq(subj, pats[i])) return hp_of_bool(true);
    return hp_of_bool(false);
}

hval hp_match_no_default(void) {
    hp_throw_str("UnhandledMatchError");
    return hp_null;
}

/* hp_div is a static inline in hphp_rt.h so gcc can fold away the
 * zero-check for literal divisors and vectorize loops like hand-written C */

/* ---------------- exceptions ---------------- */
/* Thread-local: every spawned worker thread gets its own exception state
 * and closure environment (see hphp_thr.c), so workers can throw/catch and
 * use captured variables without racing the main thread. */
HP_THREAD_QUAL hp_try_frame *hp_cur_try = NULL;
HP_THREAD_QUAL hval hp_exception = { HV_NULL, { 0 } };

typedef struct hp_cleanup_node {
    void *slot;
    HKind kind;
    unsigned mode; /* 0=hval, 1=pointer, 2=by-reference box */
} hp_cleanup_node;
static HP_THREAD_QUAL hp_cleanup_node *hp_cleanups;
HP_THREAD_QUAL size_t hp_cleanup_len;
static HP_THREAD_QUAL size_t hp_cleanup_cap;


static void hp_cleanup_register(void *slot, HKind kind, unsigned mode) {
    if (hp_cleanup_len == hp_cleanup_cap) {
        size_t cap = hp_cleanup_cap ? hp_cleanup_cap * 2 : 128;
        hp_cleanup_node *nodes = (hp_cleanup_node *)realloc(hp_cleanups, cap * sizeof(*nodes));
        if (!nodes) { fprintf(stderr, "Fatal error: out of memory\n"); exit(255); }
        hp_cleanups = nodes;
        hp_cleanup_cap = cap;
    }
    hp_cleanup_node *n = &hp_cleanups[hp_cleanup_len++];
    n->slot = slot;
    n->kind = kind;
    n->mode = mode;
}

void hp_cleanup_register_hval(hval *slot) { hp_cleanup_register(slot, HV_NULL, 0); }
void hp_cleanup_register_ptr(void **slot, HKind kind) { hp_cleanup_register(slot, kind, 1); }
void hp_cleanup_register_box(hval **slot) { hp_cleanup_register(slot, HV_NULL, 2); }
void hp_cleanup_register_region(hp_region *region) {
    hp_cleanup_register((void *)region, HV_NULL, 3);
}

void hp_cleanup_pop_to_slow(void *mark) {
    size_t target = (size_t)(uintptr_t)mark;
    if (target > hp_cleanup_len) target = 0;
    while (hp_cleanup_len > target) {
        hp_cleanup_node *n = &hp_cleanups[--hp_cleanup_len];
        if (n->mode == 3) {
            hp_region_end((hp_region *)n->slot);
            continue;
        }
        if (n->mode == 0) {
            hval *slot = (hval *)n->slot;
            hval value = *slot;
            *slot = hp_null;
            hp_val_rc_dec(value);
        } else {
            void *value = NULL;
            memcpy(&value, n->slot, sizeof(value));
            memset(n->slot, 0, sizeof(value));
            if (n->mode == 2) {
                hp_box_free((hval *)value);
            } else {
                hval hvv = hp_null;
                switch (n->kind) {
                case HV_STR: hvv = hp_of_str((hstr *)value); break;
                case HV_ARR: hvv = hp_of_arr((harr *)value); break;
                case HV_CLO: hvv = hp_of_clo((hclosure *)value); break;
                case HV_OBJ: hvv = hp_of_ptr(value); break;
                default: break;
                }
                hp_val_rc_dec(hvv);
            }
        }
    }
}

void hp_throw(hval v) {
    /* Takes one owned value; generated throws retain borrowed expressions. */
    hp_val_rc_dec(hp_exception);
    hp_exception = v;
    if (!hp_cur_try) {
        hstr *m = hp_val_to_str(hp_exception);
        fprintf(stderr, "PHP Fatal error: Uncaught exception: %.*s\n",
                (int)m->len, m->data);
        hp_cleanup_pop_to(NULL);
        exit(255);
    }
    hp_cur_try->caught = false;
    hp_try_frame *f = hp_cur_try;
    hp_cleanup_pop_to(f->cleanup_mark);
    longjmp(f->jmp, 1);
}

void hp_throw_str(const char *msg) {
    hp_throw(hp_of_str(hp_str_cstr(msg)));
}

hstr *hp_exception_msg(void) {
    return hp_val_to_str(hp_exception);
}

void hp_try_push(hp_try_frame *f) {
    f->caught = false;
    f->prev = hp_cur_try;
    f->cleanup_mark = hp_cleanup_mark();
    hp_cur_try = f;
}

void hp_try_pop(hp_try_frame *f) {
    if (hp_cur_try == f) hp_cur_try = f->prev;
}

/* The ONLY setjmp in generated programs lives here (Lua/CPython pattern).
 * Runs the try body; on a throw, runs the catch handler. A handler that
 * returns a non-null hval signals "not handled": we hand the exception back
 * to the call site, which runs its finally block and only then rethrows. */
hval hp_try_run(hp_try_fn body_fn, hp_try_fn catch_fn, void *env) {
    hp_try_frame fr, hfr;
    hp_try_push(&fr);
    if (setjmp(fr.jmp) == 0) {
        hval r = body_fn(env, hp_null);
        hp_try_pop(&fr);
        return r; /* body completion (normally hp_null) */
    }
    hp_try_pop(&fr);
    /* The handler runs under its OWN frame: a throw raised inside catch (a
     * bare rethrow or a fresh exception) lands back here instead of unwinding
     * past this try statement — otherwise the caller's finally block would be
     * skipped. We then hand the exception back (non-null), and the call site
     * runs finally before propagating. */
    hp_try_push(&hfr);
    if (setjmp(hfr.jmp) == 0) {
        hval r = catch_fn(env, hp_exception);
        hp_try_pop(&hfr);
        if (r.tag == HV_NULL) {
            hp_val_rc_dec(hp_exception);
            hp_exception = hp_null;
        } else if (r.tag == hp_exception.tag && r.u.p == hp_exception.u.p) {
            /* Unhandled exception result takes over the TLS owner's ref. */
            hp_exception = hp_null;
        }
        return r; /* hp_null = handled; else rethrow */
    }
    hp_try_pop(&hfr);
    return hp_exception; /* handler threw: caller runs finally, then rethrows */
}

/* ---------------- closures ---------------- */
HP_THREAD_QUAL void *hp_cur_env = NULL;

void *hp_closure_env(void) { return hp_cur_env; }

hclosure *hpclosure_new(void *env, size_t envsz, void *fn,
                        void (*drop_env)(void *), void (*trace_env)(void *)) {
    hclosure *c = hp_alloc(sizeof(hclosure) + envsz);
    c->fn = (hfn_t)fn;
    c->rc = 1;
    c->envsz = envsz;
    c->drop_env = drop_env;
    c->trace_env = trace_env;
    if (env && envsz) memcpy((char *)c + sizeof(hclosure), env, envsz);
    hp_gc_register(c, &c->rc, &c->gc, hp_gc_trace_closure,
                   hp_gc_clear_closure, hp_gc_destroy_closure);
    return c;
}

void hpclosure_free(hclosure *c) {
    if (!c) return;
    hp_gc_unregister(&c->gc);
    hp_gc_drop_depth++;
    if (c->drop_env) c->drop_env((char *)c + sizeof(hclosure));
    free(c);
    hp_gc_drop_depth--;
}

hval hp_of_clo(hclosure *c) {
    hval v = { HV_CLO, { .p = c } };
    return v;
}

/* The closure trampoline: closure impls have C signature hval(void*).
 * We build a real call via a union-free double-cast through a function
 * pointer of the exact impl type (hval (*)(void*)). Args travel through a
 * heap hval array whose data pointer becomes the impl's single argument. */
typedef hval (*hclo_impl_t)(void *);

hval hp_closure_call_arr(hclosure *c, hval *args, int nargs) {
    if (!c) return hp_null;
    harr *aa = hp_arr_new();
    void *mark = hp_cleanup_mark();
    hp_cleanup_register_ptr((void **)&aa, HV_ARR);
    for (int i = 0; i < nargs; i++) hp_arr_push_take(aa, args[i]);
    void *saved = hp_cur_env;
    hp_cur_env = (char *)c + sizeof(hclosure);
    hclo_impl_t fn = (hclo_impl_t)c->fn;
    hval r = fn(aa->vals);   /* hval* data of the packed args */
    hp_cur_env = saved;
    hp_cleanup_pop_to(mark);
    return r;
}

hval hp_closure_call(hclosure *c, int nargs, const hval *args) {
    return hp_closure_call_arr(c, (hval *)args, nargs);
}

/* ---------------- smart pointers / objects ---------------- */
/* Literal strings have a saturated count and are process-lifetime values.
 * Mutable strings and arrays start at one. Arrays own their entries once the
 * compiler's ownership-aware container API is used. */
void hp_val_free(hval v) { hp_val_rc_dec(v); }

void hp_val_rc_inc(hval v) {
    bool gc_value = v.tag == HV_ARR || v.tag == HV_CLO || v.tag == HV_OBJ;
    if (gc_value) hp_gc_lock();
    switch (v.tag) {
    case HV_STR: if (v.u.s) hp_rc_retain(&v.u.s->rc); break;
    case HV_ARR: if (v.u.a) hp_rc_retain(&v.u.a->rc); break;
    case HV_CLO: if (v.u.c) hp_rc_retain(&v.u.c->rc); break;
    case HV_OBJ: hp_rc_inc(v.u.p); break;
    default: break;
    }
    if (gc_value) hp_gc_unlock();
}

void hp_val_rc_dec(hval v) {
    bool gc_value = v.tag == HV_ARR || v.tag == HV_CLO || v.tag == HV_OBJ;
    if (gc_value) hp_gc_lock();
    switch (v.tag) {
    case HV_STR:
        if (v.u.s && hp_rc_release(&v.u.s->rc) == 1) hp_str_free(v.u.s);
        break;
    case HV_ARR:
        if (v.u.a) {
            uint32_t old = hp_rc_release(&v.u.a->rc);
            if (old == 1) hp_arr_free(v.u.a);
            else if (old != 0 && old != UINT32_MAX)
                hp_gc_candidate(v.u.a->gc);
        }
        break;
    case HV_CLO:
        if (v.u.c) {
            uint32_t old = hp_rc_release(&v.u.c->rc);
            if (old == 1) hpclosure_free(v.u.c);
            else if (old != 0 && old != UINT32_MAX)
                hp_gc_candidate(v.u.c->gc);
        }
        break;
    case HV_OBJ: hp_rc_dec(v.u.p); break;
    default: break;
    }
    if (gc_value) hp_gc_unlock();
}

static void hp_weak_unlink(hp_weak_ref *weak) {
    if (!weak || !weak->prev) return;
    *weak->prev = weak->next;
    if (weak->next) weak->next->prev = weak->prev;
    weak->target = NULL;
    weak->next = NULL;
    weak->prev = NULL;
}

hval hp_weak_assign(hp_weak_ref **slot, hval value) {
    if (!slot) return value;
    hobj *target = value.tag == HV_OBJ ? (hobj *)value.u.p : NULL;
    hp_weak_ref *weak = *slot;
    if (weak && weak->target == target) return value;
    if (weak) hp_weak_unlink(weak);
    if (!target) {
        free(weak);
        *slot = NULL;
        return value;
    }
    if (!weak) weak = (hp_weak_ref *)calloc(1, sizeof(*weak));
    if (!weak) { *slot = NULL; return value; }
    weak->target = target;
    weak->next = target->weak_refs;
    weak->prev = &target->weak_refs;
    if (weak->next) weak->next->prev = &weak->next;
    target->weak_refs = weak;
    *slot = weak;
    return value;
}

hval hp_weak_get(const hp_weak_ref *weak) {
    hp_gc_collect_now(weak ? weak->target : NULL);
    return hp_of_ptr(weak ? weak->target : NULL);
}

void hp_weak_free(hp_weak_ref *weak) {
    if (!weak) return;
    hp_weak_unlink(weak);
    free(weak);
}

void hp_weak_invalidate(hobj *object) {
    if (!object) return;
    hp_weak_ref *weak = object->weak_refs;
    object->weak_refs = NULL;
    while (weak) {
        hp_weak_ref *next = weak->next;
        weak->target = NULL;
        weak->next = NULL;
        weak->prev = NULL;
        weak = next;
    }
}

hval hp_assign_hval(hval *slot, hval value, bool retain_value) {
    if (!slot) return value;
    if (retain_value) hp_val_rc_inc(value);
    hval old = *slot;
    *slot = value;
    hp_val_rc_dec(old);
    return value;
}

void *hp_assign_ptr(void **slot, void *value, HKind kind, bool retain_value) {
    if (!slot) return value;
    hval next = hp_null, old = hp_null;
    switch (kind) {
    case HV_STR: next = hp_of_str((hstr *)value); old = hp_of_str((hstr *)*slot); break;
    case HV_ARR: next = hp_of_arr((harr *)value); old = hp_of_arr((harr *)*slot); break;
    case HV_CLO: next = hp_of_clo((hclosure *)value); old = hp_of_clo((hclosure *)*slot); break;
    case HV_OBJ: next = hp_of_ptr(value); old = hp_of_ptr(*slot); break;
    default: *slot = value; return value;
    }
    if (retain_value) hp_val_rc_inc(next);
    *slot = value;
    hp_val_rc_dec(old);
    return value;
}

hval hp_arg_keep_hval(hval value, bool retain_value) {
    if (retain_value) hp_val_rc_inc(value);
    return value;
}

void *hp_arg_keep_ptr(void *value, HKind kind, bool retain_value) {
    if (retain_value) {
        hval v = hp_null;
        switch (kind) {
        case HV_STR: v = hp_of_str((hstr *)value); break;
        case HV_ARR: v = hp_of_arr((harr *)value); break;
        case HV_CLO: v = hp_of_clo((hclosure *)value); break;
        case HV_OBJ: v = hp_of_ptr(value); break;
        default: break;
        }
        hp_val_rc_inc(v);
    }
    return value;
}

/* ---------------- lifecycle ---------------- */
static bool hp_inited = false;
typedef struct { uint32_t rc; hval value; } hp_box_layout;
hval *hp_box_new(void) {
    hp_box_layout *box = (hp_box_layout *)malloc(sizeof(hp_box_layout));
    if (!box) return NULL;
    box->rc = 1;
    box->value = hp_null;
    return &box->value;
}

void hp_box_retain(hval *b) {
    if (!b) return;
    hp_box_layout *box = (hp_box_layout *)((char *)b - offsetof(hp_box_layout, value));
    if (box->rc != UINT32_MAX) ++box->rc;
}

void hp_box_free(hval *b) {
    if (!b) return;
    hp_box_layout *box = (hp_box_layout *)((char *)b - offsetof(hp_box_layout, value));
    if (!box->rc || box->rc == UINT32_MAX) return;
    if (--box->rc == 0) {
        hp_val_rc_dec(box->value);
        free(box);
    }
}

#ifdef _WIN32
static UINT hp_old_out_cp, hp_old_in_cp;
static int hp_cp_saved;
static void hp_restore_console_cp(void) {
    if (!hp_cp_saved) return;
    SetConsoleOutputCP(hp_old_out_cp);
    SetConsoleCP(hp_old_in_cp);
}
#endif

void hp_init(void) {
    if (hp_inited) return;
    hp_inited = true;
#ifdef _WIN32
    /* A Windows console defaults to an OEM codepage (437/850), so UTF-8
     * strings print as mojibake. Switch a real console to UTF-8 and restore
     * it on exit; a pipe or a file already receives the raw bytes. */
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            if (!hp_cp_saved) {
                hp_old_out_cp = GetConsoleOutputCP();
                hp_old_in_cp = GetConsoleCP();
                hp_cp_saved = 1;
                atexit(hp_restore_console_cp);
            }
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
        }
    }
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
}

void hp_shutdown(void) {
    if (hp_cur_try) {
        /* uncaught */
    }
}
