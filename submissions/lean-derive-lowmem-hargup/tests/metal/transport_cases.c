/* Exercise transport fragment ownership without opening a ring or a database. */
#include <assert.h>
#include "out.c"
#include "http.c"
#include "uring.c"
static void app_decode(const req_t *rq,job_t *j,u32 slot) {
  (void)slot; j->kind=1;
  if(rq->method==M_POST)j->is_write=1;
}
static void app_batch(job_t *js,int n) {(void)js;(void)n;}
static void clean(conn *c) {
  conn_reset(c);njobs=npend=nrearm=0;c->pending=0;c->state=C_OPEN;
}
int main(void) {
  ring *r=&rings[0];conn c={.state=C_OPEN};r->conns=&c;r->idx=0;nrings=1;
  const char *get="GET /feed HTTP/1.1\r\nHost: x\r\n\r\n";
  u32 gn=strlen(get);
  for(u32 split=1;split<gn;split++) {
    on_data(r,0,(u8*)get,split);
    assert(c.in && c.in_len==split && !njobs);
    on_data(r,0,(u8*)get+split,gn-split);
    assert(njobs==1 && !c.in && !c.in_len && !c.in_cap);
    clean(&c);
  }
  char body[33000];
  int h=snprintf(body,sizeof body,"POST /posts HTTP/1.1\r\nContent-Length: 16000\r\n\r\n");
  memset(body+h,'x',16000);u32 n=h+16000;
  for(u32 step=1;step<2048;step=step*2+1) {
    for(u32 pos=0;pos<n;) {
      u32 k=n-pos<step?n-pos:step;on_data(r,0,(u8*)body+pos,k);pos+=k;
      if(pos>=h && pos<n)assert(fragment_of(c.in)->frame.phase==FRAME_BODY);
    }
    assert(njobs==1 && jobs[0].is_write && c.hold && !c.in);
    clean(&c);
  }
  /* A write barrier stores a following request and then resumes at a new frame. */
  memcpy(body+n,get,gn);on_data(r,0,(u8*)body,n+gn);
  assert(njobs==1 && c.hold && c.in_len==gn);
  njobs=0;release_holds();
  assert(njobs==1 && !c.hold && !c.in);clean(&c);
  /* Capacity hold occurs after framing: it must reset before replaying the same bytes. */
  njobs=MAX_JOBS-1;on_data(r,0,(u8*)get,gn);
  assert(c.hold && c.in_len==gn && fragment_of(c.in)->frame.phase==FRAME_HEAD);
  njobs=0;release_holds();assert(njobs==1 && !c.in && !c.hold);clean(&c);
  /* A close/reset while waiting for the body must release the cold state. */
  on_data(r,0,(u8*)body,h+10);assert(c.in);clean(&c);assert(!c.in);
  puts("transport: fragmented allocations, growth, free-on-idle, write/capacity holds and reset passed");
  return 0;
}
