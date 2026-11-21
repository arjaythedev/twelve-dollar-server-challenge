#include "sqlite3.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

/* Every page is covered either by high or by the single separately tracked
 * rekey destination. We deliberately retain high across truncation and
 * eviction. A stale bound can only send an unnecessary truncate downstream. */
typedef struct {
    sqlite3_pcache *inner;
    atomic_uint high;
    unsigned renamed;
    pthread_mutex_t rare;
} BoundedCache;

static sqlite3_pcache_methods2 backend;

#ifdef PCACHE_TRACE
#include <stdio.h>
static atomic_ulong skips, forwards, exceptional_moves;
void swap_trace_dump(void) {
    fprintf(stderr, "public-cache {\"skip\":%lu,\"forward\":%lu,\"high_rekey\":%lu}\n",
            atomic_load(&skips), atomic_load(&forwards), atomic_load(&exceptional_moves));
}
#define TRACE(v) ((void)atomic_fetch_add_explicit(&(v), 1, memory_order_relaxed))
#else
#define TRACE(v) ((void)0)
#endif

static sqlite3_pcache *bounded_create(int size, int extra, int purgeable) {
    BoundedCache *cache = malloc(sizeof *cache);
    if (!cache) return NULL;
    if (pthread_mutex_init(&cache->rare, NULL)) {
        free(cache);
        return NULL;
    }
    cache->inner = backend.xCreate(size, extra, purgeable);
    if (!cache->inner) {
        pthread_mutex_destroy(&cache->rare);
        free(cache);
        return NULL;
    }
    atomic_init(&cache->high, 0);
    cache->renamed = 0;
    return (sqlite3_pcache *)cache;
}

static void bounded_size(sqlite3_pcache *handle, int size) {
    BoundedCache *cache = (BoundedCache *)handle;
    backend.xCachesize(cache->inner, size);
}

static int bounded_count(sqlite3_pcache *handle) {
    BoundedCache *cache = (BoundedCache *)handle;
    return backend.xPagecount(cache->inner);
}

static sqlite3_pcache_page *bounded_fetch(sqlite3_pcache *handle, unsigned key, int create) {
    BoundedCache *cache = (BoundedCache *)handle;
    if (create && key > atomic_load_explicit(&cache->high, memory_order_relaxed)) {
        pthread_mutex_lock(&cache->rare);
        if (key > atomic_load_explicit(&cache->high, memory_order_relaxed)) {
            /* Publish before calling the backend. Failed creations may leave
             * an overestimate, but never a key invisible to xTruncate. */
            atomic_store_explicit(&cache->high, key, memory_order_relaxed);
        }
        pthread_mutex_unlock(&cache->rare);
    }
    return backend.xFetch(cache->inner, key, create);
}

static void bounded_unpin(sqlite3_pcache *handle, sqlite3_pcache_page *page, int discard) {
    BoundedCache *cache = (BoundedCache *)handle;
    backend.xUnpin(cache->inner, page, discard);
}

static void bounded_rekey(sqlite3_pcache *handle, sqlite3_pcache_page *page,
                          unsigned old_key, unsigned new_key) {
    if (old_key == new_key) return;
    BoundedCache *cache = (BoundedCache *)handle;
    pthread_mutex_lock(&cache->rare);
    unsigned high = atomic_load_explicit(&cache->high, memory_order_relaxed);
    /* The public rekey contract discards any unpinned destination. SQLite's
     * pager normally did this already. Some providers require it to be
     * absent, so honor the contract for a direct caller as well. */
    if (new_key <= high || new_key == cache->renamed) {
        sqlite3_pcache_page *previous = backend.xFetch(cache->inner, new_key, 0);
        if (previous) backend.xUnpin(cache->inner, previous, 1);
    }
    backend.xRekey(cache->inner, page, old_key, new_key);
    if (old_key == cache->renamed) cache->renamed = 0;
    if (new_key > high && new_key != cache->renamed) {
        TRACE(exceptional_moves);
        /* Track the greater of two exceptional destinations independently.
         * The smaller can safely join the permanent bound. */
        unsigned lower = cache->renamed;
        if (new_key > lower) cache->renamed = new_key;
        else lower = new_key;
        if (lower > high) atomic_store_explicit(&cache->high, lower, memory_order_relaxed);
    }
    pthread_mutex_unlock(&cache->rare);
}

static void bounded_truncate(sqlite3_pcache *handle, unsigned limit) {
    BoundedCache *cache = (BoundedCache *)handle;
    pthread_mutex_lock(&cache->rare);
    if (cache->renamed >= limit || atomic_load_explicit(&cache->high, memory_order_relaxed) >= limit) {
        TRACE(forwards);
        backend.xTruncate(cache->inner, limit);
        if (cache->renamed >= limit) cache->renamed = 0;
    } else TRACE(skips);
    pthread_mutex_unlock(&cache->rare);
}

static void bounded_destroy(sqlite3_pcache *handle) {
    BoundedCache *cache = (BoundedCache *)handle;
    backend.xDestroy(cache->inner);
    pthread_mutex_destroy(&cache->rare);
    free(cache);
}

static void bounded_shrink(sqlite3_pcache *handle) {
    BoundedCache *cache = (BoundedCache *)handle;
    backend.xShrink(cache->inner);
}

/* Called once before the application's first SQLite call. The inherited
 * xInit, xShutdown and pArg continue to belong to the original provider. */
int pcache_install(void) {
    sqlite3_pcache_methods2 methods;
    int rc = sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &methods);
    if (rc != SQLITE_OK) return rc;
    if (methods.xCreate == bounded_create) return SQLITE_OK;
    backend = methods;
    methods.xCreate = bounded_create;
    methods.xCachesize = bounded_size;
    methods.xPagecount = bounded_count;
    methods.xFetch = bounded_fetch;
    methods.xUnpin = bounded_unpin;
    methods.xRekey = bounded_rekey;
    methods.xTruncate = bounded_truncate;
    methods.xDestroy = bounded_destroy;
    if (methods.xShrink) methods.xShrink = bounded_shrink;
    return sqlite3_config(SQLITE_CONFIG_PCACHE2, &methods);
}
