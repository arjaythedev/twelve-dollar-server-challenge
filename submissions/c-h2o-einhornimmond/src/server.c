/*
 * The HTTP side: h2o's evloop, one loop per thread, each with its own listening socket under
 * SO_REUSEPORT -- gradido-unicorn's thread-per-loop model. On the one-vCPU box that is one loop.
 *
 *   reads    answered on the loop thread, from that loop's own SQLite connection
 *   writes   handed to the writer thread; the request is parked and answered when the
 *            transaction holding its row has committed (see db.c, group commit)
 *
 * No request object crosses a thread. A parked request is a slot on its loop; the writer gets
 * the job inside that slot and gives it back through h2o's multithread queue.
 */
#include "server.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "h2o.h"

#include "arnm/arena.h"
#include "arnm/graded_arena_pool.h"
#include "arnm/json_reader.h"
#include "arnm/json_writer.h"
#include "arnm/mono_timer.h"

#include "db.h"
#include "jwt.h"
#include "parse_id.h"

/* Writes one loop may have waiting for their commit. Beyond it a write is answered 503. */
#define SLOTS 2048
/* How long a keep-alive connection may sit idle. h2o's defaults are 10 s and 5 s, shorter than
 * the pause of a user who is reading; Nginx in front would want at least 65 s. */
#define IDLE_TIMEOUT_MS 120000
#define MAX_BODY (16 * 1024)
/* The arena the writer builds a document in: its values, and the strings it copies. */
#define WRITER_ARENA (256u * 1024u)
/* Arenas kept in stock per size once they have been used; more than that many of one size
 * lent out at once are served by the host and given back to it. */
#define ARENAS_SPARE 64u
/* The sizes a lent arena comes in. Two things ask for one: a response text, for which the
 * serializer reserves six bytes per byte of an escaped string, so the largest feed (20 posts of
 * 2000 bytes) needs the last; and a parsed request body, see post_create. */
static const uint32_t arena_sizes[] = {1024u, 4096u, 16384u, 65536u, 262144u};

struct loop;

typedef struct slot {
    h2o_multithread_message_t message;
    db_job job;
    struct loop *loop;
    /* NULL once the client has gone; the write still commits and the slot still comes back. */
    h2o_req_t *req;
    struct tracker *tracker;
    h2o_iovec_t author;
    /* A create's parsed request body: the job's `body` points into it, so it is held until the
     * writer is done with the job and the answer is written. NULL for a like. */
    arnm *body_arena;
    struct slot *next_free;
} slot;

/* Lives in the request's pool, so h2o disposes it with the request -- which is how a parked
 * request that lost its client is noticed. */
typedef struct tracker {
    slot *slot;
} tracker;

typedef struct loop {
    h2o_context_t ctx;
    h2o_accept_ctx_t accept_ctx;
    h2o_multithread_queue_t *queue;
    h2o_multithread_receiver_t receiver;
    h2o_socket_t *listener;
    h2o_timer_t accept_timer;
    db_reader *db;
    slot *slots, *free_slots;
    /* One writer, used for every response. Its arena holds the document while it is built;
     * the release after each write gives that back. */
    arnm writer_arena;
    arnm_json_writer writer;
    /* Arenas lent for as long as one piece of work needs them. A rendered response text gets
     * one when it is written and gives it back when h2o disposes the request -- so the writer
     * is free for the next response while this one is still on its way out, and the text is
     * never copied. A parsed request body gets one for as long as its write is parked. */
    arnm_graded_arena_pool arenas;
    pthread_t thread;
    int listen_fd;
} loop;

static h2o_globalconf_t config;
static const char *db_path;
static arnm_mono_timer started;
static __thread loop *self;

/* --- replies ------------------------------------------------------------------------------ */

static const char *reason_of(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

/* @p body must stay valid until the response is on the wire: a literal, or request-pool memory. */
static void reply_borrowed(h2o_req_t *req, int status, const char *body, size_t len)
{
    static h2o_generator_t generator = {NULL, NULL};
    h2o_iovec_t buf = h2o_iovec_init(body, len);

    req->res.status = status;
    req->res.reason = reason_of(status);
    req->res.content_length = len;
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL,
                   H2O_STRLIT("application/json"));
    h2o_start_response(req, &generator);
    h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
}

