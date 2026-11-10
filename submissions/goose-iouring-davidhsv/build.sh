#!/usr/bin/env bash
# Builds bin/server as a normal user: fetches the pinned SQLite amalgamation (sha256-checked),
# compiles server.goose to C with the pinned Goose compiler, and builds it with net.c and db.c.
set -euo pipefail
cd "$(dirname "$0")"

goose_bin=${GOOSE_BIN:-$(command -v goose || true)}
[[ -n $goose_bin && -x $goose_bin ]] || { echo 'goose not found: run install.sh (or set GOOSE_BIN)' >&2; exit 1; }
goose_bin=$(readlink -f "$goose_bin")
goose_src=$(cd "$(dirname "$goose_bin")/.." && pwd)
[[ -f $goose_src/src/runtime/goose.h ]] || { echo 'the Goose source tree must sit beside its build directory' >&2; exit 1; }

mkdir -p .deps .build bin
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [[ ! -f .deps/sqlite3.c ]]; then
  curl -fsSL --retry 3 -o .deps/$SQ.zip "https://sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  .deps/$SQ.zip" | sha256sum -c -
  unzip -o -j -q .deps/$SQ.zip -d .deps "$SQ/sqlite3.c" "$SQ/sqlite3.h"
fi

"$goose_bin" -O2 --standalone -o .build/app.c server.goose
cp "$goose_src/src/runtime/goose.h" .build/goose.h

cc=${CC:-gcc}
# x86-64-v3 tuned for the box's Cascade Lake, with AVX-512 left alone: on Cascade Lake a 512-bit
# instruction lowers the core's clock for the code that follows. -mcpu=native elsewhere.
if [[ -n ${ARCH_FLAGS:-} ]]; then
  read -r -a arch <<< "$ARCH_FLAGS"
elif [[ $(uname -m) == x86_64 ]]; then
  arch=(-march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256 -fcf-protection=none)
else
  arch=(-mcpu=native -mbranch-protection=none)
fi
# Ubuntu's gcc turns on stack protectors, stack-clash probes, _FORTIFY_SOURCE and CET by default;
# none of them is needed on this path.
common=(-O3 "${arch[@]}" -DNDEBUG -pthread -fno-plt -fno-semantic-interposition
        -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE -fomit-frame-pointer
        -I.build -I.deps -I. ${EXTRA_CFLAGS:-})
sqlite_defs=(
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE
  -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1
)
# Link-time optimization: one optimizer sees Goose's code, the two C files and SQLite together.
# Profile-guided: an instrumented build serves train/train.py first (a synthetic database and
# tokens of its own, the load test's request mix), and the real build uses what it measured.
# PGO=0 skips the training.
build() { # build <extra flags...>
  "$cc" "${common[@]}" -flto "$@" -c .build/app.c -o .build/app.o
  "$cc" "${common[@]}" -flto "$@" -c net.c -o .build/net.o
  "$cc" "${common[@]}" -flto "$@" "${sqlite_defs[@]}" -c db.c -o .build/db.o
  "$cc" "${common[@]}" -flto=2 -Wno-stringop-overread "$@" .build/app.o .build/net.o .build/db.o "${extra[@]}" -lm -pthread -static -o bin/server
}
extra=()
if [[ ${PGO:-1} == 1 ]]; then
  profile=$PWD/.build/profile
  rm -rf "$profile"
  "$cc" "${common[@]}" -c train/pgo_exit.c -o .build/pgo_exit.o
  extra=(.build/pgo_exit.o)
  build -fprofile-generate="$profile" -fprofile-update=prefer-atomic
  extra=()
  if python3 train/train.py bin/server; then
    build -fprofile-use="$profile" -fprofile-correction -fprofile-partial-training -Wno-missing-profile
  else
    echo 'profile training failed: building without a profile' >&2
    build
  fi
else
  build
fi
"$goose_bin" version
"$cc" --version | sed -n 1p
ls -la bin/server
