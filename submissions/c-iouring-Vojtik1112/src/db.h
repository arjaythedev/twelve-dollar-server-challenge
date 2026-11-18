// SQLite persistence, pure-PCACHE2 direct hugepage cache over unix-excl,
// commit-frame WAL coalescing VFS, carried read transactions, and non-blocking nice-19 checkpointing.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// -----------------------------------------------------------------------------
// FastCache (SQLITE_CONFIG_PCACHE2): O(1) direct pgno->page array backed by
// MADV_HUGEPAGE slab for the main 4096-byte purgeable cache, delegating all
// secondary or non-4096 caches cleanly to SQLite's built-in pcache1.
// Uses pure PCACHE2 contract only (tracks pinned state via xFetch/xUnpin).
// -----------------------------------------------------------------------------
enum {
    FC_FREE = 0,
    FC_USED = 1
};

struct fast_slot {
    sqlite3_pcache_page pg;   // MUST be first (cast from sqlite3_pcache_page*)
    uint32_t key;
    int32_t next_free;
    uint8_t state;
    uint8_t ref;
    uint8_t pinned;
    uint8_t pad;
};

struct fast_cache {
    int is_fast;
    sqlite3_pcache *orig;
    int sz_page;
    int sz_extra;
    uint32_t n_max;
    uint32_t n_alloc;
    uint32_t max_key;
    uint32_t direct_cap;
    struct fast_slot **direct;
    struct fast_slot *slots;
    uint32_t slab_cap;
    int32_t free_head;
    uint32_t clock_hand;
};

static sqlite3_pcache_methods2 g_orig_pcache;
static int g_pcache_registered = 0;
static atomic_int g_fc_taken = 0;

static struct fast_slot *g_fc_slots = NULL;
static uint8_t *g_fc_bufs = NULL;
static uint8_t *g_fc_extras = NULL;
static size_t g_fc_buf_bytes = 0;
static size_t g_fc_extra_bytes = 0;
static size_t g_fc_slots_bytes = 0;
static uint32_t g_fc_slab_pages = 0;
static int g_fc_extra_stride = 0;
static int g_fc_prefault = 0;

static void fc_configure(uint32_t cache_pages, int do_prefault) {
    // Keep +512 headroom slots above cache_size for concurrent pinned/dirty pages.
    g_fc_slab_pages = cache_pages + 512u;
    g_fc_prefault = do_prefault;
}

static void fc_ensure_slab(int sz_extra) {
    int stride = (sz_extra + 63) & ~63;
    if (stride < 320) stride = 320;
    uint32_t want_n = g_fc_slab_pages ? g_fc_slab_pages : (16384u + 512u);
    if (g_fc_slots && stride <= g_fc_extra_stride && want_n <= g_fc_slab_pages) return;

    if (g_fc_slots) {
        munmap(g_fc_slots, g_fc_slots_bytes);
        munmap(g_fc_bufs, g_fc_buf_bytes);
        munmap(g_fc_extras, g_fc_extra_bytes);
        g_fc_slots = NULL;
    }

    uint32_t n = want_n;
    g_fc_slab_pages = n;
    g_fc_extra_stride = stride;

    g_fc_slots_bytes = ((size_t)n * sizeof(struct fast_slot) + 4095u) & ~(size_t)4095u;
    g_fc_buf_bytes   = (size_t)n * 4096u;
    g_fc_extra_bytes = ((size_t)n * (size_t)stride + 4095u) & ~(size_t)4095u;

    g_fc_slots = (struct fast_slot *)mmap(NULL, g_fc_slots_bytes,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_fc_bufs  = (uint8_t *)mmap(NULL, g_fc_buf_bytes,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_fc_extras = (uint8_t *)mmap(NULL, g_fc_extra_bytes,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_fc_slots == MAP_FAILED || g_fc_bufs == MAP_FAILED || g_fc_extras == MAP_FAILED) {
        if (g_fc_slots != MAP_FAILED) munmap(g_fc_slots, g_fc_slots_bytes);
        if (g_fc_bufs != MAP_FAILED) munmap(g_fc_bufs, g_fc_buf_bytes);
        if (g_fc_extras != MAP_FAILED) munmap(g_fc_extras, g_fc_extra_bytes);
        g_fc_slots = NULL;
        g_fc_bufs = NULL;
        g_fc_extras = NULL;
        return;
    }
    madvise(g_fc_bufs, g_fc_buf_bytes, MADV_HUGEPAGE);
    madvise(g_fc_extras, g_fc_extra_bytes, MADV_HUGEPAGE);
    if (g_fc_prefault) {
        prefault(g_fc_bufs, g_fc_buf_bytes);
        prefault(g_fc_extras, g_fc_extra_bytes);
    }
}

static int fc_xInit(void *arg) {
    if (g_orig_pcache.xInit) return g_orig_pcache.xInit(g_orig_pcache.pArg);
    (void)arg;
    return SQLITE_OK;
}

static void fc_xShutdown(void *arg) {
    if (g_orig_pcache.xShutdown) g_orig_pcache.xShutdown(g_orig_pcache.pArg);
    (void)arg;
}

static sqlite3_pcache *fc_xCreate(int szPage, int szExtra, int bPurgeable) {
    struct fast_cache *fc = (struct fast_cache *)calloc(1, sizeof(*fc));
    if (!fc) return NULL;

    int expected = 0;
    if (bPurgeable && szPage == 4096 &&
        atomic_compare_exchange_strong(&g_fc_taken, &expected, 1)) {
        fc_ensure_slab(szExtra);
        if (g_fc_slots) {
            uint32_t dcap = 131072u; // Covers up to 512 MB DB before grow
            if (g_fc_slab_pages + 4096u > dcap) dcap = 262144u;
            struct fast_slot **dir = (struct fast_slot **)mmap(
                NULL, (size_t)dcap * sizeof(*dir), PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (dir != MAP_FAILED) {
                fc->is_fast = 1;
                fc->sz_page = szPage;
                fc->sz_extra = szExtra;
                fc->n_max = g_fc_slab_pages > 512u ? g_fc_slab_pages - 512u : g_fc_slab_pages;
                fc->direct_cap = dcap;
                fc->direct = dir;
                fc->slots = g_fc_slots;
                fc->slab_cap = g_fc_slab_pages;
                for (uint32_t i = 0; i < g_fc_slab_pages; i++) {
                    struct fast_slot *s = &fc->slots[i];
                    s->pg.pBuf = g_fc_bufs + (size_t)i * 4096u;
                    s->pg.pExtra = g_fc_extras + (size_t)i * (size_t)g_fc_extra_stride;
                    s->key = 0;
                    s->state = FC_FREE;
                    s->ref = 0;
                    s->pinned = 0;
                    s->next_free = (i + 1u < g_fc_slab_pages) ? (int32_t)(i + 1u) : -1;
                }
                fc->free_head = 0;
                fc->clock_hand = 0;
                return (sqlite3_pcache *)fc;
            }
        }
        atomic_store(&g_fc_taken, 0);
    }

    fc->is_fast = 0;
    fc->orig = g_orig_pcache.xCreate(szPage, szExtra, bPurgeable);
    if (!fc->orig) {
        free(fc);
        return NULL;
    }
    return (sqlite3_pcache *)fc;
}

static void fc_xCachesize(sqlite3_pcache *p, int nCachesize) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (!fc->is_fast) {
        g_orig_pcache.xCachesize(fc->orig, nCachesize);
        return;
    }
    uint32_t lim = nCachesize > 16 ? (uint32_t)nCachesize : 16u;
    if (fc->slab_cap > 256u && lim > fc->slab_cap - 256u) lim = fc->slab_cap - 256u;
    fc->n_max = lim;
}

static int fc_xPagecount(sqlite3_pcache *p) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (!fc->is_fast) return g_orig_pcache.xPagecount(fc->orig);
    return (int)fc->n_alloc;
}

static int fc_grow_direct(struct fast_cache *fc, uint32_t key) {
    uint32_t new_cap = fc->direct_cap;
    while (new_cap <= key) {
        if (new_cap >= (1u << 28)) return -1;
        new_cap <<= 1;
    }
    size_t old_sz = (size_t)fc->direct_cap * sizeof(*fc->direct);
    size_t new_sz = (size_t)new_cap * sizeof(*fc->direct);
    void *nd = mremap(fc->direct, old_sz, new_sz, MREMAP_MAYMOVE);
    if (nd == MAP_FAILED) return -1;
    fc->direct = (struct fast_slot **)nd;
    fc->direct_cap = new_cap;
    return 0;
}

static struct fast_slot *fc_clock_evict(struct fast_cache *fc) {
    uint32_t n = fc->slab_cap;
    uint32_t h = fc->clock_hand;
    for (uint32_t tries = 0; tries < 2u * n; tries++) {
        struct fast_slot *s = &fc->slots[h];
        if (++h == n) h = 0;
        if (s->state == FC_USED && !s->pinned) {
            if (s->ref) {
                s->ref = 0;
            } else {
                fc->clock_hand = h;
                if (likely(s->key < fc->direct_cap && fc->direct[s->key] == s))
                    fc->direct[s->key] = NULL;
                return s;
            }
        }
    }
    fc->clock_hand = h;
    return NULL;
}

static __attribute__((noinline)) sqlite3_pcache_page *fc_alloc_page(
    struct fast_cache *fc, unsigned key, int createFlag) {
    if (unlikely(key >= fc->direct_cap)) {
        if (fc_grow_direct(fc, key) != 0) return NULL;
    }
    struct fast_slot *s = NULL;
    if (likely(fc->free_head >= 0 && fc->n_alloc < fc->n_max)) {
        int32_t idx = fc->free_head;
        s = &fc->slots[idx];
        fc->free_head = s->next_free;
        fc->n_alloc++;
    } else {
        s = fc_clock_evict(fc);
        if (!s) {
            if (createFlag == 1) return NULL;
            if (fc->free_head >= 0) {
                int32_t idx = fc->free_head;
                s = &fc->slots[idx];
                fc->free_head = s->next_free;
                fc->n_alloc++;
            } else {
                return NULL;
            }
        }
    }
    // Clear pExtra so SQLite sees an uninitialized PgHdr on newly allocated/evicted slots.
    memset(s->pg.pExtra, 0, (size_t)fc->sz_extra);
    s->key = key;
    s->state = FC_USED;
    s->ref = 1;
    s->pinned = 1;
    fc->direct[key] = s;
    if (key > fc->max_key) fc->max_key = key;
    return &s->pg;
}

static sqlite3_pcache_page *fc_xFetch(sqlite3_pcache *p, unsigned key, int createFlag) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (unlikely(!fc->is_fast)) return g_orig_pcache.xFetch(fc->orig, key, createFlag);
    if (likely(key < fc->direct_cap)) {
        struct fast_slot *s = fc->direct[key];
        if (likely(s != NULL)) {
            s->pinned = 1;
            if (unlikely(!s->ref)) s->ref = 1;
            return &s->pg;
        }
    }
    if (!createFlag) return NULL;
    return fc_alloc_page(fc, key, createFlag);
}

