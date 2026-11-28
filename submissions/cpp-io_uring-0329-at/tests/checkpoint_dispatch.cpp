#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sys/resource.h>
struct CkState { std::mutex mu; std::condition_variable cv; std::atomic<bool> stop{false}; int pending=-1; bool running=false; };
struct dbx {};
CkState object; CkState* ck_=&object; dbx handle; dbx* h_=&handle;
std::atomic<int> phase{0}, request{1}, calls{0}, active{0}, maxactive{0}, actual{-99};
std::atomic<long long> frames{16384};
std::mutex testmu; std::condition_variable testcv; bool entered=false, release_run=false;
long long dbx_wal_frames_now(dbx*) { return frames.load(); }
int dbx_ck_wants(dbx*, int tail) { return phase.load()==(tail?2:1); }
int dbx_ck_kick(dbx*) { if(phase.load()==0) { phase=request.load(); return 1; } return 0; }
void dbx_ck_run(dbx*, bool) {
  int n=++active; if(n>maxactive) maxactive=n;
  ++calls; actual=getpriority(PRIO_PROCESS,0);
  phase=0; // Publish DONE before returning: dispatcher must still exclude ownership.
  std::unique_lock<std::mutex> lk(testmu); entered=true; testcv.notify_all();
  testcv.wait(lk,[]{return release_run;}); --active;
}
enum { kCkBulk = 0, kCkUrgent = 1, kCkTail = 2 };
constexpr long long kCkBehindFrames = 32768;

void checkpoint_loop(dbx* h, CkState* st, int role) {
  if (std::getenv("NO_CKPT")) return;
  const int requested = role == kCkBulk ? 19 : role == kCkUrgent ? 10 : 0;
  // Tail inherits the event-loop priority. Do not try to restore it to zero.
  if (role != kCkTail && setpriority(PRIO_PROCESS, 0, requested) != 0) {
    const int e = errno;
    std::fprintf(stderr, "checkpoint role=%d setpriority(%d) failed: %s (%d)\n",
                 role, requested, std::strerror(e), e);
  }
  errno = 0;
  const int actual = getpriority(PRIO_PROCESS, 0);
  const int e = errno;
  if (e != 0) {
    std::fprintf(stderr, "checkpoint role=%d getpriority failed: %s (%d)\n",
                 role, std::strerror(e), e);
  } else {
    std::fprintf(stderr, "checkpoint role=%d requested_nice=%d actual_nice=%d\n",
                 role, requested, actual);
  }
  std::unique_lock<std::mutex> lk(st->mu);
  while (!st->stop.load()) {
    st->cv.wait(lk, [&] { return st->stop.load() || st->pending == role; });
    if (st->stop.load()) break;
    st->pending = -1;
    st->running = true;
    lk.unlock();
    dbx_ck_run(h, role == kCkTail);
    lk.lock();
    st->running = false;
  }
}


void kick() {
  std::lock_guard<std::mutex> lk(ck_->mu);
  // Keep the shared SQLITE_OPEN_NOMUTEX checkpoint connection single-owner,
  // including the gap between publishing *_DONE and returning from ck_run.
  if (ck_->running || ck_->pending != -1) return;
  if (!dbx_ck_kick(h_)) return;
  if (dbx_ck_wants(h_, true)) {
    ck_->pending = kCkTail;
  } else if (dbx_ck_wants(h_, false)) {
    ck_->pending = dbx_wal_frames_now(h_) >= kCkBehindFrames ? kCkUrgent : kCkBulk;
  }
  if (ck_->pending != -1) ck_->cv.notify_all();
}

int main() {
  std::thread a([&]{checkpoint_loop(h_,ck_,kCkBulk);});
  std::thread b([&]{checkpoint_loop(h_,ck_,kCkUrgent);});
  std::thread c([&]{checkpoint_loop(h_,ck_,kCkTail);});
  const int inherited=getpriority(PRIO_PROCESS,0);
  for(int round=0;round<3;++round) {
    frames=round==1?40000:16384; request=round==2?2:1;
    {std::lock_guard<std::mutex> lk(testmu); entered=false; release_run=false;}
    kick();
    {std::unique_lock<std::mutex> lk(testmu); assert(testcv.wait_for(lk,std::chrono::seconds(5),[]{return entered;}));}
    assert(calls==round+1); assert(actual==(round==0?19:round==1?10:inherited));
    frames=70000; for(int i=0;i<100;++i)kick();
    assert(calls==round+1); // DONE publication must not release ownership early.
    {std::lock_guard<std::mutex> lk(testmu); release_run=true;} testcv.notify_all();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) { {std::lock_guard<std::mutex> lk(ck_->mu); if(!ck_->running)break;} assert(std::chrono::steady_clock::now()<deadline); std::this_thread::yield(); }
  }
  {std::lock_guard<std::mutex> lk(ck_->mu);ck_->stop=true;}ck_->cv.notify_all();
  a.join();b.join();c.join();assert(maxactive==1);assert(calls==3);
  puts("PASS: fixed 19/10/inherited nice dispatch; one owner; no in-flight redispatch; clean shutdown");
}
