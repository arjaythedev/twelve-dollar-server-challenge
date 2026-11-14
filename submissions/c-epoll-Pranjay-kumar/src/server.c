#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
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
#include <sys/uio.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_REQUEST_BUF 32768
#define MAX_BODY 16384
#define MAX_EVENTS 256

typedef struct {
  int fd;
  size_t used, sent, out_len;
  char input[MAX_REQUEST_BUF];
  char header[256];
  size_t header_len;
  char *output;
  bool close_after;
} Client;

typedef struct {
  sqlite3 *db;
  sqlite3_stmt *health, *feed, *post, *create, *exists, *like;
  const char *secret;
  int epfd, listener;
  uint64_t started;
} App;

static App app;
static Client **clients;
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
      if(rc!=SQLITE_ROW){sqlite3_reset(app.create);sqlite3_clear_bindings(app.create);json_object_put(j);*status=500;return error_body("internal server error");}
      int64_t id=sqlite3_column_int64(app.create,0);const char *dt=(const char*)sqlite3_column_text(app.create,1);
      char created[64];snprintf(created,sizeof created,"%s",dt?dt:"");
      json_object *p=obj();json_object_object_add(p,"id",json_object_new_int64(id));
      json_object_object_add(p,"body",json_object_new_string_len(raw+lo,(int)(hi-lo)));
      add_s(p,"created_at",created);add_s(p,"author",username);json_object_object_add(p,"like_count",json_object_new_int(0));
      json_object *o=obj();json_object_object_add(o,"post",p);
      rc=sqlite3_step(app.create);sqlite3_reset(app.create);sqlite3_clear_bindings(app.create);json_object_put(j);
      if(rc!=SQLITE_DONE){*status=500;return error_body("internal server error");}
      *status=201;return json_take(o);
    }
    size_t n=strlen(path)-12;char idbuf[64];
    if(n>=sizeof idbuf){*status=400;return error_body("invalid post id");}
    memcpy(idbuf,path+7,n);idbuf[n]=0;char *tail=NULL;errno=0;long long id=strtoll(idbuf,&tail,10);
    if(errno||!n||!tail||*tail||id<=0){*status=400;return error_body("invalid post id");}
    sqlite3_bind_int64(app.exists,1,id);int rc=step_reset(app.exists);
    if(rc!=SQLITE_ROW){*status=rc==SQLITE_DONE?404:500;return error_body(rc==SQLITE_DONE?"post not found":"internal server error");}
    sqlite3_bind_int64(app.like,1,(sqlite3_int64)uid);sqlite3_bind_int64(app.like,2,id);
    rc=step_reset(app.like);if(rc!=SQLITE_DONE){*status=500;return error_body("internal server error");}
    bool inserted=sqlite3_changes(app.db)!=0;*status=inserted?201:200;
    json_object *o=obj();json_object_object_add(o,"liked",json_object_new_boolean(true));
    json_object_object_add(o,"already_liked",json_object_new_boolean(!inserted));json_object_object_add(o,"post_id",json_object_new_int64(id));return json_take(o);
  }
  *status=404;return error_body("not found");
}
static bool request_complete(Client *c,size_t *header_len,size_t *body_len) {
  char *end=memmem(c->input,c->used,"\r\n\r\n",4);if(!end)return false;
  *header_len=(size_t)(end-c->input)+4;*body_len=0;
  char *line=memmem(c->input,*header_len,"\r\n",2);if(!line)return false;
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
static int set_interest(Client *c) {
  struct epoll_event e={.events=EPOLLRDHUP|(c->output?EPOLLOUT:EPOLLIN),.data.fd=c->fd};
  return epoll_ctl(app.epfd,EPOLL_CTL_MOD,c->fd,&e);
}
static void drop_client(int fd) {
  epoll_ctl(app.epfd,EPOLL_CTL_DEL,fd,NULL);close(fd);
  if(fd>=0&&(size_t)fd<client_cap){free(clients[fd]->output);free(clients[fd]);clients[fd]=NULL;}
}
static int queue_response(Client *c,int status,char *body,bool close_after) {
  const char *reason=status==200?"OK":status==201?"Created":status==400?"Bad Request":status==401?"Unauthorized":status==404?"Not Found":status==503?"Service Unavailable":"Internal Server Error";
  size_t n=strlen(body);int h=snprintf(c->header,sizeof c->header,
    "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
    status,reason,n,close_after?"close":"keep-alive");
  if(h<0||(size_t)h>=sizeof c->header||(size_t)h+n>MAX_REQUEST_BUF){free(body);return -1;}
  c->output=body;c->header_len=(size_t)h;c->out_len=(size_t)h+n;c->sent=0;c->close_after=close_after;return 0;
}

static int parse_one(Client *c) {
  size_t hlen,blen;if(!request_complete(c,&hlen,&blen))return 0;
  char method[16]={0},path[512]={0},version[16]={0};
  char *line=memmem(c->input,hlen,"\r\n",2);size_t first=(size_t)(line-c->input);
  char request[544];if(first>=sizeof request)return -1;
  memcpy(request,c->input,first);request[first]=0;
  if(sscanf(request,"%15s %511s %15s",method,path,version)!=3)return -1;
  bool close_after=!strcmp(version,"HTTP/1.0");char auth[4096]={0};char *end=c->input+hlen-4;
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
  int status=200;char *response=handle(method,path,auth,c->input+hlen,blen,&status);
  if(!response||queue_response(c,status,response,close_after))return -1;
  size_t consumed=hlen+blen,left=c->used-consumed;
  if(left)memmove(c->input,c->input+consumed,left);c->used=left;
  return 1;
}
static int write_client(Client *c) {
  while(c->sent<c->out_len){
    struct iovec vec[2];int count=0;
    if(c->sent<c->header_len){vec[count++]=(struct iovec){.iov_base=c->header+c->sent,.iov_len=c->header_len-c->sent};
      vec[count++]=(struct iovec){.iov_base=c->output,.iov_len=c->out_len-c->header_len};}
    else vec[count++]=(struct iovec){.iov_base=c->output+(c->sent-c->header_len),.iov_len=c->out_len-c->sent};
    struct msghdr msg={.msg_iov=vec,.msg_iovlen=(size_t)count};ssize_t n=sendmsg(c->fd,&msg,MSG_NOSIGNAL);
    if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return 0;return -1;}
    if(!n)return -1;c->sent+=(size_t)n;
  }
  free(c->output);c->output=NULL;c->out_len=c->sent=c->header_len=0;
  if(c->close_after)return -1;
  int parsed=parse_one(c);if(parsed<0)return -1;
  return set_interest(c);
}
static int read_client(Client *c) {
  for(;;){
    if(c->output)return set_interest(c);
    if(c->used==sizeof c->input)return -1;
    ssize_t n=recv(c->fd,c->input+c->used,sizeof(c->input)-c->used,0);
    if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return 0;return -1;}
    if(!n)return -1;c->used+=(size_t)n;
    int parsed=parse_one(c);if(parsed<0)return -1;if(parsed>0)return set_interest(c);
  }
}
static void accept_clients(void) {
  for(;;){int fd=accept4(app.listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if(fd<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return;fail("accept4");}
    if((size_t)fd>=client_cap){close(fd);continue;}
    Client *c=calloc(1,sizeof *c);if(!c){close(fd);continue;}c->fd=fd;clients[fd]=c;
    struct epoll_event e={.events=EPOLLIN|EPOLLRDHUP,.data.fd=fd};
    if(epoll_ctl(app.epfd,EPOLL_CTL_ADD,fd,&e)){clients[fd]=NULL;free(c);close(fd);}
  }
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
static void run_worker(const char *host,long p) {
  signal(SIGPIPE,SIG_IGN);init_database();
  app.listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(app.listener<0)fail("socket");
  int yes=1;setsockopt(app.listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
  if(setsockopt(app.listener,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof yes))fail("SO_REUSEPORT");
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)p)};
  if(inet_pton(AF_INET,host,&addr.sin_addr)!=1)fail("inet_pton");
  if(bind(app.listener,(struct sockaddr*)&addr,sizeof addr))fail("bind");
  if(listen(app.listener,4096))fail("listen");
  struct rlimit lim;if(getrlimit(RLIMIT_NOFILE,&lim))fail("getrlimit");
  client_cap=(size_t)lim.rlim_cur;if(client_cap>131072)client_cap=131072;
  clients=calloc(client_cap,sizeof *clients);if(!clients)fail("calloc clients");
  app.epfd=epoll_create1(EPOLL_CLOEXEC);if(app.epfd<0)fail("epoll_create1");
  struct epoll_event ev={.events=EPOLLIN,.data.fd=app.listener};if(epoll_ctl(app.epfd,EPOLL_CTL_ADD,app.listener,&ev))fail("epoll add listener");
  fprintf(stderr,"independent C/epoll server on %s:%ld\n",host,p);
  struct epoll_event events[MAX_EVENTS];
  for(;;){int n=epoll_wait(app.epfd,events,MAX_EVENTS,-1);if(n<0){if(errno==EINTR)continue;fail("epoll_wait");}
    for(int i=0;i<n;i++){int fd=events[i].data.fd;if(fd==app.listener){accept_clients();continue;}
      if(fd<0||(size_t)fd>=client_cap||!clients[fd])continue;Client *c=clients[fd];int rc=0;
      if(events[i].events&(EPOLLERR|EPOLLHUP|EPOLLRDHUP))rc=-1;
      else if(events[i].events&EPOLLOUT)rc=write_client(c);
      else if(events[i].events&EPOLLIN)rc=read_client(c);
      if(rc<0)drop_client(fd);
    }
  }
}
int main(void) {
  struct rlimit lim;if(getrlimit(RLIMIT_NOFILE,&lim))fail("getrlimit");
  if(lim.rlim_cur<lim.rlim_max){lim.rlim_cur=lim.rlim_max;if(setrlimit(RLIMIT_NOFILE,&lim))fail("setrlimit RLIMIT_NOFILE");}
  const char *host=getenv("HOST"),*port=getenv("PORT");if(!host)host="0.0.0.0";if(!port)port="80";
  char *tail=NULL;long p=strtol(port,&tail,10);if(!tail||*tail||p<1||p>65535){fprintf(stderr,"invalid PORT\n");return 2;}
  long online=sysconf(_SC_NPROCESSORS_ONLN);if(online<1)online=1;if(online>8)online=8;
  const char *setting=getenv("WORKERS");long workers=setting?strtol(setting,&tail,10):online;
  if(workers<1||workers>32||(setting&&(!tail||*tail))){fprintf(stderr,"WORKERS must be from 1 to 32\n");return 2;}
  fprintf(stderr,"starting %ld C/epoll worker(s) on %s:%ld\n",workers,host,p);
  for(long i=0;i<workers;i++){
    pid_t pid=fork();if(pid<0)fail("fork");
    if(pid==0){if(prctl(PR_SET_PDEATHSIG,SIGTERM)<0)fail("prctl");if(getppid()==1)_exit(1);run_worker(host,p);_exit(0);}
  }
  int status;for(;;){if(wait(&status)<0){if(errno==EINTR)continue;if(errno==ECHILD)break;fail("wait");}}
  return 0;
}