static inline void fc_drop_slot(struct fast_cache *fc, struct fast_slot *s) {
    if (likely(s->key < fc->direct_cap && fc->direct[s->key] == s))
        fc->direct[s->key] = NULL;
    s->key = 0;
    s->state = FC_FREE;
    s->ref = 0;
    s->pinned = 0;
    s->next_free = fc->free_head;
    fc->free_head = (int32_t)(s - fc->slots);
    fc->n_alloc--;
}

static void fc_xUnpin(sqlite3_pcache *p, sqlite3_pcache_page *pPg, int discard) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (unlikely(!fc->is_fast)) {
        g_orig_pcache.xUnpin(fc->orig, pPg, discard);
        return;
    }
    struct fast_slot *s = (struct fast_slot *)pPg;
    if (likely(!discard)) {
        s->pinned = 0;
        return;
    }
    fc_drop_slot(fc, s);
}

static void fc_xRekey(sqlite3_pcache *p, sqlite3_pcache_page *pPg,
                      unsigned oldKey, unsigned newKey) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (unlikely(!fc->is_fast)) {
        g_orig_pcache.xRekey(fc->orig, pPg, oldKey, newKey);
        return;
    }
    struct fast_slot *s = (struct fast_slot *)pPg;
    if (oldKey < fc->direct_cap && fc->direct[oldKey] == s)
        fc->direct[oldKey] = NULL;
    if (unlikely(newKey >= fc->direct_cap)) {
        if (fc_grow_direct(fc, newKey) != 0) abort();
    }
    struct fast_slot *prev = fc->direct[newKey];
    if (unlikely(prev && prev != s)) fc_drop_slot(fc, prev);
    s->key = newKey;
    fc->direct[newKey] = s;
    if (newKey > fc->max_key) fc->max_key = newKey;
}

static void fc_xTruncate(sqlite3_pcache *p, unsigned iLimit) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (unlikely(!fc->is_fast)) {
        g_orig_pcache.xTruncate(fc->orig, iLimit);
        return;
    }
    if (iLimit <= fc->max_key && fc->n_alloc > 0) {
        uint32_t end = fc->max_key < fc->direct_cap ? fc->max_key : fc->direct_cap - 1u;
        for (uint32_t k = iLimit; k <= end; k++) {
            struct fast_slot *s = fc->direct[k];
            if (s) fc_drop_slot(fc, s);
        }
        fc->max_key = iLimit > 0 ? iLimit - 1u : 0;
    }
}