#define REPLY_LIT(req, status, json) reply_borrowed(req, status, json, sizeof(json) - 1)
#define REPLY_ERROR(req, status, message) REPLY_LIT(req, status, "{\"error\":\"" message "\"}")

/* Lives in the request's pool, so h2o disposes it with the request, which is after the
 * response went out: that is when the text's arena goes back to its pool. */
typedef struct {
    struct loop *loop;
    arnm *arena;
} text_lease;

static void on_text_dispose(void *p)
{
    text_lease *lease = p;

    (void)arnm_graded_arena_pool_free(&lease->loop->arenas, lease->arena);
}

/* Starts the loop's writer on the next response, which will hold @p values values -- so the
 * document's pool is opened once, at that size. No string is copied into a document here:
 * every one of them is borrowed. */
static arnm_json_writer *json_begin(uint32_t values)
{
    const arnm_json_writer_hint hint = {.values = values, .string_bytes = 0};

    (void)arnm_json_writer_init(&self->writer, &self->writer_arena, ARNM_JSON_WRITE_DEFAULT, &hint);
    return &self->writer;
}

/* Lets go of a document, sent or not: its memory goes back to the writer's arena. */
static void json_discard(void)
{
    (void)arnm_json_writer_release(&self->writer);
}

/* Renders the writer's document into a lent arena of at least @p size bytes. On success the
 * arena is the request's until h2o disposes it; otherwise it has gone back already. */
static int render(h2o_req_t *req, uint32_t size, arnm_memory_block *text, uint32_t *len)
{
    arnm *arena = NULL;
    arnm_result result;
    text_lease *lease;

    if (arnm_graded_arena_pool_alloc(&self->arenas, size, &arena) != ARNM_SUCCESS)
        return 0;
    result = arnm_json_writer_write(&self->writer, arena, text, len);
    if (result != ARNM_SUCCESS && result != ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED) {
        (void)arnm_graded_arena_pool_free(&self->arenas, arena);
        return 0;
    }
    lease = h2o_mem_alloc_shared(&req->pool, sizeof *lease, on_text_dispose);
    lease->loop = self;
    lease->arena = arena;
    return 1;
}

/*
 * Renders the writer's document into an arena of its own and sends it from there.
 *
 * @p values is what json_begin() was told and @p text_bytes how many bytes of strings the
 * document holds. The arena is sized for what the text will come to when its strings need no
 * escaping, which is what plain ASCII is: the strings as they are, 18 bytes per value for
 * everything around them -- the serializer's own first estimate -- and room for the six bytes
 * per byte it insists on having free ahead of the string it is about to write. That last part
 * is capped, so a document of long strings or of many escapes can outgrow the arena; the
 * serializer then says so and the text is rendered again with room for the worst case, every
 * byte of every string as a \uXXXX. A document that fits neither is a 500.
 */
static void reply_json(h2o_req_t *req, int status, uint32_t values, uint32_t text_bytes)
{
    const uint32_t around = 18u * values + 64u;
    const uint32_t ahead = 6u * text_bytes < 2048u ? 6u * text_bytes : 2048u;
    arnm_memory_block text = {0};
    uint32_t len = 0;
    const int rendered = render(req, text_bytes + around + ahead, &text, &len) ||
                         render(req, 6u * text_bytes + around, &text, &len);

    json_discard();
    if (!rendered) {
        REPLY_ERROR(req, 500, "internal server error");
        return;
    }
    reply_borrowed(req, status, (const char *)text.data, len);
}

/* --- parked writes ------------------------------------------------------------------------ */

static void on_tracker_dispose(void *p)
{
    tracker *t = p;

    if (t->slot != NULL)
        t->slot->req = NULL;
}

static slot *park(h2o_req_t *req)
{
    slot *s = self->free_slots;

    if (s == NULL)
        return NULL;
    self->free_slots = s->next_free;
    s->req = req;
    s->tracker = h2o_mem_alloc_shared(&req->pool, sizeof *s->tracker, on_tracker_dispose);
    s->tracker->slot = s;
    return s;
}

