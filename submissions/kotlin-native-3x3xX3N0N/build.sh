#!/usr/bin/env bash
# Builds build/server.kexe as a normal user, from source:
#   1. downloads the SQLite 3.54.1 amalgamation (SHA3-256 as published on sqlite.org) and liburing 2.9 (sha256),
#   2. compiles them and csrc/*.c with the clang + glibc sysroot that ship with Kotlin/Native (fetched by Kotlin/Native
#      itself into ~/.konan on first use), so the C objects match what the Kotlin/Native linker links against,
#   3. cinterop (kn.def -> klib) and kotlinc-native -opt.
#   4. Profile-guided C (KN_CPGO=1, default): steps 2-3 first build an instrumented server, which serves a synthetic
#      scratch database (train/mkdb.c, random rows, none of the seed) to a training client (train/train.c) on
#      127.0.0.1:KN_TRAIN_PORT (default 3999) for KN_TRAIN_REQS requests (~8 s); then the C is rebuilt with the
#      profile recorded HERE, during this build, and linked again. No profile is committed (LEADERBOARD ruling of
#      2026-11-15 on rule 10). Every step has a time limit; any failure falls back to the plain build.
# sqlite3.c is used unchanged; csrc/db.c #includes it so the B-tree read paths can use SQLite's cursor API.
# Measured peak memory: see README ("Build").
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
KN="${KN_HOME:-/opt/kotlin-native-2.4.21}"
[ -x "$KN/bin/kotlinc-native" ] || KN="$(dirname "$(dirname "$(command -v kotlinc-native)")")"
if [ -x /opt/jdk-25/bin/java ]; then export JAVA_HOME=/opt/jdk-25; fi
# Two malloc arenas and a capped heap keep the compiler well inside the box (measured: a cold build, toolchain download
# included, peaks at ~560 MB anonymous memory in a 1.5 GB no-swap cgroup; the rest of the cgroup peak is page cache).
export MALLOC_ARENA_MAX=2 JAVA_OPTS="${JAVA_OPTS:--Xmx800m -XX:+UseSerialGC}"

SQLITE_URL=https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
SQLITE_SHA3=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
URING_URL=https://github.com/axboe/liburing/archive/refs/tags/liburing-2.9.tar.gz
URING_SHA=897b1153b55543e8b92a5a3eb9b906537a5fedcf8afaf241f8b8787940c79f8d

B="$ROOT/build"; D="$B/deps"; C="$B/c"; P="$B/pgo"
mkdir -p "$D" "$C"
if [ ! -f "$D/sqlite/sqlite3.c" ]; then
  curl -fsSL --retry 3 "$SQLITE_URL" -o "$D/sqlite.zip"
  got=$(openssl dgst -sha3-256 -r "$D/sqlite.zip" | cut -d' ' -f1)
  [ "$got" = "$SQLITE_SHA3" ] || { echo "sqlite amalgamation: SHA3-256 $got, expected $SQLITE_SHA3" >&2; exit 1; }
  mkdir -p "$D/sqlite" && unzip -j -o -q "$D/sqlite.zip" '*/sqlite3.c' '*/sqlite3.h' -d "$D/sqlite" && rm "$D/sqlite.zip"
fi
if [ ! -f "$D/liburing/src/include/liburing.h" ]; then
  curl -fsSL --retry 3 "$URING_URL" -o "$D/liburing.tgz"
  echo "$URING_SHA  $D/liburing.tgz" | sha256sum -c -
  rm -rf "$D/liburing" && mkdir -p "$D/liburing" && tar -xzf "$D/liburing.tgz" -C "$D/liburing" --strip-components=1 && rm "$D/liburing.tgz"
fi

# Kotlin/Native's toolchain (clang, lld, the x86_64 glibc sysroot): a one-line cinterop run makes it download it.
KONAN="${KONAN_DATA_DIR:-$HOME/.konan}/dependencies"
if ! ls -d "$KONAN"/llvm-*-x86_64-linux-essentials-* >/dev/null 2>&1; then
  echo "headers = stdint.h" > "$B/probe.def"
  "$KN/bin/cinterop" -def "$B/probe.def" -o "$B/probe" >/dev/null
fi
LLVM="$(ls -d "$KONAN"/llvm-*-x86_64-linux-essentials-* | head -1)"
GT="$(ls -d "$KONAN"/x86_64-unknown-linux-gnu-gcc-* | head -1)"
CC="$LLVM/bin/clang --target=x86_64-unknown-linux-gnu --sysroot=$GT/x86_64-unknown-linux-gnu/sysroot --gcc-toolchain=$GT"

if [ ! -f "$C/liburing.a" ]; then
  ( cd "$D/liburing" && ./configure --cc="$CC" >/dev/null && make -s -C src CC="$CC" AR="$LLVM/bin/llvm-ar" RANLIB=true liburing.a )
  cp "$D/liburing/src/liburing.a" "$C/liburing.a"
