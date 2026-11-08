#define _GNU_SOURCE
#include "vendor/sqlite-3.54.1/sqlite3.c"
#include "ring.h"
#include "wal_vfs.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <emmintrin.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <linux/io_uring.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 512
#define MAX_REQUEST_BODY (16U * 1024U)
#define REQUEST_CAP (MAX_REQUEST_BODY + 8192U + 5U)
#define PIPELINE_INPUT_CAP (16U * 1024U * 1024U)
#define INPUT_INITIAL_CAP 512
#define RESPONSE_CAP (128U * 1024U)
#define RESPONSE_INITIAL_CAP 4096
#define RESPONSE_POOL_MAX_CAP 16384
#define HEADER_LIMIT 8192
#define JSON_FIELDS 32
#define RING_POOL_SIZE 20
#define RING_ACCEPT_LISTENER_COUNT 8
#define RING_FILE_LIMIT 65535
#define RING_BUFFER_COUNT 1024
#define RING_BUFFER_SIZE 2048
#define RING_CQ_ENTRIES 4096
#define RING_CQ_BATCH 256
#define RING_CQ_DRAIN_PER_RING 1024
#define RING_CQ_DRAIN_BUDGET 32768
#define PIPELINE_INPUT_PAUSE_THRESHOLD \
    (PIPELINE_INPUT_CAP - (RING_CQ_ENTRIES * RING_BUFFER_SIZE) - \
     RING_BUFFER_SIZE)
#define RING_READY_ANSWER_BUDGET 2048
#define RING_RECOVERY_INFLIGHT_LIMIT 1024
#define RESPONSE_POOL_LIMIT 256
#define SECRET_DEFAULT "twelve-dollar-challenge"
#define WAL_HOLD_THRESHOLD_FRAMES 4096
#define WAL_HOLD_MAX_NS 250000000ULL
#define WAL_HOLD_RETRY_NS 1000000L
#define WAL_HOLD_RETRY_LIMIT 250U
#define WAL_DATABASE_CHUNK_SIZE (64 * 1024 * 1024)
#define WAL_CHECKPOINT_IDLE_NS 250000000L
#define WAL_RESUME_BATCH 128

/* SQLite's default cache, wrapped to avoid scans after a B-tree page's temporary
 * lock-byte-page rekey has inflated pcache1's high-water mark. */
typedef struct {
    /* Each NOMUTEX connection is thread-confined: g_db belongs to the event loop,
     * and the checkpoint worker owns a separate connection and cache. */
    sqlite3_pcache *base;
    unsigned max_page;
    unsigned lock_page;
    bool may_have_parked_lock_page; /* conservative; cleared by rekey/truncate */
} PageCacheWrapper;

static sqlite3_pcache_methods2 g_page_cache_base;

static int page_cache_wrapper_init(void *arg) {
    (void)arg;
    return g_page_cache_base.xInit
        ? g_page_cache_base.xInit(g_page_cache_base.pArg) : SQLITE_OK;
}

static void page_cache_wrapper_shutdown(void *arg) {
    (void)arg;
    if (g_page_cache_base.xShutdown)
        g_page_cache_base.xShutdown(g_page_cache_base.pArg);
}

static sqlite3_pcache *page_cache_wrapper_create(int page_size, int extra,
                                                  int purgeable) {
    if (page_size <= 0) return NULL;
    PageCacheWrapper *wrapper = sqlite3_malloc(sizeof(*wrapper));
    if (!wrapper) return NULL;
    wrapper->base = g_page_cache_base.xCreate(page_size, extra, purgeable);
    if (!wrapper->base) {
        sqlite3_free(wrapper);
        return NULL;
    }
    wrapper->max_page = 0;
    wrapper->lock_page = 0x40000000U / (unsigned)page_size + 1U;
    wrapper->may_have_parked_lock_page = false;
    return (sqlite3_pcache *)wrapper;
}

#define PAGE_CACHE_BASE(p) (((PageCacheWrapper *)(p))->base)

static void page_cache_wrapper_cachesize(sqlite3_pcache *cache, int pages) {
    g_page_cache_base.xCachesize(PAGE_CACHE_BASE(cache), pages);
}

static int page_cache_wrapper_pagecount(sqlite3_pcache *cache) {
    return g_page_cache_base.xPagecount(PAGE_CACHE_BASE(cache));
}

static sqlite3_pcache_page *page_cache_wrapper_fetch(sqlite3_pcache *cache,
                                                      unsigned key,
                                                      int create) {
    PageCacheWrapper *wrapper = (PageCacheWrapper *)cache;
    sqlite3_pcache_page *page =
        g_page_cache_base.xFetch(wrapper->base, key, create);
    if (page && create) {
        if (key == wrapper->lock_page)
            wrapper->may_have_parked_lock_page = true;
        else if (key > wrapper->max_page) wrapper->max_page = key;
    }
    return page;
}

static void page_cache_wrapper_unpin(sqlite3_pcache *cache,
                                      sqlite3_pcache_page *page,
                                      int discard) {
    g_page_cache_base.xUnpin(PAGE_CACHE_BASE(cache), page, discard);
}

static void page_cache_wrapper_rekey(sqlite3_pcache *cache,
                                      sqlite3_pcache_page *page,
                                      unsigned from, unsigned to) {
    PageCacheWrapper *wrapper = (PageCacheWrapper *)cache;
    g_page_cache_base.xRekey(wrapper->base, page, from, to);
    if (to == wrapper->lock_page)
        wrapper->may_have_parked_lock_page = true;
    else {
        if (from == wrapper->lock_page)
            wrapper->may_have_parked_lock_page = false;
        if (to > wrapper->max_page) wrapper->max_page = to;
    }
}

static void page_cache_wrapper_truncate(sqlite3_pcache *cache,
                                         unsigned limit) {
    PageCacheWrapper *wrapper = (PageCacheWrapper *)cache;
    if (!wrapper->may_have_parked_lock_page && wrapper->max_page < limit) return;
    g_page_cache_base.xTruncate(wrapper->base, limit);
    if (limit > 0U && wrapper->max_page >= limit)
        wrapper->max_page = limit - 1U;
    if (wrapper->lock_page >= limit) wrapper->may_have_parked_lock_page = false;
}

static void page_cache_wrapper_destroy(sqlite3_pcache *cache) {
    g_page_cache_base.xDestroy(PAGE_CACHE_BASE(cache));
    sqlite3_free(cache);
}

static void page_cache_wrapper_shrink(sqlite3_pcache *cache) {
    g_page_cache_base.xShrink(PAGE_CACHE_BASE(cache));
}

#undef PAGE_CACHE_BASE

static int page_cache_wrapper_install(void) {
    static sqlite3_pcache_methods2 methods;
    int rc = sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &g_page_cache_base);
    if (rc != SQLITE_OK) return rc;
    methods = g_page_cache_base;
    methods.pArg = NULL;
    methods.xInit = page_cache_wrapper_init;
    methods.xShutdown = page_cache_wrapper_shutdown;
    methods.xCreate = page_cache_wrapper_create;
    methods.xCachesize = page_cache_wrapper_cachesize;
    methods.xPagecount = page_cache_wrapper_pagecount;
    methods.xFetch = page_cache_wrapper_fetch;
    methods.xUnpin = page_cache_wrapper_unpin;
    methods.xRekey = page_cache_wrapper_rekey;
    methods.xTruncate = page_cache_wrapper_truncate;
    methods.xDestroy = page_cache_wrapper_destroy;
    methods.xShrink = page_cache_wrapper_shrink;
    return sqlite3_config(SQLITE_CONFIG_PCACHE2, &methods);
}

typedef struct {
    char key[64];
    int type;
    size_t start;
    size_t end;
} JsonField;

typedef struct {
    const char *data;
    size_t len;
    JsonField fields[JSON_FIELDS];
    size_t count;
} JsonObject;

typedef struct Conn {
    int fd;
    unsigned ring_index;
    unsigned recv_capacity;
    unsigned cancel_pending_mask;
    unsigned cancel_armed_mask;
    bool direct_file : 1;
    bool close_after_write : 1;
    bool peer_eof : 1;
    bool closing : 1;
    bool fd_closed : 1;
    bool recv_armed : 1;
    bool recv_shared_buffer : 1;
    bool recv_recovery : 1;
    bool recv_multishot : 1;
    bool recv_paused : 1;
    bool input_shared : 1;
    bool rearm_pending : 1;
    bool rearm_recovery : 1;
    bool ready_pending : 1;
    bool batch_sensitive : 1;
    bool wal_write_deferred : 1;
    bool send_deferred : 1;
    bool send_pending : 1;
    bool send_armed : 1;
    bool cancel_queued : 1;
    uint32_t input_len;
    uint32_t input_cap;
    uint32_t output_len;
    uint32_t output_pos;
    uint32_t request_consume;
    uint32_t pending_input_len;
    uint32_t pending_input_cap;
    size_t output_cap;
    struct Conn *rearm_next;
    struct Conn *ready_next;
    struct Conn *batch_next;
    struct Conn *send_next;
    struct Conn *wal_next;
    struct Conn *cancel_next;
    char *input;
    char *output;
    char *pending_input;
#ifdef RING_DIAG
    uint64_t diag_rearm_since_ns;
    uint64_t diag_response_ready_ns;
    uint64_t diag_send_queued_ns;
    uint64_t diag_send_submitted_ns;
    unsigned diag_route;
    bool diag_response_sampled;
    bool diag_batch_release_seen;
    bool diag_first_send_cqe_seen;
    bool diag_send_outstanding;
    bool diag_output_accounted;
    uint64_t diag_output_bytes;
#endif
} Conn;

_Static_assert(REQUEST_CAP <= UINT32_MAX, "request capacity must fit Conn lengths");
_Static_assert(RESPONSE_CAP <= UINT32_MAX, "response capacity must fit Conn lengths");
_Static_assert(PIPELINE_INPUT_CAP <= UINT32_MAX,
               "pipeline capacity must fit Conn lengths");

#ifdef RING_DIAG
enum {
    RING_DIAG_ROUTE_COUNT = 5,
    RING_DIAG_STAGE_COUNT = 6,
    RING_DIAG_LATENCY_BIN_COUNT = 20
};

typedef struct {
    uint64_t recv_sqe_selected;
    uint64_t recv_sqe_plain;
    uint64_t recv_sqe_enospc;
    uint64_t send_sqe;
    uint64_t recv_cqe_selected;
    uint64_t recv_cqe_plain;
    uint64_t recv_bytes_selected;
    uint64_t recv_bytes_plain;
    uint64_t recv_errors;
    uint64_t recv_enobufs;
    uint64_t recv_eagain;
    uint64_t recv_eintr;
    uint64_t recv_eof;
    uint64_t send_cqe;
    uint64_t send_errors;
    uint64_t send_sqe_enospc;
    uint64_t send_partial_cqe;
    uint64_t send_retry_cqe;
    uint64_t responses_complete;
    uint64_t cq_ready_samples;
    uint64_t cq_ready_total;
    uint64_t cq_ready_max;
    uint64_t send_pending_peak;
    uint64_t latency_samples[RING_DIAG_ROUTE_COUNT][RING_DIAG_STAGE_COUNT];
    uint64_t latency_hist[RING_DIAG_ROUTE_COUNT][RING_DIAG_STAGE_COUNT]
                        [RING_DIAG_LATENCY_BIN_COUNT];
    uint64_t rearm_queue_adds;
    uint64_t rearm_waits;
    uint64_t rearm_wait_total_ns;
    uint64_t rearm_wait_max_ns;
    uint64_t rearm_pending;
    uint64_t rearm_pending_peak;
    uint64_t ready_enqueued;
    uint64_t ready_answered;
    uint64_t ready_pending;
    uint64_t ready_pending_peak;
    uint64_t enter_syscalls;
    uint64_t accepted_recv_sqes;
    uint64_t accepted_send_sqes;
    uint64_t accepted_other_sqes;
} RingDiagCounters;

enum {
    RING_DIAG_STAGE_READY_TO_SQE = 0,
    RING_DIAG_STAGE_SQE_TO_ACCEPT = 1,
    RING_DIAG_STAGE_ACCEPT_TO_FIRST_CQE = 2,
    RING_DIAG_STAGE_READY_TO_FIRST_CQE = 3,
    RING_DIAG_STAGE_READY_TO_BATCH = 4,
    RING_DIAG_STAGE_READY_TO_COMPLETE = 5
};
#endif

typedef struct {
    ring io;
    int event_fd;
    bool files_registered;
    bool buffers_registered;
    bool accept_outstanding;
    bool accept_paused;
    bool accept_multishot;
    bool recv_multishot_enabled;
    bool accept_sqe_retry;
    bool rearm_sqe_retry;
    bool cancel_sqe_retry;
    struct io_uring_buf_ring *buf_ring;
    unsigned char *buf_storage;
    size_t buf_ring_bytes;
    unsigned buf_tail;
    unsigned buf_available;
    unsigned buf_entries;
    unsigned buf_size;
    unsigned buf_group;
    unsigned recovery_inflight;
    unsigned recovery_queued;
    Conn *rearm_head;
    Conn *rearm_tail;
    Conn *send_head;
    Conn *send_tail;
    Conn *cancel_head;
    Conn *cancel_tail;
#ifdef RING_DIAG
    RingDiagCounters diag;
    unsigned send_pending_count;
#endif
} RingCtx;

typedef struct {
    char method[8];
    char path[512];
    char version[16];
    const char *body;
    size_t body_len;
    const char *authorization;
    size_t authorization_len;
    bool close;
} Request;

typedef struct {
    char *data;
    size_t cap;
    size_t len;
    bool failed;
    Conn *owner;
} Writer;

typedef struct {
    sqlite3_int64 user_id;
    char username[HEADER_LIMIT + 1U];
    size_t username_len;
} Auth;

static sqlite3 *g_db;
static const char *g_db_path;
static pthread_t g_wal_checkpoint_thread;
static sem_t g_wal_checkpoint_sem;
static _Atomic int g_wal_checkpoint_sem_ready;
static _Atomic int g_wal_checkpoint_wakeup_pending;
static _Atomic int g_wal_checkpoint_failed;
static _Atomic int g_wal_write_hold;
static _Atomic int g_wal_wakeup_fd = -1;
static sqlite3_stmt *g_feed_stmt;
static sqlite3_stmt *g_post_stmt;
static sqlite3_stmt *g_insert_stmt;
static sqlite3_stmt *g_post_exists_stmt;
static sqlite3_stmt *g_insert_like_stmt;
static sqlite3_stmt *g_health_stmt;
static int g_epoll = -1;
static int g_ring_epoll = -1;
static RingCtx g_rings[RING_POOL_SIZE];
static unsigned g_ring_count;
static unsigned g_ring_file_slots;
static bool g_direct_files;
static bool g_ring_active;
static bool g_recv_multishot = true;
static bool g_batch_write_txn;
static bool g_batch_read_txn;
static Conn *g_deferred_head;
static Conn *g_deferred_tail;
static Conn *g_wal_hold_head;
static Conn *g_wal_hold_tail;
static Conn *g_ready_head;
static Conn *g_ready_tail;
static unsigned g_ready_pending;
static unsigned g_ready_pending_peak;
static bool g_recovery_draining;
typedef struct {
    char *data;
    size_t cap;
} ResponseBuffer;
static ResponseBuffer g_response_pool[RESPONSE_POOL_LIMIT];
static unsigned g_response_pool_count;
static unsigned g_accept_next_ring;
static int g_listener_fds[RING_ACCEPT_LISTENER_COUNT];
static unsigned g_listener_count;
static time_t g_started;
static const char *g_secret = SECRET_DEFAULT;
static size_t g_secret_len;
static char *g_listener_tag = "listener";

static void ring_release_direct(Conn *c);
static void ring_defer_wal_write(Conn *c);
static int ring_copy_shared_input(Conn *c);
static uint64_t ring_monotonic_ns(void);
#ifdef RING_DIAG
/* Event-loop-owned gauges. Byte counters use requested capacities, excluding
 * allocator overhead and the fixed provided-buffer rings. Peaks are run-wide. */
typedef struct {
    uint64_t response_active, response_active_peak;
    uint64_t response_active_bytes, response_active_bytes_peak;
    uint64_t response_pool_bytes;
    uint64_t response_alloc_total, response_free_total, response_pool_hits;
    uint64_t response_grow_total, response_grow_fail, response_grow_move;
    uint64_t output_used_bytes, output_used_bytes_peak;
    uint64_t sends_active, sends_active_peak;
    uint64_t input_bytes, input_bytes_peak, input_buffers, input_buffers_peak;
    uint64_t wal_depth, wal_depth_peak;
    uint64_t accounting_errors, send_duplicate, close_with_send, close_in_wal;
} RingMemoryDiag;
static RingMemoryDiag g_ring_memory_diag;

static void ring_diag_output_account(Conn *c) {
    RingMemoryDiag *d = &g_ring_memory_diag;
    if (c->diag_output_accounted) {
        ++d->accounting_errors;
        return;
    }
    c->diag_output_accounted = true;
    c->diag_output_bytes = c->output_len;
    d->output_used_bytes += c->diag_output_bytes;
    if (d->output_used_bytes > d->output_used_bytes_peak)
        d->output_used_bytes_peak = d->output_used_bytes;
}

static void ring_diag_output_unaccount(Conn *c) {
    RingMemoryDiag *d = &g_ring_memory_diag;
    if (!c->diag_output_accounted) return;
    if (c->diag_output_bytes > d->output_used_bytes) {
        ++d->accounting_errors;
        d->output_used_bytes = 0;
    } else {
        d->output_used_bytes -= c->diag_output_bytes;
    }
    c->diag_output_accounted = false;
    c->diag_output_bytes = 0;
}

static void ring_diag_input_resize(size_t old_cap, size_t new_cap) {
    RingMemoryDiag *d = &g_ring_memory_diag;
    if (old_cap > d->input_bytes) {
        ++d->accounting_errors;
        d->input_bytes = 0;
    } else {
        d->input_bytes -= old_cap;
    }
    d->input_bytes += new_cap;
    if (old_cap == 0U && new_cap != 0U) ++d->input_buffers;
    if (old_cap != 0U && new_cap == 0U) {
        if (d->input_buffers != 0U) --d->input_buffers;
        else ++d->accounting_errors;
    }
    if (d->input_bytes > d->input_bytes_peak) d->input_bytes_peak = d->input_bytes;
    if (d->input_buffers > d->input_buffers_peak)
        d->input_buffers_peak = d->input_buffers;
}

static uint64_t g_ring_diag_sample_state = 0x9e3779b97f4a7c15ULL;
static void ring_diag_mark_response_ready(Conn *c, unsigned route);
static void ring_diag_record_stage(Conn *c, unsigned stage,
                                   uint64_t start_ns, uint64_t end_ns);
static void ring_diag_send_accept_observed(uint64_t user_data);

static bool ring_diag_should_sample(void) {
    uint64_t x = g_ring_diag_sample_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_ring_diag_sample_state = x;
    return ((x * 0x2545f4914f6cdd1dULL) & 1023U) == 0U;
}
#endif

static void wal_set_write_hold(int hold) {
    int previous = atomic_exchange_explicit(&g_wal_write_hold, hold,
                                            memory_order_acq_rel);
    if (previous == 0 || hold != 0) return;
    int wake_fd = atomic_load_explicit(&g_wal_wakeup_fd, memory_order_acquire);
    if (wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t written;
        do {
            written = write(wake_fd, &one, sizeof(one));
        } while (written < 0 && errno == EINTR);
    }
}

static char *response_acquire(size_t min_cap, size_t *out_cap) {
    char *buffer = NULL;
    size_t cap = 0U;
    bool from_pool = false;
    *out_cap = 0U;
    if (g_response_pool_count != 0U) {
        ResponseBuffer pooled = g_response_pool[--g_response_pool_count];
        buffer = pooled.data;
        cap = pooled.cap;
        from_pool = true;
#ifdef RING_DIAG
        ++g_ring_memory_diag.response_pool_hits;
        if (cap > g_ring_memory_diag.response_pool_bytes) {
            ++g_ring_memory_diag.accounting_errors;
            g_ring_memory_diag.response_pool_bytes = 0U;
        } else {
            g_ring_memory_diag.response_pool_bytes -= cap;
        }
#endif
    } else {
        buffer = malloc(min_cap);
        if (buffer) {
            cap = min_cap;
#ifdef RING_DIAG
            ++g_ring_memory_diag.response_alloc_total;
#endif
        }
    }
    if (!buffer) return NULL;
    if (cap < min_cap) {
        char *grown = realloc(buffer, min_cap);
        if (!grown) {
#ifdef RING_DIAG
            ++g_ring_memory_diag.response_grow_fail;
#endif
            if (from_pool) {
                g_response_pool[g_response_pool_count++] = (ResponseBuffer){buffer, cap};
#ifdef RING_DIAG
                g_ring_memory_diag.response_pool_bytes += cap;
#endif
            } else {
#ifdef RING_DIAG
                ++g_ring_memory_diag.response_free_total;
#endif
                free(buffer);
            }
            return NULL;
        }
#ifdef RING_DIAG
        ++g_ring_memory_diag.response_grow_total;
        if (grown != buffer) ++g_ring_memory_diag.response_grow_move;
#endif
        buffer = grown;
        cap = min_cap;
    }
    *out_cap = cap;
#ifdef RING_DIAG
    ++g_ring_memory_diag.response_active;
    g_ring_memory_diag.response_active_bytes += cap;
    if (g_ring_memory_diag.response_active >
        g_ring_memory_diag.response_active_peak)
        g_ring_memory_diag.response_active_peak =
            g_ring_memory_diag.response_active;
    if (g_ring_memory_diag.response_active_bytes >
        g_ring_memory_diag.response_active_bytes_peak)
        g_ring_memory_diag.response_active_bytes_peak =
            g_ring_memory_diag.response_active_bytes;
#endif
    return buffer;
}

static void response_release(char *buffer, size_t cap) {
    if (!buffer) return;
#ifdef RING_DIAG
    if (g_ring_memory_diag.response_active != 0U)
        --g_ring_memory_diag.response_active;
    else
        ++g_ring_memory_diag.accounting_errors;
    if (cap > g_ring_memory_diag.response_active_bytes) {
        ++g_ring_memory_diag.accounting_errors;
        g_ring_memory_diag.response_active_bytes = 0U;
    } else {
        g_ring_memory_diag.response_active_bytes -= cap;
    }
#endif
    if (cap <= RESPONSE_POOL_MAX_CAP && g_response_pool_count < RESPONSE_POOL_LIMIT) {
        g_response_pool[g_response_pool_count++] = (ResponseBuffer){buffer, cap};
#ifdef RING_DIAG
        g_ring_memory_diag.response_pool_bytes += cap;
#endif
    } else {
#ifdef RING_DIAG
        ++g_ring_memory_diag.response_free_total;
#endif
        free(buffer);
    }
}

