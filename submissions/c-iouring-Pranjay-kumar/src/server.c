#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <liburing.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_REQUEST_BUF 2048
#define MAX_RESPONSE_BUF 32768
#define MAX_BODY 16384
#define ACCEPT_DEPTH 128
#define INPUT_BUFFERS 2048
#define INPUT_BUFFER_SIZE MAX_REQUEST_BUF
#define MAX_CLIENTS 65536
#define SQ_SIZE 4096
#define CQ_SIZE 65536

enum { OP_ACCEPT=1, OP_POLL=2, OP_RECV=3, OP_SEND=4 };
#define UD(op,gen,fd) (((uint64_t)(op)<<56)|((uint64_t)(gen)<<32)|(uint32_t)(fd))
#define UD_OP(x) ((unsigned)((x)>>56))
#define UD_GEN(x) ((uint32_t)(((x)>>32)&0x00ffffffu))
#define UD_FD(x) ((int)(uint32_t)(x))
#define NO_BUFFER UINT16_MAX
enum { C_FREE=0, C_POLL=1, C_RECV=2, C_SEND=3, C_WAITBUF=4 };

typedef struct {
  int fd;
  uint32_t gen;
  uint16_t buf_id;
  uint8_t state;
  size_t used, sent, out_len;
  char *output;
  bool close_after;
  int next_wait;
} Client;

typedef struct {
  struct io_uring ring;
  char *inputs;
  uint16_t free_buffers[INPUT_BUFFERS];
  unsigned free_count;
  int wait_head, wait_tail;
  unsigned waiting;
} Io;

typedef struct {
  sqlite3 *db;
  sqlite3_stmt *health, *feed, *post, *create, *exists, *like;
  const char *secret;
  int listener;
  uint64_t started;
} App;

static App app;
static Io io;
static Client *clients;
static size_t client_cap;

