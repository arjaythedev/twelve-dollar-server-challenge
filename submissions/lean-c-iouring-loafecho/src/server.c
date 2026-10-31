/* server.c — HTTP/1.1 front end of lean-c-iouring-loafecho.
 *
 * One process, one thread, one io_uring:
 *   - multishot accept straight into the ring's fixed-file table (direct
 *     descriptors; no fd per connection in the process table),
 *   - multishot recv into a provided buffer ring (falls back to one 2 KB
 *     buffer per connection if the kernel refuses the ring),
 *   - send from a per-connection buffer.
 * Every completion batch runs between db_batch_begin()/db_batch_commit():
 * writes in one batch share one transaction (group commit), and nothing —
 * read or write — is handed to the kernel for sending before the commit
 * returned, so a 201 always means the row is committed.
 * The SQLite layer is in db.c (see db.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "jwt.h"

#define RECV_BUF_SZ   2048
#define NBUFS         4096
#define MAX_HDR       16384
#define MAX_BODY      65536
#define SCRATCH_CAP   (512 * 1024)
#define SQ_ENTRIES    4096
#define CQ_ENTRIES    65536

enum { UD_ACCEPT = 1, UD_RECV, UD_SEND, UD_CANCEL, UD_CLOSE };

static inline uint64_t mkud(unsigned type, uint32_t slot, uint32_t gen) {
    return (uint64_t)type << 56 | (uint64_t)(gen & 0xffffff) << 32 | slot;
}

typedef struct {
    char    *in;  size_t in_len, in_cap, need;   /* partial request; need = total bytes once known */
    char    *out; size_t out_len, out_cap;       /* responses not yet handed to the kernel */
    char    *snd; size_t snd_len, snd_off, snd_cap; /* the buffer the in-flight send reads */
    char    *rbuf;                                /* fallback mode only */
    uint32_t gen;
    uint8_t  open, sending, close_after, dirty, cont_sent;
} conn_t;

typedef struct { uint32_t slot, gen; size_t off, len; int close; } wspan_t;

static struct io_uring ring;
static conn_t   *conns;
static uint32_t  nconns;
static uint32_t *dirty;  static uint32_t ndirty;
static wspan_t  *wspans; static size_t nwspans, wspans_cap;
static struct io_uring_buf_ring *br;
static char     *bufbase;
static int       use_bufring;
static int       listen_fd;
static db_t     *db;
static hmac_key  jwt_key;
static struct timespec t_start;
static double    now_unix;
static char     *scratch;      /* db JSON bodies */
static char     *bodybuf;      /* decoded POST body */
static unsigned long n_accept_err, n_nobufs;

/* ------------------------------------------------------------------ util */

static struct io_uring_sqe *get_sqe(void) {
    struct io_uring_sqe *s = io_uring_get_sqe(&ring);
    while (!s) { io_uring_submit(&ring); s = io_uring_get_sqe(&ring); }
    return s;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "out of memory (%zu bytes)\n", n); abort(); }
    return q;
}

static inline void reserve(char **buf, size_t *cap, size_t need) {
    if (need <= *cap) return;
    size_t n = *cap ? *cap : 4096;
    while (n < need) n *= 2;
    *buf = xrealloc(*buf, n);
    *cap = n;
}

static inline void mark_dirty(uint32_t slot) {
    conn_t *c = &conns[slot];
    if (!c->dirty) { c->dirty = 1; dirty[ndirty++] = slot; }
}