static void fc_xDestroy(sqlite3_pcache *p) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (!fc->is_fast) {
        g_orig_pcache.xDestroy(fc->orig);
        free(fc);
        return;
    }
    munmap(fc->direct, (size_t)fc->direct_cap * sizeof(*fc->direct));
    free(fc);
    atomic_store(&g_fc_taken, 0);
}

static void fc_xShrink(sqlite3_pcache *p) {
    struct fast_cache *fc = (struct fast_cache *)p;
    if (!fc->is_fast && g_orig_pcache.xShrink) g_orig_pcache.xShrink(fc->orig);
}

static int fc_register(void) {
    if (g_pcache_registered) return SQLITE_OK;
    int rc = sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &g_orig_pcache);
    if (rc != SQLITE_OK) return rc;
    sqlite3_pcache_methods2 m = {
        .iVersion = 2,
        .pArg = NULL,
        .xInit = fc_xInit,
        .xShutdown = fc_xShutdown,
        .xCreate = fc_xCreate,
        .xCachesize = fc_xCachesize,
        .xPagecount = fc_xPagecount,
        .xFetch = fc_xFetch,
        .xUnpin = fc_xUnpin,
        .xRekey = fc_xRekey,
        .xTruncate = fc_xTruncate,
        .xDestroy = fc_xDestroy,
        .xShrink = fc_xShrink,
    };
    rc = sqlite3_config(SQLITE_CONFIG_PCACHE2, &m);
    if (rc == SQLITE_OK) g_pcache_registered = 1;
    return rc;
}

// -----------------------------------------------------------------------------
// "feed-wal" VFS (wrapping "unix-excl"): coalesces 24B+4096B WAL frame writes
// into a 256 KB buffer, flushed automatically on the commit frame's page
// (header bytes 4..7 != 0) as well as xSync, xRead, xTruncate, xFileSize,
// xLock, xUnlock, xFileControl, xFetch, or xClose.
// -----------------------------------------------------------------------------
#define FEED_WAL_WBUF_CAP (256u * 1024u)

struct feed_file {
    sqlite3_file base;
    sqlite3_file *real;
    sqlite3_io_methods *iom;
    int is_wal;
    int commit;
    int werr;
    uint8_t *wbuf;
    sqlite3_int64 woff;
    sqlite3_int64 last_end;
    size_t wlen;
    size_t wcap;
};

static sqlite3_vfs *g_unix_vfs = NULL;
static sqlite3_vfs g_feed_vfs;
static int g_vfs_registered = 0;

static int ff_flush_wal(struct feed_file *f) {
    if (!f || f->wlen == 0) return f ? f->werr : SQLITE_OK;
    size_t sent = 0;
    int rc = SQLITE_OK;
    // SQLite's unix seekAndWriteFd masks write length to 0x1ffff (<128 KB), so flush in <=64 KB chunks.
    while (sent < f->wlen) {
        size_t chunk = f->wlen - sent;
        if (chunk > 65536u) chunk = 65536u;
        rc = f->real->pMethods->xWrite(f->real, f->wbuf + sent, (int)chunk, f->woff + (sqlite3_int64)sent);
        if (rc != SQLITE_OK) {
            f->werr = rc;
            break;
        }
        sent += chunk;
    }
    f->wlen = 0;
    return rc;
}

static int ff_close(sqlite3_file *sf) {
    struct feed_file *f = (struct feed_file *)sf;
    int rc = SQLITE_OK;
    if (f->is_wal) {
        rc = ff_flush_wal(f);
        free(f->wbuf);
        f->wbuf = NULL;
    }
    int rc2 = f->real->pMethods->xClose(f->real);
    free(f->iom);
    f->iom = NULL;
    return rc != SQLITE_OK ? rc : rc2;
}

static int ff_read(sqlite3_file *sf, void *buf, int n, sqlite3_int64 off) {
    struct feed_file *f = (struct feed_file *)sf;
    if (unlikely(f->is_wal && f->wlen > 0)) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xRead(f->real, buf, n, off);
}

static int ff_write(sqlite3_file *sf, const void *buf, int n, sqlite3_int64 off) {
    struct feed_file *f = (struct feed_file *)sf;
    if (likely(f->is_wal)) {
        if (unlikely(f->werr != SQLITE_OK)) return f->werr;
        if (off != f->last_end) {
            if (f->wlen > 0) {
                int rc = ff_flush_wal(f);
                if (rc != SQLITE_OK) return rc;
            }
            f->commit = 0;
        } else if (f->wlen + (size_t)n > f->wcap) {
            int rc = ff_flush_wal(f);
            if (rc != SQLITE_OK) return rc;
        }
        f->last_end = off + n;
        if (likely(f->wbuf != NULL && (size_t)n <= f->wcap)) {
            if (f->wlen == 0) f->woff = off;
            memcpy(f->wbuf + f->wlen, buf, (size_t)n);
            f->wlen += (size_t)n;
            if (n == 24) {
                const uint8_t *h = (const uint8_t *)buf;
                f->commit = (h[4] | h[5] | h[6] | h[7]) != 0;
                return SQLITE_OK;
            }
            if (f->commit) {
                f->commit = 0;
                return ff_flush_wal(f);
            }
            return SQLITE_OK;
        }
        if (f->wlen > 0) {
            int rc = ff_flush_wal(f);
            if (rc != SQLITE_OK) return rc;
        }
        f->commit = 0;
    }
    return f->real->pMethods->xWrite(f->real, buf, n, off);
}

static int ff_truncate(sqlite3_file *sf, sqlite3_int64 size) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    f->commit = 0;
    return f->real->pMethods->xTruncate(f->real, size);
}

static int ff_sync(sqlite3_file *sf, int flags) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xSync(f->real, flags);
}

static int ff_file_size(sqlite3_file *sf, sqlite3_int64 *pSize) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xFileSize(f->real, pSize);
}

static int ff_lock(sqlite3_file *sf, int eLock) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xLock(f->real, eLock);
}

static int ff_unlock(sqlite3_file *sf, int eLock) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xUnlock(f->real, eLock);
}

static int ff_check_reserved_lock(sqlite3_file *sf, int *pResOut) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xCheckReservedLock(f->real, pResOut);
}

static int ff_file_control(sqlite3_file *sf, int op, void *pArg) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xFileControl(f->real, op, pArg);
}

static int ff_sector_size(sqlite3_file *sf) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xSectorSize(f->real);
}

static int ff_dev_chars(sqlite3_file *sf) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xDeviceCharacteristics(f->real);
}

static int ff_shm_map(sqlite3_file *sf, int iPg, int pgsz, int bExtend, void volatile **pp) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xShmMap(f->real, iPg, pgsz, bExtend, pp);
}

static int ff_shm_lock(sqlite3_file *sf, int offset, int n, int flags) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xShmLock(f->real, offset, n, flags);
}

static void ff_shm_barrier(sqlite3_file *sf) {
    struct feed_file *f = (struct feed_file *)sf;
    f->real->pMethods->xShmBarrier(f->real);
}

