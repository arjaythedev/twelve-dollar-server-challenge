// io_uring worker: multishot direct accept, multishot recv into a provided
// buffer ring, fixed (direct) socket descriptors, batched SQLite work, sends
// submitted after COMMIT.
#include "server.h"
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 2048u
#define BUF_COUNT 2048u        // per ring, power of two
#define BUF_GROUP 0
#define MAX_REQ 16384u         // bytes of one request (headers + body)
#define POOL_CAP 16384u        // pooled output buffer size
#define POOL_MAX 1024u

enum { UD_ACCEPT = 1, UD_RECV, UD_SEND, UD_CLOSE, UD_POLL };
#define MAX_RINGS 16
#define UD(t, slot, gen) ((uint64_t)(t) << 56 | (uint64_t)((gen) & 0xFFFFFFu) << 32 | (uint64_t)(slot))
#define UD_T(u) ((uint32_t)((u) >> 56))
#define UD_GEN(u) ((uint32_t)(((u) >> 32) & 0xFFFFFFu))
#define UD_SLOT(u) ((uint32_t)(u))

// ---------------------------------------------------------------- connection
// Instances live in one lazily-touched slab per worker, indexed by the fixed
// file slot the kernel picked at accept time; the isa is set in place.
@interface TDConn : TDObject {
@public
    uint32_t gen;
    bool live, recvArmed, sending, closeAfter, inBatch, needRearm;
    TDBuf in;      // partial request bytes
    TDBuf out;     // responses built, not yet submitted
    TDBuf wire;    // bytes owned by the kernel (in-flight send)
    uint32_t wireOff;
}
@end
@implementation TDConn
@end

// ---------------------------------------------------------------- handlers
@implementation TDHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b { (void)r; (void)w; (void)b; return 500; }
@end

@interface TDErrorHandler : TDHandler @end
@implementation TDErrorHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b {
    (void)w;
    td_buf_lit(b, "{\"error\":\"");
    td_buf_put(b, r->msg, (uint32_t)strlen(r->msg));
    td_buf_lit(b, "\"}");
    return r->status;
}
@end

@interface TDHealthHandler : TDHandler @end
@implementation TDHealthHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b {
    (void)r;
    return [w->store health:b uptime:w->uptime];
}
@end

@interface TDFeedHandler : TDHandler @end
@implementation TDFeedHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b { (void)r; return [w->store feed:b]; }
@end

@interface TDPostHandler : TDHandler @end
@implementation TDPostHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b { return [w->store post:r->id into:b]; }
@end

@interface TDCreateHandler : TDHandler @end
@implementation TDCreateHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b {
    return [w->store create:r->uid name:w->arena.p + r->nameOff length:r->nameLen
                       body:w->arena.p + r->bodyOff length:r->bodyLen into:b];
}
@end

@interface TDLikeHandler : TDHandler @end
@implementation TDLikeHandler
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b {
    return [w->store like:r->uid post:r->id into:b];
}
@end

typedef int (*TDHandleIMP)(id, SEL, TDReq *, TDWorker *, TDBuf *);

// ---------------------------------------------------------------- helpers
TD_INLINE bool ieq(const char *a, const char *lower, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != lower[i]) return false;
    }
    return true;
}

// positive integer id, or -1
static int64_t parse_id(const char *s, uint32_t n) {
    if (n == 0 || n > 18) return -1;
    int64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v > 0 ? v : -1;
}

static const char *status_line(int st, uint32_t *len) {
#define SL(s) do { *len = sizeof(s) - 1; return s; } while (0)
    switch (st) {
    case 200: SL("HTTP/1.1 200 OK\r\n");
    case 201: SL("HTTP/1.1 201 Created\r\n");
    case 400: SL("HTTP/1.1 400 Bad Request\r\n");
    case 401: SL("HTTP/1.1 401 Unauthorized\r\n");
    case 404: SL("HTTP/1.1 404 Not Found\r\n");
    case 503: SL("HTTP/1.1 503 Service Unavailable\r\n");
    default: SL("HTTP/1.1 500 Internal Server Error\r\n");
    }
#undef SL
}

