#include "abi.h"
#include "sqlite3.h"
#include "tree.h"
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#ifndef ASC_STORE_MODE
#define ASC_STORE_MODE 3
#endif
#ifndef ASC_SQLITE_CACHE_KIB
#define ASC_SQLITE_CACHE_KIB 6144
#endif
#ifndef ASC_SQLITE_BUSY_MS
#define ASC_SQLITE_BUSY_MS 250
#endif
#if ASC_STORE_MODE == 0
#define ASC_STORE_NAME "sqlite-3.54.1/prepared-relational"
#elif ASC_STORE_MODE == 1
#define ASC_STORE_NAME "sqlite-3.54.1/cursor-scalar"
#elif ASC_STORE_MODE == 2
#define ASC_STORE_NAME "sqlite-3.54.1/cursor-vector-records"
#elif ASC_STORE_MODE == 3
#define ASC_STORE_NAME "sqlite-3.54.1/cursor-logarithmic-runs"
#else
#define ASC_STORE_NAME "sqlite-3.54.1/cursor-vector-logarithmic"
#endif

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *health,*feed,*post,*create,*exists,*like;
    sqlite3_stmt *begin,*commit,*rollback;
    AscTree *tree;
} Store;
#define SELECT_POST "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id "
static void close_store(void *opaque) {
    Store *s=opaque;
    if (!s) return;
    asc_tree_close(s->tree);
    sqlite3_finalize(s->health); sqlite3_finalize(s->feed); sqlite3_finalize(s->post);
    sqlite3_finalize(s->create); sqlite3_finalize(s->exists); sqlite3_finalize(s->like);
    sqlite3_finalize(s->begin); sqlite3_finalize(s->commit); sqlite3_finalize(s->rollback);
    if (s->db) sqlite3_close(s->db);
    sqlite3_shutdown();
    free(s);
}
static int prepare(Store *s, sqlite3_stmt **out, const char *sql) {
    return sqlite3_prepare_v3(s->db,sql,-1,SQLITE_PREPARE_PERSISTENT,out,NULL)==SQLITE_OK;
}
static int setting(const char *name,int fallback,int minimum,int maximum,int *out) {
    const char *text=getenv(name);
    if(!text) { *out=fallback; return fallback>=minimum && fallback<=maximum; }
    if(!*text || *text<'0' || *text>'9')return 0;
    char *end;errno=0;
    long value=strtol(text,&end,10);
    if(errno || *end || value<minimum || value>maximum)return 0;
    *out=(int)value;return 1;
}
static void *open_store(const char *path) {
    int busy_ms,cache_kib,wal_pages;
    if(!setting("ASC_SQLITE_BUSY_MS",ASC_SQLITE_BUSY_MS,0,INT_MAX,&busy_ms) ||
       !setting("ASC_SQLITE_CACHE_KIB",ASC_SQLITE_CACHE_KIB,64,1048576,&cache_kib) ||
       !setting("ASC_SQLITE_WAL_PAGES",1000,1,1000000,&wal_pages)) {
        fprintf(stderr,"invalid ASC_SQLITE_BUSY_MS, ASC_SQLITE_CACHE_KIB, or ASC_SQLITE_WAL_PAGES setting\n");
        return NULL;
    }
    Store *s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    if (sqlite3_open_v2(path,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX,NULL)!=SQLITE_OK) goto fail;
    if(sqlite3_busy_timeout(s->db,busy_ms)!=SQLITE_OK)goto fail;
    char pragmas[256];
    int written=snprintf(pragmas,sizeof(pragmas),
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;"
        "PRAGMA cache_size=-%d;PRAGMA mmap_size=0;PRAGMA temp_store=MEMORY;"
        "PRAGMA wal_autocheckpoint=%d;",cache_kib,wal_pages);
    if(written<0 || (size_t)written>=sizeof(pragmas) ||
       sqlite3_exec(s->db,pragmas,NULL,NULL,NULL)!=SQLITE_OK)goto fail;
    if (!prepare(s,&s->health,"SELECT 1") ||
        !prepare(s,&s->feed,SELECT_POST "ORDER BY p.created_at DESC,p.id DESC LIMIT 20") ||
        !prepare(s,&s->post,SELECT_POST "WHERE p.id=?1") ||
        !prepare(s,&s->create,"INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at") ||
        !prepare(s,&s->exists,"SELECT 1 FROM posts WHERE id=?1") ||
        !prepare(s,&s->like,"INSERT INTO likes(user_id,post_id) VALUES(?1,?2) ON CONFLICT(user_id,post_id) DO NOTHING") ||
        !prepare(s,&s->begin,"BEGIN IMMEDIATE") || !prepare(s,&s->commit,"COMMIT") ||
        !prepare(s,&s->rollback,"ROLLBACK")) goto fail;
    if (ASC_STORE_MODE) s->tree=asc_tree_open(s->db,(unsigned)(ASC_STORE_MODE-1));
    fprintf(stderr,"SQLite direct reads: %s; mode=%d\n",s->tree?"enabled":"SQL fallback",ASC_STORE_MODE);
    fprintf(stderr,"SQLite cache=%d KiB; busy=%d ms; WAL checkpoint=%d pages\n",cache_kib,busy_ms,wal_pages);
    return s;
fail:
    fprintf(stderr,"SQLite startup: %s\n",s->db?sqlite3_errmsg(s->db):"allocation failed");
    close_store(s); return NULL;
}
static int health(void *opaque) {
    Store *s=opaque; int ok=sqlite3_step(s->health)==SQLITE_ROW;
    sqlite3_reset(s->health); return ok;
}
static int rows(sqlite3_stmt *q, PostVisitor emit, void *state, int missing) {
    int rc, count=0, result=STORE_OK;
    while ((rc=sqlite3_step(q))==SQLITE_ROW) {
        Post p={.id=sqlite3_column_int64(q,0),.likes=sqlite3_column_int64(q,4)};
        p.body=(Slice){(const char *)sqlite3_column_text(q,1),(size_t)sqlite3_column_bytes(q,1)};
        p.created=(Slice){(const char *)sqlite3_column_text(q,2),(size_t)sqlite3_column_bytes(q,2)};
        p.author=(Slice){(const char *)sqlite3_column_text(q,3),(size_t)sqlite3_column_bytes(q,3)};
        if (!p.body.p || !p.created.p || !p.author.p || !emit(state,&p)) { result=STORE_ERROR; break; }
        ++count;
    }
    if (rc!=SQLITE_DONE && result==STORE_OK) result=STORE_ERROR;
    if (result==STORE_OK && !count && missing) result=STORE_MISSING;
    sqlite3_reset(q); sqlite3_clear_bindings(q); return result;
}
static int feed(void *opaque, PostVisitor emit, void *state) {
    Store *s=opaque;
    int rc=asc_tree_read(s->tree,0,1,emit,state);
    return rc==TREE_FALLBACK?rows(s->feed,emit,state,0):rc;
}
static int post(void *opaque, int64_t id, PostVisitor emit, void *state) {
    Store *s=opaque;
    int rc=asc_tree_read(s->tree,id,0,emit,state);
    if (rc!=TREE_FALLBACK) return rc;
    if (sqlite3_bind_int64(s->post,1,id)!=SQLITE_OK) return STORE_ERROR;
    return rows(s->post,emit,state,1);
}
static int transaction(sqlite3_stmt *statement) {
    int rc=sqlite3_step(statement); sqlite3_reset(statement); return rc==SQLITE_DONE;
}
static int create(void *opaque, int64_t user, Slice body, Created *out) {
    Store *s=opaque; int ok=0;
    if (!transaction(s->begin)) return STORE_ERROR;
    if (sqlite3_bind_int64(s->create,1,user)!=SQLITE_OK ||
        sqlite3_bind_text(s->create,2,body.p,(int)body.n,SQLITE_STATIC)!=SQLITE_OK) goto done;
    if (sqlite3_step(s->create)!=SQLITE_ROW) goto done;
    out->id=sqlite3_column_int64(s->create,0);
    int n=sqlite3_column_bytes(s->create,1);
    const char *date=(const char *)sqlite3_column_text(s->create,1);
    if (!date || n<=0 || (size_t)n>=sizeof(out->created)) goto done;
    memcpy(out->created,date,(size_t)n); out->created[n]=0; out->created_n=(size_t)n;
    ok=sqlite3_step(s->create)==SQLITE_DONE;
done:
    sqlite3_reset(s->create); sqlite3_clear_bindings(s->create);
    if (ok && transaction(s->commit)) return STORE_OK;
    transaction(s->rollback); return STORE_ERROR;
}
static int like(void *opaque, int64_t user, int64_t id, int *already) {
    Store *s=opaque; int result=STORE_ERROR;
    if (!transaction(s->begin)) return result;
    if (sqlite3_bind_int64(s->exists,1,id)!=SQLITE_OK) goto done;
    int rc=sqlite3_step(s->exists);
    sqlite3_reset(s->exists); sqlite3_clear_bindings(s->exists);
    if (rc==SQLITE_DONE) { result=STORE_MISSING; goto done; }
    if (rc!=SQLITE_ROW) goto done;
    if (sqlite3_bind_int64(s->like,1,user)!=SQLITE_OK || sqlite3_bind_int64(s->like,2,id)!=SQLITE_OK) goto done;
    if (sqlite3_step(s->like)==SQLITE_DONE) { *already=sqlite3_changes(s->db)==0; result=STORE_OK; }
done:
    sqlite3_reset(s->exists); sqlite3_clear_bindings(s->exists);
    sqlite3_reset(s->like); sqlite3_clear_bindings(s->like);
    if (result==STORE_OK && transaction(s->commit)) return STORE_OK;
    transaction(s->rollback); return result==STORE_MISSING?STORE_MISSING:STORE_ERROR;
}
const void *ASC_MODULE_ENTRY(void) {
    static const StoreAPI api={{ASC_ABI,sizeof(StoreAPI),ASC_STORE_NAME},open_store,close_store,health,feed,post,create,like};
    return &api;
}