static inline char *put_u64(char *p, uint64_t v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

#define LIT(s) s, sizeof(s) - 1

/* ------------------------------------------------------------- responses */

static const char *status_line(int st, size_t *n) {
    const char *s;
    switch (st) {
    case 200: s = "HTTP/1.1 200 OK\r\n"; break;
    case 201: s = "HTTP/1.1 201 Created\r\n"; break;
    case 400: s = "HTTP/1.1 400 Bad Request\r\n"; break;
    case 401: s = "HTTP/1.1 401 Unauthorized\r\n"; break;
    case 404: s = "HTTP/1.1 404 Not Found\r\n"; break;
    case 503: s = "HTTP/1.1 503 Service Unavailable\r\n"; break;
    default:  s = "HTTP/1.1 500 Internal Server Error\r\n"; break;
    }
    *n = strlen(s);
    return s;
}

/* Append a complete response to c->out. keep10: HTTP/1.0 keep-alive. */
static void put_response(conn_t *c, int st, const char *body, size_t blen, int close, int keep10) {
    size_t sl; const char *s = status_line(st, &sl);
    reserve(&c->out, &c->out_cap, c->out_len + sl + 128 + blen);
    char *p = c->out + c->out_len;
    memcpy(p, s, sl); p += sl;
    memcpy(p, LIT("Content-Type: application/json\r\nContent-Length: ")); p += sizeof("Content-Type: application/json\r\nContent-Length: ") - 1;
    p = put_u64(p, blen);
    if (close) { memcpy(p, LIT("\r\nConnection: close\r\n\r\n")); p += sizeof("\r\nConnection: close\r\n\r\n") - 1; }
    else if (keep10) { memcpy(p, LIT("\r\nConnection: keep-alive\r\n\r\n")); p += sizeof("\r\nConnection: keep-alive\r\n\r\n") - 1; }
    else { memcpy(p, "\r\n\r\n", 4); p += 4; }
    memcpy(p, body, blen); p += blen;
    c->out_len = (size_t)(p - c->out);
}

#define ERR_BODY(m) "{\"error\":\"" m "\"}"
static const char E_NOTFOUND[]  = ERR_BODY("not found");
static const char E_BADID[]     = ERR_BODY("invalid post id");
static const char E_NOPOST[]    = ERR_BODY("post not found");
static const char E_NOBEARER[]  = ERR_BODY("missing bearer token");
static const char E_BADTOKEN[]  = ERR_BODY("invalid or expired token");
static const char E_BADPAYLD[]  = ERR_BODY("invalid token payload");
static const char E_BADJSON[]   = ERR_BODY("malformed JSON body");
static const char E_NOBODY[]    = ERR_BODY("body is required");
static const char E_TOOLONG[]   = ERR_BODY("body must be at most 500 characters");
static const char E_INTERNAL[]  = ERR_BODY("internal server error");
static const char E_BADREQ[]    = ERR_BODY("bad request");
static const char E_TOOLARGE[]  = ERR_BODY("request too large");

/* ----------------------------------------------------------- connections */

static void close_conn(uint32_t slot) {
    conn_t *c = &conns[slot];
    if (!c->open) return;
    c->open = 0;
    if (io_uring_sq_space_left(&ring) < 2) io_uring_submit(&ring);
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_cancel_fd(s, (int)slot, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_FD_FIXED);
    s->flags |= IOSQE_IO_HARDLINK;
    io_uring_sqe_set_data64(s, mkud(UD_CANCEL, slot, c->gen));
    s = get_sqe();
    io_uring_prep_close_direct(s, slot);
    io_uring_sqe_set_data64(s, mkud(UD_CLOSE, slot, c->gen));
    /* Keep small buffers for the next connection on this slot; drop big ones. */
    if (c->in_cap > 16384) { free(c->in); c->in = NULL; c->in_cap = 0; }
    c->in_len = 0; c->need = 0;
    c->out_len = 0;
}

static void arm_recv(uint32_t slot) {
    conn_t *c = &conns[slot];
    struct io_uring_sqe *s = get_sqe();
    if (use_bufring) {
        io_uring_prep_recv_multishot(s, (int)slot, NULL, 0, 0);
        s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        s->buf_group = 0;
    } else {
        io_uring_prep_recv(s, (int)slot, c->rbuf, RECV_BUF_SZ, 0);
        s->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data64(s, mkud(UD_RECV, slot, c->gen));
}

static void arm_accept(void) {
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_multishot_accept_direct(s, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(s, mkud(UD_ACCEPT, 0, 0));
}

static void post_send(uint32_t slot) {
    conn_t *c = &conns[slot];
    struct io_uring_sqe *s = get_sqe();
    io_uring_prep_send(s, (int)slot, c->snd + c->snd_off, c->snd_len - c->snd_off, MSG_NOSIGNAL);
    s->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(s, mkud(UD_SEND, slot, c->gen));
}

/* Called only at the end of a batch, after db_batch_commit(). */
static void flush_conn(uint32_t slot) {
    conn_t *c = &conns[slot];
    c->dirty = 0;
    if (!c->open || c->sending) return;
    if (c->out_len) {
        char *t = c->snd; size_t tc = c->snd_cap;
        c->snd = c->out; c->snd_cap = c->out_cap; c->snd_len = c->out_len; c->snd_off = 0;
        c->out = t; c->out_cap = tc; c->out_len = 0;
        c->sending = 1;
        post_send(slot);
    } else if (c->close_after) {
        close_conn(slot);
    }
}

/* ------------------------------------------------------------- handlers */

static int parse_id(const char *s, size_t n, int64_t *id) {
    if (n < 1 || n > 18 || s[0] < '1' || s[0] > '9') return -1;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *id = v;
    return 0;
}

static inline int is_trim_ws(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

typedef struct {
    const char *method; size_t mlen;
    const char *path;   size_t plen;
    const char *auth;   size_t alen;   /* NULL if absent */
    const char *body;   size_t blen;
    int close, keep10;
} req_t;

#define RESP(st, lit) put_response(c, st, lit, sizeof(lit) - 1, rq->close, rq->keep10)

static void record_write(uint32_t slot, size_t off, int close) {
    conn_t *c = &conns[slot];
    if (nwspans == wspans_cap) {
        wspans_cap = wspans_cap ? wspans_cap * 2 : 1024;
        wspans = xrealloc(wspans, wspans_cap * sizeof *wspans);
    }
    wspans[nwspans++] = (wspan_t){ slot, c->gen, off, c->out_len - off, close };
}

/* Returns 1 if authenticated (uid/uname filled), else writes the 401. */
static int authenticate(conn_t *c, const req_t *rq, int64_t *uid, char *uname, size_t *ulen) {
    int r = jwt_verify(&jwt_key, rq->auth, rq->alen, now_unix, uid, uname, JWT_MAX_USERNAME, ulen);
    if (r == JWT_OK) return 1;
    if (r == JWT_MISSING) RESP(401, E_NOBEARER);
    else if (r == JWT_BAD_PAYLOAD) RESP(401, E_BADPAYLD);
    else RESP(401, E_BADTOKEN);
    return 0;
}

static void handle_create(uint32_t slot, conn_t *c, const req_t *rq) {
    int64_t uid; char uname[JWT_MAX_USERNAME]; size_t ulen;
    if (!authenticate(c, rq, &uid, uname, &ulen)) return;
    json_field f = { "body", 4, 0, 0, 0 };
    int r = json_object_fields(rq->body, rq->body + rq->blen, &f, 1);
    if (r < 0) { RESP(400, E_BADJSON); return; }
    if (r == 0 || f.type != J_STR) { RESP(400, E_NOBODY); return; }
    long n = json_decode_string(f.vs, f.ve, bodybuf, MAX_BODY);
    if (n < 0) { RESP(500, E_INTERNAL); return; }
    const char *b = bodybuf, *e = bodybuf + n;
    while (b < e && is_trim_ws(*b)) b++;
    while (e > b && is_trim_ws(e[-1])) e--;
    if (b == e) { RESP(400, E_NOBODY); return; }
    size_t cps = 0;
    for (const char *p = b; p < e; p++) cps += ((unsigned char)*p & 0xC0) != 0x80;
    if (cps > 500) { RESP(400, E_TOOLONG); return; }
    int len = db_insert_post(db, uid, uname, ulen, b, (size_t)(e - b), scratch, SCRATCH_CAP);
    if (len < 0) { RESP(500, E_INTERNAL); return; }
    size_t off = c->out_len;
    put_response(c, 201, scratch, (size_t)len, rq->close, rq->keep10);
    record_write(slot, off, rq->close);
}

static void handle_like(uint32_t slot, conn_t *c, const req_t *rq, const char *ids, size_t idn) {
    int64_t uid; char uname[JWT_MAX_USERNAME]; size_t ulen;
    if (!authenticate(c, rq, &uid, uname, &ulen)) return;
    int64_t id;
    if (parse_id(ids, idn, &id) < 0) { RESP(400, E_BADID); return; }
    int already = 0;
    int r = db_like(db, uid, id, &already);
    if (r == 0) { RESP(404, E_NOPOST); return; }
    if (r < 0) { RESP(500, E_INTERNAL); return; }
    char b[96], *p = b;
    memcpy(p, LIT("{\"liked\":true,\"already_liked\":")); p += sizeof("{\"liked\":true,\"already_liked\":") - 1;
    if (already) { memcpy(p, "true", 4); p += 4; } else { memcpy(p, "false", 5); p += 5; }
    memcpy(p, LIT(",\"post_id\":")); p += sizeof(",\"post_id\":") - 1;
    p = put_u64(p, (uint64_t)id);
    *p++ = '}';
    size_t off = c->out_len;
    put_response(c, already ? 200 : 201, b, (size_t)(p - b), rq->close, rq->keep10);
    record_write(slot, off, rq->close);
}

static void handle_request(uint32_t slot, conn_t *c, const req_t *rq) {
    const char *path = rq->path; size_t pl = rq->plen;
    int get = rq->mlen == 3 && memcmp(rq->method, "GET", 3) == 0;
    int post = rq->mlen == 4 && memcmp(rq->method, "POST", 4) == 0;

    if (pl == 5 && memcmp(path, "/feed", 5) == 0 && get) {
        int n = db_feed_json(db, scratch, SCRATCH_CAP);
        if (n < 0) RESP(500, E_INTERNAL);
        else put_response(c, 200, scratch, (size_t)n, rq->close, rq->keep10);
        return;
    }
    if (pl >= 7 && memcmp(path, "/posts/", 7) == 0) {
        const char *rest = path + 7; size_t rl = pl - 7;
        const char *sl = memchr(rest, '/', rl);
        if (!sl) {
            if (!get) { RESP(404, E_NOTFOUND); return; }
            int64_t id;
            if (parse_id(rest, rl, &id) < 0) { RESP(400, E_BADID); return; }
            int n = db_post_json(db, id, scratch, SCRATCH_CAP);
            if (n < 0) RESP(500, E_INTERNAL);
            else if (n == 0) RESP(404, E_NOPOST);
            else put_response(c, 200, scratch, (size_t)n, rq->close, rq->keep10);
            return;
        }
        size_t idn = (size_t)(sl - rest);
        if (post && rl - idn - 1 == 4 && memcmp(sl + 1, "like", 4) == 0) {
            handle_like(slot, c, rq, rest, idn);
            return;
        }
        RESP(404, E_NOTFOUND);
        return;
    }
    if (pl == 6 && memcmp(path, "/posts", 6) == 0 && post) { handle_create(slot, c, rq); return; }
    if (pl == 7 && memcmp(path, "/health", 7) == 0 && get) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if (db_health(db) == 0) {
            char b[80], *p = b;
            memcpy(p, LIT("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")); p += sizeof("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":") - 1;
            p = put_u64(p, (uint64_t)(t.tv_sec - t_start.tv_sec));
            *p++ = '}';
            put_response(c, 200, b, (size_t)(p - b), rq->close, rq->keep10);
        } else {
            RESP(503, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"SELECT 1 failed\"}");
        }
        return;
    }
    RESP(404, E_NOTFOUND);
}

/* ------------------------------------------------------------ HTTP parse */

static int value_has_token(const char *v, size_t n, const char *tok, size_t tn) {
    for (size_t i = 0; i + tn <= n; i++)
        if (strncasecmp(v + i, tok, tn) == 0) return 1;
    return 0;
}

static void bad_request(conn_t *c, int st, const char *body, size_t blen) {
    put_response(c, st, body, blen, 1, 0);
    c->close_after = 1;
}

/* First "\r\n" fully inside b[0..n), or NULL (same result as memmem). */
static inline const char *find_crlf(const char *b, size_t n) {
    if (n < 2) return NULL;
    const char *p = b, *last = b + n - 1;          /* candidate starts < last */
    while (p < last) {
        p = memchr(p, '\r', (size_t)(last - p));
        if (!p) return NULL;
        if (p[1] == '\n') return p;
        p++;
    }
    return NULL;
}

/* First "\r\n\r\n" fully inside b[0..n), or NULL (same result as memmem).
 * When found, *first_crlf is the first "\r\n" in b (at or before it). */
static inline const char *find_hdr_end(const char *b, size_t n, const char **first_crlf) {
    const char *fc = NULL;
    if (n < 4) return NULL;
    const char *p = b, *last = b + n - 3;          /* candidate starts < last */
    while (p < last) {
        p = memchr(p, '\r', (size_t)(last - p));
        if (!p) return NULL;
        if (p[1] == '\n') {
            if (!fc) fc = p;
            if (p[2] == '\r' && p[3] == '\n') { *first_crlf = fc; return p; }
            p += 2;
        } else p++;
    }
    return NULL;
}

/* Parse and serve one request at buf[0..len). Returns bytes consumed,
 * 0 if incomplete (c->need set when the total size is known), -1 after
 * answering a malformed request (connection will close). */
static long parse_one(uint32_t slot, conn_t *c, const char *buf, size_t len) {
    size_t scan = len < MAX_HDR ? len : MAX_HDR;
    const char *le = NULL;
    const char *he = find_hdr_end(buf, scan, &le);
    if (!he) {
        if (len >= MAX_HDR) { bad_request(c, 400, LIT(E_TOOLARGE)); return -1; }
        return 0;
    }
    size_t hlen = (size_t)(he - buf) + 4;
    const char *p = buf;
    req_t rq = {0};
    /* request line */
    const char *sp1 = memchr(p, ' ', (size_t)(le - p));
    if (!sp1) goto bad;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1));
    if (!sp2) goto bad;
    rq.method = p; rq.mlen = (size_t)(sp1 - p);
    rq.path = sp1 + 1; rq.plen = (size_t)(sp2 - sp1 - 1);
    const char *q = memchr(rq.path, '?', rq.plen);
    if (q) rq.plen = (size_t)(q - rq.path);
    size_t vl = (size_t)(le - sp2 - 1);
    int http10;
    if (vl == 8 && memcmp(sp2 + 1, "HTTP/1.1", 8) == 0) http10 = 0;
    else if (vl == 8 && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0) http10 = 1;
    else goto bad;
    if (rq.mlen == 0 || rq.plen == 0) goto bad;

    int conn_close = 0, conn_keep = 0, expect_cont = 0, chunked = 0, have_cl = 0;
    uint64_t clen = 0;
    p = le + 2;
    while (p < he + 2) {
        le = find_crlf(p, (size_t)(he + 2 - p));
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (!colon || colon == p) goto bad;
        size_t nl = (size_t)(colon - p);
        const char *v = colon + 1, *ve = le;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vn = (size_t)(ve - v);
        switch (nl) {
        case 13:
            if (strncasecmp(p, "authorization", 13) == 0) { rq.auth = v; rq.alen = vn; }
            break;
        case 14:
            if (strncasecmp(p, "content-length", 14) == 0) {
                if (vn == 0 || vn > 12) goto bad;
                uint64_t x = 0;
                for (size_t i = 0; i < vn; i++) {
                    if (v[i] < '0' || v[i] > '9') goto bad;
                    x = x * 10 + (uint64_t)(v[i] - '0');
                }
                if (have_cl && x != clen) goto bad;
                clen = x; have_cl = 1;
            }
            break;
        case 10:
            if (strncasecmp(p, "connection", 10) == 0) {
                if (value_has_token(v, vn, "close", 5)) conn_close = 1;
                if (value_has_token(v, vn, "keep-alive", 10)) conn_keep = 1;
            }
            break;
        case 6:
            if (strncasecmp(p, "expect", 6) == 0 && value_has_token(v, vn, "100-continue", 12)) expect_cont = 1;
            break;
        case 17:
            if (strncasecmp(p, "transfer-encoding", 17) == 0) chunked = 1;
            break;
        }
        p = le + 2;
    }
    if (chunked) { bad_request(c, 400, LIT(E_BADREQ)); return -1; }
    if (clen > MAX_BODY) { bad_request(c, 400, LIT(E_TOOLARGE)); return -1; }
    size_t total = hlen + (size_t)clen;
    if (len < total) {
        c->need = total;
        if (expect_cont && !c->cont_sent && !http10) {
            reserve(&c->out, &c->out_cap, c->out_len + 32);
            memcpy(c->out + c->out_len, LIT("HTTP/1.1 100 Continue\r\n\r\n"));
            c->out_len += sizeof("HTTP/1.1 100 Continue\r\n\r\n") - 1;
            c->cont_sent = 1;
            mark_dirty(slot);
        }
        return 0;
    }
    c->need = 0; c->cont_sent = 0;
    rq.body = buf + hlen; rq.blen = (size_t)clen;
    rq.close = http10 ? !conn_keep : conn_close;
    rq.keep10 = http10 && conn_keep;
    handle_request(slot, c, &rq);
    if (rq.close) c->close_after = 1;
    mark_dirty(slot);
    return (long)total;
bad:
    bad_request(c, 400, LIT(E_BADREQ));
    return -1;
}

/* Serve every complete request in buf; return bytes consumed. */
static size_t parse_all(uint32_t slot, conn_t *c, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len && !c->close_after) {
        long r = parse_one(slot, c, buf + off, len - off);
        if (r <= 0) { if (r < 0) mark_dirty(slot); break; }
        off += (size_t)r;
    }
    return c->close_after ? len : off;
}

static void on_data(uint32_t slot, const char *data, size_t n) {
    conn_t *c = &conns[slot];
    if (c->close_after) return;
    if (c->in_len == 0) {
        size_t used = parse_all(slot, c, data, n);
        if (used < n && !c->close_after) {
            reserve(&c->in, &c->in_cap, n - used);
            memcpy(c->in, data + used, n - used);
            c->in_len = n - used;
        }
        return;
    }
    reserve(&c->in, &c->in_cap, c->in_len + n);
    memcpy(c->in + c->in_len, data, n);
    c->in_len += n;
    if (c->need && c->in_len < c->need) return;
    size_t used = parse_all(slot, c, c->in, c->in_len);
    if (c->close_after) { c->in_len = 0; return; }
    if (used) { memmove(c->in, c->in + used, c->in_len - used); c->in_len -= used; }
    if (c->in_len == 0 && c->in_cap > 16384) { free(c->in); c->in = NULL; c->in_cap = 0; }
}

/* ------------------------------------------------------------ CQE dispatch */

static inline void return_buf(unsigned bid) {
    io_uring_buf_ring_add(br, bufbase + (size_t)bid * RECV_BUF_SZ, RECV_BUF_SZ, (unsigned short)bid,
                          io_uring_buf_ring_mask(NBUFS), 0);
    io_uring_buf_ring_advance(br, 1);
}

static void on_cqe(struct io_uring_cqe *cqe) {
    uint64_t u = io_uring_cqe_get_data64(cqe);
    unsigned type = (unsigned)(u >> 56);
    uint32_t gen = (uint32_t)(u >> 32) & 0xffffff;
    uint32_t slot = (uint32_t)u;
    int res = cqe->res;

    switch (type) {
    case UD_ACCEPT:
        if (res >= 0) {
            if ((uint32_t)res >= nconns) {   /* cannot happen: table size == nconns */
                struct io_uring_sqe *s = get_sqe();
                io_uring_prep_close_direct(s, (unsigned)res);
                io_uring_sqe_set_data64(s, mkud(UD_CLOSE, 0, 0));
            } else {
                conn_t *c = &conns[res];
                c->gen = (c->gen + 1) & 0xffffff;
                c->open = 1; c->sending = 0; c->close_after = 0; c->cont_sent = 0;
                c->in_len = 0; c->need = 0; c->out_len = 0; c->snd_len = c->snd_off = 0;
                if (!use_bufring && !c->rbuf) c->rbuf = xrealloc(NULL, RECV_BUF_SZ);
                arm_recv((uint32_t)res);
            }
        } else if (n_accept_err++ % 1000 == 0) {
            fprintf(stderr, "accept: %s (%lu so far)\n", strerror(-res), n_accept_err);
        }
        if (!(cqe->flags & IORING_CQE_F_MORE)) arm_accept();
        return;

    case UD_RECV: {
        conn_t *c = &conns[slot];
        int has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
        unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        int live = c->open && c->gen == gen;
        if (!live) { if (has_buf) return_buf(bid); return; }
        if (res > 0) {
            on_data(slot, use_bufring ? bufbase + (size_t)bid * RECV_BUF_SZ : c->rbuf, (size_t)res);
            if (has_buf) return_buf(bid);
            if (!use_bufring || !(cqe->flags & IORING_CQE_F_MORE)) arm_recv(slot);
            return;
        }
        if (has_buf) return_buf(bid);
        if (res == -ENOBUFS) {
            if (n_nobufs++ % 1000 == 0) fprintf(stderr, "recv: out of provided buffers (%lu so far)\n", n_nobufs);
            if (!(cqe->flags & IORING_CQE_F_MORE)) arm_recv(slot);
            return;
        }
        if (res == 0) {
            /* Peer closed its side: finish sending what we owe, then close. */
            c->close_after = 1;
            mark_dirty(slot);
            return;
        }
        close_conn(slot);
        return;
    }

    case UD_SEND: {
        conn_t *c = &conns[slot];
        if (!c->open || c->gen != gen) return;
        if (res < 0) { c->sending = 0; close_conn(slot); return; }
        c->snd_off += (size_t)res;
        if (c->snd_off < c->snd_len) { post_send(slot); return; }
        c->sending = 0;
        if (c->snd_cap > 65536) { free(c->snd); c->snd = NULL; c->snd_cap = 0; }
        mark_dirty(slot);
        return;
    }

    default: /* UD_CANCEL, UD_CLOSE */
        return;
    }
}

/* ------------------------------------------------------------------ setup */

static void setup_bufring(void) {
    size_t sz = (size_t)NBUFS * RECV_BUF_SZ;
    bufbase = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bufbase == MAP_FAILED) { bufbase = NULL; goto fallback; }
    int ret = 0;
    br = io_uring_setup_buf_ring(&ring, NBUFS, 0, 0, &ret);
    if (br) {
        fprintf(stderr, "recv: provided buffer ring (%d x %d B)\n", NBUFS, RECV_BUF_SZ);
    } else if (ret == -EINVAL) {
        /* Retry once with a nonzero reserved field: some Ubuntu 6.8 kernels
         * (Launchpad #2162843) invert the "resv must be zero" check and
         * reject every well-formed registration with EINVAL. A fixed kernel
         * takes the first attempt above, so this path never runs there. */
        size_t rsz = NBUFS * sizeof(struct io_uring_buf);
        void *m = mmap(NULL, rsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) ret = -errno;
        else {
            struct io_uring_buf_reg reg;
            memset(&reg, 0, sizeof reg);
            reg.ring_addr = (uint64_t)(uintptr_t)m; reg.ring_entries = NBUFS; reg.bgid = 0;
            reg.resv[0] = 1;
            ret = io_uring_register_buf_ring(&ring, &reg, 0);
            if (ret == 0) {
                br = m;
                io_uring_buf_ring_init(br);
                fprintf(stderr, "recv: provided buffer ring (%d x %d B) after EINVAL retry with resv!=0 "
                        "(Launchpad #2162843 workaround)\n", NBUFS, RECV_BUF_SZ);
            } else munmap(m, rsz);
        }
    }
    if (!br) {
        fprintf(stderr, "recv: buffer ring unavailable (%s); falling back to per-connection %d B buffers\n",
                strerror(-ret), RECV_BUF_SZ);
        munmap(bufbase, sz); bufbase = NULL;
        goto fallback;
    }
    for (unsigned i = 0; i < NBUFS; i++)
        io_uring_buf_ring_add(br, bufbase + (size_t)i * RECV_BUF_SZ, RECV_BUF_SZ, (unsigned short)i,
                              io_uring_buf_ring_mask(NBUFS), (int)i);
    io_uring_buf_ring_advance(br, NBUFS);
    use_bufring = 1;
    return;
fallback:
    use_bufring = 0;
}

