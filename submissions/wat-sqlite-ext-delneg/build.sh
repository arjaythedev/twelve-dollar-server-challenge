#!/usr/bin/env bash
# Builds as a normal user into ./build (toolchain from install.sh in /opt/wat-sqx):
#   1. the stock sqlite3 shell from the SQLite 3.54.1 amalgamation (pinned sha256): shell.c + sqlite3.c, unmodified
#   2. src/serve.s -> serve.so (as + ld; links libwasmedge), the extension the shell loads
#   3. src/app.wat -> app.wasm (wat2wasm) -> app.so (`wasmedge compile`, AOT native code)
set -euo pipefail
cd "$(dirname "$0")"
T=${WAT_SQX_TOOLS:-/opt/wat-sqx}; WE=$T/wasmedge
SQ=sqlite-amalgamation-3540100; SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p build; cd build
if [ ! -f $SQ/sqlite3.c ]; then
  curl -fsSL https://www.sqlite.org/2026/$SQ.zip -o sqlite.zip
  echo "$SQ_SHA  sqlite.zip" | sha256sum -c - >/dev/null
  unzip -qo sqlite.zip && rm sqlite.zip
fi
# compile-time options only (the source is untouched): one thread uses the connection, so no mutexes
[ -x sqlite3 ] || gcc -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -o sqlite3 $SQ/shell.c $SQ/sqlite3.c -lm
as -o serve.o ../src/serve.s
ld -shared -o serve.so serve.o -L$WE/lib -lwasmedge -rpath $WE/lib
$T/wabt/bin/wat2wasm ../src/app.wat -o app.wasm
LD_LIBRARY_PATH=$WE/lib $WE/bin/wasmedge compile --optimize=3 app.wasm app.so >/dev/null
echo "built build/sqlite3, build/serve.so and build/app.so"