enum { JT_STRING = 1, JT_NUMBER = 2, JT_OTHER = 3 };

static void skip_ws(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) {
        ++*p;
    }
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static int read_u16_escape(const char **p, const char *end, uint32_t *value) {
    if ((size_t)(end - *p) < 4) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        int h = hex_digit((unsigned char)(*p)[i]);
        if (h < 0) return -1;
        v = (v << 4) | (uint32_t)h;
    }
    *p += 4;
    *value = v;
    return 0;
}

/* Return the byte length of a well-formed UTF-8 sequence, or zero. */
static size_t valid_utf8_sequence(const unsigned char *s, size_t n) {
    if (n == 0) return 0;
    unsigned c = s[0];
    if (c < 0x80) return 1;
    size_t len = c >= 0xC2 && c <= 0xDF ? 2U :
                 c >= 0xE0 && c <= 0xEF ? 3U :
                 c >= 0xF0 && c <= 0xF4 ? 4U : 0U;
    if (len == 0 || n < len) return 0;
    for (size_t i = 1; i < len; ++i)
        if ((s[i] & 0xC0U) != 0x80U) return 0;
    if ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] >= 0xA0) ||
        (c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] >= 0x90)) return 0;
    return len;
}

static int parse_string_span(const char **p, const char *end, size_t *start, size_t *finish) {
    if (*p >= end || **p != '"') return -1;
    const char *s = *p;
    ++*p;
    while (*p < end) {
        unsigned char c = (unsigned char)**p;
        if (c == '"') {
            ++*p;
            *start = 0;
            *finish = (size_t)(*p - s);
            return 0;
        }
        if (c < 0x20) return -1;
        if (c >= 0x80) {
            size_t bytes = valid_utf8_sequence((const unsigned char *)*p,
                                               (size_t)(end - *p));
            if (bytes == 0) return -1;
            *p += bytes;
            continue;
        }
        if (c != '\\') {
            ++*p;
            continue;
        }
        ++*p;
        if (*p >= end) return -1;
        char e = **p;
        ++*p;
        if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
            e == 'n' || e == 'r' || e == 't') continue;
        if (e != 'u') return -1;
        uint32_t cp;
        if (read_u16_escape(p, end, &cp) != 0) return -1;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if ((size_t)(end - *p) < 6 || (*p)[0] != '\\' || (*p)[1] != 'u') return -1;
            *p += 2;
            uint32_t low;
            if (read_u16_escape(p, end, &low) != 0 || low < 0xDC00 || low > 0xDFFF) return -1;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return -1;
        }
    }
    return -1;
}

static int put_utf8(char *out, size_t cap, size_t *len, uint32_t cp) {
    unsigned char b[4];
    size_t n;
    if (cp <= 0x7F) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FF) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp <= 0xFFFF) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else if (cp <= 0x10FFFF) {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    } else {
        return -1;
    }
    if (*len + n >= cap) return -1;
    memcpy(out + *len, b, n);
    *len += n;
    return 0;
}

static int decode_json_string(const char *raw, size_t raw_len, char *out,
                              size_t cap, size_t *out_len) {
    if (raw_len < 2 || raw[0] != '"' || raw[raw_len - 1] != '"') return -1;
    const char *p = raw + 1;
    const char *end = raw + raw_len - 1;
    size_t len = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c != '\\') {
            if (len + 1 >= cap) return -1;
            out[len++] = (char)c;
            continue;
        }
        if (p >= end) return -1;
        char e = *p++;
        if (e == '"' || e == '\\' || e == '/') c = (unsigned char)e;
        else if (e == 'b') c = '\b';
        else if (e == 'f') c = '\f';
        else if (e == 'n') c = '\n';
        else if (e == 'r') c = '\r';
        else if (e == 't') c = '\t';
        else if (e == 'u') {
            uint32_t cp;
            if (read_u16_escape(&p, end, &cp) != 0) return -1;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if ((size_t)(end - p) < 6 || p[0] != '\\' || p[1] != 'u') return -1;
                p += 2;
                uint32_t low;
                if (read_u16_escape(&p, end, &low) != 0 || low < 0xDC00 || low > 0xDFFF) return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            if (put_utf8(out, cap, &len, cp) != 0) return -1;
            continue;
        } else {
            return -1;
        }
        if (len + 1 >= cap) return -1;
        out[len++] = (char)c;
    }
    out[len] = '\0';
    if (out_len) *out_len = len;
    return 0;
}

static int parse_number(const char **p, const char *end) {
    const char *s = *p;
    if (*p < end && **p == '-') ++*p;
    if (*p >= end) return -1;
    if (**p == '0') {
        ++*p;
    } else if (**p >= '1' && **p <= '9') {
        do { ++*p; } while (*p < end && **p >= '0' && **p <= '9');
    } else {
        return -1;
    }
    if (*p < end && **p == '.') {
        ++*p;
        if (*p >= end || **p < '0' || **p > '9') return -1;
        do { ++*p; } while (*p < end && **p >= '0' && **p <= '9');
    }
    if (*p < end && (**p == 'e' || **p == 'E')) {
        ++*p;
        if (*p < end && (**p == '+' || **p == '-')) ++*p;
        if (*p >= end || **p < '0' || **p > '9') return -1;
        do { ++*p; } while (*p < end && **p >= '0' && **p <= '9');
    }
    return *p == s ? -1 : 0;
}

static int parse_value(const char **p, const char *end, int depth, int *type,
                       size_t *start, size_t *finish);

static int parse_composite(const char **p, const char *end, int depth, char open, char close) {
    (void)depth;
    (void)close;
    if (*p >= end || **p != open) return -1;
    enum { ARRAY_FIRST, ARRAY_VALUE, ARRAY_AFTER,
           OBJECT_FIRST, OBJECT_KEY, OBJECT_COLON, OBJECT_VALUE, OBJECT_AFTER };
    unsigned char states[MAX_REQUEST_BODY];
    size_t count = 1;
    states[0] = open == '[' ? ARRAY_FIRST : OBJECT_FIRST;
    ++*p;
    while (count != 0) {
        skip_ws(p, end);
        if (*p >= end) return -1;
        unsigned char *state = &states[count - 1];
        if (*state == ARRAY_FIRST) {
            if (**p == ']') { ++*p; --count; continue; }
            *state = ARRAY_VALUE;
        } else if (*state == OBJECT_FIRST) {
            if (**p == '}') { ++*p; --count; continue; }
            *state = OBJECT_KEY;
        }
        if (*state == OBJECT_KEY) {
            size_t a, b;
            if (parse_string_span(p, end, &a, &b) != 0) return -1;
            *state = OBJECT_COLON;
            continue;
        }
        if (*state == OBJECT_COLON) {
            if (**p != ':') return -1;
            ++*p;
            *state = OBJECT_VALUE;
            continue;
        }
        if (*state == ARRAY_AFTER || *state == OBJECT_AFTER) {
            char closing = *state == ARRAY_AFTER ? ']' : '}';
            if (**p == closing) { ++*p; --count; continue; }
            if (**p != ',') return -1;
            ++*p;
            *state = *state == ARRAY_AFTER ? ARRAY_VALUE : OBJECT_KEY;
            continue;
        }
        if (*state != ARRAY_VALUE && *state != OBJECT_VALUE) return -1;
        *state = *state == ARRAY_VALUE ? ARRAY_AFTER : OBJECT_AFTER;
        if (**p == '[' || **p == '{') {
            if (count >= sizeof(states)) return -1;
            states[count++] = **p == '[' ? ARRAY_FIRST : OBJECT_FIRST;
            ++*p;
        } else {
            int type;
            size_t a, b;
            if (parse_value(p, end, 0, &type, &a, &b) != 0) return -1;
        }
    }
    return 0;
}

static int parse_value(const char **p, const char *end, int depth, int *type,
                       size_t *start, size_t *finish) {
    skip_ws(p, end);
    if (*p >= end || depth > 32) return -1;
    const char *s = *p;
    if (**p == '"') {
        if (parse_string_span(p, end, start, finish) != 0) return -1;
        *start = 0;
        *finish = (size_t)(*p - s);
        *type = JT_STRING;
        return 0;
    }
    if (**p == '{') {
        if (parse_composite(p, end, depth + 1, '{', '}') != 0) return -1;
        *type = JT_OTHER;
    } else if (**p == '[') {
        if (parse_composite(p, end, depth + 1, '[', ']') != 0) return -1;
        *type = JT_OTHER;
    } else if (**p == '-' || (**p >= '0' && **p <= '9')) {
        if (parse_number(p, end) != 0) return -1;
        *type = JT_NUMBER;
    } else if ((size_t)(end - *p) >= 4 && memcmp(*p, "true", 4) == 0) {
        *p += 4;
        *type = JT_OTHER;
    } else if ((size_t)(end - *p) >= 5 && memcmp(*p, "false", 5) == 0) {
        *p += 5;
        *type = JT_OTHER;
    } else if ((size_t)(end - *p) >= 4 && memcmp(*p, "null", 4) == 0) {
        *p += 4;
        *type = JT_OTHER;
    } else {
        return -1;
    }
    *start = 0;
    *finish = (size_t)(*p - s);
    return 0;
}

static int parse_json_object(const char *data, size_t len, JsonObject *obj) {
    const char *p = data;
    const char *end = data + len;
    obj->data = data;
    obj->len = len;
    obj->count = 0;
    skip_ws(&p, end);
    if (p >= end || *p != '{') return -1;
    ++p;
    skip_ws(&p, end);
    if (p < end && *p == '}') {
        ++p;
        skip_ws(&p, end);
        return p == end ? 0 : -1;
    }
    for (;;) {
        const char *key_ptr = p;
        size_t unused_start, unused_len;
        if (parse_string_span(&p, end, &unused_start, &unused_len) != 0) return -1;
        char key[64];
        size_t key_out = 0;
        bool key_decoded = decode_json_string(key_ptr, (size_t)(p - key_ptr),
                                              key, sizeof(key), &key_out) == 0;
        skip_ws(&p, end);
        if (p >= end || *p != ':') return -1;
        ++p;
        skip_ws(&p, end);
        const char *value_ptr = p;
        int type;
        size_t unused_value_start, unused_value_len;
        if (parse_value(&p, end, 1, &type, &unused_value_start, &unused_value_len) != 0) return -1;
        size_t val_start = (size_t)(value_ptr - data);
        size_t val_len = (size_t)(p - value_ptr);
        /* Only these keys are consumed by callers. Ignore other valid keys,
         * even when long or numerous, and preserve last-key-wins semantics. */
        static const char *const needed[] = {"alg", "sub", "username", "exp", "body"};
        for (size_t i = 0; key_decoded && i < sizeof(needed) / sizeof(needed[0]); ++i) {
            size_t need_len = strlen(needed[i]);
            if (key_out != need_len || memcmp(key, needed[i], need_len) != 0) continue;
            size_t slot = 0;
            while (slot < obj->count && strcmp(obj->fields[slot].key, needed[i]) != 0) ++slot;
            if (slot == obj->count && obj->count < JSON_FIELDS) ++obj->count;
            if (slot < obj->count) {
                JsonField *f = &obj->fields[slot];
                memcpy(f->key, needed[i], need_len + 1);
                f->type = type;
                f->start = val_start;
                f->end = val_start + val_len;
            }
            break;
        }
        skip_ws(&p, end);
        if (p >= end) return -1;
        if (*p == '}') {
            ++p;
            skip_ws(&p, end);
            return p == end ? 0 : -1;
        }
        if (*p != ',') return -1;
        ++p;
        skip_ws(&p, end);
    }
}

/* Valid non-object JSON has no body field; it is not malformed JSON. */
static int parse_post_json(const char *data, size_t len, JsonObject *obj) {
    const char *cursor = data;
    const char *end = data + len;
    skip_ws(&cursor, end);
    if (cursor < end && *cursor == '{') return parse_json_object(data, len, obj);
    int type;
    size_t start, finish;
    if (parse_value(&cursor, end, 0, &type, &start, &finish) != 0) return -1;
    skip_ws(&cursor, end);
    if (cursor != end) return -1;
    obj->data = data;
    obj->len = len;
    obj->count = 0;
    return 0;
}

static const JsonField *json_find(const JsonObject *obj, const char *key) {
    for (size_t i = obj->count; i > 0; --i) {
        const JsonField *f = &obj->fields[i - 1];
        if (strcmp(f->key, key) == 0) return f;
    }
    return NULL;
}

static int json_string_field(const JsonObject *obj, const char *key,
                             char *out, size_t cap, size_t *len) {
    const JsonField *f = json_find(obj, key);
    if (!f || f->type != JT_STRING || f->end > obj->len || f->end <= f->start) return -1;
    return decode_json_string(obj->data + f->start, f->end - f->start, out, cap, len);
}

static int json_number_field(const JsonObject *obj, const char *key, double *value) {
    const JsonField *f = json_find(obj, key);
    if (!f || f->type != JT_NUMBER || f->end > obj->len || f->end <= f->start) return -1;
    size_t n = f->end - f->start;
    if (n > HEADER_LIMIT) return -1;
    char text[HEADER_LIMIT + 1U];
    memcpy(text, obj->data + f->start, n);
    text[n] = '\0';
    char *end;
    double parsed = strtod(text, &end);
    if (end != text + n || !isfinite(parsed)) return -1;
    *value = parsed;
    return 0;
}

static int writer_reserve(Writer *w, size_t needed) {
    if (needed <= w->cap) return 0;
    if (!w->owner || !w->owner->output || needed > SIZE_MAX - 256U)
        return -1;
    size_t required = 256U + needed;
    if (required > RESPONSE_CAP) return -1;
    size_t next_cap = w->owner->output_cap;
    while (next_cap < required && next_cap < RESPONSE_CAP) {
        if (next_cap > RESPONSE_CAP / 2U) next_cap = RESPONSE_CAP;
        else next_cap *= 2U;
    }
    if (next_cap < required) return -1;
    char *old = w->owner->output;
    char *grown = realloc(old, next_cap);
    if (!grown) {
#ifdef RING_DIAG
        ++g_ring_memory_diag.response_grow_fail;
#endif
        return -1;
    }
#ifdef RING_DIAG
    ++g_ring_memory_diag.response_grow_total;
    if (grown != old) ++g_ring_memory_diag.response_grow_move;
    if (next_cap > w->owner->output_cap) {
        uint64_t delta = next_cap - w->owner->output_cap;
        g_ring_memory_diag.response_active_bytes += delta;
        if (g_ring_memory_diag.response_active_bytes >
            g_ring_memory_diag.response_active_bytes_peak)
            g_ring_memory_diag.response_active_bytes_peak =
                g_ring_memory_diag.response_active_bytes;
    }
#endif
    w->owner->output = grown;
    w->owner->output_cap = next_cap;
    w->data = grown + 256U;
    w->cap = next_cap - 256U;
    return 0;
}

static void writer_add(Writer *w, const char *s, size_t n) {
    if (w->failed || w->len > w->cap || n > SIZE_MAX - w->len) {
        w->failed = true;
        return;
    }
    size_t needed = w->len + n;
    if (needed > w->cap && writer_reserve(w, needed) != 0) {
        w->failed = true;
        return;
    }
    memcpy(w->data + w->len, s, n);
    w->len = needed;
}

static void writer_str(Writer *w, const char *s) {
    writer_add(w, s, strlen(s));
}

static void writer_char(Writer *w, char c) {
    writer_add(w, &c, 1);
}

static void writer_i64(Writer *w, int64_t value) {
    char out[21];
    size_t start = 0;
    uint64_t v;
    if (value < 0) {
        out[start++] = '-';
        v = (uint64_t)(-(value + 1)) + 1U;
    } else {
        v = (uint64_t)value;
    }
    size_t digits_start = start;
    do {
        out[start++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0);
    for (size_t left = digits_start, right = start - 1; left < right; ++left, --right) {
        char tmp = out[left];
        out[left] = out[right];
        out[right] = tmp;
    }
    writer_add(w, out, start);
}

static bool writer_json_string_is_safe(const char *s, size_t n) {
    if (n < 16) return false;
    const __m128i control_max = _mm_set1_epi8(0x1f);
    const __m128i quote = _mm_set1_epi8('"');
    const __m128i backslash = _mm_set1_epi8('\\');
    size_t i = 0;
    for (; n - i >= 16; i += 16) {
        __m128i bytes = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
        __m128i controls = _mm_cmpeq_epi8(_mm_min_epu8(bytes, control_max), bytes);
        __m128i quotes = _mm_cmpeq_epi8(bytes, quote);
        __m128i slashes = _mm_cmpeq_epi8(bytes, backslash);
        __m128i unsafe = _mm_or_si128(controls, _mm_or_si128(quotes, slashes));
        if (_mm_movemask_epi8(unsafe) != 0) return false;
    }
    for (; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 0x20) return false;
    }
    return true;
}

static void writer_json_string(Writer *w, const char *s, size_t n) {
    writer_char(w, '"');
    if (!w->failed && writer_json_string_is_safe(s, n)) {
        writer_add(w, s, n);
        writer_char(w, '"');
        return;
    }
    size_t segment = 0;
    for (size_t i = 0; i < n && !w->failed; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c != '"' && c != '\\' && c >= 0x20) continue;
        writer_add(w, s + segment, i - segment);
        if (c == '"' || c == '\\') {
            char escape[2] = {'\\', (char)c};
            writer_add(w, escape, sizeof(escape));
        } else {
            const char *escape = NULL;
            size_t escape_len = 2;
            switch (c) {
                case '\b': escape = "\\b"; break;
                case '\f': escape = "\\f"; break;
                case '\n': escape = "\\n"; break;
                case '\r': escape = "\\r"; break;
                case '\t': escape = "\\t"; break;
                default: {
                    static const char hex[] = "0123456789abcdef";
                    char unicode_escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15U]};
                    writer_add(w, unicode_escape, sizeof(unicode_escape));
                    escape_len = 0;
                    break;
                }
            }
            if (escape_len != 0) writer_add(w, escape, escape_len);
        }
        segment = i + 1;
    }
    if (!w->failed) writer_add(w, s + segment, n - segment);
    writer_char(w, '"');
}

static void writer_json_cstr(Writer *w, const char *s) {
    writer_json_string(w, s, strlen(s));
}

static void writer_post(Writer *w, sqlite3_int64 id, const unsigned char *body,
                        int body_len, const unsigned char *created,
                        const unsigned char *author, int author_len,
                        sqlite3_int64 like_count) {
    writer_str(w, "{\"id\":");
    writer_i64(w, (int64_t)id);
    writer_str(w, ",\"body\":");
    writer_json_string(w, (const char *)body, (size_t)body_len);
    writer_str(w, ",\"created_at\":");
    writer_json_cstr(w, (const char *)created);
    writer_str(w, ",\"author\":");
    writer_json_string(w, (const char *)author, (size_t)author_len);
    writer_str(w, ",\"like_count\":");
    writer_i64(w, (int64_t)like_count);
    writer_char(w, '}');
}

static int base64url_decode(const char *src, size_t len, unsigned char *dst, size_t cap,
                           size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=') {
            for (size_t j = i; j < len; ++j) if (src[j] != '=') return -1;
            break;
        }
        int v;
        if (c >= 'A' && c <= 'Z') v = (int)(c - 'A');
        else if (c >= 'a' && c <= 'z') v = (int)(c - 'a') + 26;
        else if (c >= '0' && c <= '9') v = (int)(c - '0') + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            dst[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    if (bits == 6) return -1;
    if (bits && (acc & ((1U << bits) - 1U)) != 0) return -1;
    *out_len = n;
    return 0;
}

enum { AUTH_OK = 0, AUTH_MISSING = 1, AUTH_INVALID = 2, AUTH_PAYLOAD = 3 };

static int verify_auth(const char *header, size_t header_len, Auth *auth) {
    if (!header || header_len < 7 || memcmp(header, "Bearer ", 7) != 0) return AUTH_MISSING;
    const char *token = header + 7;
    size_t token_len = header_len - 7;
    const char *dot1 = memchr(token, '.', token_len);
    if (!dot1) return AUTH_INVALID;
    size_t left = (size_t)(dot1 - token);
    size_t remain = token_len - left - 1;
    const char *dot2 = memchr(dot1 + 1, '.', remain);
    if (!dot2 || memchr(dot2 + 1, '.', token_len - (size_t)(dot2 + 1 - token))) return AUTH_INVALID;
    size_t middle = (size_t)(dot2 - dot1 - 1);
    size_t right = token_len - (size_t)(dot2 + 1 - token);
    unsigned char hbuf[HEADER_LIMIT + 1U], pbuf[HEADER_LIMIT + 1U], sig[128], digest[EVP_MAX_MD_SIZE];
    size_t hlen = 0, plen = 0, siglen = 0;
    if (base64url_decode(token, left, hbuf, sizeof(hbuf) - 1, &hlen) != 0 ||
        base64url_decode(dot1 + 1, middle, pbuf, sizeof(pbuf) - 1, &plen) != 0 ||
        base64url_decode(dot2 + 1, right, sig, sizeof(sig), &siglen) != 0) return AUTH_INVALID;
    hbuf[hlen] = '\0';
    pbuf[plen] = '\0';
    unsigned int digest_len = 0;
    if (!HMAC(EVP_sha256(), g_secret, (int)g_secret_len, (const unsigned char *)token,
              (size_t)(dot2 - token), digest, &digest_len) || digest_len != 32 ||
        siglen != digest_len || CRYPTO_memcmp(sig, digest, digest_len) != 0)
        return AUTH_INVALID;
    JsonObject hobj;
    if (parse_json_object((const char *)hbuf, hlen, &hobj) != 0) return AUTH_INVALID;
    char alg[32];
    size_t alg_len;
    if (json_string_field(&hobj, "alg", alg, sizeof(alg), &alg_len) != 0 ||
        alg_len != 5 || memcmp(alg, "HS256", 5) != 0) return AUTH_INVALID;
    JsonObject pobj;
    if (parse_json_object((const char *)pbuf, plen, &pobj) != 0) {
        if (parse_post_json((const char *)pbuf, plen, &pobj) != 0) return AUTH_INVALID;
        return AUTH_PAYLOAD;
    }
    char sub[HEADER_LIMIT + 1U];
    char username[sizeof(auth->username)];
    size_t sub_len, username_len;
    double exp;
    if (json_number_field(&pobj, "exp", &exp) != 0) return AUTH_INVALID;
    if (json_string_field(&pobj, "sub", sub, sizeof(sub), &sub_len) != 0 ||
        json_string_field(&pobj, "username", username, sizeof(username), &username_len) != 0 ||
        sub_len == 0)
        return AUTH_PAYLOAD;
    if (exp <= (double)time(NULL)) return AUTH_INVALID;
    uint64_t uid = 0;
    for (size_t i = 0; i < sub_len; ++i) {
        if (sub[i] < '0' || sub[i] > '9') return AUTH_PAYLOAD;
        uint64_t digit = (uint64_t)(sub[i] - '0');
        if (uid > ((uint64_t)LLONG_MAX - digit) / 10U) return AUTH_PAYLOAD;
        uid = uid * 10U + digit;
    }
    if (uid == 0) return AUTH_PAYLOAD;
    auth->user_id = (sqlite3_int64)uid;
    memcpy(auth->username, username, username_len + 1);
    auth->username_len = username_len;
    return AUTH_OK;
}