static int open_listener(const char *host, const char *port) {
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    int r = getaddrinfo(host, port, &hints, &ai);
    if (r != 0) { fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(r)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  /* inherited by accepted sockets */
        if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 65535) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) fprintf(stderr, "cannot listen on %s:%s: %s\n", host, port, strerror(errno));
    return fd;
}

static void maybe_print_plan(char *last, size_t cap) {
    const char *p = db_plan(db);
    if (strncmp(p, last, cap) != 0) {
        snprintf(last, cap, "%s", p);
        printf("%s\n", db_plan_line(db));
        fflush(stdout);
    }
}

/* ------------------------------------------------------------ PGO training
 * `bin/server --pgo-train` runs once inside build.sh, between the
 * -fprofile-generate and the -fprofile-use compile. Fully in-process: no
 * socket, no ring, no seed file. It builds a small database in a new
 * directory under $TMPDIR (db_pgo_make), opens it with db_open (the plan
 * must be btree), then hands raw HTTP bytes to on_data() for a few fake
 * connections exactly as recv completions would, each completion batch
 * between db_batch_begin()/db_batch_commit() like the main loop, and
 * verifies extra JWTs directly. The io_uring loop itself is not trained.
 * The database (and its -wal/-shm) is deleted on success and on failure.
 * Request mix: the bench/load.js user loop (GET /feed, GET /posts/:id of a
 * post from that feed, like 15%, create 2%), plus /health and a small share
 * of malformed, unauthorized, pipelined and split requests. */

