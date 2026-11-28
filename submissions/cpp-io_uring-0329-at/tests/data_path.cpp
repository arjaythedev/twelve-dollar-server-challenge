#include "db.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

static unsigned long long cpu_ns() { timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec*1000000000ULL+t.tv_nsec; }
static void output(const char* tag, const app::ByteBuf& b) { std::printf("%s ", tag); std::fwrite(b.p,1,b.len,stdout); std::puts(""); }
int main(int argc, char** argv) {
  if(argc<3) return 2;
  setenv("NO_CKPT", "1", 1);
  app::Db db; std::string err;
  if(!db.open(argv[1],err)) { std::fprintf(stderr,"open: %s\n",err.c_str()); return 3; }
  app::ByteBuf out;
  try {
    if(std::strcmp(argv[2],"bench")==0) {
      const bool feed=std::strcmp(argv[3],"feed")==0;
      const int n=std::atoi(argv[4]); unsigned long long bytes=0;
      for(int i=0;i<10000;i++){ out.len=0; if(feed) db.feed(out); else db.post(1+(i*7919LL)%500000,out); if(i%256==0&&!db.commit()) return 4; }
      const auto t0=cpu_ns();
      for(int i=0;i<n;i++){ out.len=0; if(feed) db.feed(out); else if(db.post(1+(i*7919LL)%500000,out)!=app::PostLookup::Found) return 5; bytes+=out.len; if(i%256==0&&!db.commit()) return 6; }
      const auto dt=cpu_ns()-t0;
      std::printf("{\"endpoint\":\"%s\",\"requests\":%d,\"cpu_ns\":%llu,\"ns_per_request\":%.2f,\"bytes\":%llu}\n",feed?"feed":"post",n,dt,double(dt)/n,bytes);
    } else {
      db.feed(out); output("feed",out);
      for(long long id: {1LL,2LL,500000LL,999999999LL}) {out.len=0; auto status=db.post(id,out); std::printf("status %lld %d\n",id,int(status)); if(status==app::PostLookup::Found)output("post",out);}
      if(std::strcmp(argv[2],"lowlike")==0) {
        std::printf("low_insert %d",int(db.insert_like(1,1))); std::puts("");
        out.len=0; db.post(1,out); output("after_lowlike",out);
      }
      if(std::strcmp(argv[2],"counts")==0) {
        for(int i=0;i<2000;i++) {out.len=0; if(db.post(1+(i*7919LL)%500000,out)!=app::PostLookup::Found)return 12; output("post",out);}
      }
      if(std::strcmp(argv[2],"writes")==0) {
        char ts[64]; int len=0;
        const unsigned char body[]="quote\" slash\\ tab\t newline\n \xE4\xBD\xA0";
        const auto id=db.insert_post(1,body,sizeof(body)-1,ts,len);
        std::printf("new_id %lld\n",static_cast<long long>(id));
        std::printf("like %d %d\n",db.insert_like(1,id),db.insert_like(2,id));
        std::printf("duplicate %d\n",db.insert_like(1,id));
        out.len=0; db.post(id,out); output("before_commit",out);
        if(!db.commit()) return 7;
        out.len=0; db.post(id,out); output("after_commit",out);
        out.len=0; db.feed(out); output("after_feed",out);
      }
      if(std::strcmp(argv[2],"move")==0) {
        if(!db.commit())return 8;
        app::Db moved(std::move(db));
        out.len=0; moved.feed(out); output("moved_feed",out);
        std::printf("moved_cache %d\n",int(moved.author_cache_on()));
        if(!moved.commit())return 9;
      } else if(!db.commit())return 10;
    }
  } catch(const app::DbError&) { std::fputs("DbError\n",stderr); return 11; }
  app::buf_release(out);
}