static size_t find_bytes(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return SIZE_MAX;
    size_t last = hay_len - needle_len;
    for (size_t i = 0; i <= last; ++i) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return i;
    }
    return SIZE_MAX;
}

static int parse_request(Conn *c, Request *r) {
    size_t delimiter = find_bytes(c->input, c->input_len, "\r\n\r\n", 4);
    if (delimiter == SIZE_MAX) {
        return c->input_len > HEADER_LIMIT ? -1 : 0;
    }
    size_t first_line = find_bytes(c->input, delimiter, "\r\n", 2);
    if (first_line == SIZE_MAX) return -1;
    const char *line = c->input;
    const char *line_end = c->input + first_line;
    const char *s1 = memchr(line, ' ', (size_t)(line_end - line));
    if (!s1) return -1;
    const char *s2 = memchr(s1 + 1, ' ', (size_t)(line_end - s1 - 1));
    if (!s2) return -1;
    size_t method_len = (size_t)(s1 - line);
    size_t path_len = (size_t)(s2 - s1 - 1);
    size_t version_len = (size_t)(line_end - s2 - 1);
    if (method_len == 0 || method_len >= sizeof(r->method) ||
        path_len == 0 || path_len >= sizeof(r->path) ||
        version_len == 0 || version_len >= sizeof(r->version)) return -1;
    memcpy(r->method, line, method_len);
    r->method[method_len] = '\0';
    memcpy(r->path, s1 + 1, path_len);
    r->path[path_len] = '\0';
    memcpy(r->version, s2 + 1, version_len);
    r->version[version_len] = '\0';
    if (strcmp(r->version, "HTTP/1.1") != 0 && strcmp(r->version, "HTTP/1.0") != 0) return -1;
    r->body = NULL;
    r->body_len = 0;
    r->authorization = NULL;
    r->authorization_len = 0;
    r->close = strcmp(r->version, "HTTP/1.0") == 0;
    size_t content_len = 0;
    bool has_content_len = false;
    size_t pos = first_line + 2;
    while (pos < delimiter + 2) {
        size_t e = find_bytes(c->input + pos, delimiter + 2 - pos, "\r\n", 2);
        if (e == SIZE_MAX) return -1;
        e += pos;
        const char *colon = memchr(c->input + pos, ':', e - pos);
        if (!colon) return -1;
        size_t key_len = (size_t)(colon - (c->input + pos));
        const char *v = colon + 1;
        const char *ve = c->input + e;
        while (v < ve && (*v == ' ' || *v == '\t')) ++v;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) --ve;
        size_t value_len = (size_t)(ve - v);
        if (key_len == 14 && strncasecmp(c->input + pos, "Content-Length", 14) == 0) {
            if (value_len == 0 || value_len > 20) return -1;
            char temp[24];
            memcpy(temp, v, value_len);
            temp[value_len] = '\0';
            char *endp;
            unsigned long long parsed = strtoull(temp, &endp, 10);
            if (*endp != '\0' || parsed > MAX_REQUEST_BODY) return -1;
            content_len = (size_t)parsed;
            has_content_len = true;
        } else if (key_len == 13 && strncasecmp(c->input + pos, "Authorization", 13) == 0) {
            r->authorization = v;
            r->authorization_len = value_len;
        } else if (key_len == 10 && strncasecmp(c->input + pos, "Connection", 10) == 0) {
            if (value_len == 5 && strncasecmp(v, "close", 5) == 0) r->close = true;
            else if (value_len == 10 && strncasecmp(v, "keep-alive", 10) == 0) r->close = false;
        }
        pos = e + 2;
    }
    if (!has_content_len) content_len = 0;
    size_t head_len = delimiter + 4;
    if (content_len > SIZE_MAX - head_len) return -1;
    size_t total = head_len + content_len;
    if (c->input_len < total) return 0;
    r->body = c->input + head_len;
    r->body_len = content_len;
    c->request_consume = total;
    return 1;
}

static void response_start(Conn *c, int status, const char *reason,
                           bool close_conn, Writer *body) {
#ifdef RING_DIAG
    ring_diag_output_unaccount(c);
#endif
    response_release(c->output, c->output_cap);
    c->output = NULL;
    c->output_cap = 0U;
    c->output = response_acquire(RESPONSE_INITIAL_CAP, &c->output_cap);
    body->data = c->output ? c->output + 256 : NULL;
    body->cap = c->output ? c->output_cap - 256U : 0U;
    body->len = 0;
    body->failed = c->output == NULL;
    body->owner = c;
    c->close_after_write = close_conn;
    c->output_len = 0;
    c->output_pos = 0;
    (void)status;
    (void)reason;
}

static int response_finish(Conn *c, int status, const char *reason, Writer *body) {
    if (body->failed || !c->output) return -1;
    int n = snprintf(c->output, 256,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                     status, reason, body->len, c->close_after_write ? "close" : "keep-alive");
    if (n < 0 || n >= 256) return -1;
    size_t header_start = 256U - (size_t)n;
    memmove(c->output + header_start, c->output, (size_t)n);
    c->output_pos = header_start;
    c->output_len = 256U + body->len;
#ifdef RING_DIAG
    ring_diag_output_account(c);
#endif
    return 0;
}

static void send_error(Conn *c, int status, const char *reason, const char *message, bool close_conn) {
    Writer w;
    response_start(c, status, reason, close_conn, &w);
    writer_str(&w, "{\"error\":");
    writer_json_cstr(&w, message);
    writer_char(&w, '}');
    if (response_finish(c, status, reason, &w) != 0) c->close_after_write = true;
}

static int parse_post_id(const char *s, sqlite3_int64 *id) {
    if (!s || *s < '1' || *s > '9') return -1;
    uint64_t v = 0;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10U + (uint64_t)(*p - '0');
        if (v > (uint64_t)LLONG_MAX) return -1;
    }
    *id = (sqlite3_int64)v;
    return 0;
}

static void reset_stmt(sqlite3_stmt *s) {
    (void)sqlite3_reset(s);
}

static int append_post_from_stmt(Writer *w, sqlite3_stmt *stmt) {
    sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
    const unsigned char *body = sqlite3_column_text(stmt, 1);
    int body_len = sqlite3_column_bytes(stmt, 1);
    const unsigned char *created = sqlite3_column_text(stmt, 2);
    const unsigned char *author = sqlite3_column_text(stmt, 3);
    int author_len = sqlite3_column_bytes(stmt, 3);
    sqlite3_int64 likes = sqlite3_column_int64(stmt, 4);
    if (!body || !created || !author) return -1;
    writer_post(w, id, body, body_len, created, author, author_len, likes);
    return w->failed ? -1 : 0;
}

#include "btree_feed.h"

/* Unicode whitespace accepted at the edges of a post body. */
static size_t utf8_whitespace_length(const unsigned char *s, size_t n) {
    if (!n) return 0;
    if (s[0] == ' ' || (s[0] >= 0x09 && s[0] <= 0x0D)) return 1;
    if (n >= 2 && s[0] == 0xC2 && (s[1] == 0x85 || s[1] == 0xA0)) return 2;
    if (n < 3) return 0;
    if (s[0] == 0xE1 && s[1] == 0x9A && s[2] == 0x80) return 3;
    if (s[0] == 0xE2 && s[1] == 0x80 &&
        ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 ||
         s[2] == 0xA9 || s[2] == 0xAF)) return 3;
    if (s[0] == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) return 3;
    if (s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) return 3;
    if (s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) return 3;
    return 0;
}

