/* Actual generated constructors/executors: snapshot reads, ordered writes, error holes,
 * failed COMMIT response replacement, and reset/reuse. Run on a disposable seed copy. */
#define main server_main
#include "server.c"
#undef main
#include <assert.h>
static i64 scalar(const char *sql) {
  sqlite3_stmt *q=db_prepare(sql);assert(sqlite3_step(q)==SQLITE_ROW);
  i64 value=sqlite3_column_int64(q,0);sqlite3_finalize(q);return value;
}
static int status(const job_t *j){assert(j->resp && j->resp->len>12);return atoi((const char*)j->resp->data+9);}
static void decode(job_t *jobs,u32 slot,const char *path,int post,const char *auth) {
  const char *body="{\"body\":\"batch order\"}";
  req_t r={.method=post?M_POST:M_GET,.path=(const u8*)path,.path_len=strlen(path),
    .has_auth=post,.auth=(const u8*)auth,.auth_len=strlen(auth),.body=(const u8*)body,.body_len=strlen(body)};
  app_decode(&r,&jobs[slot],slot);
}
static int reject_commit(void *p){(*(int*)p)++;return 1;}
int main(void) {
  db_open(getenv("SQLITE_PATH"));app_init();app_check_inv();
  hmac_init((const u8*)"twelve-dollar-challenge",strlen("twelve-dollar-challenge"));now_s=time(0);
  const char *auth=getenv("BENCH_AUTH");assert(auth);
  i64 max=scalar("SELECT max(id) FROM posts");
  for(int fail=0;fail<2;fail++) {
    job_t js[6]={0};char like[80],post[80];
    snprintf(like,sizeof like,"/posts/%lld/like",max+1);
    snprintf(post,sizeof post,"/posts/%lld",max+1);
    decode(js,0,"/posts",1,auth);
    decode(js,1,"/missing",0,auth); /* error hole must not corrupt any chain */
    decode(js,2,like,1,auth);
    decode(js,3,post,0,auth);       /* read snapshot must not see earlier queued create */
    decode(js,4,"/health",0,auth);
    decode(js,5,"/feed",0,auth);
    assert(stream_writes.head==0 && js[0].next==2 && js[2].next==UINT32_MAX);
    assert(stream_post.head==3 && stream_health.head==4 && stream_feed.head==5);
    int rejected=0;if(fail)sqlite3_commit_hook(db,reject_commit,&rejected);
    app_batch(js,6);
    sqlite3_commit_hook(db,0,0);
    assert(status(&js[0])==(fail?500:201));
    assert(status(&js[1])==404 && status(&js[2])==(fail?500:201));
    assert(status(&js[3])==404 && status(&js[4])==200 && status(&js[5])==200);
    assert(!arena_used && !stream_writes.tail && stream_post.head==UINT32_MAX);
    if(fail){assert(rejected==1);assert(scalar("SELECT max(id) FROM posts")==max);}
    else {max++;assert(scalar("SELECT max(id) FROM posts")==max);}
    for(unsigned i=0;i<6;i++)obuf_release(js[i].resp);
  }
  job_t last={0};decode(&last,0,"/posts",1,auth);app_batch(&last,1);
  assert(status(&last)==201 && scalar("SELECT max(id) FROM posts")==max+1);obuf_release(last.resp);
  puts("batch: read snapshot, exact queues, ordered create/like, error holes, COMMIT rollback and reuse passed");
  return 0;
}
