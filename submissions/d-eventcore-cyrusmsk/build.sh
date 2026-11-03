#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-cc}"
SQLITE_CFLAGS=(
  -O3 -fno-plt -march=native -mtune=native -fno-stack-protector
  -DSQLITE_THREADSAFE=0
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_USE_ALLOCA
  -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_DIRECT_OVERFLOW_READ
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0
  -DNDEBUG
)

D_COMMON=(
  -O3 -release -boundscheck=off -mcpu=native
)

LDC2="$(command -v ldc2 || true)"
if [ -n "$LDC2" ]; then
  LDC_BIN="$(readlink -f "$LDC2" 2>/dev/null || printf '%s' "$LDC2")"
  LDC_DIR="$(dirname "$LDC_BIN")"
else
  LDC_DIR=""
fi
LDC_PROFDATA="${LDC_DIR:+$LDC_DIR/ldc-profdata}"
LDC_RT_PROFILE="${LDC_DIR:+$LDC_DIR/../lib/libldc_rt.profile.a}"

PGO_DIR="$PWD/.pgo-build"
rm -rf "$PGO_DIR"

pgo_available=0
if [ "${PGO:-1}" = "1" ] && [ -n "$LDC_PROFDATA" ] && [ -x "$LDC_PROFDATA" ] \
    && [ -n "$LDC_RT_PROFILE" ] && [ -f "$LDC_RT_PROFILE" ]; then
  pgo_available=1
fi

if [ "$pgo_available" = "1" ]; then
  echo "== SQLite PGO instrumentation"
  mkdir -p "$PGO_DIR"
  if "$CC" "${SQLITE_CFLAGS[@]}" -fprofile-generate="$PGO_DIR" \
        -c sqlite_lib/sqlite3.c -o sqlite_lib/sqlite3.o \
      && DFLAGS="${D_COMMON[*]} -d-version=PGO -fprofile-generate=$PGO_DIR/d.profraw -L=-lgcov -L=$LDC_RT_PROFILE" \
        dub build --build=release --compiler=ldc2 --force \
      && LLVM_PROFILE_FILE="$PGO_DIR/d.profraw" ./bin/server --pgo-train \
      && "$LDC_PROFDATA" merge -o "$PGO_DIR/d.profdata" "$PGO_DIR/d.profraw"; then
    echo "== SQLite + D PGO final build"
    if "$CC" "${SQLITE_CFLAGS[@]}" -fprofile-use="$PGO_DIR" -fprofile-correction \
          -Wno-missing-profile -c sqlite_lib/sqlite3.c -o sqlite_lib/sqlite3.o \
        && DFLAGS="${D_COMMON[*]} -flto=full -fprofile-use=$PGO_DIR/d.profdata" \
          dub build --build=release --compiler=ldc2 --force; then
      rm -rf "$PGO_DIR"
      echo "== build complete (PGO + LTO)"
      exit 0
    fi
  fi
  echo "== PGO build failed; falling back to a plain optimized build"
  rm -rf "$PGO_DIR"
fi

"$CC" "${SQLITE_CFLAGS[@]}" -c sqlite_lib/sqlite3.c -o sqlite_lib/sqlite3.o
dub build --build=release --compiler=ldc2 --force
echo "== build complete"
