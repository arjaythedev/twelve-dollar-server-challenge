#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ $(zig version) == 0.17.0 ]] || { echo 'Zig 0.17.0 required; run install.sh' >&2; exit 1; }
mkdir -p .deps bin
fetch() {
  [[ -f $2 ]] || curl -fsSL --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | sha256sum -c -
}
fetch https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip .deps/sqlite.zip \
  1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
fetch https://codeload.github.com/uNetworking/uSockets/tar.gz/86097c490263ab662d62e8e7b541390bdec7d149 .deps/usockets.tar.gz \
  0d341b94157720d9081d47348a8cba87ae350b6607c2f7d2ccf102353cbda553
fetch https://codeload.github.com/h2o/picohttpparser/tar.gz/465a7ff09fbd3432fe56c673451f5460154d1f07 .deps/pico.tar.gz \
  d65d80b362fcb11f6e982ebc4515442d93b937b166558120031c38a8b6c173cd
[[ -f .deps/sqlite-amalgamation-3530400/sqlite3.c ]] || unzip -q .deps/sqlite.zip -d .deps
for name in usockets pico; do
  if [[ ! -f .deps/$name/.extracted ]]; then
    mkdir -p ".deps/$name"
    tar -xzf ".deps/$name.tar.gz" --strip-components=1 -C ".deps/$name"
    touch ".deps/$name/.extracted"
  fi
done
sqlite=.deps/sqlite-amalgamation-3530400
if [[ ! -f .deps/sqlite3.o || build.sh -nt .deps/sqlite3.o ]]; then
  cc -O3 -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED \
    -c "$sqlite/sqlite3.c" -o .deps/sqlite3.o
fi
if [[ ! -f .deps/usockets/uSockets.a || build.sh -nt .deps/usockets/uSockets.a ]]; then
  (cd .deps/usockets
    cc -O3 -DNDEBUG -std=gnu11 -pthread -DLIBUS_NO_SSL -Isrc -c src/*.c src/eventing/*.c src/crypto/*.c src/io_uring/*.c
    ar rcs uSockets.a ./*.o)
fi
cc -O3 -DNDEBUG -c .deps/pico/picohttpparser.c -o .deps/pico.o
zig translate-c -lc -I"$sqlite" -I.deps/pico -I.deps/usockets/src src/c.h > .deps/c.zig
zig build-exe -O "${ZIG_OPTIMIZE:-ReleaseFast}" -lc -lm \
  .deps/sqlite3.o .deps/pico.o .deps/usockets/uSockets.a \
  --dep c -Mroot=src/main.zig -Mc=.deps/c.zig -femit-bin=bin/server "$@"
