#!/usr/bin/env bash
# Builds bin/server from the Nexium sources in src/, as a normal user.
#
# First run only, into deps/ (about three minutes in all):
#   1. the Nexium compiler, nx, bootstrapped from its source at a pinned commit;
#   2. the pinned SQLite amalgamation, checked by SHA-256.
# To build offline, set NX_SOURCE to a tarball of that commit (testing only: a
# tarball is not checked against the pin) and SQLITE_TARBALL to a local copy.
# NX_BIN points at an nx that is already built and skips step 1.
set -euo pipefail
cd "$(dirname "$0")"
CC="${CC:-gcc}"
mkdir -p deps bin

# --- 1. nx 1.5.0, from source --------------------------------------------
# Nothing prebuilt is downloaded. The commit is fetched by its full hash, so
# git itself checks every byte of the source against the pin.
# The compiler is written in Nexium, so its repository carries a C seed: the C
# an earlier nx emitted for itself. The steps below are those of the
# repository's bootstrap/build.sh for gcc on x86-64, written out here so that
# what runs is in this file: gcc builds nx0 from the seed, nx0 builds the
# compiler's Nexium source into nx1, nx1 emits that source as C, gcc builds nx2
# from it, and nx2 must emit the same C or the build stops.
NX_REPO=https://github.com/Londopy/nexium
NX_COMMIT=3224bf21716c5fe072481232b2bef0938bbc5912 # the v1.5.0 tag
export NX_OFFLINE=1 NX_NO_UPDATE_CHECK=1 # nx makes no network request of its own
NX="${NX_BIN:-$PWD/deps/nx-${NX_COMMIT:0:12}}"
if [ ! -x "$NX" ]; then
  src="deps/nexium-${NX_COMMIT:0:12}"
  mkdir -p "$src"
  (
    cd "$src"
    if [ -n "${NX_SOURCE:-}" ]; then
      tar -xzf "$NX_SOURCE" --strip-components=1
    else
      git init -q
      git fetch -q --depth 1 "$NX_REPO" "$NX_COMMIT"
      git -c advice.detachedHead=false checkout -q FETCH_HEAD
      [ "$(git rev-parse HEAD)" = "$NX_COMMIT" ]
    fi
    out=nx-out/bootstrap
    flags="-std=gnu11 -O2 -w -fno-strict-aliasing -march=x86-64"
    mkdir -p "$out"
    export NX_CC="$CC"
    $CC $flags -o "$out/nx0" bootstrap/nx.c -lm -lc
    "$out/nx0" build self/nx.nx --mode safe -o "$out/nx1" --out-dir "$out"
    "$out/nx1" emit-c self/nx.nx --mode safe > "$out/nx1.c"
    $CC $flags -o "$out/nx2" "$out/nx1.c" -lm -lc
    "$out/nx2" emit-c self/nx.nx --mode safe > "$out/nx2.c"
    cmp "$out/nx1.c" "$out/nx2.c"
    echo "nx bootstrapped: nx1 and nx2 emit the same C"
  )
  cp "$src/nx-out/bootstrap/nx2" "$NX"
fi
"$NX" version

# --- 2. SQLite -----------------------------------------------------------
SQLITE_VER=3540100
SQLITE_URL="https://sqlite.org/2026/sqlite-autoconf-${SQLITE_VER}.tar.gz"
SQLITE_SHA256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e
if [ ! -f deps/sqlite3.c ]; then
  tarball="${SQLITE_TARBALL:-deps/sqlite-autoconf-${SQLITE_VER}.tar.gz}"
  [ -f "$tarball" ] || curl -fsSL "$SQLITE_URL" -o "$tarball"
  echo "${SQLITE_SHA256}  ${tarball}" | sha256sum -c --quiet -
  tar -xzf "$tarball" -C deps --strip-components=1 \
    "sqlite-autoconf-${SQLITE_VER}/sqlite3.c" "sqlite-autoconf-${SQLITE_VER}/sqlite3.h"
fi

# --- 3. the server -------------------------------------------------------
# nx compiles Nexium to C and hands it to the C compiler named in NX_CC, with
# -O2 and the -march given by --cpu. The extra words are the C entry's flags.
# Fast mode drops the bounds and overflow checks that debug and safe keep; the
# tests run in all three. SQLite comes in through src/sqlite_shim.c, which sets
# its compile-time options itself.
export NX_CC="$CC -mtune=cascadelake -fno-plt -fno-semantic-interposition ${EXTRA_CFLAGS:-}"
cd src
"$NX" build main.nx --mode fast --cpu x86-64-v3 --out-dir ../bin/nx
cp ../bin/nx/main ../bin/server
echo "built $(cd .. && pwd)/bin/server"