static int utf8_char_count(const char *s, size_t n) {
    int count = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

static int finish_read_batch(const char *context) {
    if (!g_batch_read_txn) return 0;
    int commit_rc = sqlite3BtreeCommit(g_feed_btree.tree);
    if (commit_rc != SQLITE_OK) {
        int rollback_rc = sqlite3BtreeRollback(g_feed_btree.tree, SQLITE_ABORT, 0);
        if (g_feed_btree.tree->inTrans != TRANS_NONE) {
            fprintf(stderr,
                    "%s rollback failed (%d after commit %d); stopping server\n",
                    context, rollback_rc, commit_rc);
            fflush(stderr);
            _exit(1);
        }
        g_batch_read_txn = false;
        fprintf(stderr, "%s commit failed: %d\n", context, commit_rc);
        return -1;
    }
    g_batch_read_txn = false;
    return 0;
}

static int begin_write_batch(void) {
    if (!g_ring_active || g_batch_write_txn) return 0;
    if (finish_read_batch("read batch before write") != 0) return -1;
    int rc = sqlite3_exec(g_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "write batch begin failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    g_batch_write_txn = true;
    return 0;
}

static int route_request(Conn *c, const Request *r) {
    Writer w;
    Auth auth;
    int auth_status;
    if (strcmp(r->method, "GET") == 0 && strcmp(r->path, "/health") == 0) {
        int rc = sqlite3_step(g_health_stmt);
        bool ok = rc == SQLITE_ROW;
        reset_stmt(g_health_stmt);
        if (ok) {
            response_start(c, 200, "OK", r->close, &w);
            writer_str(&w, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            writer_i64(&w, (int64_t)(time(NULL) - g_started));
            writer_char(&w, '}');
            if (response_finish(c, 200, "OK", &w) != 0) return -1;
        } else {
            const char *message = sqlite3_errmsg(g_db);
            response_start(c, 503, "Service Unavailable", r->close, &w);
            writer_str(&w, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
            writer_json_cstr(&w, message ? message : "database unreachable");
            writer_char(&w, '}');
            if (response_finish(c, 503, "Service Unavailable", &w) != 0) return -1;
        }
        return 0;
    }
    if (strcmp(r->method, "GET") == 0 && strcmp(r->path, "/feed") == 0) {
        response_start(c, 200, "OK", r->close, &w);
        bool fast_feed = false;
        bool own_feed_txn = g_feed_btree.enabled &&
                            !g_batch_write_txn && !g_batch_read_txn;
        if (g_feed_btree.enabled && feed_btree_open() == 0) {
            size_t mark = w.len;
            if (feed_btree_render(&w) == 0 && !w.failed) {
                fast_feed = true;
                ++g_feed_btree.successes;
            }
            feed_btree_close();
            if (!fast_feed) {
                w.len = mark;
                w.failed = false;
            }
        }

        int query_rc = SQLITE_DONE;
        if (!fast_feed) {
            if (g_feed_btree.enabled && ++g_feed_btree.fallbacks == 1)
                fprintf(stderr, "SQLite B-tree feed read fell back to SQL\n");
            writer_str(&w, "{\"posts\":[");
            reset_stmt(g_feed_stmt);
            bool first = true;
            while ((query_rc = sqlite3_step(g_feed_stmt)) == SQLITE_ROW) {
                if (!first) writer_char(&w, ',');
                if (append_post_from_stmt(&w, g_feed_stmt) != 0) break;
                first = false;
            }
            reset_stmt(g_feed_stmt);
            writer_str(&w, "]}");
        }

        if (own_feed_txn && !g_ring_active &&
            finish_read_batch("feed read") != 0) return -1;
        if (query_rc != SQLITE_DONE || response_finish(c, 200, "OK", &w) != 0) return -1;
        return 0;
    }
    if (strncmp(r->path, "/posts/", 7) == 0 &&
        strcmp(r->method, "GET") == 0 && strchr(r->path + 7, '/') == NULL) {
        sqlite3_int64 id;
        if (parse_post_id(r->path + 7, &id) != 0) {
            send_error(c, 400, "Bad Request", "invalid post id", r->close);
            return 0;
        }
        int btree_rc = -1;
        FeedBtreeRow btree_row;
        bool own_post_txn = g_feed_btree.enabled &&
                            !g_batch_write_txn && !g_batch_read_txn;
        if (g_feed_btree.enabled && feed_btree_open_post() == 0) {
            btree_rc = feed_btree_read_post(id, &btree_row);
            feed_btree_close();
            if (btree_rc == 0) ++g_feed_btree.successes;
        }
        if (own_post_txn && !g_ring_active &&
            finish_read_batch("post read") != 0) return -1;
        if (btree_rc == 1) {
            send_error(c, 404, "Not Found", "post not found", r->close);
            return 0;
        }
        if (btree_rc == 0) {
            response_start(c, 200, "OK", r->close, &w);
            writer_str(&w, "{\"post\":");
            writer_post(&w, btree_row.id, btree_row.body, (int)btree_row.body_len,
                        btree_row.created, btree_row.author,
                        (int)btree_row.author_len, btree_row.like_count);
            writer_char(&w, '}');
            if (response_finish(c, 200, "OK", &w) != 0) return -1;
            return 0;
        }
        reset_stmt(g_post_stmt);
        sqlite3_bind_int64(g_post_stmt, 1, id);
        int rc = sqlite3_step(g_post_stmt);
        if (rc == SQLITE_DONE) {
            reset_stmt(g_post_stmt);
            send_error(c, 404, "Not Found", "post not found", r->close);
            return 0;
        }
        if (rc != SQLITE_ROW) {
            reset_stmt(g_post_stmt);
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        response_start(c, 200, "OK", r->close, &w);
        writer_str(&w, "{\"post\":");
        if (append_post_from_stmt(&w, g_post_stmt) != 0) {
            reset_stmt(g_post_stmt);
            return -1;
        }
        writer_char(&w, '}');
        reset_stmt(g_post_stmt);
        if (response_finish(c, 200, "OK", &w) != 0) return -1;
        return 0;
    }
    if (strcmp(r->path, "/posts") == 0 && strcmp(r->method, "POST") == 0) {
        auth_status = verify_auth(r->authorization, r->authorization_len, &auth);
        if (auth_status == AUTH_MISSING) {
            send_error(c, 401, "Unauthorized", "missing bearer token", r->close);
            return 0;
        }
        if (auth_status == AUTH_INVALID) {
            send_error(c, 401, "Unauthorized", "invalid or expired token", r->close);
            return 0;
        }
        if (auth_status == AUTH_PAYLOAD) {
            send_error(c, 401, "Unauthorized", "invalid token payload", r->close);
            return 0;
        }
        JsonObject obj;
        if (parse_post_json(r->body ? r->body : "", r->body_len, &obj) != 0) {
            send_error(c, 400, "Bad Request", "malformed JSON body", r->close);
            return 0;
        }
        char body[MAX_REQUEST_BODY + 1U];
        size_t body_len;
        if (json_string_field(&obj, "body", body, sizeof(body), &body_len) != 0) {
            send_error(c, 400, "Bad Request", "body is required", r->close);
            return 0;
        }
        size_t begin = 0, end = body_len, space_len;
        while (begin < end &&
               (space_len = utf8_whitespace_length((const unsigned char *)body + begin,
                                                   end - begin)) != 0)
            begin += space_len;
        while (end > begin) {
            size_t start = end - 1;
            while (start > begin && ((unsigned char)body[start] & 0xC0U) == 0x80U) --start;
            space_len = utf8_whitespace_length((const unsigned char *)body + start, end - start);
            if (space_len != end - start) break;
            end = start;
        }
        size_t trimmed_len = end - begin;
        if (trimmed_len == 0) {
            send_error(c, 400, "Bad Request", "body is required", r->close);
            return 0;
        }
        if (utf8_char_count(body + begin, trimmed_len) > 500) {
            send_error(c, 400, "Bad Request", "body must be at most 500 characters", r->close);
            return 0;
        }
        if (begin_write_batch() != 0) {
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        c->batch_sensitive = g_ring_active;
        reset_stmt(g_insert_stmt);
        sqlite3_bind_int64(g_insert_stmt, 1, auth.user_id);
        sqlite3_bind_text(g_insert_stmt, 2, body + begin, (int)trimmed_len, SQLITE_TRANSIENT);
        int rc = sqlite3_step(g_insert_stmt);
        if (rc != SQLITE_ROW) {
            reset_stmt(g_insert_stmt);
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        sqlite3_int64 new_id = sqlite3_column_int64(g_insert_stmt, 0);
        const unsigned char *ct = sqlite3_column_text(g_insert_stmt, 1);
        char created[40];
        if (!ct || strlen((const char *)ct) >= sizeof(created)) {
            reset_stmt(g_insert_stmt);
            return -1;
        }
        memcpy(created, ct, strlen((const char *)ct) + 1);
        rc = sqlite3_step(g_insert_stmt);
        reset_stmt(g_insert_stmt);
        if (rc != SQLITE_DONE) {
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        response_start(c, 201, "Created", r->close, &w);
        writer_str(&w, "{\"post\":");
        writer_str(&w, "{\"id\":");
        writer_i64(&w, (int64_t)new_id);
        writer_str(&w, ",\"body\":");
        writer_json_string(&w, body + begin, trimmed_len);
        writer_str(&w, ",\"created_at\":");
        writer_json_cstr(&w, created);
        writer_str(&w, ",\"author\":");
        writer_json_string(&w, auth.username, auth.username_len);
        writer_str(&w, ",\"like_count\":0}}");
        if (response_finish(c, 201, "Created", &w) != 0) return -1;
        return 0;
    }
    const char *like_suffix = "/like";
    size_t path_len = strlen(r->path);
    if (strcmp(r->method, "POST") == 0 && path_len >= 13 &&
        strncmp(r->path, "/posts/", 7) == 0 &&
        strcmp(r->path + path_len - 5, like_suffix) == 0) {
        auth_status = verify_auth(r->authorization, r->authorization_len, &auth);
        if (auth_status == AUTH_MISSING) {
            send_error(c, 401, "Unauthorized", "missing bearer token", r->close);
            return 0;
        }
        if (auth_status == AUTH_INVALID) {
            send_error(c, 401, "Unauthorized", "invalid or expired token", r->close);
            return 0;
        }
        if (auth_status == AUTH_PAYLOAD) {
            send_error(c, 401, "Unauthorized", "invalid token payload", r->close);
            return 0;
        }
        size_t id_len = path_len - 7 - 5;
        if (id_len == 0 || id_len >= 32) {
            send_error(c, 400, "Bad Request", "invalid post id", r->close);
            return 0;
        }
        char id_s[32];
        memcpy(id_s, r->path + 7, id_len);
        id_s[id_len] = '\0';
        sqlite3_int64 id;
        if (parse_post_id(id_s, &id) != 0) {
            send_error(c, 400, "Bad Request", "invalid post id", r->close);
            return 0;
        }
        reset_stmt(g_post_exists_stmt);
        sqlite3_bind_int64(g_post_exists_stmt, 1, id);
        int rc = sqlite3_step(g_post_exists_stmt);
        reset_stmt(g_post_exists_stmt);
        if (rc == SQLITE_DONE) {
            send_error(c, 404, "Not Found", "post not found", r->close);
            return 0;
        }
        if (rc != SQLITE_ROW) {
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        if (begin_write_batch() != 0) {
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        c->batch_sensitive = g_ring_active;
        reset_stmt(g_insert_like_stmt);
        sqlite3_bind_int64(g_insert_like_stmt, 1, auth.user_id);
        sqlite3_bind_int64(g_insert_like_stmt, 2, id);
        rc = sqlite3_step(g_insert_like_stmt);
        int changed = sqlite3_changes(g_db);
        reset_stmt(g_insert_like_stmt);
        if (rc != SQLITE_DONE) {
            send_error(c, 500, "Internal Server Error", "internal server error", r->close);
            return 0;
        }
        int status = changed ? 201 : 200;
        response_start(c, status, changed ? "Created" : "OK", r->close, &w);
        writer_str(&w, "{\"liked\":true,\"already_liked\":");
        writer_str(&w, changed ? "false" : "true");
        writer_str(&w, ",\"post_id\":");
        writer_i64(&w, (int64_t)id);
        writer_char(&w, '}');
        if (response_finish(c, status, changed ? "Created" : "OK", &w) != 0) return -1;
        return 0;
    }
    send_error(c, 404, "Not Found", "not found", r->close);
    return 0;
}

static int prepare(sqlite3 *db, sqlite3_stmt **stmt, const char *sql) {
    int rc = sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static uint64_t wal_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#ifdef RING_WAL_FADVISE_TEST
#define WAL_ADVISE_MIN_BYTES (16ULL * 1024ULL * 1024ULL)
#define WAL_ADVISE_KEEP_BYTES (32ULL * 1024ULL * 1024ULL)
#define WAL_ADVISE_START_BYTES (64ULL * 1024ULL * 1024ULL)
#define WAL_ADVISE_MIN_INTERVAL_NS 5000000000ULL

static void wal_advise_checkpointed_pages(sqlite3 *db, int log_frames,
                                          int checkpointed_frames) {
    static int wal_fd = -1;
    static uint64_t last_check_ns = 0;
    static uint64_t last_advice_ns = 0;

    long os_page_size = sysconf(_SC_PAGESIZE);
    if (os_page_size <= 0 || log_frames <= 0 ||
        checkpointed_frames != log_frames) {
        return;
    }

    uint64_t now_ns = wal_monotonic_ns();
    if (last_check_ns != 0 && now_ns >= last_check_ns &&
        now_ns - last_check_ns < WAL_ADVISE_MIN_INTERVAL_NS) {
        return;
    }
    last_check_ns = now_ns;

    const char *db_path = sqlite3_db_filename(db, "main");
    if (!db_path || !*db_path) return;
    char wal_path[PATH_MAX];
    int path_len = snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    if (path_len < 0 || (size_t)path_len >= sizeof(wal_path)) return;

    struct stat path_stat;
    if (stat(wal_path, &path_stat) != 0) return;
    if (wal_fd >= 0) {
        struct stat fd_stat;
        if (fstat(wal_fd, &fd_stat) != 0 || fd_stat.st_dev != path_stat.st_dev ||
            fd_stat.st_ino != path_stat.st_ino) {
            close(wal_fd);
            wal_fd = -1;
        }
    }
    if (wal_fd < 0) {
        wal_fd = open(wal_path, O_RDONLY | O_CLOEXEC);
        if (wal_fd < 0) return;
        struct stat fd_stat;
        if (fstat(wal_fd, &fd_stat) != 0) {
            close(wal_fd);
            wal_fd = -1;
            return;
        }
        fprintf(stderr,
                "SQLite WAL cache advice active (OS page=%ld)\n", os_page_size);
    }

    struct stat wal_stat;
    if (fstat(wal_fd, &wal_stat) != 0 || wal_stat.st_size <= 0) return;
    uint64_t file_size = (uint64_t)wal_stat.st_size;
    uint64_t start = WAL_ADVISE_START_BYTES;
    uint64_t end = file_size > WAL_ADVISE_KEEP_BYTES
                       ? file_size - WAL_ADVISE_KEEP_BYTES
                       : 0;
    end -= end % (uint64_t)os_page_size;
    if (end <= start || end - start < WAL_ADVISE_MIN_BYTES) return;
    if (last_advice_ns != 0 && now_ns >= last_advice_ns &&
        now_ns - last_advice_ns < WAL_ADVISE_MIN_INTERVAL_NS) return;

    off_t advice_start = (off_t)start;
    off_t advice_len = (off_t)(end - start);
    int rc = posix_fadvise(wal_fd, advice_start, advice_len,
                           POSIX_FADV_DONTNEED);
    last_advice_ns = now_ns;
    if (rc != 0) {
        fprintf(stderr, "SQLite WAL cache advice failed: %s\n", strerror(rc));
        return;
    }
    fprintf(stderr,
            "SQLite WAL cache advice complete=%d/%d file=%llu range=%llu+%llu\n",
            checkpointed_frames, log_frames, (unsigned long long)file_size,
            (unsigned long long)start, (unsigned long long)(end - start));
}
#endif

static void wal_checkpoint_wait_for_work(void) {
    if (atomic_load_explicit(&g_wal_checkpoint_sem_ready,
                             memory_order_acquire) != 0) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
            deadline.tv_nsec += WAL_CHECKPOINT_IDLE_NS;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += deadline.tv_nsec / 1000000000L;
                deadline.tv_nsec %= 1000000000L;
            }
            while (sem_timedwait(&g_wal_checkpoint_sem, &deadline) != 0 &&
                   errno == EINTR) {
            }
            atomic_store_explicit(&g_wal_checkpoint_wakeup_pending, 0,
                                  memory_order_release);
            return;
        }
    }

    struct timespec delay = {.tv_sec = 0, .tv_nsec = WAL_CHECKPOINT_IDLE_NS};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
    atomic_store_explicit(&g_wal_checkpoint_wakeup_pending, 0,
                          memory_order_release);
}

static int wal_commit_hook(void *unused, sqlite3 *db, const char *database,
                           int log_frames) {
    (void)unused;
    (void)db;
    (void)database;
    if (log_frames < WAL_HOLD_THRESHOLD_FRAMES ||
        atomic_load_explicit(&g_wal_checkpoint_sem_ready,
                             memory_order_acquire) == 0) {
        return SQLITE_OK;
    }
    if (atomic_exchange_explicit(&g_wal_checkpoint_wakeup_pending, 1,
                                 memory_order_acq_rel) == 0 &&
        sem_post(&g_wal_checkpoint_sem) != 0) {
        atomic_store_explicit(&g_wal_checkpoint_wakeup_pending, 0,
                              memory_order_release);
    }
    return SQLITE_OK;
}

static bool wal_checkpoint_needed(int log_frames, int checkpointed_frames) {
    return log_frames >= WAL_HOLD_THRESHOLD_FRAMES &&
           checkpointed_frames >= 0 && checkpointed_frames < log_frames;
}

static void sqlite_set_database_chunk_size(sqlite3 *db) {
    int chunk_size = WAL_DATABASE_CHUNK_SIZE;
    int rc = sqlite3_file_control(db, "main", SQLITE_FCNTL_CHUNK_SIZE,
                                  &chunk_size);
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "SQLite database chunk-size hint unavailable: %s (%d)\n",
                sqlite3_errmsg(db), rc);
    }
}

static int wal_checkpoint_passive(sqlite3 *db, int *log_frames,
                                 int *checkpointed_frames,
                                 int *checkpoint_rc) {
    int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                       log_frames, checkpointed_frames);
    if (checkpoint_rc) *checkpoint_rc = rc;
    if (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return 0;
    return rc;
}

static void *wal_checkpoint_main(void *arg) {
    sqlite3 *db = (sqlite3 *)arg;
    if (setpriority(PRIO_PROCESS, 0, 5) != 0) {
        fprintf(stderr, "could not set SQLite checkpoint thread priority: %s\n",
                strerror(errno));
    }

    sqlite3_stmt *probe = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1", -1, &probe, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(probe);
    if (probe) sqlite3_finalize(probe);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        fprintf(stderr, "SQLite checkpoint connection probe failed: %s\n",
                sqlite3_errmsg(db));
        wal_set_write_hold(0);
        atomic_store_explicit(&g_wal_checkpoint_failed, 1, memory_order_release);
        sqlite3_close(db);
        return NULL;
    }

    for (;;) {
        wal_checkpoint_wait_for_work();

        int log_frames = 0;
        int checkpointed_frames = 0;
        int checkpoint_rc = SQLITE_ERROR;
        rc = wal_checkpoint_passive(db, &log_frames, &checkpointed_frames,
                                    &checkpoint_rc);
        if (rc != 0) {
            fprintf(stderr, "SQLite passive checkpoint failed: %s\n",
                    sqlite3_errmsg(db));
            wal_set_write_hold(0);
            atomic_store_explicit(&g_wal_checkpoint_failed, 1,
                                  memory_order_release);
            sqlite3_close(db);
            return NULL;
        }

#ifdef RING_WAL_FADVISE_TEST
        if (checkpoint_rc == SQLITE_OK) {
            wal_advise_checkpointed_pages(db, log_frames, checkpointed_frames);
        }
#endif

        if (!wal_checkpoint_needed(log_frames, checkpointed_frames)) continue;

        /* First pass runs without pausing writes; hold only for the tail catch-up. */
        wal_set_write_hold(1);
        uint64_t hold_started_ns = wal_monotonic_ns();
        for (unsigned attempt = 0; attempt < WAL_HOLD_RETRY_LIMIT; ++attempt) {
            uint64_t now_ns = wal_monotonic_ns();
            if (now_ns != 0U && hold_started_ns != 0U &&
                now_ns - hold_started_ns >= WAL_HOLD_MAX_NS) break;

            struct timespec delay = {.tv_sec = 0, .tv_nsec = WAL_HOLD_RETRY_NS};
            while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
            }

            checkpoint_rc = SQLITE_ERROR;
            rc = wal_checkpoint_passive(db, &log_frames, &checkpointed_frames,
                                        &checkpoint_rc);
            if (rc != 0) {
                fprintf(stderr, "SQLite tail checkpoint failed: %s\n",
                        sqlite3_errmsg(db));
                wal_set_write_hold(0);
                atomic_store_explicit(&g_wal_checkpoint_failed, 1,
                                      memory_order_release);
                sqlite3_close(db);
                return NULL;
            }

#ifdef RING_WAL_FADVISE_TEST
            if (checkpoint_rc == SQLITE_OK) {
                wal_advise_checkpointed_pages(db, log_frames,
                                              checkpointed_frames);
            }
#endif
            if (!wal_checkpoint_needed(log_frames, checkpointed_frames)) break;
        }
        wal_set_write_hold(0);
    }
}

static int start_wal_checkpoint_thread(void) {
    int rc = sqlite3_wal_autocheckpoint(g_db, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "could not disable SQLite auto-checkpoint: %s\n",
                sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3 *checkpoint_db = NULL;
    rc = sqlite3_open_v2(g_db_path, &checkpoint_db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                         CHALLENGE_WAL_VFS_NAME);
    if (rc == SQLITE_OK) {
        sqlite3_busy_timeout(checkpoint_db, 0);
        char *err = NULL;
        const char *pragmas =
            "PRAGMA synchronous=NORMAL;"
            "PRAGMA wal_autocheckpoint=0;"
            "PRAGMA cache_size=-1024;"
            "PRAGMA temp_store=MEMORY;"
            "PRAGMA mmap_size=0;";
        rc = sqlite3_exec(checkpoint_db, pragmas, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "SQLite checkpoint pragmas failed: %s\n",
                    err ? err : sqlite3_errmsg(checkpoint_db));
        }
        sqlite3_free(err);
        if (rc == SQLITE_OK) sqlite_set_database_chunk_size(checkpoint_db);
    }
    if (rc != SQLITE_OK) {
        if (checkpoint_db) sqlite3_close(checkpoint_db);
        fprintf(stderr, "could not open SQLite checkpoint connection: %s\n",
                sqlite3_errmsg(g_db));
        (void)sqlite3_wal_autocheckpoint(g_db, 1000);
        return -1;
    }

    if (sem_init(&g_wal_checkpoint_sem, 0, 0) == 0) {
        atomic_store_explicit(&g_wal_checkpoint_sem_ready, 1,
                              memory_order_release);
        (void)sqlite3_wal_hook(g_db, wal_commit_hook, NULL);
    } else {
        fprintf(stderr, "SQLite checkpoint wake semaphore unavailable: %s\n",
                strerror(errno));
    }

    int thread_rc = pthread_create(&g_wal_checkpoint_thread, NULL,
                                   wal_checkpoint_main, checkpoint_db);
    if (thread_rc != 0) {
        fprintf(stderr, "could not start SQLite checkpoint thread: %s\n",
                strerror(thread_rc));
        if (atomic_exchange_explicit(&g_wal_checkpoint_sem_ready, 0,
                                     memory_order_acq_rel) != 0) {
            (void)sem_destroy(&g_wal_checkpoint_sem);
        }
        sqlite3_close(checkpoint_db);
        (void)sqlite3_wal_autocheckpoint(g_db, 1000);
        return -1;
    }
    (void)pthread_detach(g_wal_checkpoint_thread);
    fprintf(stderr,
            "SQLite catch-up checkpoint thread started (%u frames, %llu ms max hold, nice 5, %d MiB DB chunks)\n",
            WAL_HOLD_THRESHOLD_FRAMES,
            (unsigned long long)(WAL_HOLD_MAX_NS / 1000000ULL),
            WAL_DATABASE_CHUNK_SIZE / (1024 * 1024));
    return 0;
}

static void wal_checkpoint_fallback_if_needed(void) {
    if (atomic_exchange_explicit(&g_wal_checkpoint_failed, 0,
                                 memory_order_acq_rel) == 0) {
        return;
    }
    wal_set_write_hold(0);
    int rc = sqlite3_wal_autocheckpoint(g_db, 1000);
    fprintf(stderr, "SQLite checkpoint thread stopped; auto-checkpoint fallback %s\n",
            rc == SQLITE_OK ? "enabled" : "failed");
}

static int init_db(void) {
    if (sqlite3_initialize() != SQLITE_OK) {
        fprintf(stderr, "sqlite initialize failed\n");
        return -1;
    }
    int vfs_rc = challenge_wal_vfs_install();
    if (vfs_rc != SQLITE_OK) {
        fprintf(stderr, "SQLite WAL VFS install failed: %s\n",
                sqlite3_errstr(vfs_rc));
        return -1;
    }
    const char *path = getenv("SQLITE_PATH");
    if (!path || !*path) path = "seed/feed.db";
    g_db_path = path;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path, &g_db, flags, CHALLENGE_WAL_VFS_NAME) != SQLITE_OK) {
        fprintf(stderr, "sqlite open failed: %s\n", g_db ? sqlite3_errmsg(g_db) : "out of memory");
        return -1;
    }
    sqlite3_busy_timeout(g_db, 5000);
    char *err = NULL;
    const char *pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA mmap_size=0;"
        "PRAGMA cache_size=-65536;"
        "PRAGMA temp_store=MEMORY;";
    if (sqlite3_exec(g_db, pragmas, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite pragmas failed: %s\n", err ? err : sqlite3_errmsg(g_db));
        return -1;
    }
    sqlite_set_database_chunk_size(g_db);
    if (prepare(g_db, &g_feed_stmt,
        "SELECT p.id,p.body,p.created_at,u.username,"
        "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
        "FROM posts p JOIN users u ON u.id=p.user_id "
        "ORDER BY p.created_at DESC,p.id DESC LIMIT 20") != 0) return -1;
    if (prepare(g_db, &g_post_stmt,
        "SELECT p.id,p.body,p.created_at,u.username,"
        "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
        "FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?") != 0) return -1;
    if (prepare(g_db, &g_insert_stmt,
        "INSERT INTO posts (user_id,body) VALUES (?,?) RETURNING id,created_at") != 0) return -1;
    if (prepare(g_db, &g_post_exists_stmt,
        "SELECT 1 FROM posts WHERE id=?") != 0) return -1;
    if (prepare(g_db, &g_health_stmt, "SELECT 1") != 0) return -1;
    if (prepare(g_db, &g_insert_like_stmt,
        "INSERT INTO likes (user_id,post_id) VALUES (?,?) "
        "ON CONFLICT(user_id,post_id) DO NOTHING") != 0) return -1;
    (void)feed_btree_init(g_db);
    return 0;
}

static int make_listener(const char *host, const char *port, bool nonblocking,
                         bool reuseport) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *it = result; it; it = it->ai_next) {
        int type = it->ai_socktype | SOCK_CLOEXEC;
        if (nonblocking) type |= SOCK_NONBLOCK;
        fd = socket(it->ai_family, type, it->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (reuseport &&
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        /* Accepted sockets use the kernel's default TCP buffer autotuning. */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0 && listen(fd, 65535) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

static int make_listener_pool(const char *host, const char *port,
                              unsigned count, int *listeners) {
    unsigned made = 0;
    for (; made < count; ++made) {
        listeners[made] = make_listener(host, port, true, true);
        if (listeners[made] >= 0) continue;
        while (made != 0U) close(listeners[--made]);
        return -1;
    }
    return 0;
}

static void conn_release_memory(Conn *c) {
    if (!c) return;
#ifdef RING_DIAG
    if (c->input && !c->input_shared)
        ring_diag_input_resize(c->input_cap, 0U);
    if (c->diag_send_outstanding) ++g_ring_memory_diag.close_with_send;
    if (c->wal_write_deferred) ++g_ring_memory_diag.close_in_wal;
    ring_diag_output_unaccount(c);
#endif
    free(c->input);
    free(c->pending_input);
    response_release(c->output, c->output_cap);
    free(c);
}

static void conn_close_socket(Conn *c) {
    if (!c || c->fd_closed) return;
    if (c->direct_file) {
        ring_release_direct(c);
    } else {
        close(c->fd);
    }
    c->fd_closed = true;
}

static void close_conn(Conn *c) {
    if (!c) return;
    if (g_epoll >= 0) epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, NULL);
    conn_close_socket(c);
    conn_release_memory(c);
}

static int set_conn_events(Conn *c, bool writable) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (writable) ev.events |= EPOLLOUT;
    ev.data.ptr = c;
    return epoll_ctl(g_epoll, EPOLL_CTL_MOD, c->fd, &ev);
}

static bool request_is_write(const Request *r) {
    if (strcmp(r->method, "POST") != 0) return false;
    if (strcmp(r->path, "/posts") == 0) return true;
    size_t path_len = strlen(r->path);
    return path_len >= 13U && strncmp(r->path, "/posts/", 7) == 0 &&
           strcmp(r->path + path_len - 5U, "/like") == 0;
}

#ifdef RING_DIAG
static unsigned ring_diag_route(const Request *r) {
    if (strcmp(r->method, "GET") == 0 && strcmp(r->path, "/feed") == 0)
        return 0U;
    if (strcmp(r->method, "GET") == 0 &&
        strncmp(r->path, "/posts/", 7U) == 0)
        return 1U;
    if (strcmp(r->method, "POST") == 0 && strcmp(r->path, "/posts") == 0)
        return 2U;
    size_t path_len = strlen(r->path);
    if (strcmp(r->method, "POST") == 0 && path_len >= 13U &&
        strncmp(r->path, "/posts/", 7U) == 0 &&
        strcmp(r->path + path_len - 5U, "/like") == 0)
        return 3U;
    return 4U;
}
#endif

static int process_buffer(Conn *c) {
    Request req;
    memset(&req, 0, sizeof(req));
    int parsed = parse_request(c, &req);
    if (parsed == 0) return 0;
    if (parsed < 0) {
        c->request_consume = c->input_len;
        send_error(c, 400, "Bad Request", "malformed request", true);
#ifdef RING_DIAG
        ring_diag_mark_response_ready(c, 4U);
#endif
        return 1;
    }
    if (c->peer_eof && c->pending_input_len == 0U &&
        c->request_consume >= c->input_len)
        req.close = true;
    if (g_ring_active && request_is_write(&req) &&
        atomic_load_explicit(&g_wal_write_hold, memory_order_acquire) != 0) {
        if (c->input_shared && ring_copy_shared_input(c) != 0) return -1;
        c->close_after_write = req.close;
        c->batch_sensitive = false;
        ring_defer_wal_write(c);
        return 2;
    }
    c->batch_sensitive = g_ring_active && g_batch_write_txn;
    c->close_after_write = req.close;
    if (route_request(c, &req) != 0) {
        send_error(c, 500, "Internal Server Error", "internal server error", true);
    }
    c->batch_sensitive = g_ring_active && (g_batch_write_txn || g_batch_read_txn);
    if (c->output_len == 0) return -1;
#ifdef RING_DIAG
    ring_diag_mark_response_ready(c, ring_diag_route(&req));
#endif
    return 1;
}

static void finish_response(Conn *c) {
    if (c->request_consume > c->input_len) c->request_consume = c->input_len;
    c->input_len -= c->request_consume;
    if (c->input_len) {
        memmove(c->input, c->input + c->request_consume, c->input_len);
    } else if (c->input) {
#ifdef RING_DIAG
        if (!c->input_shared) ring_diag_input_resize(c->input_cap, 0U);
#endif
        free(c->input);
        c->input = NULL;
        c->input_cap = 0;
        c->input_shared = false;
    }
    c->request_consume = 0;
    c->output_len = 0;
    c->output_pos = 0;
#ifdef RING_DIAG
    ring_diag_output_unaccount(c);
#endif
    response_release(c->output, c->output_cap);
    c->output = NULL;
    c->output_cap = 0U;
}

static int flush_conn(Conn *c) {
    while (c->output_pos < c->output_len) {
        ssize_t n = send(c->fd, c->output + c->output_pos,
                         c->output_len - c->output_pos, MSG_NOSIGNAL);
        if (n > 0) {
            c->output_pos += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_conn_events(c, true);
            return 0;
        }
        close_conn(c);
        return -1;
    }
    bool must_close = c->close_after_write;
    finish_response(c);
    if (must_close) {
        close_conn(c);
        return -1;
    }
    if (c->input_len) {
        int parsed = process_buffer(c);
        if (parsed < 0) {
            close_conn(c);
            return -1;
        }
        if (parsed > 0) return flush_conn(c);
    }
    set_conn_events(c, false);
    return 0;
}

static int read_conn(Conn *c) {
    for (;;) {
        if (c->input_len == REQUEST_CAP) {
            c->request_consume = c->input_len;
            send_error(c, 400, "Bad Request", "request too large", true);
            return flush_conn(c);
        }
        if (c->input_len == c->input_cap) {
            size_t next = c->input_cap ? c->input_cap * 2 : INPUT_INITIAL_CAP;
            if (next > REQUEST_CAP) next = REQUEST_CAP;
#ifdef RING_DIAG
            size_t diag_old_cap = c->input && !c->input_shared ? c->input_cap : 0U;
#endif
            char *input = realloc(c->input, next);
            if (!input) {
                close_conn(c);
                return -1;
            }
#ifdef RING_DIAG
            ring_diag_input_resize(diag_old_cap, next);
#endif
            c->input = input;
            c->input_cap = next;
        }
        ssize_t n = recv(c->fd, c->input + c->input_len,
                         c->input_cap - c->input_len, 0);
        if (n > 0) {
            c->input_len += (size_t)n;
            continue;
        }
        if (n == 0) {
            close_conn(c);
            return -1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_conn(c);
        return -1;
    }
    int parsed = process_buffer(c);
    if (parsed < 0) {
        close_conn(c);
        return -1;
    }
    if (parsed > 0) return flush_conn(c);
    return 0;
}

static void accept_all(int listener) {
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        int fd = accept4(listener, (struct sockaddr *)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        Conn *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = c;
        if (epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev) != 0) close_conn(c);
    }
}

enum {
    RING_TAG_ACCEPT = 1,
    RING_TAG_RECV = 2,
    RING_TAG_SEND = 3,
    RING_TAG_CLOSE = 4,
    RING_TAG_RECV_MULTISHOT = 5,
    RING_TAG_CANCEL_RECV = 6,
    RING_TAG_CANCEL_SEND = 7
};

#define RING_TAG_MASK ((uintptr_t)0x0f)

static RingCtx *ring_ctx(unsigned index) {
    return index < g_ring_count ? &g_rings[index] : NULL;
}

static struct io_uring_sqe *ring_sqe(RingCtx *ctx) {
    struct io_uring_sqe *sqe = ring_get_sqe(&ctx->io);
    if (sqe) return sqe;
    int rc = ring_submit(&ctx->io);
    if (rc < 0 && rc != -EAGAIN && rc != -ENOMEM) return NULL;
    return ring_get_sqe(&ctx->io);
}

static int ring_queue_accept(RingCtx *ctx, unsigned ring_index) {
    struct io_uring_sqe *sqe = ring_sqe(ctx);
    if (!sqe) return -ENOSPC;
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = g_listener_fds[ring_index % g_listener_count];
    sqe->ioprio = ctx->accept_multishot ? IORING_ACCEPT_MULTISHOT : 0U;
    sqe->accept_flags = g_direct_files ? 0U : SOCK_CLOEXEC;
    if (g_direct_files) sqe->file_index = IORING_FILE_INDEX_ALLOC;
    sqe->user_data = RING_TAG_ACCEPT;
    ctx->accept_outstanding = true;
    ctx->accept_paused = false;
    return 0;
}

static int ring_schedule_accept(void) {
    unsigned outstanding = 0;
    for (unsigned attempt = 0; attempt < g_ring_count; ++attempt) {
        unsigned index = g_accept_next_ring++ % g_ring_count;
        RingCtx *ctx = &g_rings[index];
        if (ctx->accept_outstanding) {
            ++outstanding;
            continue;
        }
        if (ctx->accept_paused) {
            ctx->accept_sqe_retry = false;
            continue;
        }
        int rc = ring_queue_accept(ctx, index);
        if (rc == 0) {
            ++outstanding;
            ctx->accept_sqe_retry = false;
        } else if (rc == -ENOSPC) {
            ctx->accept_sqe_retry = true;
        }
    }
    return outstanding != 0U ? 0 : -ENOSPC;
}

static void ring_queue_direct_close(unsigned ring_index, unsigned slot) {
    RingCtx *ctx = ring_ctx(ring_index);
    if (!ctx) return;
    struct io_uring_sqe *sqe = ring_sqe(ctx);
    if (sqe) {
        /* Direct close addresses the registered-file slot through file_index. */
        sqe->opcode = IORING_OP_CLOSE;
        sqe->fd = 0;
        sqe->file_index = slot + 1U;
        sqe->user_data = ((uint64_t)slot << 4) | RING_TAG_CLOSE;
        return;
    }
    int rc = ring_clear_file(&ctx->io, slot);
    if (rc < 0 && rc != -EBADF) {
        fprintf(stderr, "could not release direct file slot %u: %s\n",
                slot, strerror(-rc));
    }
    if (ctx->accept_paused) {
        ctx->accept_paused = false;
        (void)ring_schedule_accept();
    }
}

static void ring_release_direct(Conn *c) {
    ring_queue_direct_close(c->ring_index, (unsigned)c->fd);
}

static void ring_buffer_publish(RingCtx *ctx, unsigned id) {
    unsigned index = ctx->buf_tail & (ctx->buf_entries - 1U);
    struct io_uring_buf *entry = &ctx->buf_ring->bufs[index];
    entry->addr = (uint64_t)(uintptr_t)(ctx->buf_storage +
                                        (size_t)id * ctx->buf_size);
    entry->len = ctx->buf_size;
    entry->bid = (uint16_t)id;
    entry->resv = 0;
    __atomic_store_n(&ctx->buf_ring->tail, (uint16_t)(ctx->buf_tail + 1U),
                     __ATOMIC_RELEASE);
    ctx->buf_tail++;
    ctx->buf_available++;
}

static void ring_buffer_return(RingCtx *ctx, unsigned id) {
    if (id >= ctx->buf_entries) return;
    ring_buffer_publish(ctx, id);
}

static int ring_buffers_init(RingCtx *ctx, unsigned ring_index) {
    ctx->buf_entries = RING_BUFFER_COUNT;
    ctx->buf_size = RING_BUFFER_SIZE;
    ctx->buf_group = ring_index + 1U;
    size_t entries_bytes = (size_t)ctx->buf_entries * sizeof(struct io_uring_buf);
    size_t ring_bytes = sizeof(struct io_uring_buf_ring) + entries_bytes;
    size_t page_size = 4096U;
    ctx->buf_ring_bytes = (ring_bytes + page_size - 1U) & ~(page_size - 1U);
    size_t storage_bytes = (size_t)ctx->buf_entries * ctx->buf_size;
    void *ring_mem = NULL;
    void *storage_mem = NULL;
    int rc = posix_memalign(&ring_mem, page_size, ctx->buf_ring_bytes);
    if (rc != 0) return -rc;
    rc = posix_memalign(&storage_mem, page_size, storage_bytes);
    if (rc != 0) {
        free(ring_mem);
        return -rc;
    }
    ctx->buf_ring = ring_mem;
    ctx->buf_storage = storage_mem;
    memset(ctx->buf_ring, 0, ctx->buf_ring_bytes);
    memset(ctx->buf_storage, 0, storage_bytes);
    rc = ring_register_buf_ring(&ctx->io, ctx->buf_ring, ctx->buf_entries,
                                ctx->buf_group);
    if (rc != 0) return rc;
    ctx->buffers_registered = true;
    for (unsigned i = 0; i < ctx->buf_entries; ++i) ring_buffer_publish(ctx, i);
    return 0;
}

static int conn_reserve_input(Conn *c, size_t needed) {
    if (needed > REQUEST_CAP) return -EMSGSIZE;
    if (needed <= c->input_cap) return 0;
    size_t cap = c->input_cap ? c->input_cap : INPUT_INITIAL_CAP;
    while (cap < needed && cap < REQUEST_CAP) {
        cap *= 2U;
        if (cap > REQUEST_CAP) cap = REQUEST_CAP;
    }
    if (cap < needed) return -EMSGSIZE;
#ifdef RING_DIAG
    size_t diag_old_cap = c->input && !c->input_shared ? c->input_cap : 0U;
#endif
    char *input = realloc(c->input, cap);
    if (!input) return -ENOMEM;
#ifdef RING_DIAG
    ring_diag_input_resize(diag_old_cap, cap);
#endif
    c->input = input;
    c->input_cap = cap;
    return 0;
}

static int conn_append_input(Conn *c, const char *data, size_t len) {
    if (len > REQUEST_CAP - c->input_len) return -EMSGSIZE;
    int rc = conn_reserve_input(c, c->input_len + len);
    if (rc != 0) return rc;
    memcpy(c->input + c->input_len, data, len);
    c->input_len += len;
    return 0;
}

static int conn_append_pending_input(Conn *c, const char *data, size_t len) {
    if (c->pending_input_len > PIPELINE_INPUT_CAP ||
        len > PIPELINE_INPUT_CAP - c->pending_input_len)
        return -EMSGSIZE;
    size_t needed = c->pending_input_len + len;
    if (needed > c->pending_input_cap) {
        size_t cap = c->pending_input_cap
            ? c->pending_input_cap : INPUT_INITIAL_CAP;
        while (cap < needed && cap < PIPELINE_INPUT_CAP) {
            cap *= 2U;
            if (cap > PIPELINE_INPUT_CAP) cap = PIPELINE_INPUT_CAP;
        }
        if (cap < needed) return -EMSGSIZE;
        char *pending = realloc(c->pending_input, cap);
        if (!pending) return -ENOMEM;
        c->pending_input = pending;
        c->pending_input_cap = cap;
    }
    memcpy(c->pending_input + c->pending_input_len, data, len);
    c->pending_input_len += len;
    return 0;
}

static int conn_append_received_input(Conn *c, const char *data, size_t len) {
    if (c->pending_input_len != 0U)
        return conn_append_pending_input(c, data, len);
    size_t room = REQUEST_CAP - c->input_len;
    size_t direct = len < room ? len : room;
    if (direct != 0U) {
        int rc = conn_append_input(c, data, direct);
        if (rc != 0) return rc;
    }
    return direct < len
        ? conn_append_pending_input(c, data + direct, len - direct) : 0;
}

static int ring_queue_recv(Conn *c, bool recovery) {
    if (c->closing) return -ECANCELED;
    if (c->recv_armed) return -EALREADY;
    if (c->input_len >= REQUEST_CAP) return -EMSGSIZE;
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) return -EINVAL;
#ifdef IORING_RECV_MULTISHOT
    if (!recovery && ctx->recv_multishot_enabled && c->direct_file) {
        if (ctx->buf_available == 0U) return -ENOBUFS;
        struct io_uring_sqe *multi_sqe = ring_sqe(ctx);
        if (!multi_sqe) {
#ifdef RING_DIAG
            ++ctx->diag.recv_sqe_enospc;
#endif
            return -ENOSPC;
        }
        multi_sqe->opcode = IORING_OP_RECV;
        multi_sqe->ioprio = IORING_RECV_MULTISHOT;
        multi_sqe->fd = c->fd;
        multi_sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        multi_sqe->buf_group = (uint16_t)ctx->buf_group;
        multi_sqe->addr = 0U;
        multi_sqe->len = 0U;
        multi_sqe->msg_flags = 0U;
        multi_sqe->user_data = (uint64_t)((uintptr_t)c | RING_TAG_RECV_MULTISHOT);
        c->recv_shared_buffer = true;
        c->recv_recovery = false;
        c->recv_multishot = true;
        c->recv_armed = true;
        c->recv_capacity = ctx->buf_size;
#ifdef RING_DIAG
        ++ctx->diag.recv_sqe_selected;
#endif
        return 0;
    }
#endif
    bool use_buffer = ctx->buffers_registered && ctx->buf_available != 0U;
    if (recovery && !use_buffer) return -ENOBUFS;
    if (recovery && ctx->recovery_inflight >= RING_RECOVERY_INFLIGHT_LIMIT)
        return -EAGAIN;
    if (!use_buffer) {
        int reserve_rc = conn_reserve_input(c,
            c->input_len < REQUEST_CAP ? c->input_len + 1U : REQUEST_CAP);
        if (reserve_rc != 0) return reserve_rc;
    }
    size_t receive_len = use_buffer ? ctx->buf_size : c->input_cap - c->input_len;
    size_t input_room = REQUEST_CAP - c->input_len;
    if (receive_len > input_room) receive_len = input_room;
    if (receive_len == 0U) return -EMSGSIZE;
    struct io_uring_sqe *sqe = ring_sqe(ctx);
    if (!sqe) {
#ifdef RING_DIAG
        ++ctx->diag.recv_sqe_enospc;
#endif
        return -ENOSPC;
    }
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = c->fd;
    if (c->direct_file) sqe->flags |= IOSQE_FIXED_FILE;
    if (use_buffer) {
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = (uint16_t)ctx->buf_group;
        sqe->len = (uint32_t)receive_len;
        sqe->addr = 0;
    } else {
        sqe->addr = (uint64_t)(uintptr_t)(c->input + c->input_len);
        sqe->len = (uint32_t)receive_len;
    }
    sqe->msg_flags = recovery ? MSG_DONTWAIT : 0U;
    sqe->user_data = (uint64_t)((uintptr_t)c | RING_TAG_RECV);
    c->recv_shared_buffer = use_buffer;
    c->recv_recovery = recovery;
    c->recv_multishot = false;
    c->recv_armed = true;
    c->recv_capacity = (unsigned)receive_len;
    if (recovery) ++ctx->recovery_inflight;
#ifdef RING_DIAG
    if (use_buffer) ++ctx->diag.recv_sqe_selected;
    else ++ctx->diag.recv_sqe_plain;
#endif
    return 0;
}

static void ring_close_conn(Conn *c);
static void ring_remove_ready(Conn *c);
static void ring_remove_rearm(RingCtx *ctx, Conn *c);
static void ring_finalize_closing(Conn *c);
static void ring_remove_send_pending(RingCtx *ctx, Conn *c);
static void ring_remove_deferred_send(Conn *c);
static void ring_remove_wal_hold(Conn *c);
static void ring_defer_cancel(RingCtx *ctx, Conn *c);
static void ring_refill_cancels(RingCtx *ctx);

static int ring_queue_send(Conn *c) {
    if (c->closing) return -ECANCELED;
    if (!c->output || c->output_pos >= c->output_len) return -EINVAL;
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) return -EINVAL;
    struct io_uring_sqe *sqe = ring_sqe(ctx);
    if (!sqe) {
#ifdef RING_DIAG
        ++ctx->diag.send_sqe_enospc;
#endif
        return -ENOSPC;
    }
#ifdef RING_DIAG
    if (c->diag_response_sampled && c->diag_send_queued_ns == 0U) {
        uint64_t now_ns = ring_monotonic_ns();
        c->diag_send_queued_ns = now_ns;
        ring_diag_record_stage(c, RING_DIAG_STAGE_READY_TO_SQE,
                               c->diag_response_ready_ns, now_ns);
    }
#endif
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = c->fd;
    if (c->direct_file) sqe->flags |= IOSQE_FIXED_FILE;
    sqe->addr = (uint64_t)(uintptr_t)(c->output + c->output_pos);
    sqe->len = (uint32_t)(c->output_len - c->output_pos);
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = (uint64_t)((uintptr_t)c | RING_TAG_SEND);
    c->send_armed = true;
#ifdef RING_DIAG
    ++ctx->diag.send_sqe;
    if (c->diag_send_outstanding) ++g_ring_memory_diag.send_duplicate;
    c->diag_send_outstanding = true;
    ++g_ring_memory_diag.sends_active;
    if (g_ring_memory_diag.sends_active > g_ring_memory_diag.sends_active_peak)
        g_ring_memory_diag.sends_active_peak = g_ring_memory_diag.sends_active;
#endif
    return 0;
}

enum {
    RING_CANCEL_RECV_BIT = 1U << 0,
    RING_CANCEL_SEND_BIT = 1U << 1
};

static int ring_queue_cancel_target(Conn *c, unsigned bit) {
    unsigned target_tag;
    unsigned cancel_tag;
    if ((c->cancel_armed_mask & bit) != 0U) {
        c->cancel_pending_mask &= ~bit;
        return 0;
    }
    if (bit == RING_CANCEL_RECV_BIT) {
        if (!c->recv_armed) {
            c->cancel_pending_mask &= ~bit;
            return 0;
        }
        target_tag = c->recv_multishot ? RING_TAG_RECV_MULTISHOT : RING_TAG_RECV;
        cancel_tag = RING_TAG_CANCEL_RECV;
    } else if (bit == RING_CANCEL_SEND_BIT) {
        if (!c->send_armed) {
            c->cancel_pending_mask &= ~bit;
            return 0;
        }
        target_tag = RING_TAG_SEND;
        cancel_tag = RING_TAG_CANCEL_SEND;
    } else {
        return -EINVAL;
    }

    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) return -EINVAL;
    struct io_uring_sqe *sqe = ring_sqe(ctx);
    if (!sqe) return -ENOSPC;
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = (uint64_t)((uintptr_t)c | target_tag);
    sqe->cancel_flags = IORING_ASYNC_CANCEL_USERDATA;
    sqe->user_data = (uint64_t)((uintptr_t)c | cancel_tag);
    c->cancel_pending_mask &= ~bit;
    c->cancel_armed_mask |= bit;
    return 0;
}

