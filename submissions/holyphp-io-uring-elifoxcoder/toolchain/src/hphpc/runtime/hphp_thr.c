/* hphp_thr.c — threading primitives for HolyPHP.
 *
 *   thr_spawn(closure, [args])  -> thread id (detached; result via channels)
 *   thr_join(tid)               -> wait for a spawned thread (when retained)
 *   thr_current()               -> int id of the calling thread
 *   thr_sleep_ms(ms)            -> sleep
 *   thr_cpu_count()             -> hardware concurrency
 *
 *   chan_new(capacity)          -> channel id (0 = unbuffered/rendezvous)
 *   chan_send(ch, value)        -> blocks until a receiver takes it (or slot)
 *   chan_recv(ch)               -> blocks; returns the sent value
 *   chan_try_send(ch, value)    -> false instead of blocking
 *   chan_try_recv(ch)           -> value or false
 *   chan_close(ch)              -> further sends fail; recv drains then false
 *
 *   mutex_new() / mutex_lock / mutex_trylock / mutex_unlock / mutex_free
 *   barrier_new(n) / barrier_wait(b) -> index of the thread that arrived last
 *   atomic_new(init) / atomic_get / atomic_set / atomic_add / atomic_cas
 *
 * Design notes:
 *  - Values cross threads as small hvals (null/int/float/bool/string/arrays).
 *    Strings and arrays are heap objects, so a pointer copy is enough; the
 *    runtime treats them as immortal (rc fields are not adjusted), matching
 *    how closures capture values today.
 *  - A worker running HolyPHP code needs its own exception/closure state,
 *    so the interpreter's globals in hphp_rt.c are thread-local.
 *  - Channel send/recv is a condvar handshake: unbuffered channels get true
 *    rendezvous semantics (sender hands value directly, like Go).
 *  - Timed waits use pthread_cond_timedwait with a wall-clock abstime.
 */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "hphp_rt.h"

/* pthreads everywhere: winpthreads on MinGW, native elsewhere */
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static hval thr_null(void) { return hp_null; }
static hval thr_bool(bool b) { return hp_of_bool(b); }

/* Dynamic values must also satisfy the channel's transfer contract. Containers
 * are scanned recursively; object identity and closures require an explicit
 * statically proven ownership/Sync path and cannot pass through an erased hval. */
static bool thr_value_transferable(hval v, int depth) {
    if (depth > 128) return false;
    switch (v.tag) {
    case HV_NULL: case HV_INT: case HV_FLOAT: case HV_BOOL: case HV_STR:
        return true;
    case HV_ARR:
        if (!v.u.a) return true;
        for (size_t i = 0; i < v.u.a->len; i++) {
            if (!thr_value_transferable(v.u.a->vals[i], depth + 1)) return false;
            if (v.u.a->is_map && v.u.a->keys &&
                !thr_value_transferable(v.u.a->keys[i], depth + 1)) return false;
        }
        return true;
    case HV_OBJ: case HV_CLO: return false;
    }
    return false;
}

static int64_t thr_now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* wall-clock abstime ms from now (reserved for timed channel ops) */
static void thr_abstime_ms(struct timespec *ts, int64_t ms) {
    int64_t target = thr_now_ms() + ms;
    ts->tv_sec = (time_t)(target / 1000);
    ts->tv_nsec = (long)(target % 1000) * 1000000L;
}

/* ------------------------------------------------------------------ */
/* threads                                                             */
/* ------------------------------------------------------------------ */

typedef struct ThrRec {
    int64_t id;
    bool joined;                       /* false until join has consumed it */
    pthread_t pt;
    pthread_t creator;
    hclosure *clo;                     /* the spawned closure */
} ThrRec;

static ThrRec *g_thr;
static size_t g_thr_len, g_thr_cap;
static int64_t g_thr_next = 1;
static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;

static ThrRec *thr_rec(int64_t id) {
    for (size_t i = 0; i < g_thr_len; i++)
        if (g_thr[i].id == id) return &g_thr[i];
    return NULL;
}