static uint64_t monotonic_seconds(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec;
}
static void fail(const char *what) { perror(what); exit(1); }
static int prepare(sqlite3_stmt **s, const char *sql) {
  return sqlite3_prepare_v3(app.db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, NULL);
}
static int step_reset(sqlite3_stmt *s) {
  int rc=sqlite3_step(s); sqlite3_reset(s); sqlite3_clear_bindings(s); return rc;
}
static json_object *obj(void) { return json_object_new_object(); }
static void add_s(json_object *o,const char *key,const char *s) {
  json_object_object_add(o,key,json_object_new_string(s?s:""));
}
static char *error_body(const char *msg) {
  json_object *o=obj(); add_s(o,"error",msg);
  char *p=strdup(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
  json_object_put(o); return p;
}
static json_object *parse_json(const char *p,size_t n) {
  if(n>INT32_MAX)return NULL;
  struct json_tokener *t=json_tokener_new();
  json_tokener_set_flags(t,JSON_TOKENER_VALIDATE_UTF8);
  json_object *o=json_tokener_parse_ex(t,p,(int)n);
  bool good=json_tokener_get_error(t)==json_tokener_success && json_tokener_get_parse_end(t)==n;
  json_tokener_free(t); if(!good&&o){json_object_put(o);o=NULL;} return o;
}
static int decode64(char *out,size_t cap,const char *in,size_t n) {
  if(!n||n>4096)return -1;
  size_t padded=(n+3)&~(size_t)3;
  if(padded+1>cap)return -1;
  char tmp[4097];
  for(size_t i=0;i<n;i++){
    char c=in[i];
    if(c=='-')c='+';else if(c=='_')c='/';
    else if(!(isalnum((unsigned char)c)||c=='+'||c=='/'))return -1;
    tmp[i]=c;
  }
  for(size_t i=n;i<padded;i++)tmp[i]='=';
  int got=EVP_DecodeBlock((unsigned char*)out,(unsigned char*)tmp,(int)padded);
  if(got<0)return -1;
  got-=(int)(padded-n); if(got<0||(size_t)got>=cap)return -1;
  out[got]=0; return got;
}
/* 0 means valid, 1 means invalid/expired, 2 means signed but malformed claims. */
static int verify_token(const char *token,uint64_t *uid,char *username,size_t ucap) {
  const char *dot=strchr(token,'.'); if(!dot)return 1;
  const char *dot2=strchr(dot+1,'.'); if(!dot2||strchr(dot2+1,'.'))return 1;
  size_t hn=(size_t)(dot-token),pn=(size_t)(dot2-dot-1),sn=strlen(dot2+1);
  if(!hn||!pn||hn>512||pn>3072||sn>128)return 1;
  char head[1024],payload[4096],sig[256];
  int hlen=decode64(head,sizeof head,token,hn),plen=decode64(payload,sizeof payload,dot+1,pn);
  int siglen=decode64(sig,sizeof sig,dot2+1,sn);
  if(hlen<0||plen<0||siglen!=32)return 1;
  json_object *h=parse_json(head,(size_t)hlen),*alg=NULL;
  bool hs=h&&json_object_is_type(h,json_type_object)&&json_object_object_get_ex(h,"alg",&alg)&&
    json_object_is_type(alg,json_type_string)&&!strcmp(json_object_get_string(alg),"HS256");
  if(h)json_object_put(h); if(!hs)return 1;
  unsigned char expected[EVP_MAX_MD_SIZE]; unsigned olen=0;
  if(!HMAC(EVP_sha256(),app.secret,(int)strlen(app.secret),(const unsigned char*)token,(int)(dot2-token),expected,&olen)||
     olen!=32||CRYPTO_memcmp(sig,expected,32))return 1;
  json_object *p=parse_json(payload,(size_t)plen),*sub=NULL,*name=NULL,*exp=NULL;
  if(!p||!json_object_is_type(p,json_type_object)){if(p)json_object_put(p);return 1;}
  bool has_exp=json_object_object_get_ex(p,"exp",&exp)&&json_object_is_type(exp,json_type_int);
  if(!has_exp){json_object_put(p);return 2;}
  if(json_object_get_int64(exp)<=(int64_t)time(NULL)){json_object_put(p);return 1;}
  bool claims=json_object_object_get_ex(p,"sub",&sub)&&json_object_is_type(sub,json_type_string)&&
    json_object_object_get_ex(p,"username",&name)&&json_object_is_type(name,json_type_string);
  if(!claims){json_object_put(p);return 2;}
  const char *s=json_object_get_string(sub),*u=json_object_get_string(name);
  uint64_t val=0; bool valid=s&&*s;
  for(const char *q=s;valid&&*q;q++){
    if(*q<'0'||*q>'9'||val>(UINT64_MAX-(unsigned)(*q-'0'))/10)valid=false;
    else val=val*10+(unsigned)(*q-'0');
  }
  if(!valid||!val||!u||strlen(u)>=ucap){json_object_put(p);return 2;}
  *uid=val;strcpy(username,u);json_object_put(p);return 0;
}
static int utf8_count(const char *s,size_t n) {
  int count=0; for(size_t i=0;i<n;i++)if(((unsigned char)s[i]&0xc0)!=0x80)count++; return count;
}
static char *json_take(json_object *o) {
  const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
  char *copy=strdup(s);json_object_put(o);return copy;
}
typedef struct { char *s; size_t n,cap; bool failed; } JsonOut;
static bool jout_reserve(JsonOut *b,size_t add) {
  if(b->failed||add>SIZE_MAX-b->n-1){b->failed=true;return false;}
  size_t need=b->n+add+1;if(need<=b->cap)return true;
  size_t cap=b->cap?b->cap:4096;while(cap<need){if(cap>SIZE_MAX/2){cap=need;break;}cap*=2;}
  char *p=realloc(b->s,cap);if(!p){b->failed=true;return false;}b->s=p;b->cap=cap;return true;
}
static void jout_bytes(JsonOut *b,const char *s,size_t n) {
  if(jout_reserve(b,n)){memcpy(b->s+b->n,s,n);b->n+=n;b->s[b->n]=0;}
}
static void jout_lit(JsonOut *b,const char *s) { jout_bytes(b,s,strlen(s)); }
static void jout_i64(JsonOut *b,int64_t v) {
  char tmp[32];int n=snprintf(tmp,sizeof tmp,"%" PRId64,v);if(n>0)jout_bytes(b,tmp,(size_t)n);
}
static void jout_string(JsonOut *b,const char *s,size_t n) {
  jout_lit(b,"\"");size_t from=0;
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)s[i];const char *esc=NULL;char u[7];size_t elen=0;
    switch(c){case '"':esc="\\\"";elen=2;break;case '\\':esc="\\\\";elen=2;break;
      case '\b':esc="\\b";elen=2;break;case '\f':esc="\\f";elen=2;break;case '\n':esc="\\n";elen=2;break;
      case '\r':esc="\\r";elen=2;break;case '\t':esc="\\t";elen=2;break;default:break;}
    if(!esc&&c<0x20){snprintf(u,sizeof u,"\\u%04x",c);esc=u;elen=6;}
    if(esc){jout_bytes(b,s+from,i-from);jout_bytes(b,esc,elen);from=i+1;}
  }
  jout_bytes(b,s+from,n-from);jout_lit(b,"\"");
}
static void jout_colstr(JsonOut *b,sqlite3_stmt *s,int col) {
  const char *v=(const char*)sqlite3_column_text(s,col);int n=sqlite3_column_bytes(s,col);
  jout_string(b,v?v:"",n>0?(size_t)n:0);
}
static void jout_post(JsonOut *b,sqlite3_stmt *s) {
  jout_lit(b,"{\"id\":");jout_i64(b,sqlite3_column_int64(s,0));
  jout_lit(b,",\"body\":");jout_colstr(b,s,1);
  jout_lit(b,",\"created_at\":");jout_colstr(b,s,2);
  jout_lit(b,",\"author\":");jout_colstr(b,s,3);
  jout_lit(b,",\"like_count\":");jout_i64(b,sqlite3_column_int64(s,4));jout_lit(b,"}");
}
static char *jout_finish(JsonOut *b) {
  if(b->failed){free(b->s);return NULL;}if(!b->s)return strdup("");return b->s;
}
static char *handle(const char *method,const char *path,const char *auth,const char *body,size_t blen,int *status) {
  *status=200;
  if(!strcmp(method,"GET")&&!strcmp(path,"/health")){
    if(step_reset(app.health)!=SQLITE_ROW){*status=503;return strdup("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}");}
    json_object *o=obj();add_s(o,"status","ok");add_s(o,"db","ok");
    json_object_object_add(o,"uptime_s",json_object_new_int64((int64_t)(monotonic_seconds()-app.started)));
    return json_take(o);
  }
  if(!strcmp(method,"GET")&&!strcmp(path,"/feed")){
    JsonOut b={.cap=8192};b.s=malloc(b.cap);if(!b.s){*status=500;return error_body("internal server error");}
    b.s[0]=0;int rc;jout_lit(&b,"{\"posts\":[");int count=0;
    while((rc=sqlite3_step(app.feed))==SQLITE_ROW){if(count++)jout_lit(&b,",");jout_post(&b,app.feed);}
    sqlite3_reset(app.feed);sqlite3_clear_bindings(app.feed);
    if(rc!=SQLITE_DONE){free(b.s);*status=500;return error_body("internal server error");}
    jout_lit(&b,"]}");char *out=jout_finish(&b);if(!out){*status=500;return error_body("internal server error");}return out;
  }
  if(!strcmp(method,"GET")&&!strncmp(path,"/posts/",7)){
    const char *idstr=path+7;char *tail=NULL;errno=0;long long id=strtoll(idstr,&tail,10);
    if(errno||!idstr[0]||!tail||*tail||id<=0){*status=400;return error_body("invalid post id");}
    sqlite3_bind_int64(app.post,1,id);int rc=sqlite3_step(app.post);
    if(rc==SQLITE_ROW){JsonOut b={.cap=2048};b.s=malloc(b.cap);if(!b.s){sqlite3_reset(app.post);sqlite3_clear_bindings(app.post);*status=500;return error_body("internal server error");}b.s[0]=0;jout_lit(&b,"{\"post\":");jout_post(&b,app.post);jout_lit(&b,"}");sqlite3_reset(app.post);sqlite3_clear_bindings(app.post);char *out=jout_finish(&b);if(out)return out;*status=500;return error_body("internal server error");}
    sqlite3_reset(app.post);sqlite3_clear_bindings(app.post);
    *status=rc==SQLITE_DONE?404:500;return error_body(rc==SQLITE_DONE?"post not found":"internal server error");
  }
  bool create=!strcmp(method,"POST")&&!strcmp(path,"/posts");
  bool like=!strcmp(method,"POST")&&!strncmp(path,"/posts/",7)&&strlen(path)>12&&
    !strcmp(path+strlen(path)-5,"/like");
  if(create||like){
    if(strncmp(auth,"Bearer ",7)){*status=401;return error_body("missing bearer token");}
    uint64_t uid=0;char username[512];int a=verify_token(auth+7,&uid,username,sizeof username);
    if(a){*status=401;return error_body(a==1?"invalid or expired token":"invalid token payload");}
    if(create){
      json_object *j=parse_json(body,blen);
      if(!j){*status=400;return error_body("malformed JSON body");}
      json_object *b=NULL;
      if(!json_object_is_type(j,json_type_object)||!json_object_object_get_ex(j,"body",&b)||!json_object_is_type(b,json_type_string)){
        json_object_put(j);*status=400;return error_body("body is required");
      }
      const char *raw=json_object_get_string(b);size_t n=(size_t)json_object_get_string_len(b),lo=0,hi=n;
      while(lo<hi&&isspace((unsigned char)raw[lo]))lo++;
      while(hi>lo&&isspace((unsigned char)raw[hi-1]))hi--;
      if(lo==hi){json_object_put(j);*status=400;return error_body("body is required");}
      if(utf8_count(raw+lo,hi-lo)>500){json_object_put(j);*status=400;return error_body("body must be at most 500 characters");}
      sqlite3_bind_int64(app.create,1,(sqlite3_int64)uid);
      sqlite3_bind_text(app.create,2,raw+lo,(int)(hi-lo),SQLITE_TRANSIENT);
      int rc=sqlite3_step(app.create);
      if(rc!=SQLITE_ROW){fprintf(stderr,"create step rc=%d: %s\n",rc,sqlite3_errmsg(app.db));sqlite3_reset(app.create);sqlite3_clear_bindings(app.create);json_object_put(j);*status=500;return error_body("internal server error");}
      int64_t id=sqlite3_column_int64(app.create,0);const char *dt=(const char*)sqlite3_column_text(app.create,1);
      char created[64];snprintf(created,sizeof created,"%s",dt?dt:"");
      json_object *p=obj();json_object_object_add(p,"id",json_object_new_int64(id));
      json_object_object_add(p,"body",json_object_new_string_len(raw+lo,(int)(hi-lo)));
      add_s(p,"created_at",created);add_s(p,"author",username);json_object_object_add(p,"like_count",json_object_new_int(0));
      json_object *o=obj();json_object_object_add(o,"post",p);
      rc=sqlite3_step(app.create);sqlite3_reset(app.create);sqlite3_clear_bindings(app.create);json_object_put(j);
      if(rc!=SQLITE_DONE){fprintf(stderr,"create finish rc=%d: %s\n",rc,sqlite3_errmsg(app.db));json_object_put(o);*status=500;return error_body("internal server error");}
      *status=201;return json_take(o);
    }
    size_t n=strlen(path)-12;char idbuf[64];
    if(n>=sizeof idbuf){*status=400;return error_body("invalid post id");}
    memcpy(idbuf,path+7,n);idbuf[n]=0;char *tail=NULL;errno=0;long long id=strtoll(idbuf,&tail,10);
    if(errno||!n||!tail||*tail||id<=0){*status=400;return error_body("invalid post id");}
    sqlite3_bind_int64(app.exists,1,id);int rc=step_reset(app.exists);
    if(rc!=SQLITE_ROW){*status=rc==SQLITE_DONE?404:500;return error_body(rc==SQLITE_DONE?"post not found":"internal server error");}
    sqlite3_bind_int64(app.like,1,(sqlite3_int64)uid);sqlite3_bind_int64(app.like,2,id);
    rc=step_reset(app.like);if(rc!=SQLITE_DONE){fprintf(stderr,"like step rc=%d: %s\n",rc,sqlite3_errmsg(app.db));*status=500;return error_body("internal server error");}
    bool inserted=sqlite3_changes(app.db)!=0;*status=inserted?201:200;
    json_object *o=obj();json_object_object_add(o,"liked",json_object_new_boolean(true));
    json_object_object_add(o,"already_liked",json_object_new_boolean(!inserted));json_object_object_add(o,"post_id",json_object_new_int64(id));return json_take(o);
  }
  *status=404;return error_body("not found");
}
static bool request_complete(Client *c,size_t *header_len,size_t *body_len) {
  char *input=io.inputs+(size_t)c->buf_id*INPUT_BUFFER_SIZE;
  char *end=memmem(input,c->used,"\r\n\r\n",4);if(!end)return false;
  *header_len=(size_t)(end-input)+4;*body_len=0;
  char *line=memmem(input,*header_len,"\r\n",2);if(!line)return false;
  for(char *p=line+2;p<end;){char *stop=memmem(p,(size_t)(end-p),"\r\n",2);if(!stop)stop=end;
    if((size_t)(stop-p)>=15&&!strncasecmp(p,"Content-Length:",15)){
      char *v=p+15;while(v<stop&&(*v==' '||*v=='\t'))v++;
      char num[32];size_t n=(size_t)(stop-v);if(!n||n>=sizeof num)return false;
      memcpy(num,v,n);num[n]=0;char *tail=NULL;errno=0;unsigned long x=strtoul(num,&tail,10);
      if(errno||!tail||*tail||x>MAX_BODY)return false;*body_len=(size_t)x;
    }
    p=stop+2;
  }
  return c->used>=*header_len+*body_len;
}
static char *input_bytes(Client *c) {
  return c->buf_id==NO_BUFFER?NULL:io.inputs+(size_t)c->buf_id*INPUT_BUFFER_SIZE;
}
static struct io_uring_sqe *get_sqe(void) {
  struct io_uring_sqe *s=io_uring_get_sqe(&io.ring);if(s)return s;
  int rc=io_uring_submit(&io.ring);if(rc<0){errno=-rc;fail("io_uring_submit");}
  s=io_uring_get_sqe(&io.ring);if(!s){errno=ENOSPC;fail("io_uring SQ full");}return s;
}
static uint16_t take_buffer(void) {
  if(!io.free_count)return NO_BUFFER;return io.free_buffers[--io.free_count];
}
static void queue_recv(Client *c);
static void wake_buffer_waiters(void) {
  while(io.free_count&&io.wait_head>=0){
    int fd=io.wait_head;Client *c=&clients[fd];io.wait_head=c->next_wait;
    if(io.wait_head<0)io.wait_tail=-1;io.waiting--;c->next_wait=-1;
    if(c->state==C_WAITBUF)queue_recv(c);
  }
}
static void release_buffer(Client *c) {
  if(c->buf_id==NO_BUFFER)return;
  io.free_buffers[io.free_count++]=c->buf_id;c->buf_id=NO_BUFFER;c->used=0;
  wake_buffer_waiters();
}
static void enqueue_buffer_waiter(Client *c) {
  c->state=C_WAITBUF;c->next_wait=-1;
  if(io.wait_tail<0)io.wait_head=io.wait_tail=c->fd;
  else {clients[io.wait_tail].next_wait=c->fd;io.wait_tail=c->fd;}
  io.waiting++;
}
static void unlink_buffer_waiter(int fd,Client *c) {
  if(c->state!=C_WAITBUF)return;
  int prev=-1,cur=io.wait_head;
  while(cur>=0){
    int next=clients[cur].next_wait;
    if(cur==fd){if(prev<0)io.wait_head=next;else clients[prev].next_wait=next;
      if(io.wait_tail==cur)io.wait_tail=prev;io.waiting--;c->next_wait=-1;return;}
    prev=cur;cur=next;
  }
}
static void queue_poll(Client *c) {
  struct io_uring_sqe *s=get_sqe();io_uring_prep_poll_add(s,c->fd,POLLIN);
  io_uring_sqe_set_data64(s,UD(OP_POLL,c->gen,c->fd));c->state=C_POLL;
}
static void queue_recv(Client *c) {
  if(c->buf_id==NO_BUFFER){c->buf_id=take_buffer();if(c->buf_id==NO_BUFFER){enqueue_buffer_waiter(c);return;}}
  if(c->used>=INPUT_BUFFER_SIZE){c->close_after=true;return;}
  struct io_uring_sqe *s=get_sqe();char *buf=input_bytes(c)+c->used;
  io_uring_prep_recv(s,c->fd,buf,(unsigned)(INPUT_BUFFER_SIZE-c->used),0);
  io_uring_sqe_set_data64(s,UD(OP_RECV,c->gen,c->fd));c->state=C_RECV;
}
static void drop_client(int fd) {
  if(fd<0||(size_t)fd>=client_cap)return;Client *c=&clients[fd];if(c->state==C_FREE)return;
  uint32_t gen=(c->gen+1)&0x00ffffffu;
  unlink_buffer_waiter(fd,c);close(fd);free(c->output);c->output=NULL;release_buffer(c);
  memset(c,0,sizeof *c);c->fd=-1;c->gen=gen;c->buf_id=NO_BUFFER;c->next_wait=-1;
}
static int queue_response(Client *c,int status,char *body,bool close_after) {
  const char *reason=status==200?"OK":status==201?"Created":status==400?"Bad Request":status==401?"Unauthorized":status==404?"Not Found":status==503?"Service Unavailable":"Internal Server Error";
  size_t n=strlen(body);char header[256];int h=snprintf(header,sizeof header,
    "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
    status,reason,n,close_after?"close":"keep-alive");
  if(h<0||(size_t)h>=sizeof header||(size_t)h+n>MAX_RESPONSE_BUF){free(body);return -1;}
  char *out=realloc(body,(size_t)h+n);if(!out){free(body);return -1;}
  memmove(out+h,out,n);memcpy(out,header,(size_t)h);
  c->output=out;c->out_len=(size_t)h+n;c->sent=0;c->close_after=close_after;return 0;
}