/* On the writer thread: hand the slot back to the loop that owns it. */
static void on_job_done(db_job *job)
{
    slot *s = H2O_STRUCT_FROM_MEMBER(slot, job, job);

    h2o_multithread_send_message(&s->loop->receiver, &s->message);
}

static void answer_write(slot *s)
{
    h2o_req_t *req = s->req;
    const db_job *job = &s->job;
    arnm_json_writer *w;

    if (job->status == 404) {
        REPLY_ERROR(req, 404, "post not found");
        return;
    }
    if (job->status != 200 && job->status != 201) {
        REPLY_ERROR(req, 500, "internal server error");
        return;
    }
    w = json_begin(job->kind == JOB_LIKE ? 7u : DB_POST_VALUES);
    if (job->kind == JOB_LIKE) {
        arnm_json_writer_add_bool(w, ARNM_JSON_WRITER_KEY("liked"), true);
        arnm_json_writer_add_bool(w, ARNM_JSON_WRITER_KEY("already_liked"), job->status == 200);
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("post_id"), (uint64_t)job->post_id);
        reply_json(req, job->status, 7u, 0);
    } else {
        /* Everything borrowed here stays put until the write: the slot is this request's until
         * it is answered, and the author lies in the request's pool. */
        arnm_json_writer_open_object(w, ARNM_JSON_WRITER_KEY("post"));
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("id"), (uint64_t)job->post_id);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("body"), job->body, job->body_len,
                                          ARNM_JSON_WRITER_STRING_ESCAPE);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("created_at"), job->created_at,
                                          job->created_len, ARNM_JSON_WRITER_STRING_ESCAPE);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("author"), s->author.base,
                                          s->author.len, ARNM_JSON_WRITER_STRING_ESCAPE);
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("like_count"), 0);
        arnm_json_writer_close(w);
        reply_json(req, 201, DB_POST_VALUES,
                   job->body_len + job->created_len + (uint32_t)s->author.len);
    }
}

/* On the loop thread, for every job the writer finished. */
static void on_writes_done(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages)
{
    (void)receiver;
    while (!h2o_linklist_is_empty(messages)) {
        h2o_multithread_message_t *message =
            H2O_STRUCT_FROM_MEMBER(h2o_multithread_message_t, link, messages->next);
        slot *s = H2O_STRUCT_FROM_MEMBER(slot, message, message);

        h2o_linklist_unlink(&message->link);
        if (s->req != NULL) {
            /* Answered from here on, so the request's disposal no longer concerns the slot. */
            s->tracker->slot = NULL;
            answer_write(s);
        }
        if (s->body_arena != NULL) {
            (void)arnm_graded_arena_pool_free(&self->arenas, s->body_arena);
            s->body_arena = NULL;
        }
        s->req = NULL;
        s->next_free = self->free_slots;
        self->free_slots = s;
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
/*

static inline bool is_ascii_space(unsigned char c)
{
  return c == ' ' || (c >= '\t' && c <= '\r');
}

static void trim_ascii(const unsigned char **start, const unsigned char **end) {
  const unsigned char *p = *start, *e = *end;
  if (p >= e) return;

  while (p < e && is_ascii_space(p)) ++p;
  while (e > p && is_ascii_space(e - 1)) --e,

  *start = p;
  *end = e;
}
 */

static int authenticate(h2o_req_t *req, int64_t *uid, char *name, size_t *name_len)
{
    const ssize_t i = h2o_find_header(&req->headers, H2O_TOKEN_AUTHORIZATION, -1);
    const char *auth = i != -1 ? req->headers.entries[i].value.base : NULL;
    const size_t auth_len = i != -1 ? req->headers.entries[i].value.len : 0;
    
    switch (jwt_verify(auth, auth_len, uid, name, name_len)) {
    case JWT_OK: return 0;
    case JWT_MISSING: REPLY_ERROR(req, 401, "missing bearer token"); break;
    case JWT_INVALID: REPLY_ERROR(req, 401, "invalid or expired token"); break;
    default: REPLY_ERROR(req, 401, "invalid token payload"); break;
    }
    return -1;
}

static void get_feed(h2o_req_t *req)
{
    arnm_json_writer *w = json_begin(DB_FEED_VALUES);
    uint32_t text_bytes = 0;

    if (db_read_feed(self->db, w, &text_bytes) != 0) {
        json_discard();
        REPLY_ERROR(req, 500, "internal server error");
        return;
    }
    reply_json(req, 200, DB_FEED_VALUES, text_bytes);
}

static void get_post(h2o_req_t *req, int64_t id)
{
    arnm_json_writer *w = json_begin(DB_POST_VALUES);
    uint32_t text_bytes = 0;

    switch (db_read_post(self->db, id, w, &text_bytes)) {
    case 1:
        reply_json(req, 200, DB_POST_VALUES, text_bytes);
        return;
    case 0: REPLY_ERROR(req, 404, "post not found"); break;
    default: REPLY_ERROR(req, 500, "internal server error");
    }
    json_discard();
}

static void get_health(h2o_req_t *req)
{
    const char *err = "";

    if (db_ping(self->db, &err) == 0) {
        arnm_json_writer *w = json_begin(7u);
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("status"), "ok", 2);
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("db"), "ok", 2);
        arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("uptime_s"), (uint64_t)floor(arnm_mono_timer_seconds(started)));
        reply_json(req, 200, 7u, 0);
    } else {
        arnm_json_writer *w = json_begin(7u);
        const size_t err_len = strlen(err);

        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("status"), "degraded", 8);
        arnm_json_writer_add_string(w, ARNM_JSON_WRITER_KEY("db"), "unreachable", 11);
        arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("error"), err, err_len, ARNM_JSON_WRITER_STRING_ESCAPE);
        reply_json(req, 503, 7u, (uint32_t)err_len);
    }
}