static void ring_defer_sqe_send(RingCtx *ctx, Conn *c) {
    if (c->send_pending) return;
    c->send_pending = true;
    c->send_next = NULL;
    if (ctx->send_tail) ctx->send_tail->send_next = c;
    else ctx->send_head = c;
    ctx->send_tail = c;
#ifdef RING_DIAG
    ++ctx->send_pending_count;
    if (ctx->send_pending_count > ctx->diag.send_pending_peak)
        ctx->diag.send_pending_peak = ctx->send_pending_count;
#endif
}

static int ring_send_or_defer(Conn *c) {
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) return -EINVAL;
    int rc = ring_queue_send(c);
    if (rc == -ENOSPC) {
        ring_defer_sqe_send(ctx, c);
        return 0;
    }
    return rc;
}

static void ring_refill_sends(RingCtx *ctx) {
    while (ctx->send_head) {
        Conn *c = ctx->send_head;
        ctx->send_head = c->send_next;
        if (!ctx->send_head) ctx->send_tail = NULL;
        c->send_next = NULL;
        c->send_pending = false;
#ifdef RING_DIAG
        if (ctx->send_pending_count != 0U) --ctx->send_pending_count;
#endif
        int rc = ring_queue_send(c);
        if (rc == -ENOSPC) {
            ring_defer_sqe_send(ctx, c);
            break;
        }
        if (rc != 0) ring_close_conn(c);
    }
}

static void ring_close_conn(Conn *c) {
    if (!c) return;
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (c->closing) {
        ring_finalize_closing(c);
        return;
    }
    c->closing = true;
    ring_remove_ready(c);
    ring_remove_rearm(ctx, c);
    ring_remove_send_pending(ctx, c);
    ring_remove_deferred_send(c);
    ring_remove_wal_hold(c);
    if (c->recv_armed) c->cancel_pending_mask |= RING_CANCEL_RECV_BIT;
    if (c->send_armed) c->cancel_pending_mask |= RING_CANCEL_SEND_BIT;
    if (ctx && c->cancel_pending_mask != 0U) {
        ring_defer_cancel(ctx, c);
    }
    ring_finalize_closing(c);
}

static void ring_defer_send(Conn *c) {
    if (c->send_deferred) return;
    c->send_deferred = true;
    c->batch_next = NULL;
    if (g_deferred_tail) g_deferred_tail->batch_next = c;
    else g_deferred_head = c;
    g_deferred_tail = c;
}

static void ring_defer_wal_write(Conn *c) {
    if (c->wal_write_deferred) return;
    c->wal_write_deferred = true;
#ifdef RING_DIAG
    ++g_ring_memory_diag.wal_depth;
    if (g_ring_memory_diag.wal_depth > g_ring_memory_diag.wal_depth_peak)
        g_ring_memory_diag.wal_depth_peak = g_ring_memory_diag.wal_depth;
#endif
    c->wal_next = NULL;
    if (g_wal_hold_tail) g_wal_hold_tail->wal_next = c;
    else g_wal_hold_head = c;
    g_wal_hold_tail = c;
}

static int ring_finish_batch(void) {
    bool commit_failed = false;
    if (g_batch_write_txn) {
        int rc = sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "write batch commit failed: %s\n", sqlite3_errmsg(g_db));
            int rollback_rc = sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
            if (!sqlite3_get_autocommit(g_db)) {
                fprintf(stderr, "write batch rollback failed (%d): %s; stopping server\n",
                        rollback_rc, sqlite3_errmsg(g_db));
                return -EIO;
            }
            commit_failed = true;
        }
        g_batch_write_txn = false;
    }
    if (g_batch_read_txn) {
        int rc = sqlite3BtreeCommit(g_feed_btree.tree);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "read batch commit failed: %d\n", rc);
            int rollback_rc = sqlite3BtreeRollback(g_feed_btree.tree, SQLITE_ABORT, 0);
            if (g_feed_btree.tree->inTrans != TRANS_NONE) {
                fprintf(stderr, "read batch rollback failed (%d); stopping server\n",
                        rollback_rc);
                return -EIO;
            }
            commit_failed = true;
        }
        g_batch_read_txn = false;
    }

    Conn *c = g_deferred_head;
    g_deferred_head = NULL;
    g_deferred_tail = NULL;
    while (c) {
        Conn *next = c->batch_next;
        c->batch_next = NULL;
        c->send_deferred = false;
#ifdef RING_DIAG
        if (c->diag_response_sampled && !c->diag_batch_release_seen) {
            uint64_t now_ns = ring_monotonic_ns();
            ring_diag_record_stage(c, RING_DIAG_STAGE_READY_TO_BATCH,
                                   c->diag_response_ready_ns, now_ns);
            c->diag_batch_release_seen = true;
        }
#endif
        if (commit_failed && c->batch_sensitive) {
            bool close_after_write = c->close_after_write;
            send_error(c, 500, "Internal Server Error", "internal server error",
                       close_after_write);
        }
        c->batch_sensitive = false;
        if (ring_send_or_defer(c) != 0) ring_close_conn(c);
        c = next;
    }
    return 0;
}

static void ring_defer_rearm(RingCtx *ctx, Conn *c) {
    if (c->rearm_pending) return;
    c->rearm_pending = true;
    c->rearm_next = NULL;
    if (c->rearm_recovery) ++ctx->recovery_queued;
#ifdef RING_DIAG
    if (c->diag_rearm_since_ns == 0U) {
        c->diag_rearm_since_ns = ring_monotonic_ns();
        ++ctx->diag.rearm_queue_adds;
    }
    ++ctx->diag.rearm_pending;
    if (ctx->diag.rearm_pending > ctx->diag.rearm_pending_peak)
        ctx->diag.rearm_pending_peak = ctx->diag.rearm_pending;
#endif
    if (ctx->rearm_tail) ctx->rearm_tail->rearm_next = c;
    else ctx->rearm_head = c;
    ctx->rearm_tail = c;
}

static void ring_defer_ready(Conn *c) {
    if (c->ready_pending) return;
    c->ready_pending = true;
    c->ready_next = NULL;
    if (g_ready_tail) g_ready_tail->ready_next = c;
    else g_ready_head = c;
    g_ready_tail = c;
    ++g_ready_pending;
    if (g_ready_pending > g_ready_pending_peak)
        g_ready_pending_peak = g_ready_pending;
#ifdef RING_DIAG
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (ctx) {
        ++ctx->diag.ready_enqueued;
        ++ctx->diag.ready_pending;
        if (ctx->diag.ready_pending > ctx->diag.ready_pending_peak)
            ctx->diag.ready_pending_peak = ctx->diag.ready_pending;
    }
#endif
}

static void ring_remove_ready(Conn *c) {
    if (!c->ready_pending) return;
    Conn *previous = NULL;
    Conn *current = g_ready_head;
    while (current && current != c) {
        previous = current;
        current = current->ready_next;
    }
    if (!current) return;
    if (previous) previous->ready_next = current->ready_next;
    else g_ready_head = current->ready_next;
    if (g_ready_tail == current) g_ready_tail = previous;
    current->ready_next = NULL;
    current->ready_pending = false;
    if (g_ready_pending != 0U) --g_ready_pending;
#ifdef RING_DIAG
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (ctx && ctx->diag.ready_pending != 0U) --ctx->diag.ready_pending;
#endif
}

static void ring_remove_rearm(RingCtx *ctx, Conn *c) {
    if (!ctx || !c->rearm_pending) return;
    Conn *previous = NULL;
    Conn *current = ctx->rearm_head;
    while (current && current != c) {
        previous = current;
        current = current->rearm_next;
    }
    if (!current) return;
    if (previous) previous->rearm_next = current->rearm_next;
    else ctx->rearm_head = current->rearm_next;
    if (ctx->rearm_tail == current) ctx->rearm_tail = previous;
    current->rearm_next = NULL;
    current->rearm_pending = false;
    if (current->rearm_recovery) {
        if (ctx->recovery_queued == 0U) abort();
        --ctx->recovery_queued;
    }
    current->rearm_recovery = false;
#ifdef RING_DIAG
    if (ctx->diag.rearm_pending != 0U) --ctx->diag.rearm_pending;
#endif
}

static void ring_remove_send_pending(RingCtx *ctx, Conn *c) {
    if (!ctx || !c->send_pending) return;
    Conn *previous = NULL;
    Conn *current = ctx->send_head;
    while (current && current != c) {
        previous = current;
        current = current->send_next;
    }
    if (!current) return;
    if (previous) previous->send_next = current->send_next;
    else ctx->send_head = current->send_next;
    if (ctx->send_tail == current) ctx->send_tail = previous;
    current->send_next = NULL;
    current->send_pending = false;
#ifdef RING_DIAG
    if (ctx->send_pending_count != 0U) --ctx->send_pending_count;
#endif
}