static int parse_one(Client *c) {
  size_t hlen,blen;if(!request_complete(c,&hlen,&blen))return 0;
  char *input=input_bytes(c);
  char method[16]={0},path[512]={0},version[16]={0};
  char *line=memmem(input,hlen,"\r\n",2);size_t first=(size_t)(line-input);
  char request[544];if(first>=sizeof request)return -1;
  memcpy(request,input,first);request[first]=0;
  if(sscanf(request,"%15s %511s %15s",method,path,version)!=3)return -1;
  bool close_after=!strcmp(version,"HTTP/1.0");char auth[4096]={0};char *end=input+hlen-4;
  for(char *p=line+2;p<end;){char *stop=memmem(p,(size_t)(end-p),"\r\n",2);if(!stop)stop=end;
    if((size_t)(stop-p)>=14&&!strncasecmp(p,"Authorization:",14)){
      char *v=p+14;while(v<stop&&(*v==' '||*v=='\t'))v++;
      size_t n=(size_t)(stop-v);if(n<sizeof auth){memcpy(auth,v,n);auth[n]=0;}
    }else if((size_t)(stop-p)>=11&&!strncasecmp(p,"Connection:",11)){
      char *v=p+11;while(v<stop&&(*v==' '||*v=='\t'))v++;
      if((size_t)(stop-v)==5&&!strncasecmp(v,"close",5))close_after=true;
    }
    p=stop+2;
  }
  int status=200;char *response=handle(method,path,auth,input+hlen,blen,&status);
  if(!response||queue_response(c,status,response,close_after))return -1;
  size_t consumed=hlen+blen,left=c->used-consumed;
  if(left)memmove(input,input+consumed,left);c->used=left;
  return 1;
}
static void queue_send(Client *c) {
  struct io_uring_sqe *s=get_sqe();io_uring_prep_send(s,c->fd,c->output+c->sent,(unsigned)(c->out_len-c->sent),MSG_NOSIGNAL);
  io_uring_sqe_set_data64(s,UD(OP_SEND,c->gen,c->fd));c->state=C_SEND;
}
static void request_ready(Client *c) {
  int parsed=parse_one(c);
  if(parsed<0){drop_client(c->fd);return;}
  if(parsed==0){if(c->used==INPUT_BUFFER_SIZE){drop_client(c->fd);return;}queue_recv(c);return;}
  if(c->used==0)release_buffer(c);
  queue_send(c);
}
static void queue_accept(void) {
  struct io_uring_sqe *s=get_sqe();io_uring_prep_accept(s,app.listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
  io_uring_sqe_set_data64(s,((uint64_t)OP_ACCEPT<<56));
}
static void init_database(void) {
  const char *path=getenv("SQLITE_PATH");if(!path||!*path){fprintf(stderr,"SQLITE_PATH required\n");exit(1);}
  if(sqlite3_open_v2(path,&app.db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX,NULL)!=SQLITE_OK){fprintf(stderr,"sqlite: %s\n",sqlite3_errmsg(app.db));exit(1);}
  sqlite3_busy_timeout(app.db,5000);
  if(sqlite3_exec(app.db,"PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",NULL,NULL,NULL)!=SQLITE_OK){fprintf(stderr,"sqlite setup: %s\n",sqlite3_errmsg(app.db));exit(1);}
  const char *feed="SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20";
  const char *post="SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1";
  const char *create="INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at";
  const char *like="INSERT INTO likes(user_id,post_id) VALUES(?1,?2) ON CONFLICT(user_id,post_id) DO NOTHING";
  if(prepare(&app.health,"SELECT 1")||prepare(&app.feed,feed)||prepare(&app.post,post)||prepare(&app.create,create)||prepare(&app.exists,"SELECT 1 FROM posts WHERE id=?1")||prepare(&app.like,like)){
    fprintf(stderr,"sqlite prepare: %s\n",sqlite3_errmsg(app.db));exit(1);
  }
  app.secret=getenv("JWT_SECRET");if(!app.secret)app.secret="twelve-dollar-challenge";
  app.started=monotonic_seconds();
}
static void complete(struct io_uring_cqe *cqe) {
  uint64_t data=io_uring_cqe_get_data64(cqe);unsigned op=UD_OP(data);int res=cqe->res;
  if(op==OP_ACCEPT){
    queue_accept();if(res<0)return;int fd=res;
    if(fd<0||(size_t)fd>=client_cap){close(fd);return;}
    Client *c=&clients[fd];if(c->state!=C_FREE)drop_client(fd);
    uint32_t gen=(c->gen+1)&0x00ffffffu;memset(c,0,sizeof *c);c->fd=fd;c->gen=gen;
    c->buf_id=NO_BUFFER;c->next_wait=-1;c->state=C_POLL;queue_poll(c);return;
  }
  int fd=UD_FD(data);uint32_t gen=UD_GEN(data);
  if(fd<0||(size_t)fd>=client_cap)return;Client *c=&clients[fd];
  if(c->state==C_FREE||c->gen!=gen)return;
  if(res<0){if(res==-EINTR||res==-EAGAIN||res==-EWOULDBLOCK){if(op==OP_RECV)queue_recv(c);else if(op==OP_POLL)queue_poll(c);else drop_client(fd);}else drop_client(fd);return;}
  if(op==OP_POLL){queue_recv(c);return;}
  if(op==OP_RECV){if(res==0){drop_client(fd);return;}c->used+=(size_t)res;request_ready(c);return;}
  if(op==OP_SEND){if(res==0){drop_client(fd);return;}c->sent+=(size_t)res;
    if(c->sent<c->out_len){queue_send(c);return;}
    free(c->output);c->output=NULL;c->out_len=c->sent=0;
    if(c->close_after){drop_client(fd);return;}
    if(c->used){request_ready(c);return;}
    if(c->buf_id!=NO_BUFFER)release_buffer(c);queue_poll(c);return;
  }
  drop_client(fd);
}
static void queue_accepts(void) { for(unsigned i=0;i<ACCEPT_DEPTH;i++)queue_accept(); }
static void run_worker(const char *host,long p) {
  signal(SIGPIPE,SIG_IGN);init_database();
  app.listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(app.listener<0)fail("socket");
  int yes=1;if(setsockopt(app.listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes))fail("SO_REUSEADDR");
  if(setsockopt(app.listener,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof yes))fail("SO_REUSEPORT");
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)p)};
  if(inet_pton(AF_INET,host,&addr.sin_addr)!=1)fail("inet_pton");
  if(bind(app.listener,(struct sockaddr*)&addr,sizeof addr))fail("bind");if(listen(app.listener,4096))fail("listen");
  struct rlimit lim;if(getrlimit(RLIMIT_NOFILE,&lim))fail("getrlimit");
  client_cap=(size_t)lim.rlim_cur;if(client_cap>MAX_CLIENTS)client_cap=MAX_CLIENTS;
  clients=calloc(client_cap,sizeof *clients);if(!clients)fail("calloc clients");
  for(size_t i=0;i<client_cap;i++){clients[i].fd=-1;clients[i].buf_id=NO_BUFFER;clients[i].next_wait=-1;}
  io.inputs=malloc((size_t)INPUT_BUFFERS*INPUT_BUFFER_SIZE);if(!io.inputs)fail("input pool");
  io.free_count=INPUT_BUFFERS;for(unsigned i=0;i<INPUT_BUFFERS;i++)io.free_buffers[i]=(uint16_t)(INPUT_BUFFERS-1-i);
  io.wait_head=io.wait_tail=-1;
  struct io_uring_params params={0};params.flags=IORING_SETUP_CQSIZE;params.cq_entries=CQ_SIZE;
  int rc=io_uring_queue_init_params(SQ_SIZE,&io.ring,&params);
  if(rc<0){fprintf(stderr,"large CQ unavailable (%s), using default CQ\n",strerror(-rc));rc=io_uring_queue_init(SQ_SIZE,&io.ring,0);}
  if(rc<0){errno=-rc;fail("io_uring_queue_init");}
  queue_accepts();
  fprintf(stderr,"C io_uring worker on %s:%ld, fd slots=%zu, request buffers=%u x %u\n",host,p,client_cap,INPUT_BUFFERS,INPUT_BUFFER_SIZE);
  for(;;){int submitted=io_uring_submit(&io.ring);if(submitted<0){if(submitted==-EINTR)continue;errno=-submitted;fail("io_uring_submit");}
    struct io_uring_cqe *cqe;rc=io_uring_wait_cqe(&io.ring,&cqe);
    if(rc<0){if(rc==-EINTR)continue;errno=-rc;fail("io_uring_wait_cqe");}
    complete(cqe);io_uring_cqe_seen(&io.ring,cqe);
  }
}
int main(void) {
  struct rlimit lim;if(getrlimit(RLIMIT_NOFILE,&lim))fail("getrlimit");
  if(lim.rlim_cur<lim.rlim_max){lim.rlim_cur=lim.rlim_max;if(setrlimit(RLIMIT_NOFILE,&lim))fail("setrlimit RLIMIT_NOFILE");}
  const char *host=getenv("HOST"),*port=getenv("PORT");if(!host)host="0.0.0.0";if(!port)port="80";
  char *tail=NULL;long p=strtol(port,&tail,10);if(!tail||*tail||p<1||p>65535){fprintf(stderr,"invalid PORT\n");return 2;}
  const char *setting=getenv("WORKERS");long workers=setting?strtol(setting,&tail,10):8;
  if(workers<1||workers>16||(setting&&(!tail||*tail))){fprintf(stderr,"WORKERS must be from 1 to 16\n");return 2;}
  fprintf(stderr,"starting %ld independent io_uring workers on %s:%ld\n",workers,host,p);
  for(long i=0;i<workers;i++){
    pid_t pid=fork();if(pid<0)fail("fork");
    if(pid==0){if(prctl(PR_SET_PDEATHSIG,SIGTERM)<0)fail("prctl");if(getppid()==1)_exit(1);run_worker(host,p);_exit(0);}
  }
  int status;for(;;){if(wait(&status)<0){if(errno==EINTR)continue;if(errno==ECHILD)break;fail("wait");}}
  return 0;
}
