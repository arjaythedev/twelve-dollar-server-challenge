/*
 * The HTTP side: one io_uring loop on one thread, which is what the one-vCPU box has room for.
 *
 *   accept   multishot, into the ring's table of direct descriptors
 *   receive  multishot, into buffers lent from a ring of provided buffers
 *   reads    answered on the loop thread, from its own SQLite connection
 *   writes   handed to the writer thread; the connection is parked and answered when the
 *            transaction holding its row has committed (see db.c, group commit)
 *
 * The loop enters the kernel once per pass: that one call hands over every response of the
 * pass before and brings back every request that arrived since. Under load it lets requests
 * collect for a moment first, so the cost of the call is shared by all of them.
 *
 * Nothing of a connection crosses a thread. A parked write is a slot; the writer gets the job
 * inside that slot and gives it back through a queue the loop is woken for.
 */
#include "server.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "picohttpparser.h"

#include "arnm/arena.h"
#include "arnm/graded_arena_pool.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"
#include "arnm/mono_timer.h"

#include "db.h"
#include "jwt.h"
#include "parse_id.h"
#include "uring.h"

/* Writes that may be waiting for their commit. Beyond it a write is answered 503. */
#define SLOTS 2048
/* How long a keep-alive connection may sit idle: longer than the pause of a user who is
 * reading. */
#define IDLE_TIMEOUT_S 120
/* How long a request that arrived in part, or a response the client does not take, may make no
 * progress. */
#define STALL_TIMEOUT_S 30
#define SWEEP_S 5
#define MAX_HEAD (16 * 1024)
#define MAX_BODY (16 * 1024)
#define MAX_HEADERS 32
/* Bytes one connection may have waiting: a request that is still arriving, or requests sent
 * ahead of the answer to the one before. */
#define MAX_PENDING (MAX_HEAD + MAX_BODY + 4096)
/* The most connections the table is made for, whatever the descriptor limit says. */
#define MAX_CONNS (1u << 18)

#define SQ_ENTRIES 4096
#define CQ_ENTRIES 16384
/* Buffers lent to receives. A request fits one; a longer one arrives in several. The kernel
 * is offered all of a buffer but its last bytes: the JSON reader parses a request body where it
 * arrived and wants that much room behind it. */
#define RECV_BUFFERS 4096
#define RECV_BUFFER_SIZE 2048
#define RECV_BUFFER_SPARE ARNM_JSON_READER_INSITU_PADDING
/* Under load the loop waits for this many completions or this long, whichever comes first.
 * Measured: without the wait a request costs a little more, with a longer one no less. */
#define WAIT_COMPLETIONS 32
#define WAIT_NS 1000000
#define IDLE_WAIT_NS 1000000000

/* The arena the writer builds a document in: its values. */
#define WRITER_ARENA (256u * 1024u)
/* The arena a document's text is rendered in before it is sent: the serializer wants six bytes
 * free for every byte of a string it is about to write, and the largest feed is 20 posts of
 * 2000 bytes. */
#define TEXT_ARENA (512u * 1024u)
/* Arenas kept in stock per size once they have been used; more than that many of one size
 * lent out at once are served by the host and given back to it. */
#define ARENAS_SPARE 64u
/* The sizes a lent arena comes in. Two things ask for one: a response on its way out, and the
 * document of a parsed request body, see post_create. */
static const uint32_t arena_sizes[] = {1024u, 4096u, 16384u, 65536u, 262144u};
/* The status line and the two headers of a response. */
#define HEAD_MAX 128

/* One per descriptor in the ring's table, at the descriptor's index. */
typedef struct conn {
    /* Input that could not be served when it arrived: the start of a request, or requests sent
     * ahead. NULL while there is none, which is nearly always. */
    char *in;
    uint32_t in_len, in_cap;
    /* What is left of the response on its way out, and the lent arena it lies in (NULL for a
     * literal). */
    const char *out;
    arnm *out_arena;
    uint32_t out_len;
    uint32_t last_active;
    bool open : 1;
    bool receiving : 1;   /* the multishot receive is armed */
    bool sending : 1;     /* a send is in the ring */
    bool parked : 1;      /* a write of this connection is with the writer thread */
    bool continued : 1;   /* 100 Continue was sent for the request that is arriving */
    bool close_after : 1; /* close once the response is out */
    bool closing : 1;     /* no more requests; closed when nothing of it is in flight */
    bool close_sent : 1;
} conn;

typedef struct slot {
    db_job job;
    conn *conn;
    uint32_t author_len;
    char author[JWT_NAME_MAX];
    /* A create's request body, parsed where it lay: the job's `body` points into it, so it is
     * held until the writer is done with the job and the answer is written. That is the buffer
     * the request was received in, or -1 when the body had to be copied first -- into
     * `body_arena`, which holds the parsed document in either case. NULL for a like. */
    arnm *body_arena;
    int recv_buffer;
    struct slot *next; /* in the free list, or in the list of finished writes */
} slot;

