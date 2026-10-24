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
    sqlite3_bind_int64(S.post,1,id);int rc=sqlite3_step(S.post);
    if(rc==SQLITE_ROW){JBuf b=jb_new(2048);JB(&b,"{\"post\":");jb_post(&b,S.post,0,0);JB(&b,"}");sqlite3_reset(S.post);sqlite3_clear_bindings(S.post);return jb_take(&b);}
    sqlite3_reset(S.post);sqlite3_clear_bindings(S.post);
    *status=rc==SQLITE_DONE?404:500;return error(rc==SQLITE_DONE?"post not found":"internal server error");
}
static int fromb64(char *dst,size_t cap,const char *src,size_t slen){
    if(slen>3072||slen==0)return -1;
    size_t n=((slen+3)/4)*4;if(n+1>cap)return -1;
    char tmp[4096];memcpy(tmp,src,slen);for(size_t i=0;i<slen;i++){if(tmp[i]=='-')tmp[i]='+';else if(tmp[i]=='_')tmp[i]='/';else if(!(isalnum((unsigned char)tmp[i])||tmp[i]=='+'||tmp[i]=='/'))return -1;}
    for(size_t i=slen;i<n;i++)tmp[i]='=';
    int k=EVP_DecodeBlock((unsigned char*)dst,(unsigned char*)tmp,(int)n);
    if(k<0)return -1;k-=(int)(n-slen);if(k<0||(size_t)k>=cap)return -1;dst[k]=0;return k;
}
static json_object *parsejson(const char *text,size_t len){
    struct json_tokener *tok=json_tokener_new();json_tokener_set_flags(tok,JSON_TOKENER_VALIDATE_UTF8);
    json_object *j=json_tokener_parse_ex(tok,text,(int)len);
    if(json_tokener_get_error(tok)!=json_tokener_success || json_tokener_get_parse_end(tok)!=len){if(j)json_object_put(j);j=NULL;}
    json_tokener_free(tok);return j;
}
// 0 valid; 1 malformed/expired/incorrect signature; 2 invalid semantic claims.
static int auth(const char *token,Claims *claims){
    const char *p=strchr(token,'.');if(!p)return 1;
    const char *q=strchr(p+1,'.');if(!q||strchr(q+1,'.'))return 1;
    size_t a=p-token,b=q-p-1,c=strlen(q+1);if(!a||!b||!c||a>512||b>2048||c>256)return 1;
    char h[1024],payload[4096],sig[128];
    int hl=fromb64(h,sizeof(h),token,a),pl=fromb64(payload,sizeof(payload),p+1,b),sl=fromb64(sig,sizeof(sig),q+1,c);
    if(hl<0||pl<0||sl!=32)return 1;
    json_object *jh=parsejson(h,(size_t)hl);
    json_object *alg=NULL;
    int ok=jh && json_object_get_type(jh)==json_type_object && json_object_object_get_ex(jh,"alg",&alg)&&json_object_get_type(alg)==json_type_string&&!strcmp(json_object_get_string(alg),"HS256");
    if(jh)json_object_put(jh);if(!ok)return 1;
    unsigned char expected[EVP_MAX_MD_SIZE];unsigned n=0;
    if(!HMAC(EVP_sha256(),S.secret,(int)S.secret_len,(const unsigned char*)token,(int)(q-token),expected,&n)||n!=32||CRYPTO_memcmp(sig,expected,32))return 1;
    json_object *j=parsejson(payload,(size_t)pl);if(!j||json_object_get_type(j)!=json_type_object){if(j)json_object_put(j);return 1;}
    json_object *exp=NULL,*sub=NULL,*name=NULL;
    if(!json_object_object_get_ex(j,"exp",&exp)||!json_object_is_type(exp,json_type_int)||json_object_get_int64(exp)<=time(NULL)){json_object_put(j);return 1;}
    if(!json_object_object_get_ex(j,"sub",&sub)||!json_object_is_type(sub,json_type_string)||!json_object_object_get_ex(j,"username",&name)||!json_object_is_type(name,json_type_string)){json_object_put(j);return 2;}
    const char *s=json_object_get_string(sub),*u=json_object_get_string(name);if(!s||!*s||strlen(u)>=sizeof(claims->username)){json_object_put(j);return 2;}
    uint64_t val=0;for(;*s;s++){if(*s<'0'||*s>'9'||val>UINT32_MAX/10){json_object_put(j);return 2;}val=val*10+(*s-'0');}if(!val||val>UINT32_MAX){json_object_put(j);return 2;}
    claims->uid=(unsigned)val;strcpy(claims->username,u);json_object_put(j);return 0;
}
static int utf8chars(const char *s,size_t n){int k=0;for(size_t i=0;i<n;i++)if(((unsigned char)s[i]&0xc0)!=0x80)k++;return k;}
static char *createpost(Request *r,Claims *claims,int *status){
    json_object *j=parsejson(r->body,r->blen);
    if(!j){*status=400;return error("malformed JSON body");}
    json_object *b=NULL;
    if(!json_object_is_type(j,json_type_object)||!json_object_object_get_ex(j,"body",&b)||!json_object_is_type(b,json_type_string)){
        json_object_put(j);*status=400;return error("body is required");
    }
    const char *raw=json_object_get_string(b);size_t n=(size_t)json_object_get_string_len(b),lo=0,hi=n;
    while(lo<hi&&isspace((unsigned char)raw[lo]))lo++;
    while(hi>lo&&isspace((unsigned char)raw[hi-1]))hi--;
    if(hi==lo){json_object_put(j);*status=400;return error("body is required");}
    if(utf8chars(raw+lo,hi-lo)>500){json_object_put(j);*status=400;return error("body must be at most 500 characters");}
    sqlite3_bind_int(S.create,1,(int)claims->uid);
    sqlite3_bind_text(S.create,2,raw+lo,(int)(hi-lo),SQLITE_TRANSIENT);
    if(ensure_write_batch()){json_object_put(j);*status=500;return error("internal server error");}
    int rc=sqlite3_step(S.create);
    if(rc!=SQLITE_ROW){sqlite3_reset(S.create);sqlite3_clear_bindings(S.create);json_object_put(j);*status=500;return error("internal server error");}
    long long id=sqlite3_column_int64(S.create,0);
    const char *dt=(const char*)sqlite3_column_text(S.create,1);
    char timestamp[64];snprintf(timestamp,sizeof(timestamp),"%s",dt?dt:"");
    rc=sqlite3_step(S.create);
    sqlite3_reset(S.create);sqlite3_clear_bindings(S.create);
    if(rc!=SQLITE_DONE){json_object_put(j);*status=500;return error("internal server error");}
    json_object *o=json_object_new_object(),*p=json_object_new_object();
    json_object_object_add(p,"id",json_object_new_int64(id));
    json_object_object_add(p,"body",json_object_new_string_len(raw+lo,(int)(hi-lo)));
    json_object_object_add(p,"created_at",json_object_new_string(timestamp));
    json_object_object_add(p,"author",json_object_new_string(claims->username));
    json_object_object_add(p,"like_count",json_object_new_int(0));
    json_object_object_add(o,"post",p);
    char *out=jstring(o);json_object_put(j);*status=201;return out;
}
static char *likepost(long long id,Claims *claims,int *status){
    sqlite3_bind_int64(S.exists,1,id);int ex=sqlite3_step(S.exists);sqlite3_reset(S.exists);sqlite3_clear_bindings(S.exists);
    if(ex!=SQLITE_ROW){*status=ex==SQLITE_DONE?404:500;return error(ex==SQLITE_DONE?"post not found":"internal server error");}
    sqlite3_bind_int64(S.like,1,(sqlite3_int64)claims->uid);sqlite3_bind_int64(S.like,2,id);
    if(ensure_write_batch()){*status=500;return error("internal server error");}
    int rc=sqlite_done(S.like);
    if(rc!=SQLITE_DONE){*status=500;return error("internal server error");}
    int changed=sqlite3_changes(S.db);*status=changed?201:200;
    json_object *j=json_object_new_object();json_object_object_add(j,"liked",json_object_new_boolean(true));
    json_object_object_add(j,"already_liked",json_object_new_boolean(!changed));
    json_object_object_add(j,"post_id",json_object_new_int64(id));return jstring(j);
}
static int parseid(const char *p,long long *id){
    if(!*p)return -1;uint64_t v=0;
    for(;*p;p++){if(*p<'0'||*p>'9'||v>((uint64_t)INT64_MAX-(unsigned)(*p-'0'))/10)return -1;v=v*10+(*p-'0');}
    if(!v)return -1;*id=(long long)v;return 0;
}
static char *serve(Request *r,int *status){
    *status=200;
    if(!strcmp(r->method,"GET")&&!strcmp(r->path,"/health")){
        if(sqlite_done(S.health)!=SQLITE_ROW){*status=503;return strdup("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}");}
        json_object *j=json_object_new_object();json_object_object_add(j,"status",json_object_new_string("ok"));
        json_object_object_add(j,"db",json_object_new_string("ok"));json_object_object_add(j,"uptime_s",json_object_new_int64((int64_t)(nowmono()-S.started_s)));return jstring(j);
    }
    if(!strcmp(r->method,"GET")&&!strcmp(r->path,"/feed"))return S.range_feed?feed_range(status):feed(status);
    if(!strcmp(r->method,"GET")&&!strncmp(r->path,"/posts/",7)){
        long long id;if(parseid(r->path+7,&id)){*status=400;return error("invalid post id");}return post(id,status);
    }
    if(!strcmp(r->method,"POST")&&(!strcmp(r->path,"/posts")||(!strncmp(r->path,"/posts/",7)&&strlen(r->path)>12&& !strcmp(r->path+strlen(r->path)-5,"/like")))){
        if(!r->bearer[0]){*status=401;return error("missing bearer token");}
        Claims claims;int a=auth(r->bearer,&claims);
        if(a){*status=401;return error(a==1?"invalid or expired token":"invalid token payload");}
        if(!strcmp(r->path,"/posts"))return createpost(r,&claims,status);
        char idbuf[256];size_t ilen=strlen(r->path)-7-5;if(ilen>=sizeof(idbuf)){*status=400;return error("invalid post id");}
        memcpy(idbuf,r->path+7,ilen);idbuf[ilen]=0;long long id;
        if(parseid(idbuf,&id)){*status=400;return error("invalid post id");}return likepost(id,&claims,status);
    }
    *status=404;return error("not found");
}
static void closeconn(Conn *c){int fd=c->fd;if(fd<0)return;
#ifndef USE_IO_URING
 epoll_ctl(g_epfd,EPOLL_CTL_DEL,fd,NULL);
#endif
close(fd);free(c->input);free(c->output);memset(c,0,sizeof(*c));c->fd=-1;}
static int updateev(Conn *c){
#ifdef USE_IO_URING
 (void)c;return 0;
#else
 struct epoll_event ev={.events=EPOLLIN|EPOLLRDHUP|EPOLLET|(c->output?EPOLLOUT:0),.data.fd=c->fd};return epoll_ctl(g_epfd,EPOLL_CTL_MOD,c->fd,&ev);
#endif
}
static int flush(Conn *c){
    if(!c->output)return 0;
    while(c->sent<c->total){ssize_t n=send(c->fd,c->output+c->sent,c->total-c->sent,MSG_NOSIGNAL);if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return 0;return -1;}if(!n)return -1;c->sent+=(size_t)n;}
    free(c->output);c->output=NULL;c->sent=c->total=0;
    if(c->closing)return -1;
    return updateev(c);
}
static int respond(Conn *c,int status,char *body,bool closing){
    const char *reason=status==200?"OK":status==201?"Created":status==400?"Bad Request":status==401?"Unauthorized":status==404?"Not Found":status==503?"Service Unavailable":"Internal Server Error";
    char header[256];size_t n=strlen(body);
    int h=snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",status,reason,n,closing?"close":"keep-alive");
    if(h<0||h>=(int)sizeof(header)){free(body);return -1;}
    c->output=malloc((size_t)h+n);if(!c->output){free(body);return -1;}
    memcpy(c->output,header,(size_t)h);memcpy(c->output+h,body,n);c->total=(size_t)h+n;c->sent=0;c->closing=closing;free(body);
    if(g_write_tx_active){if(g_staged_n>=EVENTS)return -1;g_staged[g_staged_n++]=c->fd;return 0;}
    if(flush(c))return -1;
    if(c->output&&updateev(c))return -1;
    return 0;
}
static int parse(Conn *c){
    while(!c->output){
        if(c->used==0){free(c->input);c->input=NULL;c->cap=0;return 0;}
        char *end=memmem(c->input,c->used,"\r\n\r\n",4);
        if(!end){if(c->used>=MAX_REQ)return -1;return 0;}
        size_t hsize=(size_t)(end-c->input)+4;
        char *lineend=memmem(c->input,hsize,"\r\n",2);if(!lineend)return -1;
        Request req={0};size_t first_len=(size_t)(lineend-c->input);if(first_len>512)return -1;
        char first[520]={0};memcpy(first,c->input,first_len);
        if(sscanf(first,"%8s %255s",req.method,req.path)!=2)return -1;
        size_t clen=0;
        for(char *p=lineend+2;p<end;){char *stop=memmem(p,(size_t)(end-p),"\r\n",2);if(!stop)stop=end;
            size_t ln=(size_t)(stop-p);char *colon=memchr(p,':',ln);
            if(colon){size_t k=(size_t)(colon-p);char *v=colon+1;while(v<stop&&(*v==' '||*v=='\t'))v++;
                size_t nv=(size_t)(stop-v);
                if(k==14&&!strncasecmp(p,"Content-Length",k)){char num[32];if(nv>=sizeof(num))return -1;memcpy(num,v,nv);num[nv]=0;char *tail=NULL;errno=0;unsigned long l=strtoul(num,&tail,10);if(errno||!tail||*tail||l>MAX_BODY)return -1;clen=(size_t)l;}
                else if(k==13&&!strncasecmp(p,"Authorization",k)){
                    if(nv>=7&&!strncmp(v,"Bearer ",7)&&nv-7<sizeof(req.bearer)){memcpy(req.bearer,v+7,nv-7);req.bearer[nv-7]=0;}
                }else if(k==10&&!strncasecmp(p,"Connection",k)&&nv==5&&!strncasecmp(v,"close",5))req.closing=true;
                else if(k==17&&!strncasecmp(p,"Transfer-Encoding",k))return -1;
            }
            p=stop+2;
        }
        size_t total=hsize+clen;if(total>MAX_REQ)return -1;
        if(c->used<total)return 0;
        req.body=c->input+hsize;req.blen=clen;
        int status;char *body=serve(&req,&status);if(!body)return -1;
        size_t left=c->used-total;
        // Do not free/reuse input until the response is ready.
        if(respond(c,status,body,req.closing))return -1;
        if(left)memmove(c->input,c->input+total,left);
        c->used=left;
        if(c->closing)return c->output?0:-1;
        if(c->output)return 0;
    }
    return 0;
}
static int readable(Conn *c){
    for(;;){
        if(c->output)return 0;
        if(c->cap-c->used<2048){size_t cap=c->cap?c->cap*2:4096;if(cap>MAX_REQ)cap=MAX_REQ;if(cap<=c->cap)return -1;char *p=realloc(c->input,cap);if(!p)return -1;c->input=p;c->cap=cap;}
        ssize_t n=recv(c->fd,c->input+c->used,c->cap-c->used,0);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)return 0;if(errno==EINTR)continue;return -1;}
        if(n==0)return -1;
        c->used+=(size_t)n;if(parse(c))return -1;
    }
}
static int nonblock(int fd){int flags=fcntl(fd,F_GETFL,0);return flags>=0?fcntl(fd,F_SETFL,flags|O_NONBLOCK):-1;}
static void dbinit(void){
    const char *path=getenv("SQLITE_PATH");if(!path||!path[0]){fprintf(stderr,"SQLITE_PATH required\n");exit(1);}
    if(sqlite3_open_v2(path,&S.db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX,NULL)!=SQLITE_OK){fprintf(stderr,"sqlite open: %s\n",sqlite3_errmsg(S.db));exit(1);}
    // All tuning is process-local, schema-preserving and independent of the load generator.
    const char *cache_env=getenv("DB_CACHE_KIB"),*mmap_env=getenv("DB_MMAP_MIB");
    unsigned cache_kib=cache_env?(unsigned)strtoul(cache_env,NULL,10):8192;
    unsigned mmap_mib=mmap_env?(unsigned)strtoul(mmap_env,NULL,10):0;
    if(cache_kib<128 || cache_kib>131072 || mmap_mib>2048){fprintf(stderr,"bad cache/mmap settings\n");exit(2);}
    char pragmas[360];
    snprintf(pragmas,sizeof(pragmas),"PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;PRAGMA locking_mode=EXCLUSIVE;PRAGMA cache_size=-%u;PRAGMA mmap_size=%llu;",cache_kib,(unsigned long long)mmap_mib*1024*1024);
    char *err=NULL;if(sqlite3_exec(S.db,pragmas,NULL,NULL,&err)!=SQLITE_OK){fprintf(stderr,"pragmas: %s\n",err);exit(1);}
    fprintf(stderr,"cache=%uKiB mmap=%uMiB\n",cache_kib,mmap_mib);
    const char *feed="SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) AS like_count FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20";
    const char *post="SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) AS like_count FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1";
    const char *like="INSERT INTO likes (user_id,post_id) VALUES(?1,?2) ON CONFLICT(user_id,post_id) DO NOTHING";
    if(prep(&S.feed,feed)||prep(&S.post,post)||prep(&S.create,"INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at")||prep(&S.exists,"SELECT 1 FROM posts WHERE id=?1")||prep(&S.like,like)||prep(&S.health,"SELECT 1")||prep(&S.ids,"SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20")||prep(&S.counts,"SELECT post_id,count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id")||prep(&S.range_rows,"SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC")||prep(&S.begin,"BEGIN")||prep(&S.commit,"COMMIT")){fprintf(stderr,"prepare: %s\n",sqlite3_errmsg(S.db));exit(1);}
    S.secret=getenv("JWT_SECRET");if(!S.secret)S.secret="twelve-dollar-challenge";S.secret_len=strlen(S.secret);S.started_s=nowmono();S.range_feed=!(getenv("FEED_IMPL")&&!strcmp(getenv("FEED_IMPL"),"reference"));S.group_mode=getenv("GROUP_COMMIT")?atoi(getenv("GROUP_COMMIT")):1;
    if(S.group_mode<0||S.group_mode>2){fprintf(stderr,"invalid GROUP_COMMIT\n");exit(2);}
}
int main(void){
    signal(SIGPIPE,SIG_IGN);dbinit();
    const char *host=getenv("HOST"),*port=getenv("PORT");if(!host)host="127.0.0.1";if(!port)port="3000";
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(listener<0)die("socket");int yes=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    const char *sock_env=getenv("SOCKET_BUF_KIB");int socket_buf_kib=sock_env?atoi(sock_env):0;
    if(socket_buf_kib<0||socket_buf_kib>256){fprintf(stderr,"bad socket buffer settings\n");exit(2);}
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)atoi(port))};if(inet_pton(AF_INET,host,&addr.sin_addr)!=1)die("inet_pton");
    if(bind(listener,(void*)&addr,sizeof(addr)))die("bind");if(listen(listener,4096))die("listen");
    struct rlimit limit;if(getrlimit(RLIMIT_NOFILE,&limit))die("getrlimit");g_cap=(size_t)limit.rlim_cur;
    if(g_cap>131072)g_cap=131072;
    g_conn=calloc(g_cap,sizeof(*g_conn));if(!g_conn)die("calloc");for(size_t i=0;i<g_cap;i++)g_conn[i].fd=-1;