// ---------------------------------------------------------------- worker
// One io_uring per fixed-file table. All rings are driven by this one thread:
// ring 0 waits, and holds a multishot poll on every other active ring's fd.
typedef struct {
    struct io_uring ring;
    struct io_uring_buf_ring *br;
    char *bufs;
    uint32_t brMask, live;
    bool acceptArmed, full, active, pollArmed;
} TDRingState;

@implementation TDWorker {
    TDRingState R[MAX_RINGS];
    int nrings;
    char *slab;                 // TDConn instances
    size_t stride;
    Class connClass;
    uint32_t slots;
    int listenFd;
    TDAuth *auth;
    int64_t started, now;
    // batch
    TDReq *reqs;
    uint32_t nreq, reqCap;
    uint32_t *dirty, ndirty;
    uint32_t *rearm, nrearm;
    TDBuf body, scratch;
    char **pool;
    uint32_t npool;
    id handlers[R_COUNT];
    TDHandleIMP imps[R_COUNT];
    SEL handleSel;
}

// A connection id is ring * slots + fixed-file slot.
TD_INLINE TDConn *conn_at(TDWorker *w, uint32_t slot) {
    return (TDConn *)(void *)(w->slab + (size_t)slot * w->stride);
}
TD_INLINE struct io_uring *ring_of(TDWorker *w, uint32_t cid) { return &w->R[cid / w->slots].ring; }
TD_INLINE int fslot(TDWorker *w, uint32_t cid) { return (int)(cid % w->slots); }

static struct io_uring_sqe *sqe_get(struct io_uring *r) {
    struct io_uring_sqe *s = io_uring_get_sqe(r);
    while (TD_UNLIKELY(!s)) {
        io_uring_submit(r);
        s = io_uring_get_sqe(r);
    }
    return s;
}

- (instancetype)initWithConfig:(const TDWorkerConfig *)cfg {
    store = cfg->store;
    auth = cfg->auth;
    started = cfg->started;
    slots = cfg->slots;
    listenFd = cfg->listenFd;
    nrings = cfg->rings < 1 ? 1 : cfg->rings > MAX_RINGS ? MAX_RINGS : cfg->rings;
    connClass = objc_getClass("TDConn");
    stride = (class_getInstanceSize(connClass) + 15) & ~(size_t)15;
    slab = mmap(NULL, stride * slots * (size_t)nrings, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (slab == MAP_FAILED) { perror("mmap slab"); exit(1); }
    reqCap = 1024;
    reqs = malloc(sizeof(TDReq) * reqCap);
    dirty = malloc(sizeof(uint32_t) * slots * (size_t)nrings);
    rearm = malloc(sizeof(uint32_t) * slots * (size_t)nrings);
    pool = malloc(sizeof(char *) * POOL_MAX);
    handleSel = sel_registerName("handle:worker:body:");
    const char *names[R_COUNT] = {
        [R_ERR] = "TDErrorHandler", [R_HEALTH] = "TDHealthHandler", [R_FEED] = "TDFeedHandler",
        [R_POST] = "TDPostHandler", [R_CREATE] = "TDCreateHandler", [R_LIKE] = "TDLikeHandler",
    };
    for (int i = 0; i < R_COUNT; i++) {
        Class c = objc_getClass(names[i]);
        handlers[i] = [c make];
        imps[i] = (TDHandleIMP)class_getMethodImplementation(c, handleSel);
    }
    return self;
}

- (void)setupRing:(int)k TD_DIRECT {
    TDRingState *rs = &R[k];
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    // Ring 0 is the one we wait on: completions run as deferred task work in
    // batches. The other rings post completions directly so that ring 0's poll
    // on their fds sees them.
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    if (k == 0) p.flags |= IORING_SETUP_DEFER_TASKRUN;
    p.cq_entries = 16384;
    int ret = io_uring_queue_init_params(4096, &rs->ring, &p);
    if (ret < 0) {
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_CQSIZE;
        p.cq_entries = 16384;
        ret = io_uring_queue_init_params(4096, &rs->ring, &p);
    }
    if (ret < 0) { fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret)); exit(1); }
    ret = io_uring_register_files_sparse(&rs->ring, slots);
    if (ret < 0) { fprintf(stderr, "register_files_sparse(%u): %s\n", slots, strerror(-ret)); exit(1); }
    int err = 0;
    rs->br = io_uring_setup_buf_ring(&rs->ring, BUF_COUNT, BUF_GROUP, 0, &err);
    if (!rs->br) { fprintf(stderr, "setup_buf_ring: %s\n", strerror(-err)); exit(1); }
    rs->bufs = mmap(NULL, (size_t)BUF_SIZE * BUF_COUNT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rs->bufs == MAP_FAILED) { perror("mmap bufs"); exit(1); }
    rs->brMask = io_uring_buf_ring_mask(BUF_COUNT);
    for (uint32_t i = 0; i < BUF_COUNT; i++)
        io_uring_buf_ring_add(rs->br, rs->bufs + (size_t)i * BUF_SIZE, BUF_SIZE, (unsigned short)i, rs->brMask, (int)i);
    io_uring_buf_ring_advance(rs->br, BUF_COUNT);
}

