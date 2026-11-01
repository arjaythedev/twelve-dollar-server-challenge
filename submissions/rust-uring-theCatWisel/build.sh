#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/bin:${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor bin

if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  if [ -f "/tmp/sqlite-cache/$SQLITE.zip" ]; then
    cp "/tmp/sqlite-cache/$SQLITE.zip" "vendor/$SQLITE.zip"
  else
    curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  fi
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
fi

CC=${CC:-gcc}
command -v "${CC%% *}" >/dev/null 2>&1 || CC=gcc
export CC

ARCH_FLAGS="-march=native -mtune=native -mprefer-vector-width=256"
case "$(uname -m)" in
  aarch64|arm64) ARCH_FLAGS="-mcpu=native" ;;
esac

SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_COMPLETE
  -DSQLITE_OMIT_JSON -DSQLITE_TEMP_STORE=3 -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DNDEBUG"

CFLAGS="-std=gnu11 -O3 $ARCH_FLAGS -fno-plt -fno-semantic-interposition
  -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE
  -fomit-frame-pointer -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
  -I vendor/$SQLITE -I csrc $SQLOPTS"

PGO_DIR="$(pwd)/pgo"
export RUSTFLAGS="-C target-cpu=native -C opt-level=3 -C codegen-units=1 -C panic=abort"

if [ "${PGO:-1}" = "1" ]; then
  echo "== stage 1: PGO instrumented build"
  rm -rf "$PGO_DIR" && mkdir -p "$PGO_DIR"
  $CC $CFLAGS -fprofile-generate -fprofile-update=single -fprofile-dir="$PGO_DIR" \
    -c csrc/sqlite_engine.c -o vendor/sqlite_engine.o
  ar rcs vendor/libsqlite_engine.a vendor/sqlite_engine.o
  rm -f target/release/rust-uring-thecatwisel target/release/deps/rust_uring_thecatwisel*
  PGO_STAGE=gen cargo build --release --locked
  cp target/release/rust-uring-thecatwisel bin/server.inst

  echo "== stage 1b: PGO training run"
  if bin/server.inst --pgo-train; then
    echo "== stage 2: PGO optimized build"
    $CC $CFLAGS -fprofile-use -fprofile-dir="$PGO_DIR" -fprofile-correction \
      -fprofile-partial-training -freorder-blocks-and-partition -Wno-missing-profile \
      -c csrc/sqlite_engine.c -o vendor/sqlite_engine.o
    ar rcs vendor/libsqlite_engine.a vendor/sqlite_engine.o
    rm -f target/release/rust-uring-thecatwisel target/release/deps/rust_uring_thecatwisel*
    PGO_STAGE=use cargo build --release --locked
  else
    echo "PGO training skipped; building standard -O3" >&2
    $CC $CFLAGS -c csrc/sqlite_engine.c -o vendor/sqlite_engine.o
    ar rcs vendor/libsqlite_engine.a vendor/sqlite_engine.o
    rm -f target/release/rust-uring-thecatwisel target/release/deps/rust_uring_thecatwisel*
    PGO_STAGE=none cargo build --release --locked
  fi
  rm -f bin/server.inst
else
  $CC $CFLAGS -c csrc/sqlite_engine.c -o vendor/sqlite_engine.o
  ar rcs vendor/libsqlite_engine.a vendor/sqlite_engine.o
  PGO_STAGE=none cargo build --release --locked
fi

cp target/release/rust-uring-thecatwisel bin/server
echo "built bin/server ($(stat -c %s bin/server) bytes)"
