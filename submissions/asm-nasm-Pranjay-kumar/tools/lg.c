// Closed-loop HTTP/1.1 load generator: C connections, each running the k6 user loop
// (feed -> post -> 15% like -> 2% create) with no think time. Reports req/s and latency.
// usage: lg host port conns seconds tokens.json [pipeline=1]
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct C_ C; struct C_ { C*next; double due; int fd; int step; char out[1024]; int olen, osent; char in[65536]; int ilen; double t0; long feedid[20]; int nfeed; int tok; };
static char **toks; static int ntok;
static long hist[100000]; static long nreq, nerr, nbad;
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static unsigned rnd(void){ static unsigned s=12345; s=s*1103515245+12345; return s>>8; }
static const char *host; static int port; static double think;
#define NB 8192
static C* wheel[NB]; static long wpos;
static void sched(C*c,double t){ long b=(long)(t*1000); if(b<=wpos) b=wpos+1; if(b-wpos>=NB) b=wpos+NB-1; c->next=wheel[b%NB]; wheel[b%NB]=c; }

static void build(C*c){
  int r; const char*t=toks[c->tok];
  for(;;){
    switch(c->step){
    case 0: c->olen=sprintf(c->out,"GET /feed HTTP/1.1\r\nHost: x\r\nUser-Agent: lg\r\n\r\n"); return;
    case 1: { long id = c->nfeed? c->feedid[rnd()%c->nfeed] : 1+rnd()%500000;
      c->olen=sprintf(c->out,"GET /posts/%ld HTTP/1.1\r\nHost: x\r\nUser-Agent: lg\r\n\r\n",id); return; }
    case 2: if(rnd()%100<15){ long id = c->nfeed? c->feedid[rnd()%c->nfeed] : 1+rnd()%500000;
      c->olen=sprintf(c->out,"POST /posts/%ld/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n",id,t); return; }
      c->step=3; break;
    case 3: if(rnd()%100<2){ char b[200]; int n=sprintf(b,"{\"body\":\"lg says hi at %.3f (conn %d)\"}",now(),c->fd);
      c->olen=sprintf(c->out,"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",t,n,b); return; }
      c->step=0; break;
    }
  }
}
static void connect_c(C*c,int ep){
  c->fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0); int one=1; setsockopt(c->fd,IPPROTO_TCP,TCP_NODELAY,&one,4);
  struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)}; inet_pton(AF_INET,host,&a.sin_addr);
  connect(c->fd,(void*)&a,sizeof a);
  struct epoll_event e={.events=EPOLLIN|EPOLLOUT|EPOLLET,.data.ptr=c}; epoll_ctl(ep,EPOLL_CTL_ADD,c->fd,&e);
  c->step=0; c->ilen=0; build(c); c->osent=0; c->t0=now();
}
static void flush(C*c){ while(c->osent<c->olen){ int n=write(c->fd,c->out+c->osent,c->olen-c->osent); if(n<=0) return; c->osent+=n; } }
static int parse(C*c){ // returns bytes of one complete response, 0 if incomplete
  char*h=memmem(c->in,c->ilen,"\r\n\r\n",4); if(!h) return 0; int hl=h-c->in+4;
  char*cl=memmem(c->in,hl,"Content-Length: ",16); if(!cl) return -1; int bl=atoi(cl+16);
  if(c->ilen<hl+bl) return 0;
  int st=atoi(c->in+9); if(st>=500) nerr++; if(st>=400 && st<500) nbad++;
  if(c->step==0 && st==200){ c->nfeed=0; char*p=c->in+hl,*e=p+bl; while(c->nfeed<20 && (p=memmem(p,e-p,"{\"id\":",6))){ p+=6; c->feedid[c->nfeed++]=atol(p);} }
  return hl+bl;
}
int main(int argc,char**argv){
  host=argv[1]; port=atoi(argv[2]); int nc=atoi(argv[3]); double dur=atof(argv[4]); think=argc>6?atof(argv[6])/1000.0:0; wpos=(long)(now()*1000);
  FILE*f=fopen(argv[5],"r"); fseek(f,0,2); long sz=ftell(f); rewind(f); char*j=malloc(sz+1); fread(j,1,sz,f); j[sz]=0;
  toks=malloc(sizeof(char*)*30000); for(char*p=j;(p=strstr(p,"\"token\":\""));){ p+=9; char*e=strchr(p,'"'); *e=0; toks[ntok++]=p; p=e+1; }
  int ep=epoll_create1(0); C*cs=calloc(nc,sizeof(C));
  for(int i=0;i<nc;i++){ cs[i].tok=i%ntok; connect_c(&cs[i],ep); if(i%500==499) usleep(20000); }
  double start=now(), warm=start+1.0; struct epoll_event ev[512]; int measuring=0; long base=0;
  while(now()<start+dur+1.0){
    int n=epoll_wait(ep,ev,512,think>0?1:100);
    if(think>0){ long t=(long)(now()*1000); while(wpos<t){ wpos++; C*c=wheel[wpos%NB]; wheel[wpos%NB]=0; while(c){ C*nx=c->next; c->t0=now(); flush(c); c=nx; } } }
    if(!measuring && now()>=warm){ measuring=1; memset(hist,0,sizeof hist); base=nreq; nerr=nbad=0; start=now()-1.0; }
    for(int i=0;i<n;i++){ C*c=ev[i].data.ptr;
      if(ev[i].events&(EPOLLERR|EPOLLHUP)){ close(c->fd); nerr++; connect_c(c,ep); continue; }
      if(ev[i].events&EPOLLOUT) flush(c);
      if(ev[i].events&EPOLLIN){ for(;;){ int r=read(c->fd,c->in+c->ilen,sizeof c->in-c->ilen); if(r==0){ close(c->fd); nerr++; connect_c(c,ep); break;} if(r<0) break; c->ilen+=r;
          int used; while((used=parse(c))>0){ double lat=(now()-c->t0)*1e3; int b=(int)(lat*10); if(b>=100000)b=99999; hist[b]++; nreq++;
            memmove(c->in,c->in+used,c->ilen-used); c->ilen-=used; c->step=(c->step+1)%4; build(c); c->osent=0;
            if(think>0){ sched(c, now()+think*2*(rnd()%1000)/1000.0); } else { c->t0=now(); flush(c);} }
          if(used<0){ fprintf(stderr,"bad response\n"); exit(1);} } }
    }
  }
  double el=now()-start-1.0; long tot=nreq-base; long acc=0; double p50=0,p95=0,p99=0,mx=0;
  for(int b=0;b<100000;b++){ if(!hist[b]) continue; acc+=hist[b]; if(!p50&&acc>=tot*0.5)p50=b/10.0; if(!p95&&acc>=tot*0.95)p95=b/10.0; if(!p99&&acc>=tot*0.99)p99=b/10.0; mx=b/10.0; }
  printf("conns=%d reqs=%ld rps=%.0f p50=%.1fms p95=%.1fms p99=%.1fms max=%.1fms 5xx/conn-err=%ld 4xx=%ld\n",nc,tot,tot/el,p50,p95,p99,mx,nerr,nbad);
}
