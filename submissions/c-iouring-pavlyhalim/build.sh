#!/usr/bin/env bash
# Builds bin/server as a normal user: downloads the pinned SQLite amalgamation (SHA-256 checked)
# and compiles it together with the server as one translation unit.
#
#   PGO=0 ./build.sh     plain -O3 build (default: profile-guided, about two extra minutes)
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor bin
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
fi

CC=${CC:-gcc}
ARCH=${ARCH:--march=x86-64-v3 -mtune=cascadelake}
case "$(uname -m)" in aarch64|arm64) ARCH=${ARCH_ARM:--mcpu=native} ;; esac
# SQLite: no mutexes on the request path (THREADSAFE=2 + OPEN_NOMUTEX: the checkpoint thread has
# its own connection), no memory statistics, no progress-callback check per opcode, nothing unused.
SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_COMPLETE
  -DSQLITE_OMIT_JSON -DSQLITE_TEMP_STORE=3 -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DNDEBUG"
CFLAGS="-std=gnu11 -O3 -g $ARCH -mprefer-vector-width=256 -fno-plt -fno-semantic-interposition
  -fno-stack-protector -fcf-protection=none -fno-stack-clash-protection -U_FORTIFY_SOURCE
  -fomit-frame-pointer -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -I vendor/$SQLITE -I src $SQLOPTS"
case "$(uname -m)" in aarch64|arm64) CFLAGS=${CFLAGS//-mprefer-vector-width=256/}; CFLAGS=${CFLAGS//-fcf-protection=none/} ;; esac
LDFLAGS="-static -lpthread -lm"

build() { $CC $CFLAGS "$@" src/server.c -o "$1.tmp" $LDFLAGS 2>&1 | grep -v '^$' || true; }

if [ "${PGO:-1}" = 1 ] && [ -f src/train.sh ]; then
  echo "== instrumented build"
  rm -rf pgo && mkdir -p pgo
  $CC $CFLAGS -fprofile-generate -fprofile-update=single -fprofile-dir=pgo src/server.c -o bin/server.inst $LDFLAGS
  echo "== training"
  if bash src/train.sh bin/server.inst; then
    echo "== optimized build"
    $CC $CFLAGS -fprofile-use -fprofile-dir=pgo -fprofile-partial-training -Wno-missing-profile src/server.c -o bin/server.new $LDFLAGS
  else
    echo "training failed; plain build" >&2
    $CC $CFLAGS src/server.c -o bin/server.new $LDFLAGS
  fi
  rm -f bin/server.inst
else
  $CC $CFLAGS src/server.c -o bin/server.new $LDFLAGS
fi
mv bin/server.new bin/server
echo "built bin/server ($(stat -c %s bin/server 2>/dev/null || stat -f %z bin/server) bytes)"
