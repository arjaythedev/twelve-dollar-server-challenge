#!/usr/bin/env bash
# Builds bin/server as a normal user: fetches and verifies the pinned SQLite 3.54.1
# amalgamation, runs SHA-256/HMAC self-tests, records an on-box PGO profile over a
# synthetic database (Rule 10), and builds the final LTO+PGO static binary.
#
#   PGO=0 bash build.sh   plain build without PGO training
#   LTO=0 bash build.sh   disable LTO
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor bin
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --connect-timeout 20 --max-time 300 --retry 5 --retry-all-errors --retry-delay 5 \
    -o "vendor/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
  rm -f "vendor/$SQLITE.zip"
fi

CC=${CC:-gcc}
# Cascade Lake optimization without 512-bit vectors (which cause AVX-512 clock throttling).
ARCH=${ARCH:--march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256}
case "$(uname -m)" in aarch64|arm64) ARCH=${ARCH_ARM:--mcpu=native} ;; esac
SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_COMPLETE
  -DSQLITE_OMIT_JSON -DSQLITE_OMIT_FOREIGN_KEY -DSQLITE_OMIT_AUTOINCREMENT
  -DSQLITE_TEMP_STORE=3 -DHAVE_POSIX_FALLOCATE=1 -DHAVE_FDATASYNC=1
  -DHAVE_USLEEP=1 -DNDEBUG"
CFLAGS="-std=gnu11 -O3 $ARCH -fno-plt -fno-semantic-interposition -fno-stack-protector
  -fcf-protection=none -fno-stack-clash-protection -U_FORTIFY_SOURCE -fomit-frame-pointer
  -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-result
  -I vendor/$SQLITE -I src $SQLOPTS"
case "$(uname -m)" in aarch64|arm64) CFLAGS=${CFLAGS//-mprefer-vector-width=256/}; CFLAGS=${CFLAGS//-fcf-protection=none/} ;; esac
[ "${PROFILE:-0}" = 1 ] && CFLAGS="${CFLAGS//-fomit-frame-pointer/} -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
LDFLAGS="-static -lpthread -lm"
CFLAGS="$CFLAGS -DSHA256_ROLLING16=${SHA256_ROLLING16:-0}"
if [ "${LTO:-1}" = 1 ]; then
  CFLAGS="$CFLAGS -flto=auto"
  LDFLAGS="-flto=auto $LDFLAGS"
fi

# Verify SHA-256 and RFC 4231 HMAC-SHA256 vectors for both schedules.
for mode in 0 1; do
  $CC -std=gnu11 -O2 -Isrc -DSHA256_ROLLING16="$mode" src/test_sha256.c -o "bin/test-sha256-$mode"
  "bin/test-sha256-$mode"
  rm -f "bin/test-sha256-$mode"
done

$CC -std=gnu11 -O2 -I src -o bin/train src/train.c
if [ "${PGO:-1}" = 1 ]; then
  echo "== instrumented build"
  rm -rf pgo && mkdir -p pgo
  $CC $CFLAGS -fprofile-generate -fprofile-update=prefer-atomic -fprofile-dir=pgo -c src/server.c -o bin/server.o
  $CC -fprofile-generate -o bin/server.inst bin/server.o $LDFLAGS
  echo "== training"
  if bash src/train.sh bin/server.inst bin/train; then
    echo "== optimized build"
    $CC $CFLAGS -fprofile-use -fprofile-dir=pgo -fprofile-partial-training ${PGO_WARN:--Wno-missing-profile} -c src/server.c -o bin/server.o
  else
    echo "training failed; plain build" >&2
    $CC $CFLAGS -c src/server.c -o bin/server.o
  fi
  rm -f bin/server.inst
else
  $CC $CFLAGS -c src/server.c -o bin/server.o
fi
$CC -o bin/server.new bin/server.o $LDFLAGS
rm -f bin/server.o
mv bin/server.new bin/server
echo "built bin/server ($(stat -c %s bin/server) bytes)"