static void post_like(h2o_req_t *req, int64_t uid, int64_t id)
{
    slot *s;
    if ((s = park(req)) == NULL) {
        REPLY_ERROR(req, 503, "server busy");
        return;
    }
    s->job.kind = JOB_LIKE;
    s->job.user_id = uid;
    s->job.post_id = id;
    db_writer_submit(&s->job);
}

/* Parses the request body into a lent arena of at least @p size bytes, which is the caller's
 * to give back whatever this answers. */
static arnm_result parse_body(h2o_req_t *req, uint32_t size, arnm **arena, arnm_json_value **root)
{
    arnm_json_reader reader;
    arnm_result result = arnm_graded_arena_pool_alloc(&self->arenas, size, arena);

    if (result != ARNM_SUCCESS)
        return result;
    if ((result = arnm_json_reader_init(&reader, *arena)) != ARNM_SUCCESS)
        return result;
    return arnm_json_reader_parse(&reader, req->entity.base, (uint32_t)req->entity.len, false, root);
}

/*
 * The request body is copied once. The parser copies its input before reading it, and the
 * arena it is given for that comes from the pool and stays with the write: the `body` string
 * the parser hands back lies in that copy, the writer thread reads it from there, and the
 * arena goes back when the slot does. Nothing the writer thread touches belongs to h2o.
 *
 * The arena is sized by the parser's own estimate: the copy of the input, and sixteen bytes for
 * each of the values it expects, one in six bytes. A document of denser values -- a long array
 * of single digits -- outgrows that, the parser says it ran out of memory, and the body is
 * parsed again with room for the densest there is, one value in two bytes.
 */