static int ff_shm_unmap(sqlite3_file *sf, int deleteFlag) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xShmUnmap(f->real, deleteFlag);
}

static int ff_fetch(sqlite3_file *sf, sqlite3_int64 iOfst, int iAmt, void **pp) {
    struct feed_file *f = (struct feed_file *)sf;
    if (f->is_wal && f->wlen > 0) {
        int rc = ff_flush_wal(f);
        if (rc != SQLITE_OK) return rc;
    }
    return f->real->pMethods->xFetch(f->real, iOfst, iAmt, pp);
}

static int ff_unfetch(sqlite3_file *sf, sqlite3_int64 iOfst, void *p) {
    struct feed_file *f = (struct feed_file *)sf;
    return f->real->pMethods->xUnfetch(f->real, iOfst, p);
}

static int ff_open(sqlite3_vfs *vfs, sqlite3_filename zName, sqlite3_file *sf,
                   int flags, int *pOutFlags) {
    (void)vfs;
    struct feed_file *f = (struct feed_file *)sf;
    memset(f, 0, sizeof(*f));
    f->last_end = -1;
    f->real = (sqlite3_file *)(((uintptr_t)(f + 1) + 7u) & ~(uintptr_t)7u);
    int rc = g_unix_vfs->xOpen(g_unix_vfs, zName, f->real, flags, pOutFlags);
    if (rc != SQLITE_OK) return rc;

    const struct sqlite3_io_methods *rm = f->real->pMethods;
    if (!rm) return SQLITE_OK;

    sqlite3_io_methods *iom = (sqlite3_io_methods *)calloc(1, sizeof(*iom));
    if (!iom) {
        rm->xClose(f->real);
        return SQLITE_NOMEM;
    }
    iom->iVersion = rm->iVersion;
    iom->xClose = ff_close;
    iom->xRead = ff_read;
    iom->xWrite = ff_write;
    iom->xTruncate = ff_truncate;
    iom->xSync = ff_sync;
    iom->xFileSize = ff_file_size;
    iom->xLock = ff_lock;
    iom->xUnlock = ff_unlock;
    iom->xCheckReservedLock = ff_check_reserved_lock;
    iom->xFileControl = ff_file_control;
    iom->xSectorSize = ff_sector_size;
    iom->xDeviceCharacteristics = ff_dev_chars;
    if (rm->iVersion >= 2) {
        iom->xShmMap = ff_shm_map;
        iom->xShmLock = ff_shm_lock;
        iom->xShmBarrier = ff_shm_barrier;
        iom->xShmUnmap = ff_shm_unmap;
    }
    if (rm->iVersion >= 3) {
        iom->xFetch = rm->xFetch ? ff_fetch : NULL;
        iom->xUnfetch = rm->xUnfetch ? ff_unfetch : NULL;
    }

    int is_wal = (flags & SQLITE_OPEN_WAL) != 0;
    if (!is_wal && zName) {
        size_t l = strlen(zName);
        if (l >= 4 && strcmp(zName + l - 4, "-wal") == 0) is_wal = 1;
    }
    if (is_wal) {
        if (posix_memalign((void **)&f->wbuf, 4096, FEED_WAL_WBUF_CAP) == 0) {
            f->wcap = FEED_WAL_WBUF_CAP;
        }
        f->is_wal = 1;
    }

    f->iom = iom;
    f->base.pMethods = iom;
    return SQLITE_OK;
}

static int ff_delete(sqlite3_vfs *vfs, const char *zName, int syncDir) {
    (void)vfs;
    return g_unix_vfs->xDelete(g_unix_vfs, zName, syncDir);
}
static int ff_access(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut) {
    (void)vfs;
    return g_unix_vfs->xAccess(g_unix_vfs, zName, flags, pResOut);
}
static int ff_full_pathname(sqlite3_vfs *vfs, const char *zName, int nOut, char *zOut) {
    (void)vfs;
    return g_unix_vfs->xFullPathname(g_unix_vfs, zName, nOut, zOut);
}
static int ff_randomness(sqlite3_vfs *vfs, int nByte, char *zOut) {
    (void)vfs;
    return g_unix_vfs->xRandomness(g_unix_vfs, nByte, zOut);
}
static int ff_sleep(sqlite3_vfs *vfs, int microseconds) {
    (void)vfs;
    return g_unix_vfs->xSleep(g_unix_vfs, microseconds);
}
static int ff_current_time(sqlite3_vfs *vfs, double *prNow) {
    (void)vfs;
    return g_unix_vfs->xCurrentTime(g_unix_vfs, prNow);
}
static int ff_get_last_error(sqlite3_vfs *vfs, int nBuf, char *zBuf) {
    (void)vfs;
    return g_unix_vfs->xGetLastError(g_unix_vfs, nBuf, zBuf);
}
static int ff_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *piNow) {
    (void)vfs;
    return g_unix_vfs->xCurrentTimeInt64(g_unix_vfs, piNow);
}

static int ff_vfs_register(void) {
    if (g_vfs_registered) return SQLITE_OK;
    g_unix_vfs = sqlite3_vfs_find("unix-excl");
    if (!g_unix_vfs) g_unix_vfs = sqlite3_vfs_find("unix");
    if (!g_unix_vfs) return SQLITE_ERROR;
    memset(&g_feed_vfs, 0, sizeof(g_feed_vfs));
    g_feed_vfs.iVersion = g_unix_vfs->iVersion > 2 ? 2 : g_unix_vfs->iVersion;
    g_feed_vfs.szOsFile = ((int)sizeof(struct feed_file) + 7 + g_unix_vfs->szOsFile) & ~7;
    g_feed_vfs.mxPathname = g_unix_vfs->mxPathname;
    g_feed_vfs.zName = "feed-wal";
    g_feed_vfs.xOpen = ff_open;
    g_feed_vfs.xDelete = ff_delete;
    g_feed_vfs.xAccess = ff_access;
    g_feed_vfs.xFullPathname = ff_full_pathname;
    g_feed_vfs.xRandomness = ff_randomness;
    g_feed_vfs.xSleep = ff_sleep;
    g_feed_vfs.xCurrentTime = ff_current_time;
    g_feed_vfs.xGetLastError = ff_get_last_error;
    g_feed_vfs.xCurrentTimeInt64 = g_unix_vfs->xCurrentTimeInt64 ? ff_current_time_int64 : NULL;
    int rc = sqlite3_vfs_register(&g_feed_vfs, 0);
    if (rc == SQLITE_OK) g_vfs_registered = 1;
    return rc;
}

// -----------------------------------------------------------------------------
// Database handle, prepared statements, carried read tx, nice-19 checkpointer
// -----------------------------------------------------------------------------
enum {
    CK_IDLE = 0,
    CK_RUN  = 1,
    CK_DONE = 2
};

struct db {
    sqlite3 *c;
    sqlite3 *ck_c;
    struct bt bt;

