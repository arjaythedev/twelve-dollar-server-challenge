#!/usr/bin/env bash
# Runs as a normal user. Downloads SQLite 3.53.2 and compiles it with render.c (which #includes it) into bin/ for the FFI driver with
# profile-guided optimization (bench/pgo.ts is the training run), then type-checks the entrypoint.
set -euo pipefail
cd "$(dirname "$0")"
# SQLite 3.53.2 from sqlite.org, pinned by the sha256 of the sqlite3.c inside the tarball.
SQLITE=sqlite-autoconf-3530200 SQLITE_SHA256=0a409f1633283fa31a9126b11fbfd64a1991c5d30defad07e5745d4667f5e23d
sha256() { { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | cut -d' ' -f1; }
if [ ! -f vendor/sqlite3.c ] || [ "$(sha256 vendor/sqlite3.c)" != "$SQLITE_SHA256" ]; then
  mkdir -p vendor
  curl -fsSL --retry 3 "https://www.sqlite.org/2026/$SQLITE.tar.gz" | tar -xzO "$SQLITE/sqlite3.c" > vendor/sqlite3.c
  [ "$(sha256 vendor/sqlite3.c)" = "$SQLITE_SHA256" ] || { rm -f vendor/sqlite3.c; echo "sqlite3.c sha256 mismatch"; exit 1; }
fi
mkdir -p bin
case "$(uname -s)" in Darwin) LIB=bin/libsqlite3.dylib CPU=-mcpu=native LINK= ;; *) LIB=bin/libsqlite3.so CPU=-march=native LINK=-lpthread ;; esac
FLAGS=(-O3 "$CPU" -fPIC
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_DQS=0 -DSQLITE_USE_ALLOCA
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION)
# A fixed object path, so gcc finds its .gcda again on the second compile.
compile() { cc "${FLAGS[@]}" "$@" -c -o bin/sqlite3.o render.c && cc -shared "$@" -o "$LIB" bin/sqlite3.o $LINK; }
train() { deno run --allow-ffi --allow-env --allow-read --allow-write bench/pgo.ts; }

PROF="$(mktemp -d)"
trap 'rm -rf "$PROF" bin/sqlite3.o' EXIT
if cc --version | grep -q clang; then
  compile -fprofile-instr-generate
  LLVM_PROFILE_FILE="$PROF/%p.profraw" train
  $(xcrun -f llvm-profdata 2>/dev/null || echo llvm-profdata) merge -o "$PROF/sqlite.profdata" "$PROF"/*.profraw
  compile -fprofile-instr-use="$PROF/sqlite.profdata"
else
  compile -fprofile-generate="$PROF"
  train
  compile -fprofile-use="$PROF" -fprofile-partial-training -Wno-missing-profile
fi
# Linux: the Rust network layer (poller.rs, epoll) that server.ts uses instead of Deno.listen. rustc from apt, no cargo.
[ "$(uname -s)" = Linux ] && rustc --edition 2021 -O -C target-cpu=native -C panic=abort -C strip=symbols \
  --crate-type cdylib -o bin/libpoller.so poller.rs
deno check server.ts