TD_INLINE void buf_recycle(TDRingState *rs, uint32_t bid) {
    io_uring_buf_ring_add(rs->br, rs->bufs + (size_t)bid * BUF_SIZE, BUF_SIZE, (unsigned short)bid, rs->brMask, 0);
    io_uring_buf_ring_advance(rs->br, 1);
}

// Accept goes to one ring at a time, filling ring 0 first: below one table's
// worth of connections only ring 0 is in use.
- (void)armAccept:(int)k TD_DIRECT {
    TDRingState *rs = &R[k];
    struct io_uring_sqe *s = sqe_get(&rs->ring);
    io_uring_prep_multishot_accept_direct(s, listenFd, NULL, NULL, 0);
    io_uring_sqe_set_data64(s, UD(UD_ACCEPT, k, 0));
    rs->acceptArmed = true;
    rs->active = true;
}

- (bool)acceptArmedAnywhere TD_DIRECT {
    for (int k = 0; k < nrings; k++) if (R[k].acceptArmed) return true;
    return false;
}

TD_INLINE void arm_recv(TDWorker *w, TDConn *c, uint32_t slot) {
    struct io_uring_sqe *s = sqe_get(ring_of(w, slot));
    io_uring_prep_recv_multishot(s, fslot(w, slot), NULL, 0, 0);
    s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    s->buf_group = BUF_GROUP;
    io_uring_sqe_set_data64(s, UD(UD_RECV, slot, c->gen));
    c->recvArmed = true;
}

TD_INLINE void pool_take(TDWorker *w, TDBuf *b) {
    if (b->p) return;
    if (w->npool) b->p = w->pool[--w->npool];
    else b->p = malloc(POOL_CAP);
    b->cap = POOL_CAP;
    b->len = 0;
}

