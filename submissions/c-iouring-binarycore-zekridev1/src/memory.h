#ifndef BINARYCORE_MEMORY_H
#define BINARYCORE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Arena allocator: bump-pointer allocation with a free-list pool. No
 * per-request malloc/free in the hot path. Arenas are refcounted so a batch
 * can hand references to in-flight sends and release them when the send
 * completes. Modeled on c-iouring-pavlyhalim/src/server.c (arena_get /
 * arena_unref / arena_reserve). */

struct bc_arena {
    struct bc_arena *next;
    uint32_t refs, used, cap;
    uint8_t data[];
};

struct bc_arena_pool {
    struct bc_arena *head;
    unsigned count;
};

static inline struct bc_arena *bc_arena_get(struct bc_arena_pool *pool,
                                            size_t cap) {
    struct bc_arena *a = pool->head;
    if (a) {
        pool->head = a->next;
        pool->count--;
    } else {
        a = (struct bc_arena *)malloc(sizeof *a + cap);
        if (!a) return NULL;
        a->cap = (uint32_t)cap;
    }
    a->refs = 1;
    a->used = 0;
    return a;
}

static inline void bc_arena_put(struct bc_arena_pool *pool, struct bc_arena *a) {
    if (!a) return;
    if (--a->refs) return;
    if (pool->count < 64) {
        a->next = pool->head;
        pool->head = a;
        pool->count++;
    } else {
        free(a);
    }
}

/* Reserve `n` bytes in the current batch arena, growing the arena chain as
 * needed. Returns a pointer into the arena; `cur` is updated in place. */
static inline uint8_t *bc_arena_reserve(struct bc_arena_pool *pool,
                                        struct bc_arena **cur, size_t cap,
                                        unsigned n) {
    if (!*cur || (*cur)->used + n > (*cur)->cap) {
        struct bc_arena *a = bc_arena_get(pool, cap);
        if (!a) return NULL;
        *cur = a;
    }
    uint8_t *p = (*cur)->data + (*cur)->used;
    (*cur)->used += n;
    return p;
}

#endif /* BINARYCORE_MEMORY_H */
