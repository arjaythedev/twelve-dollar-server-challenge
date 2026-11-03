#!/usr/bin/env bash
# Builds the server as a normal user. Lean checks every proof while compiling Feed/ to C.
set -euo pipefail
cd "$(dirname "$0")"
LEAN_HOME="${LEAN_HOME:-/opt/lean-nightly-2026-10-30}"
export PATH="$LEAN_HOME/bin:$PATH"
SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor build bin
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
fi
LEAN_INC="$(lean --print-prefix)/include"
# lean.h needs FLT_EVAL_METHOD = 0, which gcc changes when the CPU has AVX512-FP16
NOFP16=""; echo "int x;" | cc -mno-avx512fp16 -x c -c -o /dev/null - 2>/dev/null && NOFP16="-mno-avx512fp16"
echo "== lake: check the proofs, compile Feed/ to C"
MODS="Main Feed.Bytes Feed.Json Feed.Sha256 Feed.Jwt Feed.Http Feed.Store Feed.Handler Feed.Server Feed.Spec Feed.Model Feed.Selftest"
lake build Feed $(for m in $MODS; do printf '%s:c ' "$m"; done)
LEAN_LIB="$(lean --print-prefix)/lib"
# gcc compiles both the C that Lean emitted and the C runtime (SQLite, storage, leaves, transport),
# profile-guided: an instrumented build runs a made-up workload (runtime/train.c), then the
# final build uses the profile. Any failure there falls back to the plain build.
CFLAGS="-O3 -march=native $NOFP16 -fno-plt -pthread"
SQLDEFS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS"
JOBS="$(nproc)"   # one compiler at a time on the 1-vCPU box: gcc -O3 on SQLite takes most of its memory
compile() {  # compile <extra flags> <binary>
  local pids="" objs="" m o n=0
  cc $CFLAGS $1 -c -o build/server.o $SQLDEFS -I"$LEAN_INC" -Iruntime -I"vendor/$SQLITE" runtime/server.c &
  pids="$!"; n=1
  for m in $MODS; do
    if [ "$n" -ge "$JOBS" ]; then for p in $pids; do wait "$p" || return 1; done; pids=""; n=0; fi
    o="build/ir-$m.o"; objs="$objs $o"
    cc $CFLAGS $1 -w -DNDEBUG -DLEAN_EXPORTING -ffp-contract=off -c -o "$o" -I"$LEAN_INC" ".lake/build/ir/${m//.//}.c" &
    pids="$pids $!"; n=$((n + 1))
  done
  for p in $pids; do wait "$p" || return 1; done
  cc $CFLAGS $1 -o "$2" $objs build/server.o -L"$LEAN_LIB/lean" -Wl,--start-group -lInit -lleanrt -Wl,--end-group -lcadical \
    "$LEAN_LIB/libc++.a" "$LEAN_LIB/libc++abi.a" "$LEAN_LIB/libunwind.a" "$LEAN_LIB/libgmp.a" "$LEAN_LIB/libuv.a" \
    -lpthread -ldl -lrt -lm
}
PGO="$PWD/build/pgo"
pgo() {
  rm -rf "$PGO" build/train && mkdir -p "$PGO" build/train
  echo "== cc: instrumented build"
  compile "-fprofile-generate -fprofile-update=prefer-atomic -fprofile-dir=$PGO" bin/server-train || return 1
  echo "== training run"
  bin/server-train --train build/train || return 1
  echo "== cc: profile-guided build"
  compile "-fprofile-use -fprofile-partial-training -fprofile-dir=$PGO -Wno-missing-profile -Wno-error=coverage-mismatch" bin/server
}
if [ "${PGO_BUILD:-1}" = 1 ] && pgo; then :; else
  echo "== cc: plain build"
  compile "" bin/server
fi
rm -rf bin/server-train build/train

echo "== self-test: the C leaves and storage operations against their Lean definitions"
rm -f build/selftest.db*
bin/server --selftest build/selftest.db
rm -f build/selftest.db*
echo "built bin/server"