static struct {
    uring ring;
    conn *conns;
    uint32_t max_conns;
    uint32_t now; /* seconds, read once per pass */
    int listen_fd;
    bool accepting;
    db_reader *db;
    slot *slots, *free_slots;
    /* The buffer the input being served was received in, and where that input ends; -1 when
     * there is none, or a parked write has taken it over. */
    int lent;
    const char *lent_end;
    /* One writer, used for every response. Its arena holds the document while it is built;
     * the release after each write gives that back. */
    arnm writer_arena;
    arnm_json_writer writer;
    /* Where a document's text is rendered; started over with every response. */
    arnm text_arena;
    /* Arenas lent for as long as one piece of work needs them: a response until it has been
     * sent, the document of a request body for as long as its write is parked. */
    arnm_graded_arena_pool arenas;
    /* Writes the writer thread has finished, and what wakes the loop for them. */
    pthread_mutex_t done_mu;
    slot *done_head, *done_tail;
    int done_fd;
    uint64_t done_count;
} L = {.done_mu = PTHREAD_MUTEX_INITIALIZER, .lent = -1};

static arnm_mono_timer started;

/* --- the ring ----------------------------------------------------------------------------- */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_DONE, OP_CANCEL };

#define USER_DATA(op, index) ((uint64_t)(op) << 56 | (uint64_t)(index))

static struct io_uring_sqe *sqe_for(int op, uint32_t index)
{
    struct io_uring_sqe *sqe = uring_sqe(&L.ring);

    if (sqe == NULL) {
        perror("io_uring_enter");
        exit(1);
    }
    sqe->user_data = USER_DATA(op, index);
    return sqe;
}

static inline uint32_t index_of(const conn *c)
{
    return (uint32_t)(c - L.conns);
}

static void arm_accept(void)
{
    struct io_uring_sqe *sqe = sqe_for(OP_ACCEPT, 0);

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = L.listen_fd;
    sqe->ioprio = IORING_ACCEPT_MULTISHOT;
    sqe->file_index = IORING_FILE_INDEX_ALLOC;
    L.accepting = true;
}

static void arm_recv(conn *c)
{
    struct io_uring_sqe *sqe = sqe_for(OP_RECV, index_of(c));

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = (int)index_of(c);
    sqe->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->buf_group = 0;
    c->receiving = true;
}

static void submit_send(conn *c)
{
    struct io_uring_sqe *sqe = sqe_for(OP_SEND, index_of(c));

    sqe->opcode = IORING_OP_SEND;
    sqe->fd = (int)index_of(c);
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->addr = (uint64_t)(uintptr_t)c->out;
    sqe->len = c->out_len;
    sqe->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
}

static void cancel(int op, conn *c)
{
    struct io_uring_sqe *sqe = sqe_for(OP_CANCEL, 0);

    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = -1;
    sqe->addr = USER_DATA(op, index_of(c));
}

static void arm_done(void)
{
    struct io_uring_sqe *sqe = sqe_for(OP_DONE, 0);

    sqe->opcode = IORING_OP_READ;
    sqe->fd = L.done_fd;
    sqe->addr = (uint64_t)(uintptr_t)&L.done_count;
    sqe->len = sizeof L.done_count;
}

/* --- connections -------------------------------------------------------------------------- */

static inline bool busy(const conn *c)
{
    return c->sending || c->parked;
}

/* The descriptor is closed when nothing of the connection is in the ring or with the writer
 * any more -- so no completion can arrive for an index that already belongs to someone else. */
static void close_if_idle(conn *c)
{
    struct io_uring_sqe *sqe;

    if (!c->closing || c->receiving || c->sending || c->parked || c->close_sent)
        return;
    sqe = sqe_for(OP_CLOSE, index_of(c));
    sqe->opcode = IORING_OP_CLOSE;
    sqe->file_index = index_of(c) + 1;
    c->close_sent = true;
}

/* No more requests from this connection. The receive is called back; a response on its way out
 * is called back as well when @p drop says the client is not taking it. */
static void close_conn(conn *c, bool drop)
{
    if (!c->closing) {
        c->closing = true;
        if (c->receiving)
            cancel(OP_RECV, c);
    }
    if (drop && c->sending)
        cancel(OP_SEND, c);
    close_if_idle(c);
}

static void on_closed(conn *c)
{
    free(c->in);
    memset(c, 0, sizeof *c);
    /* A full table stopped the accept; there is room again. */
    if (!L.accepting)
        arm_accept();
}

/* --- replies ------------------------------------------------------------------------------ */