static void ring_remove_deferred_send(Conn *c) {
    if (!c->send_deferred) return;
    Conn *previous = NULL;
    Conn *current = g_deferred_head;
    while (current && current != c) {
        previous = current;
        current = current->batch_next;
    }
    if (!current) return;
    if (previous) previous->batch_next = current->batch_next;
    else g_deferred_head = current->batch_next;
    if (g_deferred_tail == current) g_deferred_tail = previous;
    current->batch_next = NULL;
    current->send_deferred = false;
}

static void ring_remove_wal_hold(Conn *c) {
    if (!c->wal_write_deferred) return;
    Conn *previous = NULL;
    Conn *current = g_wal_hold_head;
    while (current && current != c) {
        previous = current;
        current = current->wal_next;
    }
    if (!current) return;
    if (previous) previous->wal_next = current->wal_next;
    else g_wal_hold_head = current->wal_next;
    if (g_wal_hold_tail == current) g_wal_hold_tail = previous;
    current->wal_next = NULL;
    current->wal_write_deferred = false;
#ifdef RING_DIAG
    if (g_ring_memory_diag.wal_depth != 0U) --g_ring_memory_diag.wal_depth;
    else ++g_ring_memory_diag.accounting_errors;
    ++g_ring_memory_diag.close_in_wal;
#endif
}

static void ring_defer_cancel(RingCtx *ctx, Conn *c) {
    if (c->cancel_queued || c->cancel_pending_mask == 0U) return;
    c->cancel_queued = true;
    c->cancel_next = NULL;
    if (ctx->cancel_tail) ctx->cancel_tail->cancel_next = c;
    else ctx->cancel_head = c;
    ctx->cancel_tail = c;
}

static bool ring_response_pending(const Conn *c);
static void ring_resume_recv_if_drained(Conn *c);
static void ring_start_recv(Conn *c);

static void ring_resume_paused_after_cancel(Conn *c) {
    if (!c || c->closing || !c->recv_paused) return;
    if (!ring_response_pending(c) &&
        (c->input_len != 0U || c->pending_input_len != 0U))
        ring_defer_ready(c);
    ring_resume_recv_if_drained(c);
    if (!c->recv_paused) ring_start_recv(c);
}

static void ring_refill_cancels(RingCtx *ctx) {
    ctx->cancel_sqe_retry = false;
    while (ctx->cancel_head) {
        Conn *c = ctx->cancel_head;
        const unsigned bits[] = { RING_CANCEL_RECV_BIT, RING_CANCEL_SEND_BIT };
        for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
            unsigned bit = bits[i];
            if ((c->cancel_pending_mask & bit) == 0U) continue;
            int rc = ring_queue_cancel_target(c, bit);
            if (rc == -ENOSPC) {
                ctx->cancel_sqe_retry = true;
                return;
            }
            if (rc != 0) {
                fprintf(stderr, "could not queue io_uring cancellation: %s\n",
                        strerror(-rc));
                ctx->cancel_sqe_retry = true;
                return;
            }
        }
        if (c->cancel_pending_mask != 0U) {
            ctx->cancel_sqe_retry = true;
            return;
        }
        ctx->cancel_head = c->cancel_next;
        if (!ctx->cancel_head) ctx->cancel_tail = NULL;
        c->cancel_next = NULL;
        c->cancel_queued = false;
        if (c->closing) ring_finalize_closing(c);
        else ring_resume_paused_after_cancel(c);
    }
}

static void ring_pause_recv(Conn *c) {
    if (!c || c->closing) return;
    c->recv_paused = true;
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (ctx && c->rearm_pending) ring_remove_rearm(ctx, c);
    if (!ctx || !c->recv_armed ||
        (c->cancel_pending_mask & RING_CANCEL_RECV_BIT) != 0U ||
        (c->cancel_armed_mask & RING_CANCEL_RECV_BIT) != 0U)
        return;
    c->cancel_pending_mask |= RING_CANCEL_RECV_BIT;
    ring_defer_cancel(ctx, c);
}

static void ring_finalize_closing(Conn *c) {
    if (!c || !c->closing || c->recv_armed || c->send_armed ||
        c->cancel_pending_mask != 0U || c->cancel_armed_mask != 0U ||
        c->cancel_queued) return;
    RingCtx *ctx = ring_ctx(c->ring_index);
    bool resume_accept = ctx && ctx->accept_paused && !c->direct_file;
    conn_close_socket(c);
    if (resume_accept) {
        ctx->accept_paused = false;
        (void)ring_schedule_accept();
    }
    conn_release_memory(c);
}

static void ring_start_recv(Conn *c) {
    if (!c || c->closing || c->recv_paused || c->recv_armed ||
        c->rearm_pending)
        return;
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) {
        ring_close_conn(c);
        return;
    }
    int rc = ring_queue_recv(c, false);
    if (rc == -ENOBUFS || rc == -ENOSPC) {
        if (rc == -ENOSPC) ctx->rearm_sqe_retry = true;
        ring_defer_rearm(ctx, c);
    } else if (rc != 0) {
        ring_close_conn(c);
    }
}

static bool ring_reject_full_pipeline_request(Conn *c);
static int ring_fill_pending_input(Conn *c);

static void ring_resume_wal_writes(void) {
    unsigned resumed = 0;
    while (g_wal_hold_head &&
           resumed < WAL_RESUME_BATCH &&
           atomic_load_explicit(&g_wal_write_hold, memory_order_acquire) == 0) {
        ++resumed;
        Conn *c = g_wal_hold_head;
        g_wal_hold_head = c->wal_next;
        if (!g_wal_hold_head) g_wal_hold_tail = NULL;
        c->wal_next = NULL;
        c->wal_write_deferred = false;
#ifdef RING_DIAG
        if (g_ring_memory_diag.wal_depth != 0U) --g_ring_memory_diag.wal_depth;
        else ++g_ring_memory_diag.accounting_errors;
#endif
        c->batch_sensitive = false;

        if (ring_fill_pending_input(c) != 0) {
            ring_close_conn(c);
            continue;
        }
        int parsed = process_buffer(c);
        if (parsed < 0) {
            ring_close_conn(c);
        } else if (parsed == 0) {
            if (ring_reject_full_pipeline_request(c)) continue;
            if (c->peer_eof && c->pending_input_len == 0U)
                ring_close_conn(c);
            else {
                ring_resume_recv_if_drained(c);
                ring_start_recv(c);
            }
        } else if (parsed == 2) {
            /* A new checkpoint hold began while the queue was being drained. */
        } else if (c->batch_sensitive) {
            ring_defer_send(c);
        } else {
            if (ring_send_or_defer(c) != 0) ring_close_conn(c);
        }
    }
}

static void ring_refill_rearms(RingCtx *ctx) {
    ctx->rearm_sqe_retry = false;
    while (ctx->rearm_head &&
           (!ctx->buffers_registered || ctx->buf_available != 0U)) {
        Conn *c = ctx->rearm_head;
        if (c->rearm_recovery &&
            ctx->recovery_inflight >= RING_RECOVERY_INFLIGHT_LIMIT) break;
        bool recovery = c->rearm_recovery;
        int rc = recovery ? ring_queue_recv(c, true) : 0;
        if (recovery && (rc == -ENOBUFS || rc == -ENOSPC || rc == -EAGAIN)) {
            if (rc == -ENOSPC) ctx->rearm_sqe_retry = true;
            break;
        }
        ctx->rearm_head = c->rearm_next;
        if (!ctx->rearm_head) ctx->rearm_tail = NULL;
        c->rearm_next = NULL;
        c->rearm_pending = false;
        if (recovery) {
            if (ctx->recovery_queued == 0U) abort();
            --ctx->recovery_queued;
        }
#ifdef RING_DIAG
        if (ctx->diag.rearm_pending != 0U) --ctx->diag.rearm_pending;
#endif
        if (!recovery) rc = ring_queue_recv(c, false);
        if (rc == -ENOBUFS || rc == -ENOSPC || rc == -EAGAIN) {
            if (rc == -ENOSPC) ctx->rearm_sqe_retry = true;
            ring_defer_rearm(ctx, c);
            break;
        }
        c->rearm_recovery = false;
        if (rc != 0) ring_close_conn(c);
#ifdef RING_DIAG
        else if (c->diag_rearm_since_ns != 0U) {
            uint64_t now_ns = ring_monotonic_ns();
            uint64_t wait_ns = now_ns >= c->diag_rearm_since_ns
                ? now_ns - c->diag_rearm_since_ns : 0U;
            ++ctx->diag.rearm_waits;
            ctx->diag.rearm_wait_total_ns += wait_ns;
            if (wait_ns > ctx->diag.rearm_wait_max_ns)
                ctx->diag.rearm_wait_max_ns = wait_ns;
            c->diag_rearm_since_ns = 0U;
        }
#endif
    }
    if (!ctx->rearm_head) ctx->rearm_sqe_retry = false;
}

static void ring_handle_recv_plain(Conn *c, int result) {
    c->recv_armed = false;
    c->recv_shared_buffer = false;
    c->recv_capacity = 0U;
    if (result == 0) {
        c->peer_eof = true;
        if (ring_response_pending(c)) return;
        if (c->input_len != 0U || c->pending_input_len != 0U)
            ring_defer_ready(c);
        else ring_close_conn(c);
        return;
    }
    if (result < 0) {
        if (result == -ECANCELED && c->recv_paused) {
            ring_resume_paused_after_cancel(c);
            return;
        }
        if (result == -EINTR || result == -EAGAIN || result == -EWOULDBLOCK) {
            ring_start_recv(c);
            return;
        }
        ring_close_conn(c);
        return;
    }
    if ((size_t)result > c->input_cap - c->input_len) {
        ring_close_conn(c);
        return;
    }
    c->input_len += (size_t)result;
    if (g_recovery_draining) {
        ring_defer_ready(c);
        return;
    }
    int parsed = process_buffer(c);
    if (parsed < 0) {
        ring_close_conn(c);
    } else if (parsed == 0) {
        if (c->peer_eof && c->pending_input_len == 0U) ring_close_conn(c);
        else ring_start_recv(c);
    } else if (parsed == 2) {
        return;
    } else if (c->batch_sensitive) {
        ring_defer_send(c);
    } else if (ring_send_or_defer(c) != 0) {
        ring_close_conn(c);
    }
}

static int ring_copy_shared_input(Conn *c) {
    size_t cap = c->input_len < INPUT_INITIAL_CAP ? INPUT_INITIAL_CAP : c->input_len;
    if (cap > REQUEST_CAP) cap = REQUEST_CAP;
    char *input = malloc(cap);
    if (!input) return -ENOMEM;
    memcpy(input, c->input, c->input_len);
#ifdef RING_DIAG
    ring_diag_input_resize(0U, cap);
#endif
    c->input = input;
    c->input_cap = cap;
    c->input_shared = false;
    return 0;
}

static int ring_fill_pending_input(Conn *c) {
    if (c->pending_input_len == 0U) return 0;
    if (c->input_shared && ring_copy_shared_input(c) != 0) return -ENOMEM;
    size_t room = REQUEST_CAP - c->input_len;
    if (room == 0U) return 0;
    size_t amount = c->pending_input_len < room
        ? c->pending_input_len : room;
    int rc = conn_reserve_input(c, c->input_len + amount);
    if (rc != 0) return rc;
    memcpy(c->input + c->input_len, c->pending_input, amount);
    c->input_len += amount;
    c->pending_input_len -= amount;
    if (c->pending_input_len != 0U) {
        memmove(c->pending_input, c->pending_input + amount,
                c->pending_input_len);
    } else {
        free(c->pending_input);
        c->pending_input = NULL;
        c->pending_input_cap = 0U;
    }
    return 0;
}

static bool ring_response_pending(const Conn *c) {
    return c->send_armed || c->send_pending || c->send_deferred ||
           c->wal_write_deferred || c->batch_sensitive ||
           (c->output && c->output_len > c->output_pos);
}

static void ring_pause_if_pipeline_blocked(Conn *c) {
    if (c->pending_input_len >= PIPELINE_INPUT_PAUSE_THRESHOLD)
        ring_pause_recv(c);
}

static void ring_resume_recv_if_drained(Conn *c) {
    if (!c || c->closing || c->pending_input_len != 0U ||
        ring_response_pending(c) || c->recv_armed || c->rearm_pending ||
        c->wal_write_deferred || c->ready_pending ||
        (c->cancel_pending_mask & RING_CANCEL_RECV_BIT) != 0U ||
        (c->cancel_armed_mask & RING_CANCEL_RECV_BIT) != 0U)
        return;
    c->recv_paused = false;
}

static bool ring_reject_full_pipeline_request(Conn *c) {
    if (c->pending_input_len == 0U || c->input_len < REQUEST_CAP)
        return false;
    c->request_consume = c->input_len;
    send_error(c, 400, "Bad Request", "request too large", true);
    ring_pause_recv(c);
    if (c->batch_sensitive) ring_defer_send(c);
    else if (ring_send_or_defer(c) != 0) ring_close_conn(c);
    return true;
}

