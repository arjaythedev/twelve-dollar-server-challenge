#!/usr/bin/env python3
"""Exercise the real pass's ready/drain order under sustained fresh completions."""
from pathlib import Path
import subprocess
import sys
import tempfile
src = (Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[1]/'main.cpp').read_text()
a=src.index('      last_cqes = 0;', src.index('void run()'))
b=src.index('      const uint64_t p2', a)
src=src[a:b]
# Keep both calls and the drain loop in their real order; discard commentary.
lines=[]
for line in src.splitlines():
    line=line.strip()
    if line == 'run_ready();': lines.append(line)
    elif line == 'for (size_t i = 0; i < workers_.size(); i++)': lines.append(line)
    elif 'last_cqes += drain(' in line: lines.append(line)
assert len(lines) in (3,4), lines
code=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
constexpr size_t kDrainBudget=2048;
struct Test {
 std::vector<int*> workers_{nullptr,nullptr};
 int answers_left_=2, old=5, answered_old=0, drains=0, last_cqes=0;
 void run_ready() { while(old && answers_left_) {--old; --answers_left_; ++answered_old;} }
 int drain(int*, uint32_t, size_t) { ++drains; answers_left_=0; return 256; }
 void pass() { BODY }
};
int main() {
 Test t;
 for(int i=0;i<3;++i) { t.answers_left_=2; t.pass(); }
 assert(t.answered_old==5); // continuous fresh CQEs must not starve old work
 assert(t.drains==6);      // receive buffers must still be reclaimed with zero answer quota
}
'''.replace('BODY','\n'.join(lines)).replace('drain(*workers_[i]', 'drain(workers_[i]')
with tempfile.TemporaryDirectory(prefix='ready-priority-') as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++23','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: old ready work gets quota under sustained new CQEs; all rings still drained')
