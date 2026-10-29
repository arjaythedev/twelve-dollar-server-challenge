#!/usr/bin/env bash
# Builds target/release/feed-server. SQLite 3.54.1 (official amalgamation, SHA-256 pinned) is compiled into
# deps/libsqlite3.a and linked statically; Rust dependencies are pinned by Cargo.lock.
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p deps
if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi
SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1"
MARCH=; [ "$(uname -m)" != x86_64 ] || MARCH=x86-64-v2  # the benchmark box is x86-64
if [ ! -f deps/libsqlite3.a ]; then
  gcc -O2 ${MARCH:+-march=$MARCH} -fPIC -w $SQLITE_FLAGS -c deps/$SQLITE/sqlite3.c -o deps/sqlite3.o
  ar rcs deps/libsqlite3.a deps/sqlite3.o
fi
export RUSTUP_HOME=/opt/rust CARGO_HOME="$PWD/deps/cargo-home" PATH="/opt/rust/cargo/bin:$PATH"
RUSTFLAGS="${MARCH:+-C target-cpu=$MARCH}" cargo build --release --locked
