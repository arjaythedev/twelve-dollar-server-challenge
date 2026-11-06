#include "abi.h"

#define TEXT(s) {s,sizeof(s)-1}
#define ERROR(s) TEXT("{\"error\":\"" s "\"}")
#if defined(__GNUC__)
__attribute__((cold,noinline))
#endif
static void error(Response *r, int status, Slice json) {
    buffer_clear(&r->body); r->status=status;
    buffer_add(&r->body,json.p,json.n);
}
typedef struct { Context *ctx; Buffer *out; unsigned count; } Emit;
static int emit_post(void *state, const Post *post) {
    Emit *e=state;
    buffer_add(e->out,",",e->count!=0); ++e->count;
    return e->ctx->codec->post(e->out,post,e->ctx->math);
}
static void not_found(Context *c, const Request *q, Slice id, Response *r) {
    (void)c; (void)q; (void)id; error(r,404,(Slice)ERROR("not found"));
}
static void health(Context *c, const Request *q, Slice id, Response *r) {
    (void)q; (void)id;
    if (!c->store->health(c->database)) {
        r->status=503;
        buffer_text(&r->body,"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"SQLite SELECT 1 failed\"}");
        return;
    }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    int64_t seconds=now.tv_sec-c->started.tv_sec-(now.tv_nsec<c->started.tv_nsec);
    buffer_text(&r->body,"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    c->codec->integer(&r->body,seconds>0?seconds:0); buffer_add(&r->body,"}",1);
}
static void feed(Context *c, const Request *q, Slice id, Response *r) {
    (void)q; (void)id;
    Emit e={c,&r->body,0}; buffer_text(&r->body,"{\"posts\":[");
    int rc=c->store->feed(c->database,emit_post,&e);
    buffer_text(&r->body,"]}");
    if (rc!=STORE_OK) error(r,500,(Slice)ERROR("internal server error"));
}
static int positive_id(Context *c, Slice id, int64_t *value, Response *r) {
    if (!c->math->decimal(id,value) || *value<=0) { error(r,400,(Slice)ERROR("invalid post id")); return 0; }
    return 1;
}
static int authenticate(Context *c, const Request *q, User *user, Response *r) {
    static const Slice messages[]={ERROR(""),ERROR("missing bearer token"),ERROR("invalid or expired token"),ERROR("invalid token payload")};
    unsigned rc=c->auth->verify(c->authentication,q->authorization,c->codec,c->math,user);
    if (rc) { error(r,401,messages[rc<4?rc:AUTH_INVALID]); return 0; }
    return 1;
}
static void post(Context *c, const Request *q, Slice id, Response *r) {
    (void)q; int64_t value;
    if (!positive_id(c,id,&value,r)) return;
    Emit e={c,&r->body,0}; buffer_text(&r->body,"{\"post\":");
    int rc=c->store->post(c->database,value,emit_post,&e); buffer_add(&r->body,"}",1);
    if (rc==STORE_MISSING) error(r,404,(Slice)ERROR("post not found"));
    else if (rc!=STORE_OK) error(r,500,(Slice)ERROR("internal server error"));
}
static void create(Context *c, const Request *q, Slice id, Response *r) {
    (void)id; User user;
    if (!authenticate(c,q,&user,r)) return;
    Body body;
    unsigned rc=(unsigned)c->codec->body(q->body,&body);
    static const Slice messages[]={ERROR(""),ERROR("malformed JSON body"),ERROR("body is required"),ERROR("body must be at most 500 characters")};
    if (rc) { error(r,400,messages[rc<4?rc:BODY_MALFORMED]); return; }
    Created created;
    if (c->store->create(c->database,user.id,(Slice){body.text,body.n},&created)!=STORE_OK) {
        error(r,500,(Slice)ERROR("internal server error")); return;
    }

    Post p={.id=created.id,.likes=0,.body={body.text,body.n},
            .created={created.created,created.created_n},.author={user.username,user.username_n}};
    r->status=201; buffer_text(&r->body,"{\"post\":");
    c->codec->post(&r->body,&p,c->math); buffer_add(&r->body,"}",1);
}
static void like(Context *c, const Request *q, Slice id, Response *r) {
    User user; int64_t value;
    if (!authenticate(c,q,&user,r) || !positive_id(c,id,&value,r)) return;
    int already=0, rc=c->store->like(c->database,user.id,value,&already);
    if (rc==STORE_MISSING) { error(r,404,(Slice)ERROR("post not found")); return; }
    if (rc!=STORE_OK) { error(r,500,(Slice)ERROR("internal server error")); return; }
    static const Slice prefixes[]={
        TEXT("{\"liked\":true,\"already_liked\":false,\"post_id\":"),
        TEXT("{\"liked\":true,\"already_liked\":true,\"post_id\":")
    };
    r->status=201-already;
    buffer_add(&r->body,prefixes[already].p,prefixes[already].n);
    c->codec->integer(&r->body,value); buffer_add(&r->body,"}",1);
}
static void handle(Context *c, const Request *q, Response *r) {
    typedef void (*Handler)(Context *, const Request *, Slice, Response *);
    static const Handler handlers[]={not_found,health,feed,post,create,like};
    Slice id={0}; unsigned route=c->math->route(q->method,q->path,&id);
    r->status=200;
    handlers[route<6?route:0](c,q,id,r);
    if (r->body.failed) error(r,500,(Slice)ERROR("internal server error"));
}
#undef ERROR
#undef TEXT
const void *ASC_MODULE_ENTRY(void) {
    static const AppAPI api={{ASC_ABI,sizeof(AppAPI),"one-hot-api"},handle}; return &api;
}