#define TR_USERS   2000
#define TR_POSTS   20000
#define TR_LIKES   50000
#define TR_REQS    70000
#define TR_JWT     30000
#define TR_CONNS   64
#define TR_TOKENS  256
#define TR_NBAD    8

typedef struct {
    uint64_t rng;
    int64_t  max_id;                /* newest committed post id (from the 201 bodies) */
    char    *tok[TR_TOKENS];  size_t toklen[TR_TOKENS];    /* "Bearer <valid jwt>", user i+1 */
    char    *bad[TR_NBAD];    size_t badlen[TR_NBAD];      /* invalid Authorization values */
    unsigned long feeds, posts, inserts, likes, parses, jwt, batches;
    unsigned long st[6];            /* responses by class: 1xx..5xx */
    unsigned long jwt_wrong;        /* direct verifications with an unexpected result */
} trainer;

static inline uint32_t tr_rand(trainer *t) {
    t->rng = t->rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(t->rng >> 33);
}
static inline uint32_t tr_below(trainer *t, uint32_t n) { return tr_rand(t) % n; }

static size_t tr_b64url(const void *src, size_t n, char *out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const uint8_t *s = src; size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i+1] << 8 | s[i+2];
        out[o++] = a[v >> 18 & 63]; out[o++] = a[v >> 12 & 63]; out[o++] = a[v >> 6 & 63]; out[o++] = a[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)s[i] << 16;
        out[o++] = a[v >> 18 & 63]; out[o++] = a[v >> 12 & 63];
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i+1] << 8;
        out[o++] = a[v >> 18 & 63]; out[o++] = a[v >> 12 & 63]; out[o++] = a[v >> 6 & 63];
    }
    return o;
}