static void post_create(h2o_req_t *req, int64_t uid, char *name, size_t name_len)
{
    arnm *arena = NULL;
    arnm_json_value *root = NULL;
    arnm_memory_block body = {0};
    arnm_json_field fields[1] = {ARNM_JSON_FIELD_STRING("body", &body)};
    uint64_t found = 0;
    const unsigned char *p, *e;
    const uint32_t len = (uint32_t)req->entity.len;
    arnm_result parsed;
    size_t chars = 0;
    slot *s;

    if (len == 0) {
        REPLY_ERROR(req, 400, "malformed JSON body");
        return;
    }
    parsed = parse_body(req, 4u * len + 512u, &arena, &root);
    if (parsed == ARNM_ERROR_OUT_OF_MEMORY) {
        if (arena != NULL)
            (void)arnm_graded_arena_pool_free(&self->arenas, arena);
        arena = NULL;
        parsed = parse_body(req, 10u * len + 1024u, &arena, &root);
    }

    if (parsed == ARNM_ERROR_OUT_OF_MEMORY || arena == NULL) {
        REPLY_ERROR(req, 500, "internal server error");
    } else if (parsed != ARNM_SUCCESS) {
        REPLY_ERROR(req, 400, "malformed JSON body");
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
            REPLY_ERROR(req, 400, "body is required");
        } else if (chars > 500 || (size_t)(e - p) > DB_BODY_MAX) {
            REPLY_ERROR(req, 400, "body must be at most 500 characters");
        } else if ((s = park(req)) == NULL) {
            REPLY_ERROR(req, 503, "server busy");
        } else {
            s->job.kind = JOB_CREATE;
            s->job.user_id = uid;
            s->job.body = (const char *)p;
            s->job.body_len = (uint32_t)(e - p);
            s->author = h2o_strdup(&req->pool, name, name_len);
            s->body_arena = arena;
            db_writer_submit(&s->job);
            return;
        }
    }
    /* Answered without a write: nothing holds the parsed body any longer. */
    if (arena != NULL)
        (void)arnm_graded_arena_pool_free(&self->arenas, arena);
}
#define POSTS_SLASH_LENGTH 7
static int on_request(h2o_handler_t *handler, h2o_req_t *req)
{
    const char *path = req->path_normalized.base;
    const size_t len = req->path_normalized.len;
    const int get = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"));
    const int post = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST"));
    
    (void)handler;
    if (get) {
      if(len == 5 && memcmp(path, "/feed", 5) == 0) {
        get_feed(req);
        return 0;
      } else if (len >= POSTS_SLASH_LENGTH && memcmp(path, "/posts/", 7) == 0) {
        const int64_t id = parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH);
        if (id > 0) {
          get_post(req, id);
        } else {
          char* slash_pos = memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH);
          if (slash_pos) REPLY_ERROR(req, 404, "not found");
          else if (-1 == id) REPLY_ERROR(req, 404, "post not found");
          else REPLY_ERROR(req, 400, "invalid post id");
        }
        return 0;
      } else if (len == 7 && memcmp(path, "/health", 7) == 0) {
        get_health(req);
        return 0;
      }
    } else if (post && len >= 6 && memcmp(path, "/posts", 6) == 0) {      
      char name[JWT_NAME_MAX];
      size_t name_len;
      int64_t uid;      
      if (len == 6) {
        if (0 == authenticate(req, &uid, name, &name_len)) {
          post_create(req, uid, name, name_len);
        }
        return 0;
      } else if (len > POSTS_SLASH_LENGTH && *(path + 6) == '/' && memcmp(path + len - 5, "/like", 5) == 0) {
        const int64_t id = len > POSTS_SLASH_LENGTH + 5 
          ? parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH - 5)
          : 0
        ;
        if (id <= 0) {
          char* slash_pos = memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH);  
          if (slash_pos != path + len - 5) {
            REPLY_ERROR(req, 404, "not found");
            return 0;
          }
        }
        if (0 == authenticate(req, &uid, name, &name_len)) {
          if (id > 0) post_like(req, uid, id);  
          else if (0 == id) REPLY_ERROR(req, 400, "invalid post id");  
          else REPLY_ERROR(req, 404, "post not found");
        }
        return 0;
      }
    }

    REPLY_ERROR(req, 404, "not found");
    return 0;
}

/* --- accepting ---------------------------------------------------------------------------- */

static void on_accept(h2o_socket_t *listener, const char *err);

static void resume_accept(h2o_timer_t *timer)
{
    loop *l = H2O_STRUCT_FROM_MEMBER(loop, accept_timer, timer);

    h2o_socket_read_start(l->listener, on_accept);
}

static void on_accept(h2o_socket_t *listener, const char *err)
{
    loop *l = listener->data;

    if (err != NULL)
        return;
    /* Several per wake-up: while users are arriving, one readiness event covers many of them. */
    for (int i = 0; i < 64; i++) {
        h2o_socket_t *sock = h2o_evloop_socket_accept(listener);

        if (sock == NULL) {
            /* Out of descriptors (or memory): every connection the service may hold is open.
             * The listener would stay readable and spin the loop, so it is left alone for a
             * moment; new users wait in the backlog until others have gone. */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                h2o_socket_read_stop(listener);
                h2o_timer_link(l->ctx.loop, 100, &l->accept_timer);
            }
            return;
        }
        h2o_accept(&l->accept_ctx, sock);
    }
}

