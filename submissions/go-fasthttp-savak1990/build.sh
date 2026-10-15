#!/usr/bin/env bash
# Builds bin/server as a normal user, in three steps:
#   1. fetch the pinned SQLite amalgamation (sha256-checked) into sqlite/
#   2. compile it into sqlite/libsqlite3.a for the CPU this box has
#   3. go build, linking that library through cgo (see sqlite.go / shim.c)
#
# Optional:  GOAMD64=v2 bash build.sh       force the x86-64 level (default: detected, v3 or v2)
#            SQLITE_PGO=gen bash build.sh   instrumented SQLite for gcc profile training
set -euo pipefail
cd "$(dirname "$0")"

GO=go; command -v go >/dev/null 2>&1 || GO=/usr/local/go/bin/go
CC="${CC:-gcc}"

# ---- 1. SQLite source ---------------------------------------------------------------------------
SQLITE_VER=3540000
SQLITE_URL="https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VER}.zip"
SQLITE_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d

sha256() { if command -v sha256sum >/dev/null; then sha256sum | cut -d' ' -f1; else shasum -a 256 | cut -d' ' -f1; fi; }

if [ "$(cat sqlite/.version 2>/dev/null)" != "$SQLITE_SHA256" ]; then
  echo "build: fetching SQLite $SQLITE_VER"
  tmp="$(mktemp -d)"
  curl -fsSL "$SQLITE_URL" -o "$tmp/sqlite.zip"
  [ "$(sha256 < "$tmp/sqlite.zip")" = "$SQLITE_SHA256" ] || { echo "sqlite checksum mismatch"; exit 1; }
  unzip -q -j "$tmp/sqlite.zip" "sqlite-amalgamation-${SQLITE_VER}/sqlite3.c" "sqlite-amalgamation-${SQLITE_VER}/sqlite3.h" -d "$tmp"
  rm -rf sqlite && mkdir sqlite && mv "$tmp/sqlite3.c" "$tmp/sqlite3.h" sqlite/
  echo "$SQLITE_SHA256" > sqlite/.version
  rm -rf "$tmp"
fi

# ---- 2. SQLite library --------------------------------------------------------------------------
CFLAGS="-O2 -fno-plt -fomit-frame-pointer -pipe"

# x86-64-v3 (AVX2/BMI2) when the CPU has it, else v2; same level for Go and C. Linux/x86 only.
if [ "$(uname -s)-$(uname -m)" = Linux-x86_64 ]; then
  if [ -z "${GOAMD64:-}" ]; then
    GOAMD64=v2; grep -q -w avx2 /proc/cpuinfo && grep -q -w bmi2 /proc/cpuinfo && GOAMD64=v3
  fi
  export GOAMD64
  CFLAGS="$CFLAGS -march=x86-64-$GOAMD64"
fi

# Compile-time options: single-threaded (one connection, one goroutine), no stats bookkeeping, and
# the usual OMIT set from sqlite.org/compile.html.
DEFS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION
      -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DECLTYPE
      -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
      -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_UTF16 -DSQLITE_TEMP_STORE=3"
DEFS="$(echo $DEFS)"

# gcc profile-guided optimisation. The profile (sqlite#sqlite3.gcda + gcc-version) is recorded by a
# training run and committed; it is applied only with gcc of the same major version, so a different
# compiler (CI, macOS clang) builds the same code without it.
# -fprofile-prefix-path keeps the .gcda name independent of where the checkout lives.
PGO=""; PGO_LD=""
gcc_major="$("$CC" -dumpversion 2>/dev/null | cut -d. -f1)"
"$CC" --version 2>/dev/null | head -1 | grep -qi gcc || gcc_major=""
case "${SQLITE_PGO:-use}" in
  gen) [ -n "$gcc_major" ] || { echo "SQLITE_PGO=gen needs gcc"; exit 1; }
       PGO="-fprofile-generate -fprofile-update=single -fprofile-dir=$PWD -fprofile-prefix-path=$PWD"
       PGO_LD="-fprofile-generate -DSHIM_GCOV"   # also makes shim.c flush the counters on SIGTERM
       echo "$gcc_major" > gcc-version ;;
  use) if [ -n "$gcc_major" ] && [ -s "sqlite#sqlite3.gcda" ] && [ "$gcc_major" = "$(cat gcc-version 2>/dev/null)" ]; then
         PGO="-fprofile-use -fprofile-dir=$PWD -fprofile-prefix-path=$PWD -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
         echo "build: gcc PGO profile applied (gcc $gcc_major)"
       fi ;;
esac

# Compiled outside cgo so the object has a fixed path (gcc PGO matches .gcda files by it);
# recompiled only when the flags change.
flags="$CC $CFLAGS $DEFS $PGO"
if [ "$(cat sqlite/.flags 2>/dev/null)" != "$flags" ]; then
  echo "build: compiling sqlite3.c ($CC${GOAMD64:+, x86-64-$GOAMD64})"
  # shellcheck disable=SC2086
  "$CC" $CFLAGS $DEFS $PGO -c sqlite/sqlite3.c -o sqlite/sqlite3.o
  rm -f sqlite/libsqlite3.a && ar rcs sqlite/libsqlite3.a sqlite/sqlite3.o
  echo "$flags" > sqlite/.flags
fi

# ---- 3. Go binary -------------------------------------------------------------------------------
# default.pgo (Go PGO, recorded by the same training run) is picked up by go build automatically.
export CGO_ENABLED=1 GOTOOLCHAIN=local
export CGO_CFLAGS="$CFLAGS $DEFS ${PGO_LD:+-DSHIM_GCOV}"
export CGO_LDFLAGS="${PGO_LD%% *}"
[ -s default.pgo ] && echo "build: Go PGO profile applied (default.pgo)"
"$GO" build -trimpath -ldflags='-s -w' -o bin/server .
echo "build: bin/server ready"