static const char *reason_of(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static uint32_t write_head(char *dst, int status, uint32_t len)
{
    return (uint32_t)snprintf(dst, HEAD_MAX,
                              "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                              status, reason_of(status), len);
}

/* @p text must stay where it is until it has been sent: a literal, or bytes in @p arena, which
 * goes back to the pool then. */
static void send_start(conn *c, const char *text, uint32_t len, arnm *arena)
{
    c->out = text;
    c->out_len = len;
    c->out_arena = arena;
    c->sending = true;
    c->last_active = L.now;
    submit_send(c);
}

/* A response that is the same every time: put together when it is first needed, sent from
 * where it lies ever after. */
typedef struct {
    uint32_t len;
    char text[HEAD_MAX + 64];
} literal;

static void reply_literal(conn *c, int status, literal *l, const char *json, uint32_t len)
{
    if (l->len == 0) {
        const uint32_t head = write_head(l->text, status, len);

        memcpy(l->text + head, json, len);
        l->len = head + len;
    }
    send_start(c, l->text, l->len, NULL);
}

#define REPLY_ERROR(c, status, message)                                                            \
    do {                                                                                           \
        static literal l;                                                                          \
        reply_literal(c, status, &l, "{\"error\":\"" message "\"}",                                \
                      sizeof("{\"error\":\"" message "\"}") - 1);                                  \
    } while (0)

/* Starts the loop's writer on the next response, which will hold @p values values -- so the
 * document's pool is opened once, at that size. No string is copied into a document here:
 * every one of them is borrowed. */
static arnm_json_writer *json_begin(uint32_t values)
{
    const arnm_json_writer_hint hint = {.values = values, .string_bytes = 0};

    (void)arnm_json_writer_init(&L.writer, &L.writer_arena, ARNM_JSON_WRITE_DEFAULT, &hint);
    return &L.writer;
}

/* Lets go of a document, sent or not: its memory goes back to the writer's arena. */
static void json_discard(void)
{
    (void)arnm_json_writer_release(&L.writer);
}

/*
 * Renders the writer's document and sends it.
 *
 * The text is rendered in the loop's text arena, which has room for the worst case, and then
 * set behind its head in an arena lent for exactly that many bytes. That is the one copy made
 * of a response: head and text leave in one piece, and the arena goes back when they have.
 */
static void reply_json(conn *c, int status)
{
    arnm_memory_block text = {0};
    uint32_t len = 0, head;
    arnm *arena = NULL;
    uint8_t *out = NULL;
    arnm_result result;

    arnm_reset(&L.text_arena);
    result = arnm_json_writer_write(&L.writer, &L.text_arena, &text, &len);
    json_discard();
    if ((result != ARNM_SUCCESS && result != ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED) ||
        arnm_graded_arena_pool_alloc(&L.arenas, HEAD_MAX + len, &arena) != ARNM_SUCCESS) {
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    if (arnm_alloc(&out, HEAD_MAX + len, arena) != ARNM_SUCCESS) {
        (void)arnm_graded_arena_pool_free(&L.arenas, arena);
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    head = write_head((char *)out, status, len);
    memcpy(out + head, text.data, len);
    send_start(c, (const char *)out, head + len, arena);
}

/* --- parked writes ------------------------------------------------------------------------ */

static slot *park(conn *c)
{
    slot *s = L.free_slots;

    if (s == NULL)
        return NULL;
    L.free_slots = s->next;
    s->conn = c;
    s->body_arena = NULL;
    s->recv_buffer = -1;
    c->parked = true;
    return s;
}

/* On the writer thread: hand the slot back to the loop. The loop is woken for the first of a
 * run of them and takes the rest along. */
static void on_job_done(db_job *job)
{
    slot *s = (slot *)((char *)job - offsetof(slot, job));
    const uint64_t one = 1;
    bool wake;

    s->next = NULL;
    pthread_mutex_lock(&L.done_mu);
    wake = L.done_head == NULL;
    if (L.done_tail != NULL)
        L.done_tail->next = s;
    else
        L.done_head = s;
    L.done_tail = s;
    pthread_mutex_unlock(&L.done_mu);
    if (wake && write(L.done_fd, &one, sizeof one) < 0)
        perror("eventfd");
}

static void answer_write(slot *s)
{
    conn *c = s->conn;
    const db_job *job = &s->job;
    arnm_json_writer *w;

    if (job->status == 404) {
        REPLY_ERROR(c, 404, "post not found");
        return;
    }
    if (job->status != 200 && job->status != 201) {
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    w = json_begin(job->kind == JOB_LIKE ? 7u : DB_POST_VALUES);
    if (job->kind == JOB_LIKE) {
        arnm_json_writer_add_bool(w, ARNM_JSON_WRITER_KEY("liked"), true);
        arnm_json_writer_add_bool(w, ARNM_JSON_WRITER_KEY("already_liked"), job->status == 200);
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("post_id"), (uint64_t)job->post_id);
    } else {
        /* Everything borrowed here stays put until the text is rendered: the slot is this
         * request's until it is answered. */
        arnm_json_writer_open_object(w, ARNM_JSON_WRITER_KEY("post"));
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("id"), (uint64_t)job->post_id);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("body"), job->body, job->body_len,
                                          ARNM_JSON_WRITER_STRING_ESCAPE);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("created_at"), job->created_at,
                                          job->created_len, ARNM_JSON_WRITER_STRING_ESCAPE);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("author"), s->author, s->author_len,
                                          ARNM_JSON_WRITER_STRING_ESCAPE);
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("like_count"), 0);
        arnm_json_writer_close(w);
    }
    reply_json(c, job->status);
}