typedef struct {
    hclosure *clo;
    hval *args;
    int nargs;
    int64_t self_id;
} ThrBoot;

static void *thr_trampoline(void *p) {
    ThrBoot b = *(ThrBoot *)p;
    free(p);
    hval r = hp_closure_call(b.clo, b.nargs, b.args);
    (void)r;                           /* results travel via channels */
    free(b.args);
    /* The record owns a strong closure reference until its creator joins.
     * Do not release it here: the closure's RC and global cycle registry are
     * owned by the creator thread and are not worker-thread safe. */
    return NULL;
}

hval hpbi_thr_spawn(hval clo, hval args) {
    if (clo.tag != HV_CLO || !clo.u.c) return thr_bool(false);
    hval packed[64];
    int n = 0;
    if (args.tag == HV_ARR && args.u.a) {
        int64_t an = hp_arr_len(args.u.a);
        if (an > 64) an = 64;
        for (int64_t i = 0; i < an; i++)
            packed[n++] = hp_arr_get(args.u.a, hp_of_int(i));
    }
    ThrBoot *boot = (ThrBoot *)malloc(sizeof(ThrBoot));
    if (!boot) return thr_bool(false);
    boot->clo = clo.u.c;
    boot->nargs = n;
    if (n) {
        boot->args = (hval *)malloc((size_t)n * sizeof(hval));
        if (!boot->args) { free(boot); return thr_bool(false); }
        memcpy(boot->args, packed, (size_t)n * sizeof(hval));
    } else {
        boot->args = NULL;
    }

    /* Keep the closure alive after the caller releases its temporary value.
     * The increment and eventual decrement happen on the creator thread. */
    hp_val_rc_inc(hp_of_clo(clo.u.c));
    pthread_mutex_lock(&g_thr_mu);
    if (g_thr_len == g_thr_cap) {
        g_thr_cap = g_thr_cap ? g_thr_cap * 2 : 16;
        g_thr = (ThrRec *)realloc(g_thr, g_thr_cap * sizeof(ThrRec));
        if (!g_thr) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    ThrRec *rec = &g_thr[g_thr_len];
    rec->id = g_thr_next++;
    rec->joined = false;
    rec->creator = pthread_self();
    rec->clo = clo.u.c;
    int64_t id = rec->id;
    boot->self_id = id;
    hp_runtime_thread_starting();
    if (pthread_create(&rec->pt, NULL, thr_trampoline, boot) != 0) {
        g_thr_len--;
        pthread_mutex_unlock(&g_thr_mu);
        free(boot->args);
        free(boot);
        hp_val_rc_dec(hp_of_clo(clo.u.c));
        return thr_bool(false);
    }
    pthread_mutex_unlock(&g_thr_mu);
    return hp_of_int(id);
}

hval hpbi_thr_join(hval tid) {
    int64_t id = hp_val_to_int(tid);
    pthread_mutex_lock(&g_thr_mu);
    ThrRec *rec = thr_rec(id);
    pthread_t pt;
    pthread_t creator;
    bool have = rec && !rec->joined;
    if (have) { pt = rec->pt; creator = rec->creator; rec->joined = true; }
    pthread_mutex_unlock(&g_thr_mu);
    if (!have) return thr_bool(false);
    pthread_join(pt, NULL);
    hclosure *release_clo = NULL;
    pthread_mutex_lock(&g_thr_mu);
    for (size_t i = 0; i < g_thr_len; i++)
        if (g_thr[i].id == id) {
            if (pthread_equal(creator, pthread_self()))
                release_clo = g_thr[i].clo;
            g_thr[i] = g_thr[--g_thr_len];
            break;
        }
    pthread_mutex_unlock(&g_thr_mu);
    /* A non-creator join cannot safely touch the creator's non-atomic heap
     * counts. In that uncommon case the retained closure is leaked instead
     * of being released from the wrong thread. */
    if (release_clo) hp_val_rc_dec(hp_of_clo(release_clo));
    return thr_bool(true);
}

hval hpbi_thr_current(void) {
    pthread_mutex_lock(&g_thr_mu);
    int64_t id = 0;
    pthread_t self = pthread_self();
    for (size_t i = 0; i < g_thr_len; i++)
        if (pthread_equal(g_thr[i].pt, self)) { id = g_thr[i].id; break; }
    pthread_mutex_unlock(&g_thr_mu);
    return hp_of_int(id);              /* main thread = 0 */
}

hval hpbi_thr_sleep_ms(hval ms) {
    int64_t v = hp_val_to_int(ms);
    if (v < 0) v = 0;
#ifdef _WIN32
    Sleep((DWORD)v);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(v / 1000);
    ts.tv_nsec = (long)(v % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
    return thr_null();
}

hval hpbi_thr_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return hp_of_int((int64_t)si.dwNumberOfProcessors);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return hp_of_int(n > 0 ? (int64_t)n : 1);
#endif
}

hval hpbi_thr_id(void) {
#ifdef _WIN32
    return hp_of_int((int64_t)GetCurrentThreadId());
#else
    return hp_of_int((int64_t)pthread_self());
#endif
}

/* ------------------------------------------------------------------ */
/* channels (Go-style; buffered and rendezvous)                        */
/* ------------------------------------------------------------------ */

typedef struct ChanWaiter {
    hval value;                        /* sender: value to hand over */
    bool done;                         /* handshake completed */
    struct ChanWaiter *next;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} ChanWaiter;

typedef struct ChanRec {
    int64_t id;
    int64_t cap, len, head, tail;      /* ring buffer (cap 0 = rendezvous) */
    hval *buf;
    bool closed;
    ChanWaiter *sendq, *recvq;         /* FIFO waiter queues */
    pthread_mutex_t mu;
} ChanRec;

static ChanRec *g_chan;
static size_t g_chan_len, g_chan_cap;
static int64_t g_chan_next = 1;
static pthread_mutex_t g_chan_mu = PTHREAD_MUTEX_INITIALIZER;

static ChanRec *chan_rec(int64_t id) {
    for (size_t i = 0; i < g_chan_len; i++)
        if (g_chan[i].id == id) return &g_chan[i];
    return NULL;
}

static ChanWaiter *chan_waiter_new(hval v) {
    ChanWaiter *w = (ChanWaiter *)calloc(1, sizeof(ChanWaiter));
    if (!w) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    w->value = v;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    return w;
}

static void chan_waiter_free(ChanWaiter *w) {
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    free(w);
}

/* pop from a FIFO waiter queue */
static ChanWaiter *chan_pop(ChanWaiter **q) {
    ChanWaiter *w = *q;
    if (w) *q = w->next;
    return w;
}

hval hpbi_chan_new(hval cap) {
    int64_t c = hp_val_to_int(cap);
    if (c < 0) c = 0;
    pthread_mutex_lock(&g_chan_mu);
    if (g_chan_len == g_chan_cap) {
        g_chan_cap = g_chan_cap ? g_chan_cap * 2 : 8;
        g_chan = (ChanRec *)realloc(g_chan, g_chan_cap * sizeof(ChanRec));
        if (!g_chan) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    ChanRec *ch = &g_chan[g_chan_len++];
    memset(ch, 0, sizeof *ch);
    ch->id = g_chan_next++;
    ch->cap = c;
    if (c > 0) {
        ch->buf = (hval *)calloc((size_t)c, sizeof(hval));
        if (!ch->buf) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    pthread_mutex_init(&ch->mu, NULL);
    int64_t id = ch->id;
    pthread_mutex_unlock(&g_chan_mu);
    return hp_of_int(id);
}

/* send: rendezvous handoff or ring-buffer slot; blocks until accepted */
static bool chan_send_impl(int64_t id, hval v, bool block) {
    pthread_mutex_lock(&g_chan_mu);
    ChanRec *ch = chan_rec(id);
    if (!ch) { pthread_mutex_unlock(&g_chan_mu); return false; }
    pthread_mutex_lock(&ch->mu);
    pthread_mutex_unlock(&g_chan_mu);

    if (ch->closed) { pthread_mutex_unlock(&ch->mu); return false; }

    /* fast path 1: a waiting receiver -> direct handoff */
    ChanWaiter *rw = chan_pop(&ch->recvq);
    if (rw) {
        rw->value = v;
        rw->done = true;
        pthread_mutex_lock(&rw->mu);
        pthread_cond_signal(&rw->cv);
        pthread_mutex_unlock(&rw->mu);
        pthread_mutex_unlock(&ch->mu);
        return true;
    }
    /* fast path 2: buffered with room */
    if (ch->cap > 0 && ch->len < ch->cap) {
        ch->buf[ch->tail] = v;
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->len++;
        pthread_mutex_unlock(&ch->mu);
        return true;
    }
    if (!block) { pthread_mutex_unlock(&ch->mu); return false; }

    /* slow path: queue as a sender and wait for a receiver */
    ChanWaiter *w = chan_waiter_new(v);
    w->next = NULL;
    if (!ch->sendq) { ch->sendq = w; }
    else {
        ChanWaiter *t = ch->sendq;
        while (t->next) t = t->next;
        t->next = w;
    }
    pthread_mutex_lock(&w->mu);
    pthread_mutex_unlock(&ch->mu);
    while (!w->done) pthread_cond_wait(&w->cv, &w->mu);
    pthread_mutex_unlock(&w->mu);
    bool ok = w->done;                 /* closed-while-waiting still hands over */
    chan_waiter_free(w);
    return ok;
}

/* receive: ring buffer, then queued senders (direct handoff) */
static bool chan_recv_impl(int64_t id, hval *out, bool block) {
    pthread_mutex_lock(&g_chan_mu);
    ChanRec *ch = chan_rec(id);
    if (!ch) { pthread_mutex_unlock(&g_chan_mu); return false; }
    pthread_mutex_lock(&ch->mu);
    pthread_mutex_unlock(&g_chan_mu);

    for (;;) {
        if (ch->len > 0) {
            *out = ch->buf[ch->head];
            ch->head = (ch->head + 1) % ch->cap;
            ch->len--;
            /* room freed: move one queued sender into the buffer */
            ChanWaiter *sw = chan_pop(&ch->sendq);
            if (sw) {
                ch->buf[ch->tail] = sw->value;
                ch->tail = (ch->tail + 1) % ch->cap;
                ch->len++;
                sw->done = true;
                pthread_mutex_lock(&sw->mu);
                pthread_cond_signal(&sw->cv);
                pthread_mutex_unlock(&sw->mu);
                chan_waiter_free(sw);
            }
            pthread_mutex_unlock(&ch->mu);
            return true;
        }
        /* empty: direct handoff from a queued sender */
        ChanWaiter *sw = chan_pop(&ch->sendq);
        if (sw) {
            *out = sw->value;
            sw->done = true;
            pthread_mutex_lock(&sw->mu);
            pthread_cond_signal(&sw->cv);
            pthread_mutex_unlock(&sw->mu);
            chan_waiter_free(sw);
            pthread_mutex_unlock(&ch->mu);
            return true;
        }
        if (ch->closed) { pthread_mutex_unlock(&ch->mu); return false; }
        if (!block) { pthread_mutex_unlock(&ch->mu); return false; }

        /* wait for a sender / close */
        ChanWaiter w;
        memset(&w, 0, sizeof w);
        pthread_mutex_init(&w.mu, NULL);
        pthread_cond_init(&w.cv, NULL);
        w.next = NULL;
        if (!ch->recvq) { ch->recvq = &w; }
        else {
            ChanWaiter *t = ch->recvq;
            while (t->next) t = t->next;
            t->next = &w;
        }
        pthread_mutex_lock(&w.mu);
        pthread_mutex_unlock(&ch->mu);
        while (!w.done) pthread_cond_wait(&w.cv, &w.mu);
        pthread_mutex_unlock(&w.mu);
        pthread_mutex_lock(&ch->mu);
        /* remove ourselves from the queue (we may have been woken for
         * handoff and already unlinked, or left queued by close) */
        ChanWaiter **pp = &ch->recvq;
        while (*pp && *pp != &w) pp = &(*pp)->next;
        if (*pp) *pp = w.next;
        pthread_mutex_destroy(&w.mu);
        pthread_cond_destroy(&w.cv);
        *out = w.value;
        /* loop: a closed check applies before returning */
        if (w.done && w.value.tag != HV_NULL) {
            pthread_mutex_unlock(&ch->mu);
            return true;
        }
        /* value may legitimately be null; close flag decides */
        if (ch->closed && ch->len == 0 && !ch->sendq) {
            pthread_mutex_unlock(&ch->mu);
            return false;
        }
    }
}

hval hpbi_chan_send(hval idv, hval v) {
    if (!thr_value_transferable(v, 0)) {
        hp_val_rc_dec(v);
        hp_throw_str("non-Sync value sent through channel");
        return thr_null();
    }
    bool ok = chan_send_impl(hp_val_to_int(idv), v, true);
    if (!ok) {
        hp_val_rc_dec(v); /* moved message is consumed even on a closed channel */
        hp_throw_str("send on closed or unknown channel");
        return thr_null();
    }
    return thr_bool(true);
}

hval hpbi_chan_recv(hval idv) {
    hval out = hp_null;
    if (!chan_recv_impl(hp_val_to_int(idv), &out, true)) return hp_of_bool(false);
    return out;
}

hval hpbi_chan_try_send(hval idv, hval v) {
    if (!thr_value_transferable(v, 0)) {
        hp_val_rc_dec(v);
        hp_throw_str("non-Sync value sent through channel");
        return thr_bool(false);
    }
    bool ok = chan_send_impl(hp_val_to_int(idv), v, false);
    if (!ok) hp_val_rc_dec(v);
    return thr_bool(ok);
}

hval hpbi_chan_try_recv(hval idv) {
    hval out = hp_null;
    if (!chan_recv_impl(hp_val_to_int(idv), &out, false))
        return hp_of_bool(false);
    return out;
}

hval hpbi_chan_close(hval idv) {
    pthread_mutex_lock(&g_chan_mu);
    ChanRec *ch = chan_rec(hp_val_to_int(idv));
    if (!ch) { pthread_mutex_unlock(&g_chan_mu); return thr_bool(false); }
    pthread_mutex_lock(&ch->mu);
    ch->closed = true;
    /* wake every queued receiver so they observe closed and return false */
    ChanWaiter *w = ch->recvq;
    while (w) {
        w->done = true;
        pthread_mutex_lock(&w->mu);
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);
        w = w->next;
    }
    ch->recvq = NULL;
    /* queued senders still get their handoff: leave sendq untouched */
    pthread_mutex_unlock(&ch->mu);
    pthread_mutex_unlock(&g_chan_mu);
    return thr_bool(true);
}

/* ------------------------------------------------------------------ */
/* mutexes                                                             */
/* ------------------------------------------------------------------ */

typedef struct MuRec {
    int64_t id;
    pthread_mutex_t mu;
} MuRec;

static MuRec *g_mu;
static size_t g_mu_len, g_mu_cap;
static int64_t g_mu_next = 1;
static pthread_mutex_t g_mu_reg = PTHREAD_MUTEX_INITIALIZER;

static MuRec *mu_rec(int64_t id) {
    for (size_t i = 0; i < g_mu_len; i++)
        if (g_mu[i].id == id) return &g_mu[i];
    return NULL;
}

hval hpbi_mutex_new(void) {
    pthread_mutex_lock(&g_mu_reg);
    if (g_mu_len == g_mu_cap) {
        g_mu_cap = g_mu_cap ? g_mu_cap * 2 : 8;
        g_mu = (MuRec *)realloc(g_mu, g_mu_cap * sizeof(MuRec));
        if (!g_mu) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    MuRec *m = &g_mu[g_mu_len++];
    m->id = g_mu_next++;
    pthread_mutex_init(&m->mu, NULL);
    int64_t id = m->id;
    pthread_mutex_unlock(&g_mu_reg);
    return hp_of_int(id);
}

hval hpbi_mutex_lock(hval idv) {
    pthread_mutex_lock(&g_mu_reg);
    MuRec *m = mu_rec(hp_val_to_int(idv));
    pthread_mutex_t *mu = m ? &m->mu : NULL;
    pthread_mutex_unlock(&g_mu_reg);
    if (mu) pthread_mutex_lock(mu);
    return hp_of_bool(mu != NULL);
}

hval hpbi_mutex_trylock(hval idv) {
    pthread_mutex_lock(&g_mu_reg);
    MuRec *m = mu_rec(hp_val_to_int(idv));
    pthread_mutex_t *mu = m ? &m->mu : NULL;
    pthread_mutex_unlock(&g_mu_reg);
    if (!mu) return hp_of_bool(false);
    return hp_of_bool(pthread_mutex_trylock(mu) == 0);
}

hval hpbi_mutex_unlock(hval idv) {
    pthread_mutex_lock(&g_mu_reg);
    MuRec *m = mu_rec(hp_val_to_int(idv));
    pthread_mutex_t *mu = m ? &m->mu : NULL;
    pthread_mutex_unlock(&g_mu_reg);
    if (mu) pthread_mutex_unlock(mu);
    return hp_of_bool(mu != NULL);
}

hval hpbi_mutex_free(hval idv) {
    pthread_mutex_lock(&g_mu_reg);
    for (size_t i = 0; i < g_mu_len; i++)
        if (g_mu[i].id == hp_val_to_int(idv)) {
            pthread_mutex_destroy(&g_mu[i].mu);
            g_mu[i] = g_mu[--g_mu_len];
            pthread_mutex_unlock(&g_mu_reg);
            return hp_of_bool(true);
        }
    pthread_mutex_unlock(&g_mu_reg);
    return hp_of_bool(false);
}

/* ------------------------------------------------------------------ */
/* barriers                                                            */
/* ------------------------------------------------------------------ */

typedef struct BarRec {
    int64_t id;
    size_t need, seen, round;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} BarRec;

static BarRec *g_bar;
static size_t g_bar_len, g_bar_cap;
static int64_t g_bar_next = 1;
static pthread_mutex_t g_bar_reg = PTHREAD_MUTEX_INITIALIZER;

static BarRec *bar_rec(int64_t id) {
    for (size_t i = 0; i < g_bar_len; i++)
        if (g_bar[i].id == id) return &g_bar[i];
    return NULL;
}

hval hpbi_barrier_new(hval n) {
    int64_t need = hp_val_to_int(n);
    if (need < 1) need = 1;
    pthread_mutex_lock(&g_bar_reg);
    if (g_bar_len == g_bar_cap) {
        g_bar_cap = g_bar_cap ? g_bar_cap * 2 : 4;
        g_bar = (BarRec *)realloc(g_bar, g_bar_cap * sizeof(BarRec));
        if (!g_bar) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    BarRec *b = &g_bar[g_bar_len++];
    b->id = g_bar_next++;
    b->need = (size_t)need;
    b->seen = 0;
    b->round = 0;
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    int64_t id = b->id;
    pthread_mutex_unlock(&g_bar_reg);
    return hp_of_int(id);
}

hval hpbi_barrier_wait(hval idv) {
    pthread_mutex_lock(&g_bar_reg);
    BarRec *b = bar_rec(hp_val_to_int(idv));
    if (!b) { pthread_mutex_unlock(&g_bar_reg); return hp_of_int(-1); }
    pthread_mutex_lock(&b->mu);
    pthread_mutex_unlock(&g_bar_reg);

    size_t round = b->round;
    size_t idx = b->seen++;
    if (b->seen == b->need) {          /* last arrival: release everyone */
        b->seen = 0;
        b->round++;
        pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->mu);
        return hp_of_int((int64_t)(b->need - 1));
    }
    while (round == b->round && b->seen != 0)
        pthread_cond_wait(&b->cv, &b->mu);
    pthread_mutex_unlock(&b->mu);
    return hp_of_int((int64_t)idx);
}

/* ------------------------------------------------------------------ */
/* atomics (64-bit shared counters)                                    */
/* ------------------------------------------------------------------ */

typedef struct AtRec {
    int64_t id;
    int64_t v;
    pthread_mutex_t mu;
} AtRec;

static AtRec *g_at;
static size_t g_at_len, g_at_cap;
static int64_t g_at_next = 1;
static pthread_mutex_t g_at_reg = PTHREAD_MUTEX_INITIALIZER;

static AtRec *at_rec(int64_t id) {
    for (size_t i = 0; i < g_at_len; i++)
        if (g_at[i].id == id) return &g_at[i];
    return NULL;
}

hval hpbi_atomic_new(hval init) {
    pthread_mutex_lock(&g_at_reg);
    if (g_at_len == g_at_cap) {
        g_at_cap = g_at_cap ? g_at_cap * 2 : 8;
        g_at = (AtRec *)realloc(g_at, g_at_cap * sizeof(AtRec));
        if (!g_at) { fprintf(stderr, "hphp: out of memory\n"); exit(1); }
    }
    AtRec *a = &g_at[g_at_len++];
    a->id = g_at_next++;
    a->v = hp_val_to_int(init);
    pthread_mutex_init(&a->mu, NULL);
    int64_t id = a->id;
    pthread_mutex_unlock(&g_at_reg);
    return hp_of_int(id);
}

hval hpbi_atomic_get(hval idv) {
    pthread_mutex_lock(&g_at_reg);
    AtRec *a = at_rec(hp_val_to_int(idv));
    int64_t v = a ? a->v : 0;
    pthread_mutex_unlock(&g_at_reg);
    return hp_of_int(v);
}

hval hpbi_atomic_set(hval idv, hval v) {
    pthread_mutex_lock(&g_at_reg);
    AtRec *a = at_rec(hp_val_to_int(idv));
    if (a) a->v = hp_val_to_int(v);
    pthread_mutex_unlock(&g_at_reg);
    return hp_of_bool(a != NULL);
}

hval hpbi_atomic_add(hval idv, hval delta) {
    pthread_mutex_lock(&g_at_reg);
    AtRec *a = at_rec(hp_val_to_int(idv));
    int64_t old = 0;
    if (a) {
        old = a->v;
        a->v = old + hp_val_to_int(delta);
    }
    pthread_mutex_unlock(&g_at_reg);
    return hp_of_int(old);
}

hval hpbi_atomic_cas(hval idv, hval expect, hval set) {
    pthread_mutex_lock(&g_at_reg);
    AtRec *a = at_rec(hp_val_to_int(idv));
    bool ok = false;
    if (a && a->v == hp_val_to_int(expect)) {
        a->v = hp_val_to_int(set);
        ok = true;
    }
    pthread_mutex_unlock(&g_at_reg);
    return hp_of_bool(ok);
}

/* ------------------------------------------------------------------ */
/* parallel map: n threads, round-robin inputs, channels collect       */
/* ------------------------------------------------------------------ */

typedef struct {
    hclosure *clo;
    hval item;
    int64_t ch;
    int64_t idx;
} PmapJob;

static void *pmap_worker(void *p) {
    PmapJob *j = (PmapJob *)p;
    hval args[2] = { j->item, hp_of_int(j->idx) };
    hval r = hp_closure_call(j->clo, 2, args);
    bool ok = thr_value_transferable(r, 0);
    if (!ok) hp_val_rc_dec(r);
    hval msg = hp_of_arr(hp_map_of(6, (const hval[]){
        hp_of_str(hp_str_lit("idx")), hp_of_int(j->idx),
        hp_of_str(hp_str_lit("val")), ok ? r : hp_null,
        hp_of_str(hp_str_lit("ok")), hp_of_bool(ok) }));
    if (ok) hp_val_rc_dec(r); /* hp_map_of retained the result value */
    chan_send_impl(j->ch, msg, true);
    free(j);
    return NULL;
}

hval hpbi_thr_parallel_map(hval clo, hval items, hval workersv) {
    if (clo.tag != HV_CLO || items.tag != HV_ARR || !items.u.a)
        return hp_of_bool(false);
    int64_t n = hp_arr_len(items.u.a);
    if (n == 0) return hp_of_arr(hp_arr_new());
    for (int64_t i = 0; i < n; i++) {
        hval item = hp_arr_get(items.u.a, hp_of_int(i));
        if (!thr_value_transferable(item, 0)) {
            hp_throw_str("non-Sync value passed to parallel map");
            return hp_of_bool(false);
        }
    }
    int64_t workers = hp_val_to_int(workersv);
    if (workers < 1) workers = 4;
    if (workers > n) workers = n;
    if (workers > 64) workers = 64;

    hval chv = hpbi_chan_new(hp_of_int(n));
    int64_t ch = hp_val_to_int(chv);

    pthread_t pts[64];
    int64_t launched = 0;
    for (int64_t i = 0; i < n; i++) {
        PmapJob *j = (PmapJob *)malloc(sizeof(PmapJob));
        if (!j) continue;
        j->clo = clo.u.c;
        j->item = hp_arr_get(items.u.a, hp_of_int(i));
        hp_val_rc_inc(j->item); /* owns its argument through callback cleanup */
        j->ch = ch;
        j->idx = i;
        if (i < workers) {
            hp_runtime_thread_starting();
            if (pthread_create(&pts[launched], NULL, pmap_worker, j) == 0)
                launched++;
            else { hp_val_rc_dec(j->item); free(j); continue; }
        } else {
            /* more items than workers: run inline after starting the first
             * `workers` threads — workers finish, then the rest run here */
            pmap_worker(j);
        }
    }
    for (int64_t t = 0; t < launched; t++) pthread_join(pts[t], NULL);

    /* collect n results from the channel */
    harr *out = hp_arr_new();
    hval *slots = (hval *)calloc((size_t)n, sizeof(hval));
    bool *have = (bool *)calloc((size_t)n, sizeof(bool));
    bool transfer_error = false;
    if (slots && have) {
        int64_t got = 0;
        while (got < n) {
            hval msg;
            if (!chan_recv_impl(ch, &msg, true)) break;
            if (msg.tag != HV_ARR || !msg.u.a) continue;
            hval iv = hp_arr_get(msg.u.a, hp_of_str(hp_str_lit("idx")));
            hval vv = hp_arr_get(msg.u.a, hp_of_str(hp_str_lit("val")));
            hval okv = hp_arr_get(msg.u.a, hp_of_str(hp_str_lit("ok")));
            int64_t idx = hp_val_to_int(iv);
            if (idx >= 0 && idx < n && !have[idx]) {
                have[idx] = true;
                if (!hp_val_to_bool(okv)) transfer_error = true;
                else {
                    slots[idx] = vv;
                    hp_val_rc_inc(vv);
                }
                got++;
            }
            hp_val_rc_dec(msg);
        }
        for (int64_t i = 0; i < n; i++)
            if (have[i]) hp_arr_push_take(out, slots[i]);
            else hp_arr_push(out, hp_null);
    }
    free(slots);
    free(have);
    hpbi_chan_close(chv);
    if (transfer_error) {
        hp_val_rc_dec(hp_of_arr(out));
        hp_throw_str("non-Sync result returned from parallel map");
        return hp_of_bool(false);
    }
    return hp_of_arr(out);
}