    sqlite3_stmt *st_health;
    sqlite3_stmt *st_ins_post;
    sqlite3_stmt *st_ins_like;
    sqlite3_stmt *st_post_exists;
    sqlite3_stmt *st_feed_ref;
    sqlite3_stmt *st_post_ref;
    sqlite3_stmt *st_begin_read;
    sqlite3_stmt *st_commit;
    sqlite3_stmt *st_rollback;

    int in_tx;          // 0=none, 1=active
    int wrote;
    int broken;
    int carry_read_tx;
    int writes_held;
    int is_large_db;

    uint32_t wal_frames;
    atomic_uint wal_frames_now;
    uint32_t ck_pass_frames;
    uint32_t ck_hold_at;
    uint32_t ck_last_req;
    uint32_t hold_start_ms;
    uint32_t hold_max_ms;

    char path[4096];
    char wal_path[4104];
    int db_fd;
    int wal_fd;
    int ck_efd;

    // Timestamp cache (YYYY-MM-DDTHH:MM:SS.mmmZ, 24 bytes)
    int64_t iso_sec;
    char iso_prefix[20];

    // Single nice-19 background checkpointer
    pthread_mutex_t ck_mu;
    pthread_cond_t ck_cond;
    pthread_t ck_thr;
    int ck_thread_up;
    int ck_stop;
    int ck_sync_req;
    atomic_int ck_state;
    int ck_rc;
    int ck_log;
    int ck_ckpt;
};

static inline void db_notify_efd(int efd) {
    if (efd >= 0) {
        uint64_t v = 1;
        ssize_t r = write(efd, &v, sizeof(v));
        (void)r;
    }
}

static int db_wal_hook(void *arg, sqlite3 *c, const char *name, int frames) {
    (void)c;
    (void)name;
    struct db *d = (struct db *)arg;
    uint32_t f = frames > 0 ? (uint32_t)frames : 0u;
    if (f < d->ck_last_req) d->ck_last_req = 0;
    d->wal_frames = f;
    atomic_store_explicit(&d->wal_frames_now, f, memory_order_release);
    return SQLITE_OK;
}

static inline int db_now_iso(struct db *d, char out[25]) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t sec = (int64_t)ts.tv_sec;
    if (unlikely(sec != d->iso_sec)) {
        struct tm tm;
        time_t t = (time_t)sec;
        gmtime_r(&t, &tm);
        int yr = tm.tm_year + 1900;
        d->iso_prefix[0] = (char)('0' + (yr / 1000) % 10);
        d->iso_prefix[1] = (char)('0' + (yr / 100) % 10);
        d->iso_prefix[2] = (char)('0' + (yr / 10) % 10);
        d->iso_prefix[3] = (char)('0' + yr % 10);
        d->iso_prefix[4] = '-';
        memcpy(d->iso_prefix + 5, DIGITS2 + (tm.tm_mon + 1) * 2, 2);
        d->iso_prefix[7] = '-';
        memcpy(d->iso_prefix + 8, DIGITS2 + tm.tm_mday * 2, 2);
        d->iso_prefix[10] = 'T';
        memcpy(d->iso_prefix + 11, DIGITS2 + tm.tm_hour * 2, 2);
        d->iso_prefix[13] = ':';
        memcpy(d->iso_prefix + 14, DIGITS2 + tm.tm_min * 2, 2);
        d->iso_prefix[16] = ':';
        memcpy(d->iso_prefix + 17, DIGITS2 + tm.tm_sec * 2, 2);
        d->iso_prefix[19] = '.';
        d->iso_sec = sec;
    }
    memcpy(out, d->iso_prefix, 20);
    int ms = (int)(ts.tv_nsec / 1000000L);
    out[20] = (char)('0' + (ms / 100) % 10);
    out[21] = (char)('0' + (ms / 10) % 10);
    out[22] = (char)('0' + ms % 10);
    out[23] = 'Z';
    out[24] = '\0';
    return 24;
}

static int db_exec(sqlite3 *c, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(c, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc;
}

static inline int db_step_stmt(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return rc;
}

static void db_open_wal_fd(struct db *d) {
    if (d->wal_fd < 0) {
        d->wal_fd = open(d->wal_path, O_RDONLY | O_CLOEXEC);
    }
}

static void *ck_main(void *arg) {
    struct db *d = (struct db *)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    db_exec(d->ck_c, "SELECT 1 FROM sqlite_schema LIMIT 1;");
    for (;;) {
        pthread_mutex_lock(&d->ck_mu);
        while (!d->ck_stop && atomic_load_explicit(&d->ck_state, memory_order_acquire) != CK_RUN) {
            pthread_cond_wait(&d->ck_cond, &d->ck_mu);
        }
        int stop = d->ck_stop;
        int do_sync = d->ck_sync_req;
        pthread_mutex_unlock(&d->ck_mu);
        if (stop) break;

        int log = -1, ckpt = -1;
        int rc = sqlite3_wal_checkpoint_v2(d->ck_c, "main", SQLITE_CHECKPOINT_PASSIVE, &log, &ckpt);

        db_open_wal_fd(d);
        if (rc == SQLITE_OK && ckpt > 0 && d->wal_fd >= 0) {
            (void)posix_fadvise(d->wal_fd, 0, (off_t)ckpt * 4120 + 32, POSIX_FADV_DONTNEED);
        }
        if (d->is_large_db && d->db_fd >= 0 && rc == SQLITE_OK && ckpt > 0 && ckpt == log &&
            (uint32_t)ckpt >= atomic_load_explicit(&d->wal_frames_now, memory_order_acquire)) {
            (void)posix_fadvise(d->db_fd, 0, 0, POSIX_FADV_DONTNEED);
        }
        if (do_sync && d->db_fd >= 0 && rc == SQLITE_OK) {
            (void)fdatasync(d->db_fd);
        }

        d->ck_rc = rc;
        d->ck_log = log;
        d->ck_ckpt = ckpt;
        atomic_store_explicit(&d->ck_state, CK_DONE, memory_order_release);
        db_notify_efd(d->ck_efd);
    }
    return NULL;
}

static int db_warm(struct db *d) {
    if (d->db_fd >= 0 && d->is_large_db) {
        struct stat st;
        if (fstat(d->db_fd, &st) == 0 && st.st_size > 0) {
            (void)posix_fadvise(d->db_fd, 0, st.st_size, POSIX_FADV_WILLNEED);
        }
    }
    // Surgical 38 MB working-set warmup: autoindex_likes (29.5M) + posts_user_id_idx (5.3M)
    // + users (2.5M) + hottest post/like tails (~1M), leaving ~26M headroom in 64M FastCache.
    if (db_exec(d->c, "SELECT sum(user_id) FROM likes INDEXED BY sqlite_autoindex_likes_1;") != SQLITE_OK) return -1;
    if (db_exec(d->c, "SELECT sum(user_id) FROM posts INDEXED BY posts_user_id_idx;") != SQLITE_OK) return -1;
    if (db_exec(d->c, "SELECT sum(length(username)) FROM users;") != SQLITE_OK) return -1;
    if (db_exec(d->c, "SELECT count(length(body)) FROM (SELECT body FROM posts ORDER BY id DESC LIMIT 2000);") != SQLITE_OK) return -1;
    if (db_exec(d->c, "SELECT count(*) FROM (SELECT post_id FROM likes ORDER BY post_id DESC LIMIT 8000);") != SQLITE_OK) return -1;
    if (d->db_fd >= 0 && d->is_large_db) {
        (void)posix_fadvise(d->db_fd, 0, 0, POSIX_FADV_DONTNEED);
    }
    return 0;
}

static uint64_t db_effective_ram_bytes(void) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long pgsz = sysconf(_SC_PAGE_SIZE);
    uint64_t phys = (pages > 0 && pgsz > 0) ? (uint64_t)pages * (uint64_t)pgsz : 0;
    int fd = open("/sys/fs/cgroup/memory.max", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0 && buf[0] >= '0' && buf[0] <= '9') {
            buf[n] = '\0';
            uint64_t cg = strtoull(buf, NULL, 10);
            if (cg > 0 && (phys == 0 || cg < phys)) phys = cg;
        }
    }
    return phys;
}