static void drain(conn *c);

/* On the loop thread, for every job the writer finished. */
static void on_writes_done(void)
{
    slot *s, *next;

    pthread_mutex_lock(&L.done_mu);
    s = L.done_head;
    L.done_head = L.done_tail = NULL;
    pthread_mutex_unlock(&L.done_mu);

    for (; s != NULL; s = next) {
        conn *c = s->conn;

        next = s->next;
        c->parked = false;
        /* The client may have gone meanwhile; the write committed all the same. */
        if (!c->closing)
            answer_write(s);
        if (s->body_arena != NULL)
            (void)arnm_graded_arena_pool_free(&L.arenas, s->body_arena);
        if (s->recv_buffer >= 0)
            uring_buffer_return(&L.ring, (unsigned)s->recv_buffer);
        s->next = L.free_slots;
        L.free_slots = s;
        close_if_idle(c);
    }
}

/* --- handlers ----------------------------------------------------------------------------- */

/* Bytes of whitespace at @p p, as String.prototype.trim() counts it; 0 if there is none. */
static size_t space_at(const unsigned char *p, const unsigned char *end)
{
    if (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        return 1;
    if (end - p >= 2 && p[0] == 0xC2 && p[1] == 0xA0)
        return 2;
    if (end - p >= 3 && (p[0] & 0xF0) == 0xE0) {
        const uint32_t u = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);

        if (u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 ||
            u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF)
            return 3;
    }
    return 0;
}

static void trim(const unsigned char **start, const unsigned char **end)
{
    const unsigned char *p = *start, *e = *end;
    size_t n;

    while (p < e && (n = space_at(p, e)) != 0)
        p += n;
    for (;;) {
        /* The last character starts one to three bytes before the end. */
        if (e - p >= 1 && space_at(e - 1, e) == 1) e -= 1;
        else if (e - p >= 2 && space_at(e - 2, e) == 2) e -= 2;
        else if (e - p >= 3 && space_at(e - 3, e) == 3) e -= 3;
        else break;
    }
    *start = p;
    *end = e;
}

/* One request, as the parser found it in the connection's input. Nothing here outlives the
 * handler: what a parked write needs, the handler that parks it copies or takes over. */
typedef struct {
    const char *path, *auth, *body;
    size_t path_len, auth_len, body_len;
    bool get, post;
} request;

static int authenticate(conn *c, const request *req, int64_t *uid, char *name, size_t *name_len)
{
    switch (jwt_verify(req->auth, req->auth_len, uid, name, name_len)) {
    case JWT_OK: return 0;
    case JWT_MISSING: REPLY_ERROR(c, 401, "missing bearer token"); break;
    case JWT_INVALID: REPLY_ERROR(c, 401, "invalid or expired token"); break;
    default: REPLY_ERROR(c, 401, "invalid token payload"); break;
    }
    return -1;
}

static void get_feed(conn *c)
{
    arnm_json_writer *w = json_begin(DB_FEED_VALUES);
    uint32_t text_bytes = 0;

    if (db_read_feed(L.db, w, &text_bytes) != 0) {
        json_discard();
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    reply_json(c, 200);
}

static void get_post(conn *c, int64_t id)
{
    arnm_json_writer *w = json_begin(DB_POST_VALUES);
    uint32_t text_bytes = 0;

    switch (db_read_post(L.db, id, w, &text_bytes)) {
    case 1:
        reply_json(c, 200);
        return;
    case 0: REPLY_ERROR(c, 404, "post not found"); break;
    default: REPLY_ERROR(c, 500, "internal server error");
    }
    json_discard();
}

static void get_health(conn *c)
{
    const char *err = "";
    arnm_json_writer *w = json_begin(7u);

    if (db_ping(L.db, &err) == 0) {
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("status"), "ok", 2);
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("db"), "ok", 2);
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("uptime_s"), (uint64_t)floor(arnm_mono_timer_seconds(started)));
        reply_json(c, 200);
    } else {
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("status"), "degraded", 8);
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("db"), "unreachable", 11);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("error"), err, strlen(err), ARNM_JSON_WRITER_STRING_ESCAPE);
        reply_json(c, 503);
    }
}

