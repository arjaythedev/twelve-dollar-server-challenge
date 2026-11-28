#!/usr/bin/env python3
"""Compile the real response/commit methods against a tiny deterministic harness.
No server, SQLite database, or production process is started. Optional --source
allows running the same regression against a pre-fix main.cpp snapshot.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def method(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    pos = opening + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'main.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    methods = '\n'.join(method(source, signature) for signature in
                        ('  void flush_response(', '  void commit_and_flush('))
    harness = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace app {
struct ByteBuf { uint32_t len = 0; std::string text; };
void error(ByteBuf& b, int, const char*) { b.text = "500"; b.len = 3; }
}
namespace net {
enum class WriteState { Idle, Sending, Flushed };
struct Connection {
 bool answered=false, close_after=false, wrote_db=false, in_batch=false;
 bool open=true, closing=false, commit_pending=false;
 uint16_t gen=1; uint32_t sent=0;
 WriteState write=WriteState::Idle;
 app::ByteBuf *out=nullptr, *wire=nullptr;
};
}
struct Worker {
 std::vector<net::Connection> conns=std::vector<net::Connection>(4);
 std::vector<uint32_t> dirty_;
 auto& table() { return conns; }
 auto& dirty() { return dirty_; }
 uint32_t index() { return 0; }
};
struct Db { bool ok=true; int commits=0; bool commit() { ++commits; return ok; } };
struct App { bool wrote=false; Db db; };
struct ReadyRef { uint32_t wi, idx; uint16_t gen; uint64_t ts; };
struct Server {
 App owned; App* app_=&owned;
 std::vector<std::unique_ptr<Worker>> workers_;
 std::vector<ReadyRef> pend_commit_;
 uint64_t now=110, last_commit_ns_=100, commit_window_ns_=20, responded_=0;
 std::vector<std::string> sent;
 Server() { workers_.push_back(std::make_unique<Worker>()); }
 uint64_t now_ns() { return now; }
 app::ByteBuf& out_of(net::Connection& c) { assert(c.out); return *c.out; }
 void arm_send(Worker&, uint32_t, uint32_t, net::Connection& c) {
   sent.push_back(c.wire->text); c.write=net::WriteState::Sending;
 }
 void arm_close(Worker&, uint32_t, uint32_t, net::Connection&) {}
 void response(unsigned idx, app::ByteBuf& b, bool wrote=false) {
   auto& c=workers_[0]->conns[idx]; c.out=&b; c.answered=true;
   c.in_batch=true; c.wrote_db=wrote; workers_[0]->dirty_.push_back(idx);
 }
METHODS
};
int main() {
 // A writes; B is an old ready request answered by this pass. Both must wait.
 {
  Server s; app::ByteBuf a{3,"201"}, b{3,"dirty-read"};
  s.app_->wrote=true; s.response(0,a,true); s.response(1,b);
  s.commit_and_flush();
  assert(s.sent.empty() && "cross-client read escaped before COMMIT");
  assert(s.pend_commit_.size()==2);
  // Another pass's C read shares the still-open transaction.
  app::ByteBuf c{3,"later-dirty-read"}; s.response(2,c);
  s.now=115; s.commit_and_flush();
  assert(s.sent.empty() && s.pend_commit_.size()==3);
  s.now=121; s.commit_and_flush();
  assert(s.app_->db.commits==1 && s.sent.size()==3);
  assert(s.pend_commit_.empty() && !s.app_->wrote);
 }
 // COMMIT failure must invalidate writer AND all dependent readers.
 {
  Server s; app::ByteBuf a{3,"201"}, b{3,"dirty-read"};
  s.app_->wrote=true; s.response(0,a,true); s.commit_and_flush();
  s.response(1,b); s.now=115; s.commit_and_flush();
  assert(s.sent.empty()); s.app_->db.ok=false; s.now=121; s.commit_and_flush();
  assert(s.sent.size()==2 && s.sent[0]=="500" && s.sent[1]=="500");
  assert(s.workers_[0]->conns[0].close_after && s.workers_[0]->conns[1].close_after);
 }
 // A true read-only transaction should not acquire a time-window delay.
 {
  Server s; app::ByteBuf b{3,"200"}; s.response(1,b); s.commit_and_flush();
  assert(s.sent.size()==1 && s.app_->db.commits==0 && s.pend_commit_.empty());
 }
 // Slot reuse must not send the previous generation's pending response.
 {
  Server s; app::ByteBuf a{3,"201"}; s.app_->wrote=true; s.response(0,a,true);
  s.commit_and_flush(); ++s.workers_[0]->conns[0].gen;
  s.now=121; s.commit_and_flush(); assert(s.sent.empty());
 }
}
'''.replace('METHODS', methods)
    with tempfile.TemporaryDirectory(prefix='commit-visibility-') as directory:
        cpp = Path(directory) / 'check.cpp'
        exe = Path(directory) / 'check'
        cpp.write_text(harness)
        subprocess.run(['g++', '-std=c++20', '-O0', '-Wall', '-Wextra', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('PASS: real commit/flush methods preserve transaction response visibility')


if __name__ == '__main__':
    main()
