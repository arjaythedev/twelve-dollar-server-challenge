#!/usr/bin/env python3
import re
import subprocess
import sys
import tempfile
from pathlib import Path

src_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'main.cpp'
src = src_path.read_text(encoding='utf-8')
a = src.index('  void retry_held() {')
b = src.index('\n  }', a) + len('\n  }')
retry = src[a:b]
# Extract the budget-reset and replay operations in their event-loop order.
a = src.index('      writes_ok_ = !app_->db.writes_held();')
b = src.index('      // 先处理', a)
ops = '\n'.join(line.strip() for line in src[a:b].splitlines()
                if line.strip() in ('if (writes_ok_) retry_held();', 'answers_left_ = answers_cap_;'))
assert ops.count('retry_held') == 1 and ops.count('answers_cap_') == 1
held_type = re.search(r'(std::deque<[^&]+>)& held\(\)', src).group(1)
enqueue = next(line.strip() for line in src.splitlines() if 'w.held().' in line and ('push_back' in line or 'emplace_back' in line))
cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <utility>
#include <vector>
constexpr size_t kHeldPerPass = 1024;
namespace net {
struct Connection { uint16_t gen = 1; bool held = true, open = true, closing = false; };
}
struct Worker {
  QUEUE queue;
  std::vector<net::Connection> slots = std::vector<net::Connection>(4);
  auto& held() { return queue; }
  auto& table() { return slots; }
  void enqueue(uint32_t idx) { auto& w = *this; auto& c = slots.at(idx); (void)c; ENQUEUE }
};
struct Server {
  bool writes_ok_ = true;
  uint32_t answers_left_ = 0, answers_cap_ = 2;
  size_t held_ring_ = 0;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<uint32_t> executed;
  void serve(Worker&, uint32_t wi, uint32_t idx, const void*, size_t) {
    if (answers_left_) { --answers_left_; executed.push_back(wi * 10 + idx); }
  }
  RETRY
  void pass() { PASS }
};
int main() {
  Server s;
  s.workers_.push_back(std::make_unique<Worker>());
  s.workers_.push_back(std::make_unique<Worker>());
  for (uint32_t i = 0; i < 4; i++) s.workers_[0]->enqueue(i);
  s.workers_[1]->enqueue(0);
  s.workers_[1]->enqueue(1);
  ++s.workers_[1]->slots[0].gen;  // stale queue entry, current connection stays held
  s.workers_[1]->enqueue(0);
  s.pass();
  assert((s.executed == std::vector<uint32_t>{0,1}));
  assert(s.answers_left_ == 0 && s.workers_[0]->held().size() == 2);
  s.pass();
  assert((s.executed == std::vector<uint32_t>{0,1,11,10}));
  assert(s.answers_left_ == 0 && s.workers_[1]->held().empty());
  s.pass();
  assert((s.executed == std::vector<uint32_t>{0,1,11,10,2,3}));
  s.pass();
  assert(s.answers_left_ == 2);
}
'''.replace('ENQUEUE', enqueue).replace('QUEUE', held_type).replace('RETRY', retry).replace('PASS', ops)
with tempfile.TemporaryDirectory(prefix='held-regression-') as tmp:
    p = Path(tmp)
    (p/'test.cpp').write_text(cpp)
    subprocess.run(['g++', '-std=c++23', '-Wall', '-Wextra', '-Werror', str(p/'test.cpp'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
print('Held replay budget, rollover and slot generations passed')