/* "Bearer " + base64url(hdr) "." base64url(pay) "." base64url(HMAC-SHA256),
 * signed with jwt_key (the server's own key schedule). Heap string. */
static char *tr_mint(const char *hdr, const char *pay, size_t *len) {
    char *o = xrealloc(NULL, 7 + 2 * (strlen(hdr) + strlen(pay)) + 64);
    memcpy(o, "Bearer ", 7);
    size_t n = 7;
    n += tr_b64url(hdr, strlen(hdr), o + n);
    o[n++] = '.';
    n += tr_b64url(pay, strlen(pay), o + n);
    uint8_t mac[32];
    hmac_sha256(&jwt_key, o + 7, n - 7, mac);
    o[n++] = '.';
    n += tr_b64url(mac, 32, o + n);
    o[n] = 0;
    *len = n;
    return o;
}

static char *tr_dup(const char *s, size_t *len) {
    *len = strlen(s);
    char *o = xrealloc(NULL, *len + 1);
    memcpy(o, s, *len + 1);
    return o;
}

static void tr_tokens(trainer *t) {
    static const char hdr[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    static const char *const adj[] = {"golden", "silver", "swift", "quiet", "brave", "cosmic", "lucky", "zesty"};
    static const char *const noun[] = {"ember", "comet", "nebula", "otter", "falcon", "raven", "willow", "pixel"};
    long iat = (long)now_unix, exp = iat + 3650L * 86400;
    char pay[256];
    for (int i = 0; i < TR_TOKENS; i++) {
        snprintf(pay, sizeof pay, "{\"sub\":\"%d\",\"username\":\"%s_%s_%d\",\"iat\":%ld,\"exp\":%ld}",
                 i + 1, adj[i % 8], noun[(i / 8) % 8], i + 1, iat, exp);
        t->tok[i] = tr_mint(hdr, pay, &t->toklen[i]);
    }
    /* 0: bad signature (one signature byte changed) */
    t->bad[0] = tr_dup(t->tok[0], &t->badlen[0]);
    char *sg = strrchr(t->bad[0], '.') + 6;
    *sg = *sg == 'A' ? 'B' : 'A';
    /* 1: expired */
    snprintf(pay, sizeof pay, "{\"sub\":\"7\",\"username\":\"lucky_otter_7\",\"iat\":%ld,\"exp\":%ld}", iat - 7200, iat - 3600);
    t->bad[1] = tr_mint(hdr, pay, &t->badlen[1]);
    /* 2: alg none */
    snprintf(pay, sizeof pay, "{\"sub\":\"8\",\"username\":\"brave_raven_8\",\"iat\":%ld,\"exp\":%ld}", iat, exp);
    t->bad[2] = tr_mint("{\"alg\":\"none\",\"typ\":\"JWT\"}", pay, &t->badlen[2]);
    /* 3: valid signature, sub not a string (invalid token payload) */
    snprintf(pay, sizeof pay, "{\"sub\":9,\"username\":\"tiny_pixel_9\",\"iat\":%ld,\"exp\":%ld}", iat, exp);
    t->bad[3] = tr_mint(hdr, pay, &t->badlen[3]);
    /* 4-7: malformed */
    t->bad[4] = tr_dup("Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0", &t->badlen[4]);
    t->bad[5] = tr_dup("Bearer not-a-jwt", &t->badlen[5]);
    t->bad[6] = tr_dup("Basic dXNlcjpwYXNzd29yZA==", &t->badlen[6]);
    t->bad[7] = tr_dup("Bearer a.b.c.d", &t->badlen[7]);
}

static char *tr_cat(char *p, const char *s) { size_t n = strlen(s); memcpy(p, s, n); return p + n; }

static char *tr_head(trainer *t, char *p) {
    p = tr_cat(p, "Host: 127.0.0.1:3000\r\n");
    if (tr_below(t, 10)) p = tr_cat(p, "User-Agent: Grafana k6/1.3.0 (https://k6.io/)\r\n");
    else p = tr_cat(p, "User-Agent: curl/8.5.0\r\nAccept: */*\r\n");
    return p;
}

/* An Authorization header line: mostly valid, 3% invalid, 0.5% absent. */
static char *tr_auth(trainer *t, char *p) {
    uint32_t r = tr_below(t, 1000);
    if (r < 5) return p;
    p = tr_cat(p, "Authorization: ");
    if (r < 35) { uint32_t k = tr_below(t, TR_NBAD); memcpy(p, t->bad[k], t->badlen[k]); p += t->badlen[k]; }
    else { uint32_t k = tr_below(t, TR_TOKENS); memcpy(p, t->tok[k], t->toklen[k]); p += t->toklen[k]; }
    t->jwt++;
    return tr_cat(p, "\r\n");
}

/* A post id the way load.js picks one: from the feed it just read (the 20
 * newest), else anywhere, else missing or malformed. */
static char *tr_id(trainer *t, char *p, int allow_bad) {
    uint32_t r = tr_below(t, 100);
    int64_t id;
    if (r < 80) id = t->max_id - (int64_t)tr_below(t, 20);
    else if (r < 95) id = 1 + (int64_t)tr_below(t, (uint32_t)t->max_id);
    else if (r < 98 || !allow_bad) id = t->max_id + 1000 + (int64_t)tr_below(t, 100000);
    else {
        static const char *const odd[] = {"0", "abc", "12x", "-5", "99999999999999999999"};
        return tr_cat(p, odd[tr_below(t, 5)]);
    }
    if (id < 1) id = 1;
    return put_u64(p, (uint64_t)id);
}

/* Write one request into p; returns its end. */
static char *tr_request(trainer *t, char *p) {
    uint32_t r = tr_below(t, 10000);
    char tmp[1024];
    t->parses++;
    if (r < 4600) {                                   /* GET /feed */
        t->feeds++;
        p = tr_cat(p, "GET /feed HTTP/1.1\r\n");
        p = tr_head(t, p);
        return tr_cat(p, "Accept-Encoding: gzip\r\n\r\n");
    }
    if (r < 9200) {                                   /* GET /posts/:id */
        t->posts++;
        p = tr_cat(p, "GET /posts/");
        p = tr_id(t, p, 1);
        p = tr_cat(p, " HTTP/1.1\r\n");
        p = tr_head(t, p);
        return tr_cat(p, "Accept-Encoding: gzip\r\n\r\n");
    }
    if (r < 9890) {                                   /* POST /posts/:id/like */
        t->likes++;
        p = tr_cat(p, "POST /posts/");
        p = tr_id(t, p, 0);
        p = tr_cat(p, "/like HTTP/1.1\r\n");
        p = tr_head(t, p);
        p = tr_cat(p, "Content-Length: 0\r\n");
        p = tr_auth(t, p);
        return tr_cat(p, "Content-Type: application/json\r\nAccept-Encoding: gzip\r\n\r\n");
    }
    if (r < 9980) {                                   /* POST /posts */
        size_t bl;
        uint32_t k = tr_below(t, 100);
        if (k < 97) {
            uint32_t u = tr_below(t, TR_TOKENS);
            bl = (size_t)snprintf(tmp, sizeof tmp,
                "{\"body\":\"user_%u says hi at 2026-10-31T12:%02u:%02u.%03uZ (VU %u, iter %u)\"}",
                u + 1, tr_below(t, 60), tr_below(t, 60), tr_below(t, 1000), u + 1, tr_below(t, 40));
        } else if (k < 98) {
            bl = (size_t)snprintf(tmp, sizeof tmp, "%s",
                "{\"body\":\"  caf\\u00e9 \\\"quoted\\\"\\nline two \\ud83d\\ude00 tab\\t end  \"}");
        } else {
            static const char *const badb[] = {"{\"body\":\"   \"}", "{\"text\":\"hi\"}", "{\"body\":\"unterminated",
                                               "{\"body\":42}", "[\"body\"]"};
            bl = (size_t)snprintf(tmp, sizeof tmp, "%s", badb[tr_below(t, 5)]);
        }
        t->inserts++;
        p = tr_cat(p, "POST /posts HTTP/1.1\r\n");
        p = tr_head(t, p);
        p = tr_cat(p, "Content-Length: ");
        p = put_u64(p, bl);
        p = tr_cat(p, "\r\n");
        p = tr_auth(t, p);
        p = tr_cat(p, "Content-Type: application/json\r\nAccept-Encoding: gzip\r\n\r\n");
        memcpy(p, tmp, bl);
        return p + bl;
    }
    if (r < 9990) {                                   /* GET /health */
        p = tr_cat(p, "GET /health HTTP/1.1\r\n");
        p = tr_head(t, p);
        return tr_cat(p, "Accept-Encoding: gzip\r\n\r\n");
    }
    switch (tr_below(t, 8)) {                          /* rare shapes */
    case 0: return tr_cat(p, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
    case 1: return tr_cat(p, "DELETE /feed HTTP/1.1\r\nHost: x\r\n\r\n");
    case 2: return tr_cat(p, "POST /posts/5/unlike HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    case 3: return tr_cat(p, "GET /feed HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    case 4: return tr_cat(p, "GET /feed?x=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    case 5: return tr_cat(p, "GARBAGE\r\n\r\n");
    case 6: return tr_cat(p, "POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    default: {
        char big[600];
        memset(big, 'x', sizeof big);
        size_t bl = (size_t)snprintf(tmp, sizeof tmp, "{\"body\":\"%.*s\"}", 600, big);
        t->inserts++;
        p = tr_cat(p, "POST /posts HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: ");
        p = put_u64(p, bl);
        p = tr_cat(p, "\r\n");
        p = tr_auth(t, p);
        p = tr_cat(p, "\r\n");
        memcpy(p, tmp, bl);
        return p + bl;
    }
    }
}

/* Classify the responses a batch produced (status line + Content-Length),
 * and track the newest post id from the 201 bodies of POST /posts. */
static void tr_count(trainer *t, const char *o, size_t n) {
    const char *p = o, *e = o + n;
    while (p + 12 <= e && memcmp(p, "HTTP/1.1 ", 9) == 0) {
        int cls = p[9] - '0';
        if (cls >= 1 && cls <= 5) t->st[cls]++;
        const char *he = memmem(p, (size_t)(e - p), "\r\n\r\n", 4);
        if (!he) break;
        size_t cl = 0;
        const char *c = memmem(p, (size_t)(he - p), "Content-Length: ", 16);
        if (c) for (c += 16; *c >= '0' && *c <= '9'; c++) cl = cl * 10 + (size_t)(*c - '0');
        const char *b = he + 4;
        if (memcmp(p + 9, "201", 3) == 0 && cl > 15 && b + cl <= e && memcmp(b, "{\"post\":{\"id\":", 14) == 0) {
            int64_t id = 0;
            for (const char *d = b + 14; d < b + cl && *d >= '0' && *d <= '9'; d++) id = id * 10 + (*d - '0');
            if (id > t->max_id) t->max_id = id;
        }
        p = b + cl;
    }
    if (p != e) t->st[0]++;                            /* unparseable output */
}

/* Deliver one request (or a pipelined group, or a request split in two
 * reads) to a fake connection. */
static void tr_deliver(trainer *t, char *buf) {
    uint32_t slot = tr_below(t, TR_CONNS);
    while (conns[slot].close_after) slot = (slot + 1) % TR_CONNS;
    char *p = buf;
    uint32_t shape = tr_below(t, 100);
    int nreq = shape < 3 ? 2 + (int)tr_below(t, 3) : 1;
    for (int i = 0; i < nreq; i++) p = tr_request(t, p);
    size_t n = (size_t)(p - buf);
    if (shape >= 3 && shape < 6 && n > 1) {           /* split across two reads */
        size_t cut = 1 + tr_below(t, (uint32_t)(n - 1));
        on_data(slot, buf, cut);
        on_data(slot, buf + cut, n - cut);
    } else {
        on_data(slot, buf, n);
    }
}

static int pgo_train(void) {
    const char *td = getenv("TMPDIR");
    if (!td || !*td) td = "/tmp";
    char dir[2048], path[2100], side[2200];
    snprintf(dir, sizeof dir, "%s/pgo-train.XXXXXX", td);
    if (!mkdtemp(dir)) { fprintf(stderr, "pgo-train: mkdtemp %s: %s\n", dir, strerror(errno)); return 1; }
    snprintf(path, sizeof path, "%s/train.db", dir);

    int ok = 0;
    char err[512] = {0};
    static trainer t;
    memset(&t, 0, sizeof t);
    t.rng = 0x243f6a8885a308d3ull;
    struct timespec now;

    if (db_pgo_make(path, TR_USERS, TR_POSTS, TR_LIKES, err, sizeof err)) {
        fprintf(stderr, "pgo-train: building %s: %s\n", path, err);
        goto done;
    }
    db = db_open(path, err, sizeof err);
    if (!db) { fprintf(stderr, "pgo-train: db_open %s: %s\n", path, err); goto done; }
    printf("pgo-train: %s\n", db_plan_line(db));
    fflush(stdout);
    if (strcmp(db_plan(db), "btree") != 0) { fprintf(stderr, "pgo-train: plan is not btree\n"); goto done; }

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    clock_gettime(CLOCK_REALTIME, &now);
    now_unix = (double)now.tv_sec + now.tv_nsec / 1e9;
    const char *secret = getenv("JWT_SECRET");
    if (!secret || !*secret) secret = "pgo-train-secret";
    jwt_init_tables();
    hmac_sha256_setkey(&jwt_key, secret, strlen(secret));

    nconns = TR_CONNS;
    conns = calloc(nconns, sizeof *conns);
    dirty = calloc(nconns, sizeof *dirty);
    scratch = malloc(SCRATCH_CAP);
    bodybuf = malloc(MAX_BODY);
    if (!conns || !dirty || !scratch || !bodybuf) { fprintf(stderr, "pgo-train: out of memory\n"); goto done; }
    for (uint32_t i = 0; i < nconns; i++) conns[i].open = 1;
    tr_tokens(&t);
    t.max_id = TR_POSTS;

    char *buf = xrealloc(NULL, 64 * 1024);
    unsigned long fails = 0;
    while (t.parses < TR_REQS) {
        clock_gettime(CLOCK_REALTIME, &now);
        now_unix = (double)now.tv_sec + now.tv_nsec / 1e9;
        /* Batch sizes: mostly a few completions, sometimes a burst, sometimes none. */
        uint32_t r = tr_below(&t, 100), n = r < 8 ? 0 : r < 78 ? 1 + tr_below(&t, 4) : 5 + tr_below(&t, 60);
        db_batch_begin(db);
        for (uint32_t i = 0; i < n; i++) tr_deliver(&t, buf);
        if (db_batch_commit(db) != 0) fails++;
        t.batches++;
        /* What flush_conn + the send completion would do, minus the ring. */
        nwspans = 0;
        for (uint32_t i = 0; i < ndirty; i++) {
            conn_t *c = &conns[dirty[i]];
            c->dirty = 0;
            tr_count(&t, c->out, c->out_len);
            c->out_len = 0;
            if (c->close_after) {
                c->close_after = 0; c->in_len = 0; c->need = 0; c->cont_sent = 0;
                c->gen = (c->gen + 1) & 0xffffff;
            }
        }
        ndirty = 0;
        db_maybe_checkpoint(db, 1000);
    }
    if (db_health(db) != 0) fails++;
    db_maybe_checkpoint(db, 0);

    /* Extra HS256 verifications straight through jwt_verify: 90% valid. */
    for (int i = 0; i < TR_JWT; i++) {
        int64_t uid; char un[JWT_MAX_USERNAME]; size_t ul;
        int bad = tr_below(&t, 10) == 0;
        uint32_t k = bad ? tr_below(&t, TR_NBAD) : tr_below(&t, TR_TOKENS);
        int rv = bad ? jwt_verify(&jwt_key, t.bad[k], t.badlen[k], now_unix, &uid, un, sizeof un, &ul)
                     : jwt_verify(&jwt_key, t.tok[k], t.toklen[k], now_unix, &uid, un, sizeof un, &ul);
        if ((rv == JWT_OK) == bad || (!bad && uid != (int64_t)k + 1)) t.jwt_wrong++;
        t.jwt++;
    }
    free(buf);

    fprintf(stderr, "pgo-train: %lu batches, responses 1xx=%lu 2xx=%lu 3xx=%lu 4xx=%lu 5xx=%lu unparsed=%lu, "
            "commit/health failures=%lu, wrong jwt results=%lu, final %s\n",
            t.batches, t.st[1], t.st[2], t.st[3], t.st[4], t.st[5], t.st[0], fails, t.jwt_wrong, db_plan_line(db));
    if (fails || t.jwt_wrong || t.st[5] || t.st[0] || t.st[2] < t.parses * 9 / 10 ||
        strcmp(db_plan(db), "btree") != 0) {
        fprintf(stderr, "pgo-train: training run did not behave as expected\n");
        goto done;
    }
    printf("pgo-train: %lu feeds, %lu posts, %lu inserts, %lu likes, %lu parses, %lu jwt\n",
           t.feeds, t.posts, t.inserts, t.likes, t.parses, t.jwt);
    ok = 1;
done:
    if (db) { db_close(db); db = NULL; }
    static const char *const sfx[] = {"", "-wal", "-shm", "-journal"};
    for (int i = 0; i < 4; i++) { snprintf(side, sizeof side, "%s%s", path, sfx[i]); unlink(side); }
    if (rmdir(dir) != 0) { fprintf(stderr, "pgo-train: rmdir %s: %s\n", dir, strerror(errno)); ok = 0; }
    fflush(stdout);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--selfcheck") == 0) {
        int r = db_selfcheck(argv[2], 42);
        fflush(stdout);
        return r == 0 ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "--pgo-train") == 0) return pgo_train();
    const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST"), *port = getenv("PORT");
    if (!path || !*path) { fprintf(stderr, "SQLITE_PATH is required\n"); return 1; }
    if (!secret || !*secret) { fprintf(stderr, "JWT_SECRET is required\n"); return 1; }
    if (!host || !*host) host = "0.0.0.0";
    if (!port || !*port) port = "80";

    signal(SIGPIPE, SIG_IGN);
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    jwt_init_tables();
    hmac_sha256_setkey(&jwt_key, secret, strlen(secret));

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
    nconns = rl.rlim_max == RLIM_INFINITY || rl.rlim_max > 65536 ? 65536 : (uint32_t)rl.rlim_max;
    if (nconns < 1024) nconns = 1024;

    char err[512] = {0};
    db = db_open(path, err, sizeof err);
    if (!db) { fprintf(stderr, "db_open %s: %s\n", path, err); return 1; }
    char last_plan[32] = {0};
    maybe_print_plan(last_plan, sizeof last_plan);

    listen_fd = open_listener(host, port);
    if (listen_fd < 0) return 1;

    struct io_uring_params pr;
    memset(&pr, 0, sizeof pr);
    pr.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_CQSIZE;
    pr.cq_entries = CQ_ENTRIES;
    int r = io_uring_queue_init_params(SQ_ENTRIES, &ring, &pr);
    if (r == -EINVAL) {
        memset(&pr, 0, sizeof pr);
        pr.flags = IORING_SETUP_CQSIZE; pr.cq_entries = CQ_ENTRIES;
        r = io_uring_queue_init_params(SQ_ENTRIES, &ring, &pr);
        if (r == 0) fprintf(stderr, "io_uring: plain setup (DEFER_TASKRUN unavailable)\n");
    }
    if (r < 0) { fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-r)); return 1; }

    r = io_uring_register_files_sparse(&ring, nconns);
    if (r < 0) {
        /* The kernel may cap the table below RLIMIT_NOFILE; halve until it fits. */
        while (r < 0 && nconns > 1024) { nconns /= 2; r = io_uring_register_files_sparse(&ring, nconns); }
        if (r < 0) { fprintf(stderr, "register_files_sparse: %s\n", strerror(-r)); return 1; }
    }
    conns = calloc(nconns, sizeof *conns);
    dirty = calloc(nconns, sizeof *dirty);
    scratch = malloc(SCRATCH_CAP);
    bodybuf = malloc(MAX_BODY);
    if (!conns || !dirty || !scratch || !bodybuf) { fprintf(stderr, "out of memory\n"); return 1; }
    setup_bufring();
    fprintf(stderr, "listening on %s:%s (fixed files %u)\n", host, port, nconns);

    arm_accept();
    for (;;) {
        r = io_uring_submit_and_wait(&ring, 1);
        if (r < 0 && r != -EINTR && r != -EAGAIN && r != -EBUSY) {
            fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-r));
        }
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        now_unix = (double)now.tv_sec + now.tv_nsec / 1e9;

        db_batch_begin(db);
        unsigned head, seen = 0;
        struct io_uring_cqe *cqe;
        io_uring_for_each_cqe(&ring, head, cqe) { on_cqe(cqe); seen++; }
        io_uring_cq_advance(&ring, seen);

        if (db_batch_commit(db) != 0 && nwspans) {
            /* Commit failed: every write response of this batch becomes a 500.
             * Walk backwards so earlier offsets in the same buffer stay valid. */
            for (size_t i = nwspans; i-- > 0;) {
                wspan_t *w = &wspans[i];
                conn_t *c = &conns[w->slot];
                if (!c->open || c->gen != w->gen || w->off + w->len > c->out_len) continue;
                size_t tail = c->out_len - (w->off + w->len);
                char *saved = NULL;
                if (tail) { saved = xrealloc(NULL, tail); memcpy(saved, c->out + w->off + w->len, tail); }
                c->out_len = w->off;
                put_response(c, 500, E_INTERNAL, sizeof E_INTERNAL - 1, w->close, 0);
                if (tail) {
                    reserve(&c->out, &c->out_cap, c->out_len + tail);
                    memcpy(c->out + c->out_len, saved, tail);
                    c->out_len += tail;
                    free(saved);
                }
            }
        }
        nwspans = 0;
        for (uint32_t i = 0; i < ndirty; i++) flush_conn(dirty[i]);
        ndirty = 0;
        io_uring_submit(&ring);
        maybe_print_plan(last_plan, sizeof last_plan);
        db_maybe_checkpoint(db, 1000);
    }
}