#ifdef USE_IO_URING
 g_generation=calloc(g_cap,sizeof(*g_generation));if(!g_generation)die("generation allocation");
#endif
    #ifndef USE_IO_URING
 g_epfd=epoll_create1(EPOLL_CLOEXEC);if(g_epfd<0)die("epoll_create1");struct epoll_event e={.events=EPOLLIN,.data.fd=listener};if(epoll_ctl(g_epfd,EPOLL_CTL_ADD,listener,&e))die("epoll_ctl");
#else
 if(ring_init(2048))die("io_uring_setup");if(ring_poll_add(listener,POLLIN,UINT64_MAX))die("listener poll");
#endif
    fprintf(stderr,"experimental C/epoll server listening at %s:%s; sqlite=%s, descriptors=%zu\n",host,port,sqlite3_libversion(),g_cap);
    struct epoll_event events[EVENTS];
#ifdef USE_IO_URING
 struct TdEvent completions[EVENTS];
#endif
    for(;;){
#ifndef USE_IO_URING
 int n=epoll_wait(g_epfd,events,EVENTS,50);
#else
 int n=ring_wait(completions,EVENTS);
 for(int i=0;i<n;i++){struct TdEvent v=completions[i];events[i]=(struct epoll_event){.events=((v.flags&POLLIN)?EPOLLIN:0)|((v.flags&POLLOUT)?EPOLLOUT:0)|((v.flags&POLLERR)?EPOLLERR:0)|((v.flags&POLLHUP)?EPOLLHUP:0)|((v.flags&POLLRDHUP)?EPOLLRDHUP:0),.data.fd=(v.fd==-2?listener:v.fd)};}
#endif
if(n<0){if(errno==EINTR)continue;die("epoll_wait");}
        if(n>0 && S.group_mode){
            g_batch_active=true;g_staged_n=0;g_write_tx_active=false;
            // Mode 1 is original eager group commit; mode 2 lazily begins on the first write.
            if(S.group_mode==1 && ensure_write_batch()){
                fprintf(stderr,"batch BEGIN failed: %s\n",sqlite3_errmsg(S.db));exit(1);
            }
        }
        for(int i=0;i<n;i++){int fd=events[i].data.fd;
#ifdef USE_IO_URING
 if(fd!=listener && (fd<0||(size_t)fd>=g_cap||g_generation[fd]!=completions[i].generation))continue;
#endif
            if(fd==listener){for(;;){int client=accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);if(client<0){if(errno==EAGAIN||errno==EWOULDBLOCK)break;if(errno==EINTR)continue;break;}if((size_t)client>=g_cap){close(client);continue;}if(socket_buf_kib){int bytes=socket_buf_kib*1024;setsockopt(client,SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes));setsockopt(client,SOL_SOCKET,SO_SNDBUF,&bytes,sizeof(bytes));}Conn *c=&g_conn[client];c->fd=client;struct epoll_event ev={.events=EPOLLIN|EPOLLET|EPOLLRDHUP,.data.fd=client};
#ifndef USE_IO_URING
 if(epoll_ctl(g_epfd,EPOLL_CTL_ADD,client,&ev)){closeconn(c);continue;}
#else
 (void)ev;g_generation[client]++;if(ring_poll_add(client,POLLIN|POLLRDHUP,((uint64_t)g_generation[client]<<32)|(unsigned)client)){closeconn(c);continue;}
#endif
}}
            else if(fd>=0&&(size_t)fd<g_cap){Conn *c=&g_conn[fd];if(c->fd!=fd)continue;
                if((events[i].events&(EPOLLERR|EPOLLHUP))||((events[i].events&EPOLLRDHUP)&&!c->output)){closeconn(c);continue;}
                if((events[i].events&EPOLLOUT)&&c->output){if(flush(c)){closeconn(c);continue;}if(!c->output&&parse(c)){closeconn(c);continue;}}
                if((events[i].events&EPOLLIN)&&!c->output){if(readable(c)){closeconn(c);continue;}}
            }
        }
        if(g_batch_active){
            int rc=g_write_tx_active?sqlite_done(S.commit):SQLITE_DONE;
            if(rc!=SQLITE_DONE)sqlite3_exec(S.db,"ROLLBACK",NULL,NULL,NULL);
            g_batch_active=false;g_write_tx_active=false;
            for(int i=0;i<g_staged_n;i++){
                int fd=g_staged[i];if(fd<0||(size_t)fd>=g_cap)continue;
                Conn *c=&g_conn[fd];if(c->fd!=fd || !c->output)continue;
                if(rc!=SQLITE_DONE){
                    bool closing=c->closing;free(c->output);c->output=NULL;
                    if(respond(c,500,strdup("{\"error\":\"internal server error\"}"),closing)){closeconn(c);continue;}
                }else if(flush(c)){closeconn(c);continue;}
                if(c->fd==fd && !c->output){
                    if(parse(c)){closeconn(c);continue;}
                    if(c->fd==fd && !c->output && readable(c)){closeconn(c);continue;}
                }
            }
        }
#ifdef USE_IO_URING
        // One-shot polls: re-arm only after requests/commit/flush have finished.
        for(int i=0;i<n;i++){
            int fd=events[i].data.fd;
            if(fd==listener){if(ring_poll_add(listener,POLLIN,UINT64_MAX))die("listener rearm");continue;}
            if(fd<0||(size_t)fd>=g_cap)continue;
            if(g_generation[fd]!=completions[i].generation)continue;
            Conn *c=&g_conn[fd];if(c->fd!=fd)continue;
            unsigned mask=POLLIN|POLLRDHUP|(c->output?POLLOUT:0);
            if(ring_poll_add(fd,mask,((uint64_t)g_generation[fd]<<32)|(unsigned)fd))die("client rearm");
        }
#endif
    }
}
