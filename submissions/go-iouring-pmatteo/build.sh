#!/usr/bin/env bash
# Builds the server as a normal user. Downloads the pinned SQLite amalgamation, verifies it, and
# builds it twice: once instrumented, once against the profile that instrumented build produced.
# No prebuilt binaries are shipped.
#
#   ./build.sh          full build, profile guided when the toolchain allows it
#   ./build.sh --lib    only (re)build .deps/libsqlite3.a, for the development loop
#   PGO=0 ./build.sh    skip the profile entirely
set -euo pipefail
cd "$(dirname "$0")"

# SQLite 3.54.1. Two hashes are pinned: sqlite.org/download.html publishes SHA3-256, so that one
# can be checked against the site by hand, and SHA-256 is what sha256sum verifies here.
SQLITE_YEAR=2026
SQLITE_ZIP=sqlite-amalgamation-3540100.zip
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
SQLITE_SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30

DEPS="$PWD/.deps"
PROFILE="$DEPS/pgo"
CC="${CC:-cc}"
mkdir -p "$DEPS" bin .sqlite-download

fetch_sqlite() {
  local zip=".sqlite-download/$SQLITE_ZIP"
  if [ ! -f "$zip" ]; then
    curl -fsSL --retry 3 -o "$zip.part" "https://www.sqlite.org/$SQLITE_YEAR/$SQLITE_ZIP"
    mv "$zip.part" "$zip"
  fi
  printf '%s  %s\n' "$SQLITE_SHA256" "$zip" | sha256sum -c -
  local got3; got3="$(openssl dgst -sha3-256 -r "$zip" | cut -d' ' -f1)"
  [ "$got3" = "$SQLITE_SHA3_256" ] || { echo "$zip: SHA3-256 mismatch ($got3)" >&2; exit 1; }
  unzip -o -j -q "$zip" '*/sqlite3.c' '*/sqlite3.h' -d "$DEPS"
}

# The amalgamation is compiled here rather than by cgo, which is what makes the profile usable:
# GCC looks the profile up by the object file's path, and cgo's object paths are temporary.
# sqlite_cfg.h carries the build options so db.go reads the header with the same ones.
base_flags() {
  printf '%s' "-O3 -std=c99 -Wno-unused-function -fno-semantic-interposition -fomit-frame-pointer"
  printf ' %s' "-I$PWD -include $PWD/sqlite_cfg.h"
}

# cc_accepts probes a flag rather than assuming it. -march=native is safe under these rules -
# the server is built from source on the box it runs on - but a build somewhere else must not
# fail because of it.
cc_accepts() {
  echo 'int main(void){return 0;}' |
    $CC -xc - "$@" -o /dev/null >/dev/null 2>&1
}

compile_sqlite() {
  # shellcheck disable=SC2046  # the flags are deliberately word split
  $CC $(base_flags) $NATIVE "$@" -c "$DEPS/sqlite3.c" -o "$DEPS/sqlite3.o"
  rm -f "$DEPS/libsqlite3.a"
  ar rcs "$DEPS/libsqlite3.a" "$DEPS/sqlite3.o"
}

fetch_sqlite
NATIVE=""
cc_accepts -march=native && NATIVE="-march=native"

export PATH="/usr/local/go/bin:$PATH"
export GOTOOLCHAIN=local CGO_ENABLED=1 GOAMD64=v2

if [ "${1:-}" = "--lib" ]; then
  compile_sqlite
  exit 0
fi

go version
go_pgo=()

# Probed, not detected by vendor string: cc identifies itself as "cc (Debian ...)" with no "gcc"
# in it. -fprofile-correction is the flag that separates the two toolchains, because clang takes
# -fprofile-generate but wants llvm-profdata to merge the result, which is a different workflow.
if [ "${PGO:-1}" = 1 ] && cc_accepts -fprofile-generate="$DEPS/probe" &&
   cc_accepts -fprofile-correction -Wno-missing-profile; then
  rm -rf "$PROFILE"; mkdir -p "$PROFILE"
  echo "build: instrumenting SQLite and running the training workload"
  compile_sqlite "-fprofile-generate=$PROFILE"
  # -lgcov is what the instrumented objects need at link time; cgo does the linking.
  if CGO_LDFLAGS="-lgcov" go build -o "$DEPS/train" . && "$DEPS/train" train; then
    compile_sqlite "-fprofile-use=$PROFILE" -fprofile-correction -Wno-missing-profile
    [ -s default.pgo ] && go_pgo=(-pgo=default.pgo)
    echo "build: profile guided, $(find "$PROFILE" -name '*.gcda' | wc -l) counter files"
  else
    echo "build: training run failed, building without profiles" >&2
    compile_sqlite
  fi
else
  compile_sqlite
fi

go build -trimpath "${go_pgo[@]}" -ldflags='-s -w' -o bin/server .