static int listen_on(const struct sockaddr_in *addr)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    if (fd == -1 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) != 0 ||
        bind(fd, (const struct sockaddr *)addr, sizeof *addr) != 0 || listen(fd, SOMAXCONN) != 0) {
        perror("listen");
        return -1;
    }
    return fd;
}

static void *loop_main(void *arg)
{
    loop *l = arg;

    self = l;
    if (arnm_init_arena(&l->writer_arena, WRITER_ARENA) != ARNM_SUCCESS ||
        arnm_graded_arena_pool_init(&l->arenas, arena_sizes,
                                    sizeof arena_sizes / sizeof arena_sizes[0], ARENAS_SPARE) !=
            ARNM_SUCCESS)
        abort();
    l->db = db_reader_open(db_path);
    while (h2o_evloop_run(l->ctx.loop, INT32_MAX) == 0)
        ;
    perror("h2o_evloop_run");
    exit(1);
}

int server_run(const char *host, uint16_t port, const char *path, int threads)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    h2o_hostconf_t *hostconf;
    h2o_pathconf_t *pathconf;
    h2o_handler_t *handler;
    loop *loops;

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "HOST must be an IPv4 address, got %s\n", host);
        return 1;
    }
    if (threads <= 0)
        threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1)
        threads = 1;
    db_path = path;
    arnm_mono_timer_init();
    arnm_mono_timer_reset(&started);

    h2o_config_init(&config);
    config.server_name = h2o_iovec_init(NULL, 0);
    config.max_request_entity_size = MAX_BODY;
    /* Both timers run while a connection waits for its next request, and the shorter one closes
     * it: h2o's 5 s I/O timeout would end every keep-alive connection of a user who reads for
     * longer than that, whatever the request timeout says. */
    config.http1.req_timeout = IDLE_TIMEOUT_MS;
    config.http1.req_io_timeout = IDLE_TIMEOUT_MS;
    config.http1.upgrade_to_http2 = 0;
    hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    /* One handler for every path: routing is five comparisons, and a path nothing matches has
     * to be answered with this API's 404 body rather than h2o's. */
    pathconf = h2o_config_register_path(hostconf, "/", 0);
    handler = h2o_create_handler(pathconf, sizeof *handler);
    handler->on_req = on_request;

    db_writer_start(path, on_job_done);

    if ((loops = calloc((size_t)threads, sizeof *loops)) == NULL)
        abort();
    /* Every socket is bound here, before any loop serves: a taken port is one error at startup. */
    for (int i = 0; i < threads; i++) {
        loop *l = &loops[i];

        if ((l->listen_fd = listen_on(&addr)) == -1)
            return 1;
        h2o_context_init(&l->ctx, h2o_evloop_create(), &config);
        l->accept_ctx.ctx = &l->ctx;
        l->accept_ctx.hosts = config.hosts;
        l->queue = h2o_multithread_create_queue(l->ctx.loop);
        h2o_multithread_register_receiver(l->queue, &l->receiver, on_writes_done);
        h2o_timer_init(&l->accept_timer, resume_accept);

        if ((l->slots = calloc(SLOTS, sizeof *l->slots)) == NULL)
            abort();
        for (int k = 0; k < SLOTS; k++) {
            l->slots[k].loop = l;
            l->slots[k].next_free = l->free_slots;
            l->free_slots = &l->slots[k];
        }

        l->listener = h2o_evloop_socket_create(l->ctx.loop, l->listen_fd, H2O_SOCKET_FLAG_DONT_READ);
        l->listener->data = l;
        h2o_socket_read_start(l->listener, on_accept);
    }
    fprintf(stderr, "listening on %s:%u, %d loop%s\n", host, (unsigned)port, threads,
            threads == 1 ? "" : "s");

    for (int i = 1; i < threads; i++) {
        if (pthread_create(&loops[i].thread, NULL, loop_main, &loops[i]) != 0) {
            fprintf(stderr, "cannot start loop %d\n", i);
            return 1;
        }
    }
    loop_main(&loops[0]);
    return 1;
}
