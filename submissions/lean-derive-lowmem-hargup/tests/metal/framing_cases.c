#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "http.c"
static void equal(const req_t *a,const req_t *b) {
  assert(a->method==b->method && a->close==b->close && a->has_auth==b->has_auth);
  assert(a->path_len==b->path_len && !memcmp(a->path,b->path,a->path_len));
  assert(a->auth_len==b->auth_len && (!a->auth_len || !memcmp(a->auth,b->auth,a->auth_len)));
  assert(a->body_len==b->body_len && !memcmp(a->body,b->body,a->body_len));
}
static void fragments(const char *wire,u32 n,u32 step) {
  frame_state fs={0};u32 have=0;u8 *old=0;
  while(have<n) {
    have=have+step<n?have+step:n;
    u8 *m=malloc(n+37);assert(m);memcpy(m,wire,have);free(old);old=m;
    req_t full,inc;
    int expected=http_parse(m,have,&full,0),got=http_parse(m,have,&inc,&fs);
    assert(expected==got);
    if(got>0){equal(&full,&inc);assert(fs.phase==FRAME_HEAD && fs.head.scanned==0);break;}
    if(got<0)break;
  }
  free(old);
}
static unsigned long long clockns(void){struct timespec ts;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts);return (unsigned long long)ts.tv_sec*1000000000+ts.tv_nsec;}
int main(int argc,char **argv) {
  const char *cases[]={
    "GET /health HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /posts?q=x HTTP/1.0\r\nAuthorization: Bearer example\r\nConnection: keep-alive\r\nContent-Length: 5\r\n\r\nhelloGET /feed HTTP/1.1\r\n\r\n",
    "POST /posts HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
    "POST /posts HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!",
    "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
    "POST /posts HTTP/1.1\r\nContent-Length: 16385\r\n\r\n",
    "GET / HTTP/1.1\r\nBroken header\r\n\r\n",
    "POST /posts HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
    "POST /posts HTTP/1.1\r\nContent-Length: 9999999999\r\n\r\n",
    "GET / HTTP/1.1\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n"
  };
  for(unsigned i=0;i<sizeof cases/sizeof *cases;i++)for(u32 step=1;step<=strlen(cases[i]);step++)fragments(cases[i],strlen(cases[i]),step);
  char wire[33000];int head=snprintf(wire,sizeof wire,"POST /posts HTTP/1.1\r\nAuthorization: Bearer example\r\nContent-Length: 16000\r\n\r\n");
  memset(wire+head,'x',16000);u32 n=head+16000;
  for(u32 step=1;step<2048;step=step*2+1)fragments(wire,n,step);
  memset(wire,'x',sizeof wire);for(u32 step=1;step<1024;step=step*2+1)fragments(wire,MAX_HEAD+1,step);
  puts("framing: split boundaries, relocation, pipeline prefix, errors and limits passed");
  if(argc>1) {
    int rounds=atoi(argv[1]);
    head=snprintf(wire,sizeof wire,"POST /posts HTTP/1.1\r\nAuthorization: Bearer example\r\nContent-Length: 16000\r\n\r\n");
    memset(wire+head,'x',16000);n=head+16000;
    for(int pass=0;pass<6;pass++) {
      int inc=pass==1||pass==2||pass==5;unsigned long long t=clockns(),sum=0;
      for(int r=0;r<rounds;r++) {
        frame_state fs={0};req_t q;
        for(u32 k=1;k<n;k+=31)sum+=http_parse((u8*)wire,k,&q,inc?&fs:0);
        sum+=http_parse((u8*)wire,n,&q,inc?&fs:0);
      }
      printf("%s fragmented ns/request=%.2f checksum=%llu\n",inc?"incremental":"stateless",(double)(clockns()-t)/rounds,sum);
    }
  }
  return 0;
}
