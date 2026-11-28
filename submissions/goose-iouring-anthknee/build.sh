#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mode=${1:-build}
if [[ $# -gt 1 || ($mode != build && $mode != --emit-c) ]]; then
  echo 'Usage: bash build.sh [--emit-c]' >&2
  exit 2
fi
goose=${GOOSE_BIN:-/opt/goose-anthknee-2026.4-r1/build/goose}
if [[ ! -x $goose ]]; then echo 'Run install.sh, or set GOOSE_BIN to Goose 2026.4.' >&2; exit 1; fi
if [[ $("$goose" version) != 'goose v2026.4 '* ]]; then echo 'Goose 2026.4 is required.' >&2; exit 1; fi
goose_src=${GOOSE_SRC:-$(cd "$(dirname "$goose")/.." && pwd)}
if [[ ! -f $goose_src/src/runtime/goose.h ]]; then echo 'Set GOOSE_SRC to the pinned Goose 2026.4 source tree.' >&2; exit 1; fi
export GOOSE_HOME="$PWD/.work/goose-home"
mkdir -p .work/build .work/deps
"$goose" check server.goose
"$goose" -O2 --standalone -o .work/build/server.c server.goose
cp "$goose_src/src/runtime/goose.h" .work/build/goose.h
if [[ $mode == --emit-c ]]; then exit 0; fi
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo 'The native server targets Ubuntu 24.04 x86_64. --emit-c works on other hosts.' >&2
  exit 1
fi
# GCC 13 rejects this pragma on redeclared inline functions. The build sets the same flag below.
sed -i '/^#pragma GCC optimize("fp-contract=off")$/d' .work/build/server.c

fetch() {
  local url=$1 sha=$2 destination=$3
  if [[ -f $destination ]]; then
    printf '%s  %s\n' "$sha" "$destination" | sha256sum -c -
    return
  fi
  local partial
  partial=$(mktemp "${destination}.XXXXXX")
  if ! curl -fsSL --connect-timeout 30 --max-time 300 --retry 3 "$url" -o "$partial" ||
     ! printf '%s  %s\n' "$sha" "$partial" | sha256sum -c -; then
    rm -f "$partial"
    return 1
  fi
  mv "$partial" "$destination"
}
fetch https://sqlite.org/2026/sqlite-amalgamation-3540100.zip \
  0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9 .work/deps/sqlite.zip
unzip -o -j -q .work/deps/sqlite.zip -d .work/deps '*/sqlite3.c' '*/sqlite3.h'
fetch https://github.com/axboe/liburing/archive/refs/tags/liburing-2.15.tar.gz \
  8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a .work/deps/liburing.tar.gz
if [[ ! -f .work/deps/liburing/src/liburing.a ]]; then
  mkdir -p .work/deps/liburing
  tar -xzf .work/deps/liburing.tar.gz -C .work/deps/liburing --strip-components=1
  (cd .work/deps/liburing && ./configure && make -C src -j1 liburing.a) > .work/deps/liburing-build.log 2>&1
fi

cc=${CC:-gcc}
common=(-std=gnu11 -O3 -march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256 -ffp-contract=off
        -flto=1 -flto-partition=one -pthread -D_GNU_SOURCE -UNDEBUG -fno-plt -fno-semantic-interposition
        -I.work/build -I.work/deps -I.work/deps/liburing/src/include)
warnings=(-Wall -Wextra -Werror)
generated_warnings=(-Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable)
sqlite_warnings=(-Wno-unused-function -Wno-unused-parameter)
sqlite_defs=(-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE
  -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1)
"$cc" "${common[@]}" "${warnings[@]}" "${generated_warnings[@]}" -c .work/build/server.c -o .work/build/server.o
"$cc" "${common[@]}" "${warnings[@]}" "${sqlite_warnings[@]}" "${sqlite_defs[@]}" -c native/sqlite.c -o .work/build/sqlite.o
"$cc" "${common[@]}" "${warnings[@]}" .work/build/server.o .work/build/sqlite.o \
  .work/deps/liburing/src/liburing.a -lm -o .work/build/server.next
mv .work/build/server.next .work/build/server
"$goose" version
"$cc" --version | sed -n '1p'