static int db_open(struct db *d, const char *path, int cache_kib) {
    memset(d, 0, sizeof(*d));
    d->db_fd = -1;
    d->wal_fd = -1;
    d->ck_efd = -1;
    d->iso_sec = -1;

    snprintf(d->path, sizeof(d->path), "%s", path);
    snprintf(d->wal_path, sizeof(d->wal_path), "%s-wal", path);

    struct stat st;
    off_t db_bytes = (stat(path, &st) == 0) ? st.st_size : 0;
    d->is_large_db = (db_bytes >= (16 << 20));

    if (cache_kib <= 0) {
        const char *env_c = getenv("CACHE_KIB");
        if (env_c && *env_c) {
            cache_kib = atoi(env_c);
        } else {
            uint64_t ram_bytes = db_effective_ram_bytes();
            cache_kib = (ram_bytes > 0 && ram_bytes <= (2500ULL << 20)) ? 65536 : 131072;
        }
        if (cache_kib < 4096) cache_kib = 4096;
    }

    uint32_t cache_pages = (uint32_t)(cache_kib / 4);
    fc_configure(cache_pages, 0);
    if (fc_register() != SQLITE_OK) return -1;
    if (sqlite3_initialize() != SQLITE_OK) return -1;
    if (ff_vfs_register() != SQLITE_OK) return -1;

    d->carry_read_tx = 1;
    const char *env_carry = getenv("CARRY_READ_TX");
    if (env_carry) d->carry_read_tx = atoi(env_carry) != 0;

    const char *env_ckpass = getenv("CKPT_PASS");
    d->ck_pass_frames = (env_ckpass && *env_ckpass) ? (uint32_t)atoi(env_ckpass) : 16384u;
    if (d->ck_pass_frames < 100u) d->ck_pass_frames = 100u;

    uint32_t max_hold = 1048576u; // ~4 GB max WAL
    struct statvfs sv;
    if (statvfs(path, &sv) == 0 && sv.f_bavail > 0 && sv.f_frsize > 0) {
        uint64_t free_bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
        uint64_t half_frames = (free_bytes / 2ULL) / 4120ULL;
        if (half_frames < max_hold) max_hold = (uint32_t)half_frames;
    }
    const char *env_hold = getenv("CKPT_HOLD");
    d->ck_hold_at = (env_hold && *env_hold) ? (uint32_t)atoi(env_hold) : max_hold;
    if (d->ck_hold_at < d->ck_pass_frames * 2u) d->ck_hold_at = d->ck_pass_frames * 2u;

    const char *env_hmax = getenv("HOLD_MAX_MS");
    d->hold_max_ms = (env_hmax && *env_hmax) ? (uint32_t)atoi(env_hmax) : 1500u;

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(path, &d->c, flags, "feed-wal") != SQLITE_OK) return -1;

    if (d->is_large_db) {
        int chunk = 64 << 20;
        sqlite3_file_control(d->c, "main", SQLITE_FCNTL_CHUNK_SIZE, &chunk);
    }

    char pragmas[280];
    snprintf(pragmas, sizeof(pragmas),
             "PRAGMA journal_mode=WAL;"
             "PRAGMA synchronous=NORMAL;"
             "PRAGMA temp_store=MEMORY;"
             "PRAGMA mmap_size=0;"
             "PRAGMA journal_size_limit=0;"
             "PRAGMA wal_autocheckpoint=0;"
             "PRAGMA cache_size=-%d;",
             cache_kib);
    if (db_exec(d->c, pragmas) != SQLITE_OK) return -1;

    // Empty any leftover WAL before opening FastCache working set or warming.
    sqlite3_wal_checkpoint_v2(d->c, "main", SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    sqlite3_wal_hook(d->c, db_wal_hook, d);
    atomic_store_explicit(&d->wal_frames_now, 0, memory_order_release);

    d->db_fd = open(path, O_RDONLY | O_CLOEXEC);

    if (sqlite3_prepare_v3(d->c, "SELECT 1", -1, SQLITE_PREPARE_PERSISTENT,
                           &d->st_health, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c,
                           "INSERT INTO posts (user_id, body, created_at) VALUES (?1, ?2, ?3)",
                           -1, SQLITE_PREPARE_PERSISTENT, &d->st_ins_post, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c,
                           "INSERT INTO likes (user_id, post_id, created_at) VALUES (?1, ?2, ?3) ON CONFLICT DO NOTHING",
                           -1, SQLITE_PREPARE_PERSISTENT, &d->st_ins_like, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c, "SELECT 1 FROM posts WHERE id = ?1", -1,
                           SQLITE_PREPARE_PERSISTENT, &d->st_post_exists, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c,
                           "SELECT p.id, p.body, p.created_at, u.username, "
                           "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                           "FROM posts p JOIN users u ON u.id = p.user_id "
                           "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
                           -1, SQLITE_PREPARE_PERSISTENT, &d->st_feed_ref, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c,
                           "SELECT p.id, p.body, p.created_at, u.username, "
                           "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
                           "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1",
                           -1, SQLITE_PREPARE_PERSISTENT, &d->st_post_ref, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c, "BEGIN", -1, SQLITE_PREPARE_PERSISTENT,
                           &d->st_begin_read, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c, "COMMIT", -1, SQLITE_PREPARE_PERSISTENT,
                           &d->st_commit, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v3(d->c, "ROLLBACK", -1, SQLITE_PREPARE_PERSISTENT,
                           &d->st_rollback, NULL) != SQLITE_OK) return -1;

    bt_init(&d->bt, d->c);

    if (d->is_large_db && getenv("NO_WARM") == NULL) {
        if (db_warm(d) != 0) return -1;
    } else {
        db_exec(d->c, "SELECT 1 FROM sqlite_schema LIMIT 1;");
    }

    const char *ck_vfs = sqlite3_vfs_find("unix-excl") ? "unix-excl" : "unix";
    if (sqlite3_open_v2(path, &d->ck_c,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE,
                        ck_vfs) != SQLITE_OK) return -1;
    if (d->is_large_db) {
        int chunk = 64 << 20;
        sqlite3_file_control(d->ck_c, "main", SQLITE_FCNTL_CHUNK_SIZE, &chunk);
    }
    if (db_exec(d->ck_c,
                "PRAGMA synchronous=NORMAL;"
                "PRAGMA temp_store=MEMORY;"
                "PRAGMA mmap_size=0;"
                "PRAGMA wal_autocheckpoint=0;"
                "PRAGMA busy_timeout=0;"
                "PRAGMA cache_size=-1024;") != SQLITE_OK) return -1;

    d->ck_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_mutex_init(&d->ck_mu, NULL);
    pthread_cond_init(&d->ck_cond, NULL);
    atomic_store(&d->ck_state, CK_IDLE);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 10);
    if (pthread_create(&d->ck_thr, &attr, ck_main, d) == 0) {
        d->ck_thread_up = 1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

static void db_close(struct db *d) {
    if (d->ck_thread_up) {
        pthread_mutex_lock(&d->ck_mu);
        d->ck_stop = 1;
        pthread_cond_signal(&d->ck_cond);
        pthread_mutex_unlock(&d->ck_mu);
        pthread_join(d->ck_thr, NULL);
        d->ck_thread_up = 0;
    }
    if (d->in_tx) {
        bt_close(&d->bt);
        db_step_stmt(d->wrote ? d->st_rollback : d->st_commit);
        d->in_tx = 0;
        d->wrote = 0;
    }
    if (d->st_health) sqlite3_finalize(d->st_health);
    if (d->st_ins_post) sqlite3_finalize(d->st_ins_post);
    if (d->st_ins_like) sqlite3_finalize(d->st_ins_like);
    if (d->st_post_exists) sqlite3_finalize(d->st_post_exists);
    if (d->st_feed_ref) sqlite3_finalize(d->st_feed_ref);
    if (d->st_post_ref) sqlite3_finalize(d->st_post_ref);
    if (d->st_begin_read) sqlite3_finalize(d->st_begin_read);
    if (d->st_commit) sqlite3_finalize(d->st_commit);
    if (d->st_rollback) sqlite3_finalize(d->st_rollback);
    if (d->ck_c) sqlite3_close(d->ck_c);
    if (d->c) sqlite3_close(d->c);
    if (d->db_fd >= 0) close(d->db_fd);
    if (d->wal_fd >= 0) close(d->wal_fd);
    if (d->ck_efd >= 0) close(d->ck_efd);
    pthread_mutex_destroy(&d->ck_mu);
    pthread_cond_destroy(&d->ck_cond);
    memset(d, 0, sizeof(*d));
    d->db_fd = d->wal_fd = d->ck_efd = -1;
}

static inline int db_begin(struct db *d) {
    if (likely(d->in_tx)) {
        if (unlikely(!d->bt.open)) return bt_open(&d->bt);
        return 0;
    }
    int rc = db_step_stmt(d->st_begin_read);
    if (unlikely(rc != SQLITE_DONE)) return -1;
    d->in_tx = 1;
    if (unlikely(bt_open(&d->bt) != 0)) {
        db_step_stmt(d->st_rollback);
        d->in_tx = 0;
        return -1;
    }
    return 0;
}

static int db_commit(struct db *d) {
    if (!d->in_tx) return 0;
    bt_close(&d->bt);
    int rc;
    if (unlikely(d->broken)) {
        rc = db_step_stmt(d->st_rollback);
        d->broken = 0;
        d->wrote = 0;
        d->in_tx = 0;
        return -1;
    }
    rc = db_step_stmt(d->st_commit);
    if (unlikely(rc != SQLITE_DONE)) {
        db_step_stmt(d->st_rollback);
        d->wrote = 0;
        d->in_tx = 0;
        return -1;
    }
    d->wrote = 0;
    d->in_tx = 0;
    return 0;
}

static inline void db_ck_start(struct db *d, int do_sync) {
    pthread_mutex_lock(&d->ck_mu);
    if (atomic_load_explicit(&d->ck_state, memory_order_relaxed) == CK_IDLE) {
        d->ck_sync_req = do_sync;
        atomic_store_explicit(&d->ck_state, CK_RUN, memory_order_release);
        pthread_cond_signal(&d->ck_cond);
    }
    pthread_mutex_unlock(&d->ck_mu);
}

static inline int db_should_yield_hold(const struct db *d, uint32_t now_ms) {
    if (likely(!d->writes_held)) return 0;
    if (atomic_load_explicit(&d->ck_state, memory_order_acquire) != CK_RUN) return 0;
    return now_ms >= d->hold_start_ms && (now_ms - d->hold_start_ms) >= 20u;
}

static void db_tick(struct db *d, uint32_t now_ms) {
    int st = atomic_load_explicit(&d->ck_state, memory_order_acquire);
    if (unlikely(st == CK_DONE)) {
        int rc = d->ck_rc;
        int log = d->ck_log;
        int ckpt = d->ck_ckpt;
        atomic_store_explicit(&d->ck_state, CK_IDLE, memory_order_release);
        d->ck_last_req = d->wal_frames;

        if (rc == SQLITE_OK && ckpt > 0 && ckpt == log) {
            d->ck_last_req = 0;
        }
        if (d->writes_held) {
            if (rc == SQLITE_OK && log >= 0 && ckpt >= log) {
                d->writes_held = 0;
                d->ck_last_req = 0;
            } else if (now_ms - d->hold_start_ms >= d->hold_max_ms) {
                d->writes_held = 0;
            } else {
                db_ck_start(d, 0);
                return;
            }
        }
        if (d->wal_frames >= d->ck_hold_at) {
            int rem = (log > ckpt && rc == SQLITE_OK) ? (log - ckpt) : (int)d->wal_frames;
            if (rem <= 2000 || d->wal_frames >= d->ck_hold_at + d->ck_pass_frames) {
                if (!d->writes_held) {
                    if (d->in_tx && db_commit(d) != 0) d->broken = 0;
                    d->writes_held = 1;
                    d->hold_start_ms = now_ms;
                }
                db_ck_start(d, 0);
            } else {
                db_ck_start(d, 1);
            }
        }
        return;
    }

    if (unlikely(d->writes_held)) {
        if (now_ms - d->hold_start_ms >= d->hold_max_ms) {
            d->writes_held = 0;
        }
        return;
    }

    if (st == CK_IDLE && d->ck_thread_up) {
        if (d->wal_frames >= d->ck_hold_at) {
            if (d->in_tx && db_commit(d) != 0) d->broken = 0;
            d->writes_held = 1;
            d->hold_start_ms = now_ms;
            db_ck_start(d, 1);
        } else if (d->wal_frames >= d->ck_last_req + d->ck_pass_frames) {
            int near_hold = (d->wal_frames + d->ck_pass_frames * 2u >= d->ck_hold_at);
            d->ck_last_req = d->wal_frames;
            db_ck_start(d, near_hold);
        }
    }
}

static inline int db_end_batch(struct db *d, uint32_t now_ms) {
    if (d->in_tx) {
        if (d->wrote || d->broken || !d->carry_read_tx || d->writes_held) {
            if (db_commit(d) != 0) return -1;
        }
    }
    db_tick(d, now_ms);
    return 0;
}

static int db_health(struct db *d) {
    if (db_begin(d) != 0) return -1;
    int rc = sqlite3_step(d->st_health);
    int col = (rc == SQLITE_ROW) ? sqlite3_column_int(d->st_health, 0) : 0;
    sqlite3_reset(d->st_health);
    if (rc != SQLITE_ROW || col != 1) return -1;
    return 0;
}

static inline int db_feed(struct db *d, struct ob *o) {
    if (unlikely(db_begin(d) != 0)) return -1;
    size_t mark = o->n;
    if (likely(d->bt.open && bt_feed(&d->bt, o) == 0 && !o->err)) return 0;
    o->n = mark;
    o->err = 0;
    ob_lit(o, "{\"posts\":[");
    int n = 0, rc;
    while ((rc = sqlite3_step(d->st_feed_ref)) == SQLITE_ROW) {
        if (n++) ob_char(o, ',');
        ob_post_obj(o,
                    sqlite3_column_int64(d->st_feed_ref, 0),
                    sqlite3_column_text(d->st_feed_ref, 1), (u32)sqlite3_column_bytes(d->st_feed_ref, 1),
                    sqlite3_column_text(d->st_feed_ref, 2), (u32)sqlite3_column_bytes(d->st_feed_ref, 2),
                    sqlite3_column_text(d->st_feed_ref, 3), (u32)sqlite3_column_bytes(d->st_feed_ref, 3),
                    sqlite3_column_int64(d->st_feed_ref, 4));
    }
    sqlite3_reset(d->st_feed_ref);
    if (rc != SQLITE_DONE) {
        d->broken = 1;
        return -1;
    }
    ob_lit(o, "]}");
    return o->err ? -1 : 0;
}

static inline int db_post(struct db *d, struct ob *o, int64_t id) {
    if (unlikely(db_begin(d) != 0)) return -1;
    size_t mark = o->n;
    if (likely(d->bt.open)) {
        int r = bt_post(&d->bt, o, id);
        if (likely(r >= 0 && !o->err)) return r;
        o->n = mark;
        o->err = 0;
    }
    sqlite3_bind_int64(d->st_post_ref, 1, id);
    int rc = sqlite3_step(d->st_post_ref), ret;
    if (rc == SQLITE_ROW) {
        ob_lit(o, "{\"post\":");
        ob_post_obj(o,
                    sqlite3_column_int64(d->st_post_ref, 0),
                    sqlite3_column_text(d->st_post_ref, 1), (u32)sqlite3_column_bytes(d->st_post_ref, 1),
                    sqlite3_column_text(d->st_post_ref, 2), (u32)sqlite3_column_bytes(d->st_post_ref, 2),
                    sqlite3_column_text(d->st_post_ref, 3), (u32)sqlite3_column_bytes(d->st_post_ref, 3),
                    sqlite3_column_int64(d->st_post_ref, 4));
        ob_char(o, '}');
        ret = o->err ? -1 : 1;
    } else {
        ret = (rc == SQLITE_DONE) ? 0 : -1;
        if (ret < 0) d->broken = 1;
    }
    sqlite3_reset(d->st_post_ref);
    return ret; // 1=ok, 0=not found, -1=err
}

static inline int db_post_exists(struct db *d, int64_t id) {
    if (unlikely(db_begin(d) != 0)) return -1;
    if (likely(d->bt.open)) {
        int r = bt_post_exists(&d->bt, id);
        if (likely(r >= 0)) return r;
    }
    sqlite3_bind_int64(d->st_post_exists, 1, id);
    int rc = sqlite3_step(d->st_post_exists);
    sqlite3_reset(d->st_post_exists);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    d->broken = 1;
    return -1;
}

// Returns 0 on success, 1 on constraint failure, -1 on IO/busy error.
static int db_insert_post(struct db *d, int64_t uid, const char *body, int body_len,
                          int64_t *id_out, char created_out[25]) {
    if (db_begin(d) != 0) return -1;
    bt_close_mask(&d->bt, (1ULL << CUR_POSTS) | (1ULL << CUR_FEED_TAIL) | (1ULL << CUR_POST_PREV) | (1ULL << CUR_FEED_IDX));
    int clen = db_now_iso(d, created_out);
    sqlite3_bind_int64(d->st_ins_post, 1, uid);
    sqlite3_bind_text(d->st_ins_post, 2, body, body_len, SQLITE_STATIC);
    sqlite3_bind_text(d->st_ins_post, 3, created_out, clen, SQLITE_STATIC);
    int rc = sqlite3_step(d->st_ins_post);
    sqlite3_reset(d->st_ins_post);
    if (likely(rc == SQLITE_DONE)) {
        *id_out = sqlite3_last_insert_rowid(d->c);
        d->wrote = 1;
        return 0;
    }
    if ((rc & 0xff) == SQLITE_CONSTRAINT) return 1;
    d->broken = 1;
    return -1;
}

// Returns 0 on new like, 2 on already liked, 1 on constraint failure, -1 on IO/busy error.
static int db_insert_like(struct db *d, int64_t uid, int64_t pid, char created_out[25]) {
    if (db_begin(d) != 0) return -1;
    bt_close_mask(&d->bt, (1ULL << CUR_LIKES_IDX) | (1ULL << CUR_LIKES_LAST));
    char tmp[25];
    char *dst = created_out ? created_out : tmp;
    int clen = db_now_iso(d, dst);
    sqlite3_bind_int64(d->st_ins_like, 1, uid);
    sqlite3_bind_int64(d->st_ins_like, 2, pid);
    sqlite3_bind_text(d->st_ins_like, 3, dst, clen, SQLITE_STATIC);
    int rc = sqlite3_step(d->st_ins_like);
    sqlite3_reset(d->st_ins_like);
    if (likely(rc == SQLITE_DONE)) {
        int ch = sqlite3_changes(d->c);
        if (ch > 0) d->wrote = 1;
        return ch > 0 ? 0 : 2;
    }
    if ((rc & 0xff) == SQLITE_CONSTRAINT) return 1;
    d->broken = 1;
    return -1;
}