static void post_like(conn *c, int64_t uid, int64_t id)
{
    slot *s;

    if ((s = park(c)) == NULL) {
        REPLY_ERROR(c, 503, "server busy");
        return;
    }
    s->job.kind = JOB_LIKE;
    s->job.user_id = uid;
    s->job.post_id = id;
    db_writer_submit(&s->job);
}

/*
 * The request body is parsed in place: the reader unescapes its strings into the bytes they
 * arrived in, and the `body` string it hands back lies there.
 *
 * Nearly always those bytes are the end of what one receive brought, in a buffer of the ring
 * with room behind them that the kernel was never offered. Then nothing is copied at all: the
 * parked write keeps the buffer, the writer thread reads the post's text from it, and it goes
 * back to the kernel when the write has been answered.
 *
 * A body that arrived in pieces lies in the connection's own memory, which moves, and one with
 * the next request behind it has no room for the reader's padding. Those are copied once, into
 * the arena of their document, and parsed there.
 *
 * The arena is sized for the densest document there is, one value of sixteen bytes in two bytes
 * of input: the reader writes through its input, so there is no second attempt with more room.
 */
static void post_create(conn *c, const request *req, int64_t uid, const char *name, size_t name_len)
{
    const uint32_t len = (uint32_t)req->body_len;
    const bool in_place = L.lent >= 0 && req->body + len == L.lent_end;
    char *text = (char *)(uintptr_t)req->body;
    arnm *arena = NULL;
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    arnm_memory_block body = {0};
    arnm_json_field fields[1] = {ARNM_JSON_FIELD_STRING("body", &body)};
    uint64_t found = 0;
    const unsigned char *p, *e;
    arnm_result parsed;
    size_t chars = 0;
    slot *s;

    if (len == 0) {
        REPLY_ERROR(c, 400, "malformed JSON body");
        return;
    }
    parsed = arnm_graded_arena_pool_alloc(
        &L.arenas, 9u * len + 1024u + (in_place ? 0u : len + ARNM_JSON_READER_INSITU_PADDING), &arena);
    if (parsed == ARNM_SUCCESS && !in_place) {
        uint8_t *copy = NULL;

        if ((parsed = arnm_alloc(&copy, len + ARNM_JSON_READER_INSITU_PADDING, arena)) == ARNM_SUCCESS) {
            memcpy(copy, req->body, len);
            text = (char *)copy;
        }
    }
    if (parsed == ARNM_SUCCESS && (parsed = arnm_json_reader_init(&reader, arena)) == ARNM_SUCCESS)
        parsed = arnm_json_reader_parse_insitu(&reader, text, len, len + ARNM_JSON_READER_INSITU_PADDING,
                                               false, &root);

    if (parsed == ARNM_ERROR_DECODE_FAILED) {
        REPLY_ERROR(c, 400, "malformed JSON body");
    } else if (parsed != ARNM_SUCCESS) {
        REPLY_ERROR(c, 500, "internal server error");
    } else {
        /* One field, so the table matches at the first key of the body the clients send. */
        arnm_json_read_object(root, fields, 1, &found);
        p = body.data;
        e = body.data + body.size;
        if (found == 1)
            trim(&p, &e);
        for (const unsigned char *q = p; found == 1 && q < e; q++)
            chars += (*q & 0xC0) != 0x80;

        if (found != 1 || p == e) {
            REPLY_ERROR(c, 400, "body is required");
        } else if (chars > 500 || (size_t)(e - p) > DB_BODY_MAX) {
            REPLY_ERROR(c, 400, "body must be at most 500 characters");
        } else if ((s = park(c)) == NULL) {
            REPLY_ERROR(c, 503, "server busy");
        } else {
            s->job.kind = JOB_CREATE;
            s->job.user_id = uid;
            s->job.body = (const char *)p;
            s->job.body_len = (uint32_t)(e - p);
            memcpy(s->author, name, name_len);
            s->author_len = (uint32_t)name_len;
            s->body_arena = arena;
            if (in_place) {
                /* The write keeps the buffer the body lies in. */
                s->recv_buffer = L.lent;
                L.lent = -1;
            }
            db_writer_submit(&s->job);
            return;
        }
    }
    /* Answered without a write: nothing holds the parsed body any longer. */
    if (arena != NULL)
        (void)arnm_graded_arena_pool_free(&L.arenas, arena);
}