TD_INLINE void pool_give(TDWorker *w, TDBuf *b) {
    if (!b->p) return;
    if (b->cap == POOL_CAP && w->npool < POOL_MAX) w->pool[w->npool++] = b->p;
    else free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

- (void)submitClose:(TDConn *)c slot:(uint32_t)slot TD_DIRECT {
    pool_give(self, &c->out);
    pool_give(self, &c->wire);
    free(c->in.p);
    c->in = (TDBuf){0};
    struct io_uring_sqe *s;
    if (c->recvArmed) {
        // wake the pending multishot recv so the socket is released now
        s = sqe_get(ring_of(self, slot));
        io_uring_prep_shutdown(s, fslot(self, slot), SHUT_RDWR);
        s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
        io_uring_sqe_set_data64(s, UD(UD_CLOSE, slot, 0xFFFFFF));
    }
    s = sqe_get(ring_of(self, slot));
    io_uring_prep_close_direct(s, (unsigned)fslot(self, slot));
    io_uring_sqe_set_data64(s, UD(UD_CLOSE, slot, c->gen));
}

// stop using the connection; closes now or after the in-flight send
- (void)drop:(TDConn *)c slot:(uint32_t)slot TD_DIRECT {
    if (!c->live) return;
    c->live = false;
    if (c->sending) c->closeAfter = true;
    else [self submitClose:c slot:slot];
}

- (void)startSend:(TDConn *)c slot:(uint32_t)slot TD_DIRECT {
    TDBuf t = c->wire;
    c->wire = c->out;
    c->out = t;
    c->out.len = 0;
    if (c->out.p && c->out.cap != POOL_CAP) pool_give(self, &c->out);
    c->wireOff = 0;
    c->sending = true;
    struct io_uring_sqe *s = sqe_get(ring_of(self, slot));
    io_uring_prep_send(s, fslot(self, slot), c->wire.p, c->wire.len, MSG_NOSIGNAL);
    s->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(s, UD(UD_SEND, slot, c->gen));
}

// ---------------------------------------------------------------- HTTP parsing
TD_INLINE TDReq *new_req(TDWorker *w, uint32_t slot, TDConn *c, bool close) {
    if (TD_UNLIKELY(w->nreq == w->reqCap)) {
        w->reqCap *= 2;
        w->reqs = realloc(w->reqs, sizeof(TDReq) * w->reqCap);
    }
    TDReq *r = &w->reqs[w->nreq++];
    r->slot = slot;
    r->gen = c->gen;
    r->close = close;
    r->route = R_ERR;
    r->msg = NULL;
    if (!c->inBatch) {
        c->inBatch = true;
        w->dirty[w->ndirty++] = slot;
    }
    return r;
}

TD_INLINE void req_err(TDReq *r, int st, const char *msg) {
    r->route = R_ERR;
    r->status = (int16_t)st;
    r->msg = msg;
}

- (void)authed:(TDReq *)r header:(const char *)auth length:(uint32_t)authLen TD_DIRECT {
    td_buf_reserve(&arena, TD_UNAME_MAX);
    uint32_t nl = 0;
    int a = [self->auth verify:auth length:authLen now:now uid:&r->uid name:arena.p + arena.len nameLen:&nl];
    if (a == TD_AUTH_OK) {
        r->nameOff = arena.len;
        r->nameLen = nl;
        arena.len += nl;
    } else if (a == TD_AUTH_MISSING) req_err(r, 401, "missing bearer token");
    else if (a == TD_AUTH_INVALID) req_err(r, 401, "invalid or expired token");
    else req_err(r, 401, "invalid token payload");
}

- (void)createBody:(TDReq *)r data:(const char *)b length:(uint32_t)n TD_DIRECT {
    TDJField f[1] = {{ .key = "body", .keylen = 4 }};
    int k = td_json_object(b, n, f, 1);
    if (k < 0) { req_err(r, 400, "malformed JSON body"); return; }
    if (k == 0 || f[0].type != TDJ_STRING) { req_err(r, 400, "body is required"); return; }
    td_buf_reserve(&arena, f[0].vlen + 4);
    char *o = arena.p + arena.len;
    int len;
    if (f[0].escaped) len = td_json_unescape(f[0].v, f[0].vlen, o, f[0].vlen + 4);
    else { memcpy(o, f[0].v, f[0].vlen); len = (int)f[0].vlen; }
    if (len < 0) { req_err(r, 400, "malformed JSON body"); return; }
    uint32_t s = 0, e = (uint32_t)len;
    while (s < e && (o[s] == ' ' || (o[s] >= '\t' && o[s] <= '\r'))) s++;
    while (e > s && (o[e - 1] == ' ' || (o[e - 1] >= '\t' && o[e - 1] <= '\r'))) e--;
    if (s == e) { req_err(r, 400, "body is required"); return; }
    uint32_t chars = 0;
    for (uint32_t i = s; i < e; i++) chars += ((uint8_t)o[i] & 0xC0) != 0x80;
    if (chars > 500) { req_err(r, 400, "body must be at most 500 characters"); return; }
    r->route = R_CREATE;
    r->bodyOff = arena.len + s;
    r->bodyLen = e - s;
    arena.len += (uint32_t)len;
}

// Parses complete requests from d[0..n). Returns bytes consumed, or -1 to drop the connection.
- (int)parse:(TDConn *)c slot:(uint32_t)slot data:(const char *)d length:(uint32_t)n TD_DIRECT {
    uint32_t off = 0;
    while (off < n) {
        const char *p = d + off;
        uint32_t avail = n - off;
        const char *he = memmem(p, avail, "\r\n\r\n", 4);
        if (!he) {
            if (avail > MAX_REQ) return -1;
            break;
        }
        uint32_t hlen = (uint32_t)(he + 4 - p);
        const char *le = memchr(p, '\r', hlen);
        const char *sp1 = memchr(p, ' ', (size_t)(le - p));
        if (!sp1) return -1;
        const char *path = sp1 + 1;
        const char *sp2 = memchr(path, ' ', (size_t)(le - path));
        if (!sp2) return -1;
        uint32_t mlen = (uint32_t)(sp1 - p), plen = (uint32_t)(sp2 - path);
        bool close = (le - sp2 - 1 == 8) && sp2[8] == '0'; // HTTP/1.0
        const char *auth = NULL;
        uint32_t authLen = 0;
        uint64_t clen = 0;
        const char *end = he + 2;
        for (const char *h = le + 2; h < end;) {
            const char *eol = memchr(h, '\r', (size_t)(end - h));
            if (!eol) return -1;
            const char *colon = memchr(h, ':', (size_t)(eol - h));
            if (colon) {
                uint32_t nlen = (uint32_t)(colon - h);
                const char *v = colon + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                const char *ve = eol;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                uint32_t vlen = (uint32_t)(ve - v);
                switch (nlen) {
                case 14:
                    if (ieq(h, "content-length", 14)) {
                        clen = 0;
                        for (uint32_t i = 0; i < vlen; i++) {
                            if (v[i] < '0' || v[i] > '9') return -1;
                            clen = clen * 10 + (uint64_t)(v[i] - '0');
                            if (clen > MAX_REQ) return -1;
                        }
                    }
                    break;
                case 13:
                    if (ieq(h, "authorization", 13)) { auth = v; authLen = vlen; }
                    break;
                case 10:
                    if (ieq(h, "connection", 10)) {
                        if (vlen == 5 && ieq(v, "close", 5)) close = true;
                        else if (vlen == 10 && ieq(v, "keep-alive", 10)) close = false;
                    }
                    break;
                case 17:
                    if (ieq(h, "transfer-encoding", 17)) return -1; // chunked bodies: out of scope
                    break;
                }
            }
            h = eol + 2;
        }
        uint64_t total = hlen + clen;
        if (avail < total) {
            if (total > MAX_REQ) return -1;
            break;
        }
        const char *body = p + hlen;
        const char *q = memchr(path, '?', plen);
        if (q) plen = (uint32_t)(q - path);

        TDReq *r = new_req(self, slot, c, close);
        bool get = mlen == 3 && !memcmp(p, "GET", 3);
        bool post = mlen == 4 && !memcmp(p, "POST", 4);
        if (get && plen == 5 && !memcmp(path, "/feed", 5)) r->route = R_FEED;
        else if (plen > 7 && !memcmp(path, "/posts/", 7)) {
            const char *rest = path + 7;
            uint32_t rl = plen - 7;
            const char *slash = memchr(rest, '/', rl);
            if (get && !slash) {
                int64_t id = parse_id(rest, rl);
                if (id < 0) req_err(r, 400, "invalid post id");
                else { r->route = R_POST; r->id = id; }
            } else if (post && slash && (uint32_t)(rest + rl - slash) == 5 && !memcmp(slash, "/like", 5)) {
                [self authed:r header:auth length:authLen];
                if (r->msg == NULL) {
                    int64_t id = parse_id(rest, (uint32_t)(slash - rest));
                    if (id < 0) req_err(r, 400, "invalid post id");
                    else { r->route = R_LIKE; r->id = id; }
                }
            } else req_err(r, 404, "not found");
        } else if (post && plen == 6 && !memcmp(path, "/posts", 6)) {
            [self authed:r header:auth length:authLen];
            if (r->msg == NULL) [self createBody:r data:body length:(uint32_t)clen];
        } else if (get && plen == 7 && !memcmp(path, "/health", 7)) r->route = R_HEALTH;
        else req_err(r, 404, "not found");
        off += (uint32_t)total;
        if (close) { c->live = false; c->closeAfter = true; break; }
    }
    return (int)off;
}

- (void)onData:(TDConn *)c slot:(uint32_t)slot data:(const char *)d length:(uint32_t)n TD_DIRECT {
    int used;
    if (c->in.len) {
        td_buf_put(&c->in, d, n);
        used = [self parse:c slot:slot data:c->in.p length:c->in.len];
        if (used < 0) { [self drop:c slot:slot]; return; }
        uint32_t left = c->in.len - (uint32_t)used;
        if (left) memmove(c->in.p, c->in.p + used, left);
        c->in.len = left;
        if (!left) { free(c->in.p); c->in = (TDBuf){0}; }
    } else {
        used = [self parse:c slot:slot data:d length:n];
        if (used < 0) { [self drop:c slot:slot]; return; }
        if ((uint32_t)used < n && c->live) td_buf_put(&c->in, d + used, n - (uint32_t)used);
    }
}

// ---------------------------------------------------------------- batch
- (void)flush TD_DIRECT {
    TDStore *st = store;
    uptime = now - started;
    bool db = false;
    for (uint32_t i = 0; i < nreq; i++) if (reqs[i].route != R_ERR) { db = true; break; }
    bool wrote = false;
    pthread_mutex_lock(&st->lock);
    bool txn = db && [st begin];
    for (uint32_t i = 0; i < nreq; i++) {
        TDReq *r = &reqs[i];
        TDConn *c = conn_at(self, r->slot);
        if (c->gen != r->gen || (!c->live && !c->closeAfter)) continue;
        body.len = 0;
        int st2 = imps[r->route](handlers[r->route], handleSel, r, self, &body);
        if (r->route == R_CREATE || r->route == R_LIKE) wrote = true;
        uint32_t sl;
        const char *line = status_line(st2, &sl);
        pool_take(self, &c->out);
        TDBuf *o = &c->out;
        td_buf_reserve(o, sl + 96 + body.len);
        memcpy(o->p + o->len, line, sl);
        o->len += sl;
        td_buf_lit(o, "Content-Type: application/json\r\nContent-Length: ");
        o->len += td_utoa(o->p + o->len, body.len);
        if (r->close) td_buf_lit(o, "\r\nConnection: close\r\n\r\n");
        else td_buf_lit(o, "\r\n\r\n");
        memcpy(o->p + o->len, body.p, body.len);
        o->len += body.len;
    }
    bool ok = true;
    if (txn && ![st commit]) { ok = false; [st rollback]; }
    pthread_mutex_unlock(&st->lock);
    if (!ok && wrote) {
        // never acknowledge an uncommitted write: drop every connection of this batch
        for (uint32_t i = 0; i < ndirty; i++) {
            TDConn *c = conn_at(self, dirty[i]);
            c->out.len = 0;
            [self drop:c slot:dirty[i]];
        }
    }
    for (uint32_t i = 0; i < ndirty; i++) {
        uint32_t slot = dirty[i];
        TDConn *c = conn_at(self, slot);
        c->inBatch = false;
        if (c->out.len && !c->sending) [self startSend:c slot:slot];
        else if (!c->live && !c->sending && c->closeAfter) { c->closeAfter = false; [self submitClose:c slot:slot]; }
    }
    nreq = 0;
    ndirty = 0;
    arena.len = 0;
}

// ---------------------------------------------------------------- completions
- (void)onCQE:(struct io_uring_cqe *)cqe ring:(int)k TD_DIRECT {
    uint64_t ud = cqe->user_data;
    int res = cqe->res;
    uint32_t flags = cqe->flags;
    uint32_t slot = UD_SLOT(ud);
    switch (UD_T(ud)) {
    case UD_RECV: {
        TDConn *c = conn_at(self, slot);
        bool hasBuf = flags & IORING_CQE_F_BUFFER;
        uint32_t bid = flags >> IORING_CQE_BUFFER_SHIFT;
        TDRingState *rs = &R[k];
        if (c->gen != UD_GEN(ud)) { if (hasBuf) buf_recycle(rs, bid); break; }
        if (!(flags & IORING_CQE_F_MORE)) c->recvArmed = false;
        if (res > 0) {
            if (c->live) [self onData:c slot:slot data:rs->bufs + (size_t)bid * BUF_SIZE length:(uint32_t)res];
            buf_recycle(rs, bid);
            if (!c->recvArmed && c->live) arm_recv(self, c, slot);
        } else if (res == -ENOBUFS) {
            if (c->live && !c->needRearm) { c->needRearm = true; rearm[nrearm++] = slot; }
        } else {
            if (c->live) [self drop:c slot:slot];
            else if (!c->sending && !c->closeAfter) { /* already closing */ }
        }
        break;
    }
    case UD_SEND: {
        TDConn *c = conn_at(self, slot);
        if (c->gen != UD_GEN(ud)) break;
        if (res > 0 && c->wireOff + (uint32_t)res < c->wire.len) {
            c->wireOff += (uint32_t)res;
            struct io_uring_sqe *s = sqe_get(ring_of(self, slot));
            io_uring_prep_send(s, fslot(self, slot), c->wire.p + c->wireOff, c->wire.len - c->wireOff, MSG_NOSIGNAL);
            s->flags |= IOSQE_FIXED_FILE;
            io_uring_sqe_set_data64(s, UD(UD_SEND, slot, c->gen));
            break;
        }
        c->sending = false;
        c->wire.len = 0;
        if (res <= 0) { c->live = false; c->closeAfter = true; }
        if (c->closeAfter) {
            if (c->inBatch) break;           // flush() closes it after this batch
            c->closeAfter = false;
            c->live = false;
            [self submitClose:c slot:slot];
        } else if (c->out.len) {
            if (!c->inBatch) [self startSend:c slot:slot];
        } else pool_give(self, &c->wire);
        break;
    }
    case UD_ACCEPT: {
        TDRingState *rs = &R[k];
        if (!(flags & IORING_CQE_F_MORE)) {
            rs->acceptArmed = false;
            if (res == -ENFILE || res == -EMFILE) {
                // this ring's table is full: move accept to the next ring with room
                rs->full = true;
                for (int j = 1; j <= nrings; j++) {
                    int q = (k + j) % nrings;
                    if (!R[q].full) { [self armAccept:q]; break; }
                }
            }
        }
        if (res >= 0) {
            uint32_t cid = (uint32_t)k * slots + (uint32_t)res;
            TDConn *c = conn_at(self, cid);
            uint32_t g = c->gen + 1;
            memset((char *)c, 0, stride);
            c->isa = connClass;
            c->gen = g & 0xFFFFFF;
            c->live = true;
            rs->live++;
            arm_recv(self, c, cid);
        }
        break;
    }
    case UD_CLOSE:
        if (UD_GEN(ud) != 0xFFFFFF) {
            TDRingState *rs = &R[k];
            rs->live--;
            rs->full = false;
        }
        break;
    case UD_POLL:
        if (!(flags & IORING_CQE_F_MORE)) R[slot].pollArmed = false;
        break;
    }
}

- (void)reap:(int)k TD_DIRECT {
    struct io_uring *r = &R[k].ring;
    unsigned head, n = 0;
    struct io_uring_cqe *cqe;
    io_uring_for_each_cqe(r, head, cqe) {
        n++;
        [self onCQE:cqe ring:k];
    }
    io_uring_cq_advance(r, n);
}

- (void)run {
    for (int k = 0; k < nrings; k++) [self setupRing:k];
    [self armAccept:0];
    for (;;) {
        for (int k = 1; k < nrings; k++)
            if (R[k].active && io_uring_sq_ready(&R[k].ring)) io_uring_submit(&R[k].ring);
        int ret = io_uring_submit_and_wait(&R[0].ring, 1);
        if (ret < 0 && ret != -EINTR && ret != -ETIME && ret != -EBUSY && ret != -EAGAIN) {
            fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-ret));
            exit(1);
        }
        now = time(NULL);
        for (int k = 0; k < nrings; k++) if (k == 0 || R[k].active) [self reap:k];
        if (nreq) [self flush];
        if (nrearm) {
            for (uint32_t i = 0; i < nrearm; i++) {
                TDConn *c = conn_at(self, rearm[i]);
                c->needRearm = false;
                if (c->live && !c->recvArmed) arm_recv(self, c, rearm[i]);
            }
            nrearm = 0;
        }
        if (![self acceptArmedAnywhere])
            for (int k = 0; k < nrings; k++) if (!R[k].full) { [self armAccept:k]; break; }
        for (int k = 1; k < nrings; k++) {
            if (R[k].active && !R[k].pollArmed) {
                struct io_uring_sqe *sq = sqe_get(&R[0].ring);
                io_uring_prep_poll_multishot(sq, R[k].ring.ring_fd, POLLIN);
                io_uring_sqe_set_data64(sq, UD(UD_POLL, k, 0));
                R[k].pollArmed = true;
            }
        }
    }
}
@end
