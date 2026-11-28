#!/usr/bin/env python3
"""Run isolated data-path and checkpoint-dispatch regressions; never modify seed."""
import os, re, subprocess, tempfile
from pathlib import Path
S=Path(__file__).resolve().parents[1]; R=S.parents[1]
def run(args, **kw): subprocess.run(list(map(str,args)),check=True,**kw)
def function(src, signature):
 start=src.index(signature); a=src.index('{',start); depth=1; i=a+1
 while depth:
  depth+=(src[i]=='{')-(src[i]=='}'); i+=1
 return src[start:i]
with tempfile.TemporaryDirectory(prefix='cpp-data-test-') as tmp:
 B=Path(tmp); lua=(S/'xmake.lua').read_text(); defs=re.findall(r'"([A-Z_][A-Z_0-9]*(?:=[0-9]+)?)"',lua.split('add_defines(',1)[1].split(')',1)[0])
 run(['gcc','-std=c11','-O2','-march=native','-I'+str(S/'vendor')]+['-D'+x for x in defs]+['-c',S/'src/shim.c','-o',B/'shim.o'])
 common=['g++','-std=c++23','-O3','-march=native','-I'+str(S/'src')]
 run(common+[S/'tests/data_path.cpp',S/'src/db.cpp',S/'src/json.cpp',B/'shim.o','-lpthread','-lm','-o',B/'full'])
 run(common+[S/'tests/histogram_dump.cpp',B/'shim.o','-lpthread','-lm','-o',B/'histogram_dump'])
 run(['python3',S/'tests/check_data.py',B,R]); run(['python3',S/'tests/check_histogram.py',B,R])
 src=(S/'src/db.cpp').read_text(); h=(S/'src/db.hpp').read_text(); test=(S/'tests/checkpoint_dispatch.cpp').read_text()
 a=test.index('struct CkState'); z=test.index('struct dbx',a); state=function(h,'struct CkState')
 test=test[:a]+state+';'+chr(10)+test[z:]
 a=test.index('enum { kCkBulk'); z=test.index('int main()',a)
 loop=function(src,'void checkpoint_loop('); kick=function(src,'void Db::ck_kick()').replace('void Db::ck_kick()','void kick()')
 constants=chr(10).join(['enum { kCkBulk = 0, kCkUrgent = 1, kCkTail = 2 };','constexpr long long kCkBehindFrames = 32768;',''])
 test=test[:a]+constants+loop+chr(10)+kick+chr(10)+test[z:]
 (B/'dispatch.cpp').write_text(test); run(common+[B/'dispatch.cpp','-lpthread','-o',B/'dispatch']); env=os.environ.copy();env.pop('NO_CKPT',None);run([B/'dispatch'],env=env)
 print('ALL_DATA_AND_DISPATCH_TESTS_PASS',flush=True)