static void ring_handle_recv_buffer(Conn *c, int result, unsigned flags,
                                   RingCtx *ctx, bool recovery,
                                   bool multishot_more, bool multishot_terminal) {
    unsigned recv_capacity = c->recv_capacity;
    if (!multishot_more) {
        c->recv_armed = false;
        c->recv_multishot = false;
        c->recv_shared_buffer = false;
        c->recv_capacity = 0U;
    }
    bool has_buffer = (flags & IORING_CQE_F_BUFFER) != 0;
    unsigned buffer_id = flags >> IORING_CQE_BUFFER_SHIFT;
    char *buffer = NULL;
    if (has_buffer) {
        if (buffer_id >= ctx->buf_entries || ctx->buf_available == 0U) {
            ring_close_conn(c);
            return;
        }
        ctx->buf_available--;
        buffer = (char *)(ctx->buf_storage + (size_t)buffer_id * ctx->buf_size);
    }

    if (result < 0) {
        if (has_buffer) ring_buffer_return(ctx, buffer_id);
        if (result == -ECANCELED && c->recv_paused) {
            ring_resume_paused_after_cancel(c);
            return;
        }
        if ((result == -EAGAIN || result == -EWOULDBLOCK) && recovery) {
            c->rearm_recovery = false;
            if (!ring_response_pending(c)) ring_defer_ready(c);
        } else if (result == -ENOBUFS || result == -EINTR ||
                   result == -EAGAIN || result == -EWOULDBLOCK) {
            c->rearm_recovery = result == -ENOBUFS ||
                                (recovery && result == -EINTR);
            if (result == -ENOBUFS) g_recovery_draining = true;
            if (c->recv_paused || c->input_len >= REQUEST_CAP ||
                c->pending_input_len != 0U) {
                c->rearm_recovery = false;
                if (!ring_response_pending(c) &&
                    (c->input_len != 0U || c->pending_input_len != 0U))
                    ring_defer_ready(c);
                if (c->recv_paused) ring_resume_paused_after_cancel(c);
            } else {
                ring_defer_rearm(ctx, c);
            }
        } else {
            ring_close_conn(c);
        }
        return;
    }
    if (result == 0) {
        if (has_buffer) ring_buffer_return(ctx, buffer_id);
        c->peer_eof = true;
        if (ring_response_pending(c)) return;
        if (c->input_len != 0U || c->pending_input_len != 0U)
            ring_defer_ready(c);
        else ring_close_conn(c);
        return;
    }
    if (!has_buffer || (size_t)result > ctx->buf_size) {
        ring_close_conn(c);
        return;
    }

    bool response_pending = ring_response_pending(c);
    if (c->close_after_write && response_pending) {
        ring_buffer_return(ctx, buffer_id);
        ring_pause_recv(c);
        return;
    }

    bool recovery_pass = (recovery || g_recovery_draining) && !multishot_more;
    if (recovery_pass) {
        if (c->input_shared && ring_copy_shared_input(c) != 0) {
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        int append_rc = conn_append_received_input(c, buffer, (size_t)result);
        if (append_rc != 0) {
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        ring_buffer_return(ctx, buffer_id);
        ring_pause_if_pipeline_blocked(c);
        if ((multishot_terminal || (unsigned)result == recv_capacity) &&
            c->input_len < REQUEST_CAP && !c->recv_paused) {
            c->rearm_recovery = true;
            ring_defer_rearm(ctx, c);
        } else if (response_pending) {
            c->rearm_recovery = false;
            if (c->input_len < REQUEST_CAP) ring_start_recv(c);
        } else {
            c->rearm_recovery = false;
            ring_defer_ready(c);
        }
        return;
    }

    bool defer_parse = response_pending || c->ready_pending ||
                       (g_recovery_draining && multishot_more) ||
                       c->pending_input_len != 0U;
    if (defer_parse) {
        if (c->input_shared && ring_copy_shared_input(c) != 0) {
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        if (conn_append_received_input(c, buffer, (size_t)result) != 0) {
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        ring_buffer_return(ctx, buffer_id);
        ring_pause_if_pipeline_blocked(c);
        if (multishot_terminal && c->input_len < REQUEST_CAP &&
            !c->recv_paused) {
            c->rearm_recovery = true;
            ring_defer_rearm(ctx, c);
        } else if (!response_pending) {
            ring_defer_ready(c);
        }
        return;
    }

    bool shared_input = c->input == NULL && !defer_parse;
    if (shared_input) {
        c->input = buffer;
        c->input_len = (size_t)result;
        c->input_cap = ctx->buf_size;
        c->input_shared = true;
    } else {
        if (c->input_shared && ring_copy_shared_input(c) != 0) {
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        if (conn_append_received_input(c, buffer, (size_t)result) != 0) {
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
    }

    int parsed = process_buffer(c);
    if (parsed == 0) {
        if (shared_input && ring_copy_shared_input(c) != 0) {
            c->input = NULL;
            c->input_len = 0;
            c->input_cap = 0;
            c->input_shared = false;
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        ring_buffer_return(ctx, buffer_id);
        if (c->pending_input_len != 0U) {
            ring_defer_ready(c);
            return;
        }
        if (recovery && (unsigned)result == ctx->buf_size) {
            if (!c->recv_paused) {
                c->rearm_recovery = true;
                ring_defer_rearm(ctx, c);
            }
        } else {
            ring_start_recv(c);
        }
        return;
    }
    if (parsed < 0) {
        if (shared_input) {
            c->input = NULL;
            c->input_len = 0;
            c->input_cap = 0;
            c->input_shared = false;
        }
        ring_buffer_return(ctx, buffer_id);
        ring_close_conn(c);
        return;
    }
    if (parsed == 2) {
        if (c->input_shared && ring_copy_shared_input(c) != 0) {
            c->input = NULL;
            c->input_len = 0U;
            c->input_cap = 0U;
            c->input_shared = false;
            ring_buffer_return(ctx, buffer_id);
            ring_close_conn(c);
            return;
        }
        ring_buffer_return(ctx, buffer_id);
        return;
    }

    if (shared_input) {
        if (c->input_len > c->request_consume) {
            if (ring_copy_shared_input(c) != 0) {
                c->input = NULL;
                c->input_len = 0;
                c->input_cap = 0;
                c->input_shared = false;
                c->request_consume = 0;
                ring_buffer_return(ctx, buffer_id);
                ring_close_conn(c);
                return;
            }
        } else {
            c->input = NULL;
            c->input_len = 0;
            c->input_cap = 0;
            c->input_shared = false;
            c->request_consume = 0;
        }
    }
    ring_buffer_return(ctx, buffer_id);
    if (c->batch_sensitive) ring_defer_send(c);
    else if (ring_send_or_defer(c) != 0) ring_close_conn(c);
}

static void ring_handle_recv(Conn *c, int result, unsigned flags,
                             bool multishot) {
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) {
        ring_close_conn(c);
        return;
    }
    bool recovery = c->recv_recovery;
    c->recv_recovery = false;
    if (recovery) {
        if (ctx->recovery_inflight == 0U) abort();
        --ctx->recovery_inflight;
    }
    bool multishot_more = multishot &&
                          (flags & IORING_CQE_F_MORE) != 0U;
    bool multishot_terminal = multishot && !multishot_more;
    if (multishot && !c->recv_multishot) {
        ring_close_conn(c);
        return;
    }
    if (multishot_terminal) {
        c->recv_armed = false;
        c->recv_multishot = false;
        recovery = true;
    } else if (!multishot) {
        c->recv_armed = false;
    }
    if (c->closing) {
        bool had_shared_buffer = c->recv_shared_buffer;
        if (!multishot_more) {
            c->recv_multishot = false;
            c->recv_shared_buffer = false;
            c->recv_capacity = 0U;
        }
        if (had_shared_buffer && (flags & IORING_CQE_F_BUFFER) != 0U) {
            unsigned buffer_id = flags >> IORING_CQE_BUFFER_SHIFT;
            if (buffer_id >= ctx->buf_entries || ctx->buf_available == 0U) {
                fprintf(stderr, "invalid receive buffer %u while closing connection\n",
                        buffer_id);
            } else {
                --ctx->buf_available;
                ring_buffer_return(ctx, buffer_id);
            }
        }
        if (!multishot_more) ring_finalize_closing(c);
        return;
    }
    if (multishot_terminal &&
        (result == -EINVAL || result == -EOPNOTSUPP)) {
        c->recv_shared_buffer = false;
        c->recv_capacity = 0U;
        ctx->recv_multishot_enabled = false;
        ring_start_recv(c);
        return;
    }
#ifdef RING_DIAG
    if (c->recv_shared_buffer) ++ctx->diag.recv_cqe_selected;
    else ++ctx->diag.recv_cqe_plain;
    if (result > 0) {
        if (c->recv_shared_buffer)
            ctx->diag.recv_bytes_selected += (uint64_t)result;
        else
            ctx->diag.recv_bytes_plain += (uint64_t)result;
    } else if (result == 0) {
        ++ctx->diag.recv_eof;
    } else {
        ++ctx->diag.recv_errors;
        if (result == -ENOBUFS) ++ctx->diag.recv_enobufs;
        else if (result == -EAGAIN || result == -EWOULDBLOCK)
            ++ctx->diag.recv_eagain;
        else if (result == -EINTR) ++ctx->diag.recv_eintr;
    }
#endif
    if (c->recv_shared_buffer)
        ring_handle_recv_buffer(c, result, flags, ctx, recovery,
                                multishot_more, multishot_terminal);
    else ring_handle_recv_plain(c, result);
}

static void ring_handle_send(Conn *c, int result) {
    c->send_armed = false;
#ifdef RING_DIAG
    if (!c->diag_send_outstanding) ++g_ring_memory_diag.accounting_errors;
    c->diag_send_outstanding = false;
    if (g_ring_memory_diag.sends_active != 0U) --g_ring_memory_diag.sends_active;
    else ++g_ring_memory_diag.accounting_errors;
    RingCtx *diag_ctx = ring_ctx(c->ring_index);
    if (diag_ctx) {
        ++diag_ctx->diag.send_cqe;
        if (result == -EINTR || result == -EAGAIN || result == -EWOULDBLOCK) {
            ++diag_ctx->diag.send_retry_cqe;
        } else if (result < 0) {
            ++diag_ctx->diag.send_errors;
        }
    }
    if (c->diag_response_sampled && !c->diag_first_send_cqe_seen) {
        uint64_t now_ns = ring_monotonic_ns();
        c->diag_first_send_cqe_seen = true;
        ring_diag_record_stage(c, RING_DIAG_STAGE_ACCEPT_TO_FIRST_CQE,
                               c->diag_send_submitted_ns, now_ns);
        ring_diag_record_stage(c, RING_DIAG_STAGE_READY_TO_FIRST_CQE,
                               c->diag_response_ready_ns, now_ns);
    }
#endif
    if (c->closing) {
        ring_finalize_closing(c);
        return;
    }
    if (result < 0) {
        if (result == -EINTR || result == -EAGAIN || result == -EWOULDBLOCK) {
            if (ring_send_or_defer(c) != 0) ring_close_conn(c);
            return;
        }
        ring_close_conn(c);
        return;
    }
    if (result == 0) {
        ring_close_conn(c);
        return;
    }
    size_t sent = (size_t)result;
    if (sent > c->output_len - c->output_pos) {
        ring_close_conn(c);
        return;
    }
    c->output_pos += sent;
    if (c->output_pos < c->output_len) {
#ifdef RING_DIAG
        if (diag_ctx) ++diag_ctx->diag.send_partial_cqe;
#endif
        if (ring_send_or_defer(c) != 0) ring_close_conn(c);
        return;
    }
    bool must_close = c->close_after_write;
#ifdef RING_DIAG
    if (c->diag_response_sampled) {
        ring_diag_record_stage(c, RING_DIAG_STAGE_READY_TO_COMPLETE,
                               c->diag_response_ready_ns, ring_monotonic_ns());
    }
#endif
    finish_response(c);
#ifdef RING_DIAG
    if (diag_ctx) ++diag_ctx->diag.responses_complete;
#endif
    c->batch_sensitive = false;
    if (must_close) {
        ring_close_conn(c);
        return;
    }
    if (ring_fill_pending_input(c) != 0) {
        ring_close_conn(c);
        return;
    }
    if (c->input_len) {
        if (g_recovery_draining) {
            ring_defer_ready(c);
            return;
        }
        int parsed = process_buffer(c);
        if (parsed < 0) ring_close_conn(c);
        else if (parsed == 2) return;
        else if (parsed > 0) {
            if (c->batch_sensitive) ring_defer_send(c);
            else if (ring_send_or_defer(c) != 0) ring_close_conn(c);
        } else {
            if (ring_reject_full_pipeline_request(c)) return;
            if (c->peer_eof && c->pending_input_len == 0U)
                ring_close_conn(c);
            else {
                ring_resume_recv_if_drained(c);
                ring_start_recv(c);
            }
        }
    } else {
        if (c->peer_eof && c->pending_input_len == 0U)
            ring_close_conn(c);
        else {
            ring_resume_recv_if_drained(c);
            ring_start_recv(c);
        }
    }
}

static void ring_handle_cancel(Conn *c, unsigned tag) {
    unsigned bit = tag == RING_TAG_CANCEL_RECV
        ? RING_CANCEL_RECV_BIT : RING_CANCEL_SEND_BIT;
    if ((c->cancel_armed_mask & bit) == 0U) return;
    c->cancel_armed_mask &= ~bit;
    if (c->closing) {
        ring_finalize_closing(c);
        return;
    }
    if (bit == RING_CANCEL_RECV_BIT && c->recv_paused) {
        ring_resume_paused_after_cancel(c);
    }
}

static void ring_handle_close(RingCtx *ctx, unsigned slot, int result) {
    if (result < 0 && result != -EBADF) {
        int rc = ring_clear_file(&ctx->io, slot);
        if (rc < 0 && rc != -EBADF) {
            fprintf(stderr, "direct close failed for slot %u: %s; clear: %s\n",
                    slot, strerror(-result), strerror(-rc));
        }
    }
    if (ctx->accept_paused) {
        ctx->accept_paused = false;
        (void)ring_schedule_accept();
    }
}

static void ring_handle_accept(unsigned ring_index, int result, unsigned flags) {
    RingCtx *ctx = ring_ctx(ring_index);
    if (!ctx) return;
    bool more = (flags & IORING_CQE_F_MORE) != 0U;
    ctx->accept_outstanding = more;
    if (result < 0) {
        if (!more && ctx->accept_multishot &&
            (result == -EINVAL || result == -EOPNOTSUPP)) {
            ctx->accept_multishot = false;
            fprintf(stderr,
                    "io_uring %u multishot accept unavailable; using one-shot accepts\n",
                    ring_index);
            (void)ring_schedule_accept();
            return;
        }
        if (result == -EMFILE || result == -ENFILE) {
            if (more) return;
            ctx->accept_paused = true;
            ctx->accept_sqe_retry = false;
            return;
        }
        if (!more) (void)ring_schedule_accept();
        return;
    }
    if (!more) (void)ring_schedule_accept();
    int fd = result;
    Conn *c = calloc(1, sizeof(*c));
    if (!c) {
        if (g_direct_files) ring_queue_direct_close(ring_index, (unsigned)fd);
        else close(fd);
        return;
    }
    c->fd = fd;
    c->ring_index = ring_index;
    c->direct_file = g_direct_files;
    ring_start_recv(c);
}

static void ring_dispatch_cqe(RingCtx *ctx, unsigned ring_index,
                              const struct io_uring_cqe *cqe) {
    uint64_t user_data = cqe->user_data;
    int result = cqe->res;
    unsigned flags = cqe->flags;
    uintptr_t tag = (uintptr_t)user_data & RING_TAG_MASK;
    if (tag == RING_TAG_ACCEPT) {
        ring_handle_accept(ring_index, result, flags);
    } else if (tag == RING_TAG_CLOSE) {
        ring_handle_close(ctx, (unsigned)(user_data >> 4), result);
    } else {
        Conn *c = (Conn *)(uintptr_t)(user_data & ~(uint64_t)RING_TAG_MASK);
        if (tag == RING_TAG_RECV) ring_handle_recv(c, result, flags, false);
        else if (tag == RING_TAG_RECV_MULTISHOT)
            ring_handle_recv(c, result, flags, true);
        else if (tag == RING_TAG_SEND) ring_handle_send(c, result);
        else if (tag == RING_TAG_CANCEL_RECV || tag == RING_TAG_CANCEL_SEND)
            ring_handle_cancel(c, (unsigned)tag);
    }
}

static bool ring_recovery_work_pending(void) {
    if (g_ready_head) return true;
    for (unsigned i = 0; i < g_ring_count; ++i) {
        if (g_rings[i].recovery_queued != 0U ||
            g_rings[i].recovery_inflight != 0U)
            return true;
    }
    return false;
}

static void ring_process_ready(unsigned budget) {
    while (budget != 0U && g_ready_head) {
        Conn *c = g_ready_head;
        RingCtx *ctx = ring_ctx(c->ring_index);
        g_ready_head = c->ready_next;
        if (!g_ready_head) g_ready_tail = NULL;
        c->ready_next = NULL;
        c->ready_pending = false;
        if (g_ready_pending != 0U) --g_ready_pending;
        if (ctx) {
#ifdef RING_DIAG
            if (ctx->diag.ready_pending != 0U) --ctx->diag.ready_pending;
            ++ctx->diag.ready_answered;
#endif
        }
        --budget;

        if (ring_fill_pending_input(c) != 0) {
            ring_close_conn(c);
            continue;
        }
        int parsed = process_buffer(c);
        if (parsed < 0) {
            ring_close_conn(c);
        } else if (parsed == 0) {
            if (ring_reject_full_pipeline_request(c)) continue;
            if (c->peer_eof && c->pending_input_len == 0U)
                ring_close_conn(c);
            else {
                ring_resume_recv_if_drained(c);
                ring_start_recv(c);
            }
        } else if (parsed == 2) {
            /* The checkpoint hold owns this connection until the write can resume. */
        } else if (c->batch_sensitive) {
            ring_defer_send(c);
        } else {
            if (ring_send_or_defer(c) != 0) ring_close_conn(c);
        }
    }
}

static uint64_t ring_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#ifdef RING_DIAG
static const char *const g_ring_diag_route_names[RING_DIAG_ROUTE_COUNT] = {
    "feed", "post", "create", "like", "other"
};

static const uint64_t g_ring_diag_latency_bounds_ns[
    RING_DIAG_LATENCY_BIN_COUNT - 1] = {
        10000ULL, 25000ULL, 50000ULL, 100000ULL, 250000ULL,
        500000ULL, 1000000ULL, 2000000ULL, 5000000ULL, 10000000ULL,
        20000000ULL, 50000000ULL, 100000000ULL, 200000000ULL,
        500000000ULL, 1000000000ULL, 2000000000ULL, 5000000000ULL,
        10000000000ULL
    };

static void ring_diag_record_stage(Conn *c, unsigned stage,
                                  uint64_t start_ns, uint64_t end_ns) {
    if (!c->diag_response_sampled || start_ns == 0U || end_ns < start_ns ||
        stage >= RING_DIAG_STAGE_COUNT || c->diag_route >= RING_DIAG_ROUTE_COUNT)
        return;
    RingCtx *ctx = ring_ctx(c->ring_index);
    if (!ctx) return;
    uint64_t elapsed_ns = end_ns - start_ns;
    unsigned bin = 0;
    while (bin + 1U < RING_DIAG_LATENCY_BIN_COUNT &&
           elapsed_ns > g_ring_diag_latency_bounds_ns[bin])
        ++bin;
    ++ctx->diag.latency_samples[c->diag_route][stage];
    ++ctx->diag.latency_hist[c->diag_route][stage][bin];
}

static void ring_diag_mark_response_ready(Conn *c, unsigned route) {
    c->diag_response_sampled = ring_diag_should_sample();
    c->diag_route = route < RING_DIAG_ROUTE_COUNT ? route : 4U;
    c->diag_response_ready_ns = c->diag_response_sampled
        ? ring_monotonic_ns() : 0U;
    c->diag_send_queued_ns = 0U;
    c->diag_send_submitted_ns = 0U;
    c->diag_batch_release_seen = false;
    c->diag_first_send_cqe_seen = false;
}

static void ring_diag_send_accept_observed(uint64_t user_data) {
    if (((uintptr_t)user_data & RING_TAG_MASK) != RING_TAG_SEND) return;
    Conn *c = (Conn *)(uintptr_t)(user_data & ~(uint64_t)RING_TAG_MASK);
    if (!c->diag_response_sampled || c->diag_send_submitted_ns != 0U) return;
    uint64_t now_ns = ring_monotonic_ns();
    c->diag_send_submitted_ns = now_ns;
    ring_diag_record_stage(c, RING_DIAG_STAGE_SQE_TO_ACCEPT,
                           c->diag_send_queued_ns, now_ns);
}

static uint64_t ring_diag_hist_quantile(const RingDiagCounters *current,
                                        const RingDiagCounters *previous,
                                        unsigned route, unsigned stage,
                                        unsigned percentile) {
    uint64_t total = 0U;
    for (unsigned bin = 0; bin < RING_DIAG_LATENCY_BIN_COUNT; ++bin) {
        total += current->latency_hist[route][stage][bin] -
                 previous->latency_hist[route][stage][bin];
    }
    if (total == 0U) return 0U;
    uint64_t target = (total * percentile + 99U) / 100U;
    uint64_t seen = 0U;
    for (unsigned bin = 0; bin < RING_DIAG_LATENCY_BIN_COUNT; ++bin) {
        seen += current->latency_hist[route][stage][bin] -
                previous->latency_hist[route][stage][bin];
        if (seen >= target) {
            if (bin + 1U == RING_DIAG_LATENCY_BIN_COUNT) return 10000001ULL;
            return (g_ring_diag_latency_bounds_ns[bin] + 999ULL) / 1000ULL;
        }
    }
    return 0U;
}

static RingDiagCounters ring_diag_collect(void) {
    RingDiagCounters sum = {0};
    for (unsigned i = 0; i < g_ring_count; ++i) {
        RingCtx *ctx = &g_rings[i];
#define RING_DIAG_ADD_SERVER(field) sum.field += ctx->diag.field;
        RING_DIAG_ADD_SERVER(recv_sqe_selected)
        RING_DIAG_ADD_SERVER(recv_sqe_plain)
        RING_DIAG_ADD_SERVER(recv_sqe_enospc)
        RING_DIAG_ADD_SERVER(send_sqe)
        RING_DIAG_ADD_SERVER(recv_cqe_selected)
        RING_DIAG_ADD_SERVER(recv_cqe_plain)
        RING_DIAG_ADD_SERVER(recv_bytes_selected)
        RING_DIAG_ADD_SERVER(recv_bytes_plain)
        RING_DIAG_ADD_SERVER(recv_errors)
        RING_DIAG_ADD_SERVER(recv_enobufs)
        RING_DIAG_ADD_SERVER(recv_eagain)
        RING_DIAG_ADD_SERVER(recv_eintr)
        RING_DIAG_ADD_SERVER(recv_eof)
        RING_DIAG_ADD_SERVER(send_cqe)
        RING_DIAG_ADD_SERVER(send_errors)
        RING_DIAG_ADD_SERVER(send_sqe_enospc)
        RING_DIAG_ADD_SERVER(send_partial_cqe)
        RING_DIAG_ADD_SERVER(send_retry_cqe)
        RING_DIAG_ADD_SERVER(responses_complete)
        RING_DIAG_ADD_SERVER(cq_ready_samples)
        RING_DIAG_ADD_SERVER(cq_ready_total)
        RING_DIAG_ADD_SERVER(rearm_queue_adds)
        RING_DIAG_ADD_SERVER(rearm_waits)
        RING_DIAG_ADD_SERVER(rearm_wait_total_ns)
        RING_DIAG_ADD_SERVER(ready_enqueued)
        RING_DIAG_ADD_SERVER(ready_answered)
#undef RING_DIAG_ADD_SERVER
        if (ctx->diag.cq_ready_max > sum.cq_ready_max)
            sum.cq_ready_max = ctx->diag.cq_ready_max;
        if (ctx->diag.send_pending_peak > sum.send_pending_peak)
            sum.send_pending_peak = ctx->diag.send_pending_peak;
        for (unsigned route = 0; route < RING_DIAG_ROUTE_COUNT; ++route) {
            for (unsigned stage = 0; stage < RING_DIAG_STAGE_COUNT; ++stage) {
                sum.latency_samples[route][stage] +=
                    ctx->diag.latency_samples[route][stage];
                for (unsigned bin = 0; bin < RING_DIAG_LATENCY_BIN_COUNT; ++bin) {
                    sum.latency_hist[route][stage][bin] +=
                        ctx->diag.latency_hist[route][stage][bin];
                }
            }
        }
        if (ctx->diag.rearm_wait_max_ns > sum.rearm_wait_max_ns)
            sum.rearm_wait_max_ns = ctx->diag.rearm_wait_max_ns;
        sum.rearm_pending += ctx->diag.rearm_pending;
        if (ctx->diag.rearm_pending_peak > sum.rearm_pending_peak)
            sum.rearm_pending_peak = ctx->diag.rearm_pending_peak;
        ring_diag ring_counts;
        ring_diag_snapshot(&ctx->io, &ring_counts);
        sum.enter_syscalls += ring_counts.enter_syscalls;
        sum.accepted_recv_sqes += ring_counts.accepted_recv_sqes;
        sum.accepted_send_sqes += ring_counts.accepted_send_sqes;
        sum.accepted_other_sqes += ring_counts.accepted_other_sqes;
    }
    sum.ready_pending = g_ready_pending;
    sum.ready_pending_peak = g_ready_pending_peak;
    return sum;
}

static void ring_diag_report(uint64_t start_ns, uint64_t *next_ns,
                             RingDiagCounters *previous) {
    uint64_t now_ns = ring_monotonic_ns();
    if (now_ns < *next_ns) return;
    RingDiagCounters current = ring_diag_collect();
    uint64_t d_recv_selected = current.recv_sqe_selected - previous->recv_sqe_selected;
    uint64_t d_recv_plain = current.recv_sqe_plain - previous->recv_sqe_plain;
    uint64_t d_recv_enospc = current.recv_sqe_enospc - previous->recv_sqe_enospc;
    uint64_t d_send_sqe = current.send_sqe - previous->send_sqe;
    uint64_t d_recv_cqe_selected = current.recv_cqe_selected - previous->recv_cqe_selected;
    uint64_t d_recv_cqe_plain = current.recv_cqe_plain - previous->recv_cqe_plain;
    uint64_t d_recv_bytes = current.recv_bytes_selected + current.recv_bytes_plain -
        previous->recv_bytes_selected - previous->recv_bytes_plain;
    uint64_t d_recv_errors = current.recv_errors - previous->recv_errors;
    uint64_t d_recv_enobufs = current.recv_enobufs - previous->recv_enobufs;
    uint64_t d_recv_eagain = current.recv_eagain - previous->recv_eagain;
    uint64_t d_recv_eintr = current.recv_eintr - previous->recv_eintr;
    uint64_t d_recv_eof = current.recv_eof - previous->recv_eof;
    uint64_t d_send_cqe = current.send_cqe - previous->send_cqe;
    uint64_t d_send_errors = current.send_errors - previous->send_errors;
    uint64_t d_send_sqe_enospc = current.send_sqe_enospc - previous->send_sqe_enospc;
    uint64_t d_send_partial = current.send_partial_cqe - previous->send_partial_cqe;
    uint64_t d_send_retry = current.send_retry_cqe - previous->send_retry_cqe;
    uint64_t d_responses = current.responses_complete - previous->responses_complete;
    uint64_t d_cq_samples = current.cq_ready_samples - previous->cq_ready_samples;
    uint64_t d_cq_ready = current.cq_ready_total - previous->cq_ready_total;
    uint64_t d_enter = current.enter_syscalls - previous->enter_syscalls;
    uint64_t d_accepted_recv = current.accepted_recv_sqes - previous->accepted_recv_sqes;
    uint64_t d_accepted_send = current.accepted_send_sqes - previous->accepted_send_sqes;
    uint64_t d_accepted_other = current.accepted_other_sqes - previous->accepted_other_sqes;
    uint64_t d_rearm_adds = current.rearm_queue_adds - previous->rearm_queue_adds;
    uint64_t d_rearm_waits = current.rearm_waits - previous->rearm_waits;
    uint64_t d_rearm_wait_ns = current.rearm_wait_total_ns -
        previous->rearm_wait_total_ns;
    uint64_t d_ready_enqueued = current.ready_enqueued - previous->ready_enqueued;
    uint64_t d_ready_answered = current.ready_answered - previous->ready_answered;
    uint64_t elapsed_s = (now_ns - start_ns) / 1000000000ULL;
    uint64_t avg_rearm_us = d_rearm_waits != 0U
        ? d_rearm_wait_ns / d_rearm_waits / 1000U : 0U;
    fprintf(stderr,
            "RING_DIAG t=%llus recv_sqe(selected/plain/enospc)=%llu/%llu/%llu "
            "accepted(recv/send/other)=%llu/%llu/%llu enter=%llu "
            "recv_cqe(selected/plain)=%llu/%llu recv_bytes=%llu "
            "recv_errors(enobufs/eagain/eintr/eof)=%llu/%llu/%llu/%llu/%llu "
            "send(sqe/cqe/errors/enospc/partial/retry)=%llu/%llu/%llu/%llu/%llu/%llu "
            "responses=%llu cq_ready(samples/avg/run_max)=%llu/%.3f/%llu "
            "send_queue_run_peak=%llu "
            "rearm(adds/sqe_queued/avg_wait_us/run_max_wait_us/pending_now/peak_per_ring)="
            "%llu/%llu/%llu/%llu/%llu/%llu "
            "ready(queued/answered/pending/run_peak)=%llu/%llu/%llu/%llu\n",
            (unsigned long long)elapsed_s,
            (unsigned long long)d_recv_selected,
            (unsigned long long)d_recv_plain,
            (unsigned long long)d_recv_enospc,
            (unsigned long long)d_accepted_recv,
            (unsigned long long)d_accepted_send,
            (unsigned long long)d_accepted_other,
            (unsigned long long)d_enter,
            (unsigned long long)d_recv_cqe_selected,
            (unsigned long long)d_recv_cqe_plain,
            (unsigned long long)d_recv_bytes,
            (unsigned long long)d_recv_errors,
            (unsigned long long)d_recv_enobufs,
            (unsigned long long)d_recv_eagain,
            (unsigned long long)d_recv_eintr,
            (unsigned long long)d_recv_eof,
            (unsigned long long)d_send_sqe,
            (unsigned long long)d_send_cqe,
            (unsigned long long)d_send_errors,
            (unsigned long long)d_send_sqe_enospc,
            (unsigned long long)d_send_partial,
            (unsigned long long)d_send_retry,
            (unsigned long long)d_responses,
            (unsigned long long)d_cq_samples,
            d_cq_samples != 0U ? (double)d_cq_ready / (double)d_cq_samples : 0.0,
            (unsigned long long)current.cq_ready_max,
            (unsigned long long)current.send_pending_peak,
            (unsigned long long)d_rearm_adds,
            (unsigned long long)d_rearm_waits,
            (unsigned long long)avg_rearm_us,
            (unsigned long long)(current.rearm_wait_max_ns / 1000U),
            (unsigned long long)current.rearm_pending,
            (unsigned long long)current.rearm_pending_peak,
            (unsigned long long)d_ready_enqueued,
            (unsigned long long)d_ready_answered,
            (unsigned long long)current.ready_pending,
            (unsigned long long)current.ready_pending_peak);
    const RingMemoryDiag *mem = &g_ring_memory_diag;
    uint64_t response_resident = mem->response_active + g_response_pool_count;
    uint64_t response_resident_bytes =
        mem->response_active_bytes + mem->response_pool_bytes;
    fprintf(stderr,
            "RING_MEM t=%llus response(active/peak/active_bytes/active_bytes_peak/pool/pool_bytes/resident/resident_bytes/output_used/output_used_peak/alloc_total/free_total/pool_hits/grow/grow_fail/grow_move)="
            "%llu/%llu/%llu/%llu/%u/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
            "send_sqe(active/peak/duplicate/close_outstanding)=%llu/%llu/%llu/%llu "
            "input(bytes/peak/buffers/peak)=%llu/%llu/%llu/%llu "
            "wal_hold(depth/peak/close_queued)=%llu/%llu/%llu accounting_errors=%llu\n",
            (unsigned long long)elapsed_s,
            (unsigned long long)mem->response_active,
            (unsigned long long)mem->response_active_peak,
            (unsigned long long)mem->response_active_bytes,
            (unsigned long long)mem->response_active_bytes_peak,
            g_response_pool_count,
            (unsigned long long)mem->response_pool_bytes,
            (unsigned long long)response_resident,
            (unsigned long long)response_resident_bytes,
            (unsigned long long)mem->output_used_bytes,
            (unsigned long long)mem->output_used_bytes_peak,
            (unsigned long long)mem->response_alloc_total,
            (unsigned long long)mem->response_free_total,
            (unsigned long long)mem->response_pool_hits,
            (unsigned long long)mem->response_grow_total,
            (unsigned long long)mem->response_grow_fail,
            (unsigned long long)mem->response_grow_move,
            (unsigned long long)mem->sends_active,
            (unsigned long long)mem->sends_active_peak,
            (unsigned long long)mem->send_duplicate,
            (unsigned long long)mem->close_with_send,
            (unsigned long long)mem->input_bytes,
            (unsigned long long)mem->input_bytes_peak,
            (unsigned long long)mem->input_buffers,
            (unsigned long long)mem->input_buffers_peak,
            (unsigned long long)mem->wal_depth,
            (unsigned long long)mem->wal_depth_peak,
            (unsigned long long)mem->close_in_wal,
            (unsigned long long)mem->accounting_errors);
    for (unsigned route = 0; route < RING_DIAG_ROUTE_COUNT; ++route) {
        uint64_t n_ready_sqe = current.latency_samples[route][RING_DIAG_STAGE_READY_TO_SQE] -
                               previous->latency_samples[route][RING_DIAG_STAGE_READY_TO_SQE];
        uint64_t n_sqe_accept = current.latency_samples[route][RING_DIAG_STAGE_SQE_TO_ACCEPT] -
                                previous->latency_samples[route][RING_DIAG_STAGE_SQE_TO_ACCEPT];
        uint64_t n_accept_cqe = current.latency_samples[route][RING_DIAG_STAGE_ACCEPT_TO_FIRST_CQE] -
                                previous->latency_samples[route][RING_DIAG_STAGE_ACCEPT_TO_FIRST_CQE];
        uint64_t n_ready_cqe = current.latency_samples[route][RING_DIAG_STAGE_READY_TO_FIRST_CQE] -
                               previous->latency_samples[route][RING_DIAG_STAGE_READY_TO_FIRST_CQE];
        uint64_t n_complete = current.latency_samples[route][RING_DIAG_STAGE_READY_TO_COMPLETE] -
                              previous->latency_samples[route][RING_DIAG_STAGE_READY_TO_COMPLETE];
        uint64_t n_batch = current.latency_samples[route][RING_DIAG_STAGE_READY_TO_BATCH] -
                           previous->latency_samples[route][RING_DIAG_STAGE_READY_TO_BATCH];
        if (n_ready_sqe + n_sqe_accept + n_accept_cqe + n_ready_cqe + n_complete + n_batch == 0U)
            continue;
        fprintf(stderr,
                "RING_LAT t=%llus route=%s "
                "n(ready_sqe/sqe_accept/accept_cqe/ready_first_cqe/complete/batch)="
                "%llu/%llu/%llu/%llu/%llu/%llu "
                "ready_to_sqe_us(p95/p99)=%llu/%llu "
                "sqe_to_accept_observed_us(p95/p99)=%llu/%llu "
                "accept_to_first_cqe_dispatch_us(p95/p99)=%llu/%llu "
                "ready_to_first_cqe_dispatch_us(p95/p99)=%llu/%llu "
                "ready_to_send_complete_us(p95/p99)=%llu/%llu "
                "ready_to_batch_release_us(p95/p99)=%llu/%llu\n",
                (unsigned long long)elapsed_s,
                g_ring_diag_route_names[route],
                (unsigned long long)n_ready_sqe,
                (unsigned long long)n_sqe_accept,
                (unsigned long long)n_accept_cqe,
                (unsigned long long)n_ready_cqe,
                (unsigned long long)n_complete,
                (unsigned long long)n_batch,
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_SQE, 95U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_SQE, 99U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_SQE_TO_ACCEPT, 95U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_SQE_TO_ACCEPT, 99U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_ACCEPT_TO_FIRST_CQE, 95U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_ACCEPT_TO_FIRST_CQE, 99U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_FIRST_CQE, 95U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_FIRST_CQE, 99U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_COMPLETE, 95U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_COMPLETE, 99U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_BATCH, 95U),
                (unsigned long long)ring_diag_hist_quantile(
                    &current, previous, route, RING_DIAG_STAGE_READY_TO_BATCH, 99U));
    }
    *previous = current;
    *next_ns = now_ns + 10000000000ULL;
}
#endif

static unsigned ring_pending_total(void) {
    unsigned total = 0;
    for (unsigned i = 0; i < g_ring_count; ++i) {
        total += ring_cq_ready(&g_rings[i].io);
    }
    return total;
}

static bool ring_accept_cqe_pending(void) {
    for (unsigned i = 0; i < g_ring_count; ++i) {
        RingCtx *ctx = &g_rings[i];
        unsigned ready = ring_cq_ready(&ctx->io);
        for (unsigned offset = 0; offset < ready; ++offset) {
            struct io_uring_cqe *cqe = ring_peek_cqe_at(&ctx->io, offset);
            if (cqe && ((uintptr_t)cqe->user_data & RING_TAG_MASK) == RING_TAG_ACCEPT) {
                return true;
            }
        }
    }
    return false;
}

static void ring_consume_eventfd(RingCtx *ctx) {
    uint64_t count;
    for (;;) {
        ssize_t n = read(ctx->event_fd, &count, sizeof(count));
        if (n == (ssize_t)sizeof(count)) continue;
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

static int ring_wait_for_batch(int retry_timeout_ms) {
    struct epoll_event events[RING_POOL_SIZE];
    int ready;
    do {
        ready = epoll_wait(g_ring_epoll, events, RING_POOL_SIZE, retry_timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) return -errno;
    if (ready == 0) return 0;
    for (int i = 0; i < ready; ++i) {
        ring_consume_eventfd((RingCtx *)events[i].data.ptr);
    }

    uint64_t deadline = ring_monotonic_ns() + 1000000ULL;
    while (ring_pending_total() < RING_CQ_BATCH && !ring_accept_cqe_pending()) {
        uint64_t now = ring_monotonic_ns();
        if (now == 0 || now >= deadline) break;
        uint64_t remaining = deadline - now;
        int timeout_ms = (int)((remaining + 999999ULL) / 1000000ULL);
        if (timeout_ms < 1) timeout_ms = 1;
        ready = epoll_wait(g_ring_epoll, events, RING_POOL_SIZE, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (ready == 0) continue;
        for (int i = 0; i < ready; ++i) {
            ring_consume_eventfd((RingCtx *)events[i].data.ptr);
        }
    }
    return 0;
}

static void close_ring_pool(void) {
    atomic_store_explicit(&g_wal_wakeup_fd, -1, memory_order_release);
    for (unsigned i = 0; i < RING_POOL_SIZE; ++i) {
        if (g_rings[i].buffers_registered) {
            (void)ring_unregister_buf_ring(&g_rings[i].io, g_rings[i].buf_group);
        }
        if (g_rings[i].files_registered) {
            (void)ring_unregister_files(&g_rings[i].io);
        }
        ring_close(&g_rings[i].io);
        if (g_rings[i].event_fd >= 0) close(g_rings[i].event_fd);
        free(g_rings[i].buf_ring);
        free(g_rings[i].buf_storage);
    }
    memset(g_rings, 0, sizeof(g_rings));
    for (unsigned i = 0; i < RING_POOL_SIZE; ++i) g_rings[i].event_fd = -1;
    g_ring_count = 0;
    g_ring_file_slots = 0;
    g_direct_files = false;
}

static int init_ring_pool(unsigned pool_size) {
    for (unsigned i = 0; i < RING_POOL_SIZE; ++i) g_rings[i].event_fd = -1;
    struct rlimit lim;
    unsigned slots = RING_FILE_LIMIT;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_max != RLIM_INFINITY) {
        rlim_t limit = lim.rlim_max;
        if (limit <= 64) slots = 0;
        else {
            limit -= 64;
            if (limit < (rlim_t)slots) slots = (unsigned)limit;
        }
    }

    if (slots >= 1024U) {
        bool direct_ready = true;
        for (unsigned i = 0; i < pool_size; ++i) {
            int rc = ring_init(&g_rings[i].io, 2048, RING_CQ_ENTRIES);
            if (rc != 0) {
                direct_ready = false;
                break;
            }
            rc = ring_register_files_sparse(&g_rings[i].io, slots);
            if (rc != 0) {
                direct_ready = false;
                break;
            }
            g_rings[i].files_registered = true;
            rc = ring_buffers_init(&g_rings[i], i);
            if (rc != 0) {
                fprintf(stderr, "ring %u buffer ring unavailable: %s\n",
                        i, strerror(-rc));
                direct_ready = false;
                break;
            }
        }
        if (direct_ready) {
            g_ring_count = pool_size;
            g_ring_file_slots = slots;
            g_direct_files = true;
            return 0;
        }
        fprintf(stderr, "direct file rings unavailable; using one ordinary-FD ring\n");
        close_ring_pool();
    }

    int rc = ring_init(&g_rings[0].io, 2048, RING_CQ_ENTRIES);
    if (rc != 0) return rc;
    g_ring_count = 1;
    g_ring_file_slots = 0;
    return 0;
}

static int run_ring_server(const int *listeners, unsigned listener_count) {
    if (!listeners || listener_count == 0U ||
        listener_count > RING_ACCEPT_LISTENER_COUNT) return -EINVAL;
    g_listener_count = listener_count;
    for (unsigned i = 0; i < listener_count; ++i)
        g_listener_fds[i] = listeners[i];
    g_ring_epoll = epoll_create1(EPOLL_CLOEXEC);
    if (g_ring_epoll < 0) {
        fprintf(stderr, "could not create ring epoll: %s\n", strerror(errno));
        return -1;
    }
    bool defer_taskrun_active = false;
    for (unsigned i = 0; i < g_ring_count; ++i) {
        RingCtx *ctx = &g_rings[i];
        if (ring_defer_taskrun_enabled(&ctx->io)) defer_taskrun_active = true;
        ctx->accept_multishot = true;
#ifdef IORING_RECV_MULTISHOT
        ctx->recv_multishot_enabled = g_recv_multishot && g_direct_files &&
                                      ctx->buffers_registered;
#else
        ctx->recv_multishot_enabled = false;
#endif
        ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (ctx->event_fd < 0) {
            fprintf(stderr, "could not create ring eventfd %u: %s\n", i, strerror(errno));
            return -1;
        }
        if (i == 0U) {
            atomic_store_explicit(&g_wal_wakeup_fd, ctx->event_fd,
                                  memory_order_release);
        }
        int event_rc = ring_register_eventfd(&ctx->io, ctx->event_fd);
        if (event_rc != 0) {
            fprintf(stderr, "could not register ring eventfd %u: %s\n",
                    i, strerror(-event_rc));
            return -1;
        }
#ifdef RING_DIAG
        ring_diag_set_send_accept_observed_callback(
            &ctx->io, ring_diag_send_accept_observed);
#endif
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = ctx;
        if (epoll_ctl(g_ring_epoll, EPOLL_CTL_ADD, ctx->event_fd, &ev) != 0) {
            fprintf(stderr, "could not watch ring eventfd %u: %s\n", i, strerror(errno));
            return -1;
        }
    }
    if (ring_schedule_accept() != 0) {
        fprintf(stderr, "could not prime io_uring accept\n");
        return -1;
    }
    for (unsigned i = 0; i < g_ring_count; ++i) {
        RingCtx *ctx = &g_rings[i];
        int submit = ring_submit(&ctx->io);
        if (submit < 0 && submit != -EAGAIN && submit != -ENOMEM) {
            fprintf(stderr, "io_uring %u initial submit failed: %s\n",
                    i, strerror(-submit));
            return -1;
        }
    }
    fprintf(stderr, "io_uring %sready (%u rings, %u file slots/ring, %u listeners, defer_taskrun=%s, recv_multishot=%s)\n",
            g_direct_files ? "direct " : "", g_ring_count, g_ring_file_slots,
            g_listener_count, defer_taskrun_active ? "on" : "off",
            g_rings[0].recv_multishot_enabled ? "on" : "off");
    g_ring_active = true;
#ifdef RING_DIAG
    RingDiagCounters diag_previous = {0};
    uint64_t diag_start_ns = ring_monotonic_ns();
    uint64_t diag_next_ns = diag_start_ns + 10000000000ULL;
#endif
    unsigned dispatch_start = 0;
    for (;;) {
        wal_checkpoint_fallback_if_needed();
        (void)ring_schedule_accept();
        for (unsigned i = 0; i < g_ring_count; ++i) {
            ring_refill_cancels(&g_rings[i]);
            ring_refill_rearms(&g_rings[i]);
            ring_refill_sends(&g_rings[i]);
        }
        bool retry_submit = false;
        for (unsigned i = 0; i < g_ring_count; ++i) {
            int submit_rc = ring_submit(&g_rings[i].io);
            if (submit_rc < 0 && submit_rc != -EAGAIN && submit_rc != -ENOMEM) {
                fprintf(stderr, "io_uring %u submit failed: %s\n",
                        i, strerror(-submit_rc));
                return -1;
            }
            int overflow_pending = ring_cq_overflow_pending(&g_rings[i].io);
            if (overflow_pending < 0) {
                fprintf(stderr, "io_uring %u CQ overflow check failed: %s\n",
                        i, strerror(-overflow_pending));
                return -1;
            }
            if (overflow_pending != 0) {
                int flush_rc = ring_flush_cq_overflow(&g_rings[i].io);
                if (flush_rc < 0 && flush_rc != -EAGAIN && flush_rc != -ENOMEM) {
                    fprintf(stderr, "io_uring %u CQ overflow flush failed: %s\n",
                            i, strerror(-flush_rc));
                    return -1;
                }
                overflow_pending = ring_cq_overflow_pending(&g_rings[i].io);
                if (overflow_pending < 0) {
                    fprintf(stderr, "io_uring %u CQ overflow recheck failed: %s\n",
                            i, strerror(-overflow_pending));
                    return -1;
                }
                if (flush_rc < 0 || overflow_pending != 0) retry_submit = true;
            }
            if (ring_sq_pending(&g_rings[i].io) != 0U ||
                g_rings[i].accept_sqe_retry || g_rings[i].rearm_sqe_retry ||
                g_rings[i].send_head || g_rings[i].cancel_sqe_retry ||
                g_rings[i].cancel_head) {
                retry_submit = true;
            }
        }
        if (ring_pending_total() == 0U && !g_ready_head) {
            int wait_timeout_ms = (retry_submit || defer_taskrun_active) ? 1 : -1;
            int wait_rc = ring_wait_for_batch(wait_timeout_ms);
            if (wait_rc != 0) {
                if (wait_rc == -EINTR) continue;
                fprintf(stderr, "ring epoll wait failed: %s\n", strerror(-wait_rc));
                return -1;
            }
        }
        g_recovery_draining = ring_recovery_work_pending();
        unsigned dispatch_first = dispatch_start;
        dispatch_start = (dispatch_start + 1U) % g_ring_count;
        unsigned dispatch_order[RING_POOL_SIZE];
        unsigned dispatch_ready[RING_POOL_SIZE];
        for (unsigned offset = 0; offset < g_ring_count; ++offset) {
            unsigned ring_index = dispatch_first + offset;
            if (ring_index >= g_ring_count) ring_index -= g_ring_count;
            dispatch_order[offset] = ring_index;
            dispatch_ready[offset] = ring_cq_ready(&g_rings[ring_index].io);
        }
        for (unsigned i = 1; i < g_ring_count; ++i) {
            unsigned ring_index = dispatch_order[i];
            unsigned ready = dispatch_ready[i];
            unsigned j = i;
            while (j > 0U && dispatch_ready[j - 1U] < ready) {
                dispatch_order[j] = dispatch_order[j - 1U];
                dispatch_ready[j] = dispatch_ready[j - 1U];
                --j;
            }
            dispatch_order[j] = ring_index;
            dispatch_ready[j] = ready;
        }
        unsigned recovery_cq_budget = RING_CQ_DRAIN_BUDGET;
        for (unsigned dispatch_offset = 0; dispatch_offset < g_ring_count; ++dispatch_offset) {
            unsigned ring_index = dispatch_order[dispatch_offset];
            RingCtx *ctx = &g_rings[ring_index];
            unsigned available = ring_cq_ready(&ctx->io);
#ifdef RING_DIAG
            ++ctx->diag.cq_ready_samples;
            ctx->diag.cq_ready_total += available;
            if (available > ctx->diag.cq_ready_max)
                ctx->diag.cq_ready_max = available;
#endif
            if (available != 0U && recovery_cq_budget != 0U) {
                unsigned batch_limit = RING_CQ_BATCH;
                if (g_recovery_draining) {
                    unsigned rings_left = g_ring_count - dispatch_offset;
                    unsigned fair_share = recovery_cq_budget / rings_left;
                    if (fair_share == 0U) fair_share = 1U;
                    batch_limit = fair_share < RING_CQ_DRAIN_PER_RING
                        ? fair_share : RING_CQ_DRAIN_PER_RING;
                }
                if (batch_limit > recovery_cq_budget)
                    batch_limit = recovery_cq_budget;
                unsigned batch = available < batch_limit ? available : batch_limit;
                if (batch != 0U) {
                    for (unsigned offset = 0; offset < batch; ++offset) {
                        struct io_uring_cqe *cqe = ring_peek_cqe_at(&ctx->io, offset);
                        if (!cqe) {
                            fprintf(stderr, "io_uring CQ peek failed\n");
                            return -1;
                        }
                        ring_dispatch_cqe(ctx, ring_index, cqe);
                    }
                    if (ring_cq_advance(&ctx->io, batch) != 0) {
                        fprintf(stderr, "io_uring CQ advance failed\n");
                        return -1;
                    }
                    recovery_cq_budget -= batch;
                }
            }
            ring_refill_rearms(ctx);
        }
        g_recovery_draining = false;
        ring_process_ready(RING_READY_ANSWER_BUDGET);
        ring_resume_wal_writes();
        int finish_rc = ring_finish_batch();
        if (finish_rc != 0) {
            fprintf(stderr, "could not finish SQLite write batch; stopping server\n");
            return -1;
        }
#ifdef RING_DIAG
        ring_diag_report(diag_start_ns, &diag_next_ns, &diag_previous);
#endif
    }
}

static void on_signal(int sig) {
    (void)sig;
    _exit(0);
}

int main(void) {
    const char *secret = getenv("JWT_SECRET");
    if (secret && *secret) g_secret = secret;
    const unsigned ring_pool_size = 6;
    g_secret_len = strlen(g_secret);
    int pcache_rc = page_cache_wrapper_install();
    if (pcache_rc != SQLITE_OK) {
        fprintf(stderr, "SQLite page-cache wrapper install failed: %s\n",
                sqlite3_errstr(pcache_rc));
        return 1;
    }
    if (init_db() != 0) return 1;
    (void)start_wal_checkpoint_thread();
    const char *host = getenv("HOST");
    const char *port = getenv("PORT");
    if (!host || !*host) host = "127.0.0.1";
    if (!port || !*port) port = "3000";
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur < lim.rlim_max) {
        lim.rlim_cur = lim.rlim_max;
        setrlimit(RLIMIT_NOFILE, &lim);
    }
    int ring_rc = init_ring_pool(ring_pool_size);
    bool use_ring = ring_rc == 0;
    if (!use_ring) {
        fprintf(stderr, "io_uring unavailable (%s); using epoll\n", strerror(-ring_rc));
    }
    int listeners[RING_ACCEPT_LISTENER_COUNT];
    unsigned listener_count = 0;
    if (use_ring && g_ring_count > 1U) {
        unsigned desired = g_ring_count < RING_ACCEPT_LISTENER_COUNT
            ? g_ring_count : RING_ACCEPT_LISTENER_COUNT;
        if (desired > 1U &&
            make_listener_pool(host, port, desired, listeners) == 0) {
            listener_count = desired;
        } else {
            fprintf(stderr,
                    "SO_REUSEPORT listener pool unavailable; using one listener\n");
        }
    }
    if (listener_count == 0U) {
        int listener = make_listener(host, port, true, false);
        if (listener >= 0) {
            listeners[0] = listener;
            listener_count = 1U;
        }
    }
    if (listener_count == 0U) {
        perror("listen");
        if (use_ring) close_ring_pool();
        return 1;
    }
    g_started = time(NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "listening on %s:%s using %s (%s)\n", host, port,
            getenv("SQLITE_PATH") ? getenv("SQLITE_PATH") : "seed/feed.db",
            use_ring ? "io_uring" : "epoll");
    if (use_ring) {
        int rc = run_ring_server(listeners, listener_count);
        for (unsigned i = 0; i < listener_count; ++i) close(listeners[i]);
        if (g_ring_epoll >= 0) close(g_ring_epoll);
        close_ring_pool();
        sqlite3_close(g_db);
        return rc == 0 ? 0 : 1;
    }
    g_epoll = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll < 0) {
        perror("epoll_create1");
        close(listeners[0]);
        return 1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = g_listener_tag;
    if (epoll_ctl(g_epoll, EPOLL_CTL_ADD, listeners[0], &ev) != 0) {
        perror("epoll_ctl listener");
        close(listeners[0]);
        close(g_epoll);
        return 1;
    }
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        wal_checkpoint_fallback_if_needed();
        int n = epoll_wait(g_epoll, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            void *data = events[i].data.ptr;
            if (data == g_listener_tag) {
                accept_all(listeners[0]);
                continue;
            }
            Conn *c = data;
            if (!c) continue;
            uint32_t e = events[i].events;
            if (c->output_len > c->output_pos) {
                if (e & (EPOLLOUT | EPOLLIN)) flush_conn(c);
                continue;
            }
            if (e & (EPOLLERR | EPOLLHUP)) {
                close_conn(c);
                continue;
            }
            if (e & EPOLLIN) read_conn(c);
        }
    }
    close(listeners[0]);
    return 1;
}