#define POSTS_SLASH_LENGTH 7
static void route(conn *c, const request *req)
{
    const char *path = req->path;
    const size_t len = req->path_len;

    if (req->get) {
        if (len == 5 && memcmp(path, "/feed", 5) == 0) {
            get_feed(c);
            return;
        } else if (len >= POSTS_SLASH_LENGTH && memcmp(path, "/posts/", 7) == 0) {
            const int64_t id = parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH);

            if (id > 0) {
                get_post(c, id);
            } else if (memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH) != NULL) {
                REPLY_ERROR(c, 404, "not found");
            } else if (id == -1) {
                REPLY_ERROR(c, 404, "post not found");
            } else {
                REPLY_ERROR(c, 400, "invalid post id");
            }
            return;
        } else if (len == 7 && memcmp(path, "/health", 7) == 0) {
            get_health(c);
            return;
        }
    } else if (req->post && len >= 6 && memcmp(path, "/posts", 6) == 0) {
        char name[JWT_NAME_MAX];
        size_t name_len;
        int64_t uid;

        if (len == 6) {
            if (authenticate(c, req, &uid, name, &name_len) == 0)
                post_create(c, req, uid, name, name_len);
            return;
        } else if (len > POSTS_SLASH_LENGTH && path[6] == '/' && memcmp(path + len - 5, "/like", 5) == 0) {
            const int64_t id = len > POSTS_SLASH_LENGTH + 5
                                   ? parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH - 5)
                                   : 0;

            if (id <= 0 && memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH) != path + len - 5) {
                REPLY_ERROR(c, 404, "not found");
                return;
            }
            /* Auth first, then the id. */
            if (authenticate(c, req, &uid, name, &name_len) == 0) {
                if (id > 0) post_like(c, uid, id);
                else if (id == 0) REPLY_ERROR(c, 400, "invalid post id");
                else REPLY_ERROR(c, 404, "post not found");
            }
            return;
        }
    }
    REPLY_ERROR(c, 404, "not found");
}

/* --- requests ----------------------------------------------------------------------------- */

static inline bool header_is(const struct phr_header *h, const char *name, size_t len)
{
    return h->name_len == len && strncasecmp(h->name, name, len) == 0;
}

static inline bool value_is(const struct phr_header *h, const char *value, size_t len)
{
    return h->value_len == len && strncasecmp(h->value, value, len) == 0;
}

#define HEADER_IS(h, name) header_is(h, name, sizeof(name) - 1)
#define VALUE_IS(h, value) value_is(h, value, sizeof(value) - 1)

/*
 * Serves the request at the start of @p buf if all of it is there.
 *
 * > 0  its length: it was answered, or parked with the writer
 *   0  it has not arrived completely yet
 *  -1  it cannot be served; the answer says so and the connection closes behind it
 */
static int serve_one(conn *c, const char *buf, uint32_t len)
{
    static const char go_on[] = "HTTP/1.1 100 Continue\r\n\r\n";
    struct phr_header headers[MAX_HEADERS];
    size_t header_count = MAX_HEADERS, method_len;
    const char *method, *query;
    int minor, head;
    int64_t content_length = 0;
    bool keep_alive, close = false, keep = false, expects = false, chunked = false;
    request req = {0};

    head = phr_parse_request(buf, len, &method, &method_len, &req.path, &req.path_len, &minor, headers,
                             &header_count, 0);
    if (head == -2 && len < MAX_HEAD)
        return 0;
    if (head < 0) {
        c->close_after = true;
        REPLY_ERROR(c, 400, "bad request");
        return -1;
    }
    for (size_t i = 0; i < header_count; i++) {
        const struct phr_header *h = &headers[i];

        if (HEADER_IS(h, "content-length")) {
            content_length = parse_id(h->value, h->value_len);
            /* parse_id reads 0 for what is no number, and for "0". */
            if (content_length < 0 || (content_length == 0 && !VALUE_IS(h, "0")))
                content_length = -1;
        } else if (HEADER_IS(h, "authorization")) {
            req.auth = h->value;
            req.auth_len = h->value_len;
        } else if (HEADER_IS(h, "connection")) {
            close = VALUE_IS(h, "close");
            keep = VALUE_IS(h, "keep-alive");
        } else if (HEADER_IS(h, "expect")) {
            expects = VALUE_IS(h, "100-continue");
        } else if (HEADER_IS(h, "transfer-encoding")) {
            chunked = true;
        }
    }
    if (chunked || content_length < 0 || content_length > MAX_BODY) {
        c->close_after = true;
        if (content_length > MAX_BODY)
            REPLY_ERROR(c, 413, "body too large");
        else
            REPLY_ERROR(c, 400, "bad request");
        return -1;
    }
    if (len - (uint32_t)head < (uint32_t)content_length) {
        if (expects && !c->continued) {
            c->continued = true;
            send_start(c, go_on, sizeof go_on - 1, NULL);
        }
        return 0;
    }
    c->continued = false;
    keep_alive = minor >= 1 ? !close : keep;
    c->close_after = !keep_alive;

    req.get = method_len == 3 && memcmp(method, "GET", 3) == 0;
    req.post = method_len == 4 && memcmp(method, "POST", 4) == 0;
    if ((query = memchr(req.path, '?', req.path_len)) != NULL)
        req.path_len = (size_t)(query - req.path);
    req.body = buf + head;
    req.body_len = (size_t)content_length;
    route(c, &req);
    return head + (int)content_length;
}

