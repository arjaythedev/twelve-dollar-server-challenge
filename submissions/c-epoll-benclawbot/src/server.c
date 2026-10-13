#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#ifdef USE_IO_URING
#include "ring.h"
#endif
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

// EXPERIMENTAL 64k-oriented epoll baseline. SQLite is the only database.
// Idle connections have zero allocated input/output buffers.
#define MAX_BODY 16384
#define MAX_REQ 32768
#define EVENTS 512

typedef struct {
    int fd;
    char *input,*output;
    size_t used,cap,sent,total;
    bool closing;
} Conn;
typedef struct {
    sqlite3 *db;
    sqlite3_stmt *feed,*post,*create,*exists,*like,*health,*ids,*counts,*range_rows,*begin,*commit;
    const char *secret;
    size_t secret_len;
    uint64_t started_s;
    bool range_feed;
    int group_mode;
} State;
typedef struct {char method[9],path[256],bearer[3072];const char *body;size_t blen;bool closing;} Request;
typedef struct {unsigned uid;char username[512];} Claims;
#ifndef USE_IO_URING
static int g_epfd=-1;
#endif
static Conn *g_conn=NULL;static size_t g_cap=0;
#ifdef USE_IO_URING
static uint32_t *g_generation;
#endif
static State S;
static bool g_batch_active=false;
static bool g_write_tx_active=false;
static int sqlite_done(sqlite3_stmt *s);
static int ensure_write_batch(void){
    if(g_batch_active && !g_write_tx_active){
        if(sqlite_done(S.begin)!=SQLITE_DONE)return -1;
        g_write_tx_active=true;
    }
    return 0;
}
static int g_staged[EVENTS];
static int g_staged_n=0;
static uint64_t nowmono(void){struct timespec x;clock_gettime(CLOCK_MONOTONIC,&x);return (uint64_t)x.tv_sec;}
static int sqlite_done(sqlite3_stmt *s){int rc=sqlite3_step(s);sqlite3_reset(s);sqlite3_clear_bindings(s);return rc;}
static int prep(sqlite3_stmt **s,const char *q){return sqlite3_prepare_v3(S.db,q,-1,SQLITE_PREPARE_PERSISTENT,s,NULL);}
static void die(const char *x){perror(x);exit(1);}
// No JSON data are cached: this is only a reusable *implementation* of the response writer.
// Each response owns a freshly allocated output buffer until sent.
typedef struct {char *s;size_t n,cap;bool failed;} JBuf;
static JBuf jb_new(size_t n){JBuf b={.s=malloc(n),.cap=n};if(!b.s)b.failed=true;return b;}
static void jb_bytes(JBuf *b,const char *s,size_t n){
    if(b->failed)return;
    if(b->n+n+1>b->cap){size_t cap=b->cap?b->cap:256;while(cap<b->n+n+1)cap*=2;char *p=realloc(b->s,cap);if(!p){b->failed=true;return;}b->s=p;b->cap=cap;}
    memcpy(b->s+b->n,s,n);b->n+=n;b->s[b->n]=0;
}
#define JB(b,s) jb_bytes((b),(s),sizeof(s)-1)
static void jb_num(JBuf *b,long long x){char a[32];int n=snprintf(a,sizeof(a),"%lld",x);if(n>0)jb_bytes(b,a,(size_t)n);}
static void jb_string(JBuf *b,const char *s,size_t n){
    JB(b,"\"");size_t from=0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)s[i];const char *r=NULL;
        switch(c){case '"':r="\\\"";break;case '\\':r="\\\\";break;case '\b':r="\\b";break;case '\f':r="\\f";break;case '\n':r="\\n";break;case '\r':r="\\r";break;case '\t':r="\\t";break;default:break;}
        if(r||c<32){jb_bytes(b,s+from,i-from);if(r)jb_bytes(b,r,2);else{char e[7];snprintf(e,sizeof(e),"\\u%04x",(unsigned)c);jb_bytes(b,e,6);}from=i+1;}
    }
    jb_bytes(b,s+from,n-from);JB(b,"\"");
}
static void jb_colstr(JBuf *b,sqlite3_stmt *s,int i){const char *p=(const char*)sqlite3_column_text(s,i);int n=sqlite3_column_bytes(s,i);jb_string(b,p?p:"",n>0?(size_t)n:0);}
static void jb_post(JBuf *b,sqlite3_stmt *s,int withlikes,long long likes){
    JB(b,"{\"id\":");jb_num(b,sqlite3_column_int64(s,0));
    JB(b,",\"body\":");jb_colstr(b,s,1);
    JB(b,",\"created_at\":");jb_colstr(b,s,2);
    JB(b,",\"author\":");jb_colstr(b,s,3);
    JB(b,",\"like_count\":");jb_num(b,withlikes?likes:sqlite3_column_int64(s,4));JB(b,"}");
}
static char *jb_take(JBuf *b){if(b->failed){free(b->s);return NULL;}if(!b->s)return strdup("");return b->s;}
static json_object *jpost(sqlite3_stmt *s){
    json_object *j=json_object_new_object();
    json_object_object_add(j,"id",json_object_new_int64(sqlite3_column_int64(s,0)));
    json_object_object_add(j,"body",json_object_new_string((const char *)sqlite3_column_text(s,1)));
    json_object_object_add(j,"created_at",json_object_new_string((const char *)sqlite3_column_text(s,2)));
    json_object_object_add(j,"author",json_object_new_string((const char *)sqlite3_column_text(s,3)));
    json_object_object_add(j,"like_count",json_object_new_int64(sqlite3_column_int64(s,4)));
    return j;
}
static char *jstring(json_object *j){const char *t=json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN);char *s=strdup(t);json_object_put(j);return s;}
static char *error(const char *msg){json_object *j=json_object_new_object();json_object_object_add(j,"error",json_object_new_string(msg));return jstring(j);}
static char *feed(int *status){
    JBuf b=jb_new(8192);JB(&b,"{\"posts\":[");int rc,n=0;
    while((rc=sqlite3_step(S.feed))==SQLITE_ROW){if(n++)JB(&b,",");jb_post(&b,S.feed,0,0);}
    sqlite3_reset(S.feed);
    if(rc!=SQLITE_DONE){*status=500;free(b.s);return error("internal server error");}
    JB(&b,"]}");return jb_take(&b);
}
static int feed_tx_enter(void){return g_write_tx_active?SQLITE_DONE:sqlite_done(S.begin);}
static int feed_tx_leave(void){return g_write_tx_active?SQLITE_DONE:sqlite_done(S.commit);}
// Optimized common path: rowid DESC order is validated against the live
// created_at/id ranking obtained for THIS request. On divergence the correct
// reference SQL query is used. No inter-request data or response caching.
static char *feed_range(int *status){
    long long ids[20];int n=0,rc;long long lo=INT64_MAX,hi=0;
    if(feed_tx_enter()!=SQLITE_DONE){*status=500;return error("internal server error");}
    while((rc=sqlite3_step(S.ids))==SQLITE_ROW){
        long long id=sqlite3_column_int64(S.ids,0);
        if(n<20){ids[n++]=id;if(id<lo)lo=id;if(id>hi)hi=id;}
    }
    sqlite3_reset(S.ids);
    if(rc!=SQLITE_DONE){feed_tx_leave();*status=500;return error("internal server error");}
    // This optimization only applies where the live time ordering
    // coincides with id ordering and the range is compact.
    for(int i=1;i<n;i++)if(ids[i-1]<=ids[i]){
        feed_tx_leave();return feed(status);
    }
    if(n&&hi-lo>256){feed_tx_leave();return feed(status);}
    long long likes[20]={0};
    if(n){
        sqlite3_bind_int64(S.counts,1,lo);sqlite3_bind_int64(S.counts,2,hi);
        while((rc=sqlite3_step(S.counts))==SQLITE_ROW){
            long long id=sqlite3_column_int64(S.counts,0);
            for(int i=0;i<n;i++)if(ids[i]==id){likes[i]=sqlite3_column_int64(S.counts,1);break;}
        }
        sqlite3_reset(S.counts);sqlite3_clear_bindings(S.counts);
        if(rc!=SQLITE_DONE){feed_tx_leave();*status=500;return error("internal server error");}
    }
    JBuf b=jb_new(8192);JB(&b,"{\"posts\":[");int found=0;
    if(n){
        sqlite3_bind_int64(S.range_rows,1,lo);sqlite3_bind_int64(S.range_rows,2,hi);
        while((rc=sqlite3_step(S.range_rows))==SQLITE_ROW){
            long long id=sqlite3_column_int64(S.range_rows,0);
            // If an eligible row appears in an unexpected position,
            // emit the authoritative query instead of an incorrect feed.
            if(found>=n || id!=ids[found])break;
            if(found)JB(&b,",");
            jb_post(&b,S.range_rows,1,likes[found]);found++;
        }
        sqlite3_reset(S.range_rows);sqlite3_clear_bindings(S.range_rows);
        if(rc!=SQLITE_DONE||found!=n||b.failed){
            free(b.s);
            if(feed_tx_leave()!=SQLITE_DONE){*status=500;return error("internal server error");}
            return feed(status);
        }
    }
    if(feed_tx_leave()!=SQLITE_DONE){free(b.s);*status=500;return error("internal server error");}
    JB(&b,"]}");return jb_take(&b);
}

static char *post(long long id,int *status){
