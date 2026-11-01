#!/usr/bin/env bash
# baseline builds on opt-box: cserve.c native (epoll) and C->wasm on the WAT entry's host -> ~/watlab/out/cbase/
set -euo pipefail
W=~/watlab; WS=$W/opt/wasi-sdk-34.0-x86_64-linux; S=$W/opt/sqlite-amalgamation-3540100; WE=$W/opt/wasmedge
O=$W/out/cbase; mkdir -p $O; cd $O; C=$W/sub/experiments/baseline/cserve.c
F="-O2 -D_GNU_SOURCE -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION -I$S"
[ -f native.o ] || gcc $F -c -o native.o $S/sqlite3.c
gcc $F -o native $C native.o -lm
$WS/bin/clang --target=wasm32-wasip1 $F -DWASMEDGE_SOCKET -Wl,-z,stack-size=1048576 -o c.wasm $C $W/out/sqlite3.o
$WE/bin/wasmedge compile --optimize=3 c.wasm c.so >/dev/null
ls -la native c.so | awk '{print $5, $9}'