/* Serves requests from @p buf one after the other for as long as the connection is free to
 * answer: a response is sent, or a write committed, before the next request is looked at.
 * Returns the bytes that were served. */
static uint32_t serve(conn *c, const char *buf, uint32_t len)
{
    uint32_t done = 0;

    while (done < len && !busy(c) && !c->closing && !c->close_after) {
        const int used = serve_one(c, buf + done, len - done);

        if (used <= 0)
            break;
        done += (uint32_t)used;
    }
    return done;
}

/* Keeps input for later. False when the connection has more waiting than it may: it is closed. */
static bool keep_input(conn *c, const char *data, uint32_t len)
{
    if (c->in_len + len > c->in_cap) {
        uint32_t cap = c->in_cap != 0 ? c->in_cap : 1024;
        char *grown;

        while (cap < c->in_len + len)
            cap *= 2;
        if (cap > MAX_PENDING || (grown = realloc(c->in, cap)) == NULL) {
            close_conn(c, true);
            return false;
        }
        c->in = grown;
        c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, data, len);
    c->in_len += len;
    return true;
}

/* Serves what was kept, now that the connection is free again. */
static void drain(conn *c)
{
    const uint32_t done = serve(c, c->in, c->in_len);

    if (c->closing)
        return;
    c->in_len -= done;
    if (c->in_len != 0) {
        memmove(c->in, c->in + done, c->in_len);
    } else {
        free(c->in);
        c->in = NULL;
        c->in_cap = 0;
    }
}

/* Nearly always a whole request on a connection that is waiting for one, and then it is served
 * from the buffer it arrived in and nothing is copied. */
static void on_input(conn *c, const char *data, uint32_t len)
{
    c->last_active = L.now;
    if (c->in_len == 0 && !busy(c)) {
        const uint32_t done = serve(c, data, len);

        if (done < len && !c->closing && !c->close_after)
            (void)keep_input(c, data + done, len - done);
    } else if (keep_input(c, data, len) && !busy(c)) {
        drain(c);
    }
}

/* --- completions -------------------------------------------------------------------------- */

static void on_accept(const struct io_uring_cqe *cqe)
{
    if (!(cqe->flags & IORING_CQE_F_MORE))
        L.accepting = false;
    if (cqe->res >= 0 && (uint32_t)cqe->res < L.max_conns) {
        conn *c = &L.conns[cqe->res];

        c->open = true;
        c->last_active = L.now;
        arm_recv(c);
    }
    /* Out of room: every connection the table holds is open. The accept is armed again when one
     * of them has closed, or at the next sweep; new users wait in the backlog until then. */
    if (!L.accepting && cqe->res != -ENFILE && cqe->res != -EMFILE && cqe->res != -ENOBUFS &&
        cqe->res != -ENOMEM)
        arm_accept();
}

static void on_recv(conn *c, const struct io_uring_cqe *cqe)
{
    if (!(cqe->flags & IORING_CQE_F_MORE))
        c->receiving = false;
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        const unsigned id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

        const char *data = (const char *)L.ring.buf_base + (size_t)id * L.ring.buf_size;

        L.lent = (int)id;
        L.lent_end = data + (cqe->res > 0 ? cqe->res : 0);
        if (cqe->res > 0 && !c->closing)
            on_input(c, data, (uint32_t)cqe->res);
        /* Whatever of it is still needed has been copied -- unless a write was parked with its
         * body in there, and then the buffer comes back with the write. */
        if (L.lent >= 0)
            uring_buffer_return(&L.ring, id);
        L.lent = -1;
    }
    if (cqe->res <= 0 && cqe->res != -ENOBUFS) {
        /* The client closed, or the connection broke, or the receive was called back. */
        c->closing = true;
        close_if_idle(c);
    } else if (!c->receiving && !c->closing) {
        arm_recv(c);
    }
}

static void on_sent(conn *c, const struct io_uring_cqe *cqe)
{
    if (cqe->res > 0 && (uint32_t)cqe->res < c->out_len && !c->closing) {
        /* A part of it went out; the rest follows. */
        c->out += cqe->res;
        c->out_len -= (uint32_t)cqe->res;
        submit_send(c);
        return;
    }
    c->sending = false;
    if (c->out_arena != NULL)
        (void)arnm_graded_arena_pool_free(&L.arenas, c->out_arena);
    c->out_arena = NULL;
    if (cqe->res <= 0 || c->close_after)
        close_conn(c, false);
    else if (c->in_len != 0 && !c->closing)
        drain(c);
    close_if_idle(c);
}

