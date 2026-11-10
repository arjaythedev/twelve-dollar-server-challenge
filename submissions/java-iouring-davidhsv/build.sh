#!/usr/bin/env bash
# Builds the server as a normal user: fetches the pinned SQLite amalgamation (sha256-checked),
# builds bin/libfeed.so from net.c and db.c, compiles the Java sources, and links bin/server to
# the JDK's java launcher (so the process is named "server").
set -euo pipefail
cd "$(dirname "$0")"

java_home=${JAVA_HOME:-/opt/jdk-27.0.1}
[[ -x $java_home/bin/javac ]] || { echo "no JDK at $java_home: run install.sh (or set JAVA_HOME)" >&2; exit 1; }

mkdir -p .deps .build bin
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [[ ! -f .deps/sqlite3.c ]]; then
  curl -fsSL --retry 3 -o .deps/$SQ.zip "https://sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  .deps/$SQ.zip" | sha256sum -c -
  unzip -o -j -q .deps/$SQ.zip -d .deps "$SQ/sqlite3.c" "$SQ/sqlite3.h"
fi

rm -rf .build/classes
"$java_home/bin/javac" --release 27 -proc:none -Xlint:all,-restricted,-incubating -Werror --add-modules jdk.incubator.vector -d .build/classes src/*.java
"$java_home/bin/jar" --create --file bin/server.jar -C .build/classes .
ln -sfn "$java_home/bin/java" bin/server

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
# none of them is needed on this path. Only the functions feed.h marks are exported.
common=(-O3 "${arch[@]}" -DNDEBUG -pthread -fPIC -fvisibility=hidden -fno-plt -fno-semantic-interposition
        -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE -fomit-frame-pointer
        -I.deps -I. ${EXTRA_CFLAGS:-})
sqlite_defs=(
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE
  -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1
)
# Link-time optimization: one optimizer sees the two C files and SQLite together.
# Profile-guided: an instrumented library serves train/train.py first (a synthetic database and
# tokens of its own, the load test's request mix), and the real build uses what it measured.
# PGO=0 skips the training.
build() { # build <extra flags...>
  "$cc" "${common[@]}" -flto "$@" -c net.c -o .build/net.o
  "$cc" "${common[@]}" -flto "$@" "${sqlite_defs[@]}" -c db.c -o .build/db.o
  "$cc" "${common[@]}" -flto=2 -Wno-stringop-overread "$@" -shared -Wl,-z,now -Wl,-O1 .build/net.o .build/db.o \
    -lm -pthread -o bin/libfeed.so
}
if [[ ${PGO:-1} == 1 ]]; then
  profile=$PWD/.build/profile
  rm -rf "$profile"
  build -fprofile-generate="$profile" -fprofile-update=prefer-atomic
  if JAVA_HOME=$java_home python3 train/train.py ./start.sh; then
    build -fprofile-use="$profile" -fprofile-correction -fprofile-partial-training -Wno-missing-profile
  else
    echo 'profile training failed: building without a profile' >&2
    build
  fi
else
  build
fi
"$java_home/bin/java" -version 2>&1 | sed -n 1p
"$cc" --version | sed -n 1p
ls -la bin/libfeed.so