fi
# SQLite: threads in separate connections (checkpointer, committer), QPSG (plans fixed at prepare), no per-call memory stats.
SQLITE_OPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_ENABLE_QPSG -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS"
CFLAGS_DEFAULT="-O2"
BASE_CFLAGS="${KN_CFLAGS:-$CFLAGS_DEFAULT} -fPIC -g0 -Wall -Wno-unused-function"
PROF_RT="$(ls "$LLVM"/lib/clang/*/lib/x86_64-unknown-linux-gnu/libclang_rt.profile.a 2>/dev/null | head -1)"

# build_c plain|gen|use: build/c/libkn.a without a profile, instrumented, or optimized with the recorded profile.
build_c() {
  local pf=""
  case "$1" in
    gen) pf="-fprofile-generate=$P/raw" ;;
    use) pf="-fprofile-use=$P/kn.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin" ;;
  esac
  $CC $BASE_CFLAGS $pf $SQLITE_OPTS -w -I"$D/sqlite" -I"$ROOT/csrc" -c "$ROOT/csrc/db.c" -o "$C/db.o"
  $CC $BASE_CFLAGS $pf -I"$D/liburing/src/include" -I"$ROOT/csrc" -c "$ROOT/csrc/uring.c" -o "$C/uring.o"
  rm -f "$C/libkn.a" && "$LLVM/bin/llvm-ar" rcs "$C/libkn.a" "$C/db.o" "$C/uring.o"
}
# link_kn [extra linker options]: cinterop (kn.def -> klib, with libkn.a inside) + kotlinc-native -> build/server.kexe
link_kn() {
  "$KN/bin/cinterop" -def "$ROOT/kn.def" -compiler-option -I"$ROOT/csrc" -libraryPath "$C" \
    -staticLibrary liburing.a -o "$B/knc"
  "$KN/bin/kotlinc-native" -opt -e app.main -l "$B/knc.klib" ${1:+-linker-options "$1"} "$ROOT"/src/*.kt -o "$B/server"
}

# Profile-guided C (KN_CPGO=1, the default): the profile is recorded HERE, during this build, on this machine. The
# instrumented server serves a synthetic scratch database (train/mkdb.c: random rows, none of the seed) to the training
# client (train/train.c) for KN_TRAIN_REQS requests; then libkn.a is rebuilt with that profile. Nothing is committed.
# Every step has a time limit; if anything fails the build falls back to the plain build and says so.
train() {
  local T="$B/train" port="${KN_TRAIN_PORT:-3999}" secret="train-$RANDOM$RANDOM$RANDOM" pid rc=0
  rm -rf "$T" && mkdir -p "$T"
  [ -n "$PROF_RT" ] || { echo "no libclang_rt.profile.a in $LLVM"; return 1; }
  $CC -O1 -w -I"$D/sqlite" "$ROOT/train/mkdb.c" "$C/db.o" -lpthread -ldl -lm "$PROF_RT" -o "$T/mkdb" || return 1
  $CC -O2 -w "$ROOT/train/train.c" -lpthread -o "$T/train" || return 1
  # mkdb's own SQLite calls must not land in the profile (it links the instrumented db.o too)
  LLVM_PROFILE_FILE="$T/mkdb.profraw" timeout 300 "$T/mkdb" "$T/train.db" "${KN_TRAIN_USERS:-20000}" \
    "${KN_TRAIN_POSTS:-50000}" "${KN_TRAIN_LIKES:-200000}" || return 1
  ( cd "$T" && SQLITE_PATH="$T/train.db" JWT_SECRET="$secret" HOST=127.0.0.1 PORT="$port" \
      LLVM_PROFILE_FILE="$P/raw/kn-%p.profraw" exec timeout -s KILL 1200 "$B/server.kexe" ) > "$T/server.log" 2>&1 &
  pid=$!
  for _ in $(seq 300); do curl -sf "http://127.0.0.1:$port/health" >/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.2; done
  if curl -sf "http://127.0.0.1:$port/health" >/dev/null; then
    timeout "${KN_TRAIN_TIMEOUT:-600}" "$T/train" 127.0.0.1 "$port" "$secret" "${KN_TRAIN_USERS:-20000}" \
      "${KN_TRAIN_REQS:-200000}" "${KN_TRAIN_CONNS:-64}" || rc=1
  else
    echo "training server did not come up:"; cat "$T/server.log"; rc=1
  fi
  kill -TERM $pid 2>/dev/null || true   # the server writes its profile on SIGTERM (instrumented builds only)
  for _ in $(seq 300); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
  kill -KILL $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
  [ "$rc" = 0 ] || return 1
  ls "$P"/raw/*.profraw >/dev/null 2>&1 || { echo "no profile written"; return 1; }
  "$LLVM/bin/llvm-profdata" merge -o "$P/kn.profdata" "$P"/raw/*.profraw || return 1
  rm -rf "$T"
}

if [ "${KN_CPGO:-1}" = 1 ]; then
  rm -rf "$P" && mkdir -p "$P/raw"
  build_c gen   # (a compile error here stops the build: set -e)
  link_kn "-u__llvm_profile_runtime $PROF_RT"
  if train; then
    echo "== PGO training done; building with the recorded profile"
    build_c use
  else
    echo "!! PGO training failed: falling back to the plain build" >&2
    rm -rf "$B/train"
    build_c plain
  fi
else
  build_c plain
fi
link_kn
ls -la "$B/server.kexe"