static unsigned reap(void)
{
    uring *r = &L.ring;
    unsigned head = *r->cq_head, count = 0;

    for (;;) {
        const unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);

        if (head == tail)
            break;
        for (; head != tail; head++, count++) {
            const struct io_uring_cqe cqe = r->cqes[head & r->cq_mask];
            conn *c = &L.conns[(uint32_t)cqe.user_data];

            __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
            switch (cqe.user_data >> 56) {
            case OP_RECV: on_recv(c, &cqe); break;
            case OP_SEND: on_sent(c, &cqe); break;
            case OP_ACCEPT: on_accept(&cqe); break;
            case OP_CLOSE: on_closed(c); break;
            case OP_DONE:
                on_writes_done();
                arm_done();
                break;
            default: break;
            }
        }
    }
    return count;
}

/* Closes what has been idle for too long, and what has stopped half way. */
static void sweep(void)
{
    for (uint32_t i = 0; i < L.max_conns; i++) {
        conn *c = &L.conns[i];
        uint32_t idle;

        if (!c->open || c->closing || c->parked)
            continue;
        idle = L.now - c->last_active;
        if (idle >= IDLE_TIMEOUT_S || ((c->in_len != 0 || c->sending) && idle >= STALL_TIMEOUT_S))
            close_conn(c, true);
    }
    if (!L.accepting)
        arm_accept();
}

static uint32_t seconds_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint32_t)ts.tv_sec;
}

static int listen_on(const struct sockaddr_in *addr)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    /* TCP_NODELAY is inherited by the sockets accepted here. */
    if (fd == -1 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) != 0 ||
        bind(fd, (const struct sockaddr *)addr, sizeof *addr) != 0 || listen(fd, SOMAXCONN) != 0) {
        perror("listen");
        return -1;
    }
    return fd;
}

int server_run(const char *host, uint16_t port, const char *path)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    struct rlimit limit;
    uint32_t swept;
    bool loaded = false;

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "HOST must be an IPv4 address, got %s\n", host);
        return 1;
    }
    arnm_mono_timer_init();
    arnm_mono_timer_reset(&started);

    /* The table of direct descriptors may be as large as the descriptor limit and no larger;
     * the sockets in it do not count against that limit themselves. */
    L.max_conns = MAX_CONNS;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < L.max_conns)
        L.max_conns = (uint32_t)limit.rlim_cur;
    if ((L.conns = calloc(L.max_conns, sizeof *L.conns)) == NULL ||
        (L.slots = calloc(SLOTS, sizeof *L.slots)) == NULL)
        abort();
    for (int k = 0; k < SLOTS; k++) {
        L.slots[k].next = L.free_slots;
        L.free_slots = &L.slots[k];
    }
    if (arnm_init_arena(&L.writer_arena, WRITER_ARENA) != ARNM_SUCCESS ||
        arnm_init_arena(&L.text_arena, TEXT_ARENA) != ARNM_SUCCESS ||
        arnm_graded_arena_pool_init(&L.arenas, arena_sizes, sizeof arena_sizes / sizeof arena_sizes[0],
                                    ARENAS_SPARE) != ARNM_SUCCESS)
        abort();

    if ((L.listen_fd = listen_on(&addr)) == -1)
        return 1;
    if ((L.done_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
        perror("eventfd");
        return 1;
    }
    if (uring_init(&L.ring, SQ_ENTRIES, CQ_ENTRIES) != 0 || uring_register_table(&L.ring, L.max_conns) != 0 ||
        uring_register_buffers(&L.ring, RECV_BUFFERS, RECV_BUFFER_SIZE, RECV_BUFFER_SPARE) != 0) {
        perror("io_uring (Linux 6.1 or later is needed)");
        return 1;
    }

    db_writer_start(path, on_job_done);
    L.db = db_reader_open(path);

    L.now = swept = seconds_now();
    arm_accept();
    arm_done();
    fprintf(stderr, "listening on %s:%u, up to %u connections\n", host, (unsigned)port, L.max_conns);

    for (;;) {
        /* Idle, the loop sleeps until something arrives and serves it at once. Once there was
         * work, it lets completions collect: a pass then meets every request of those
         * milliseconds, and one call into the kernel carries all of their answers. */
        if (uring_enter(&L.ring, loaded ? WAIT_COMPLETIONS : 1, loaded ? WAIT_NS : IDLE_WAIT_NS) != 0) {
            perror("io_uring_enter");
            return 1;
        }
        L.now = seconds_now();
        loaded = reap() != 0;
        if (L.now - swept >= SWEEP_S) {
            swept = L.now;
            sweep();
        }
    }
}
