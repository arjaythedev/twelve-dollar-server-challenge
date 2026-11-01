#!/usr/bin/env bash
# dev build on opt-box: ~/watlab/sub (rsynced submission) -> ~/watlab/out/{app.so,host}
set -euo pipefail
W=~/watlab; WS=$W/opt/wasi-sdk-34.0-x86_64-linux; S=$W/opt/sqlite-amalgamation-3540100
B=$W/opt/binaryen-version_125/bin; X=$W/opt/wabt-1.0.39/bin; WE=$W/opt/wasmedge
SRC=$W/sub/src; O=${OUT:-$W/out}; WAT=${WAT:-$SRC/app.wat}; mkdir -p $O; cd $O
F="-O2 -D_GNU_SOURCE -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION ${SQFLAGS:-}"
if [ ! -f sqlite3.o ]; then
  if [ -z "${SQFLAGS:-}" ] && [ -f $W/out/sqlite3.o ]; then cp $W/out/sqlite3.o .
  else $WS/bin/clang --target=wasm32-wasip1 $F -I$S -c -o sqlite3.o $S/sqlite3.c; fi
fi
$WS/bin/clang --target=wasm32-wasip1 $F -I$S -mexec-model=reactor -Wl,-z,stack-size=1048576 -Wl,--global-base=16777216 \
  -o sq.wasm $SRC/sqlite_glue.c sqlite3.o
$X/wat2wasm --enable-all $WAT -o app.wasm
$B/wasm-merge --enable-simd --enable-bulk-memory sq.wasm sq app.wasm app -o merged.wasm
$WE/bin/wasmedge compile --optimize=3 merged.wasm app.so >/dev/null
gcc -O2 -o host $SRC/host.c -I$WE/include -L$WE/lib -lwasmedge -Wl,-rpath,$WE/lib
ls -la app.so host | awk '{print $5, $9}'
