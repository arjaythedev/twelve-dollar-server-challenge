#!/usr/bin/env bash
# Builds bin/server from the Nexium sources in src/. Fetches the pinned SQLite
# amalgamation on first run and checks its SHA-256 (set SQLITE_TARBALL to a
# local copy to build offline).
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_VER=3540100
SQLITE_URL="https://sqlite.org/2026/sqlite-autoconf-${SQLITE_VER}.tar.gz"
SQLITE_SHA256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e

mkdir -p deps bin
if [ ! -f deps/sqlite3.c ]; then
  tarball="${SQLITE_TARBALL:-deps/sqlite-autoconf-${SQLITE_VER}.tar.gz}"
  [ -f "$tarball" ] || curl -fsSL "$SQLITE_URL" -o "$tarball"
  echo "${SQLITE_SHA256}  ${tarball}" | sha256sum -c --quiet -
  tar -xzf "$tarball" -C deps --strip-components=1 \
    "sqlite-autoconf-${SQLITE_VER}/sqlite3.c" "sqlite-autoconf-${SQLITE_VER}/sqlite3.h"
fi

# nx compiles Nexium to C and hands it to the C compiler named in NX_CC, with
# -O2 and the -march given by --cpu. The extra words are the C entry's flags.
# Fast mode drops the bounds and overflow checks that debug and safe keep; the
# tests run in all three. SQLite comes in through src/sqlite_shim.c, which sets
# its compile-time options itself.
# NX_OFFLINE and NX_NO_UPDATE_CHECK keep the build off the network.
export NX_OFFLINE=1 NX_NO_UPDATE_CHECK=1
export NX_CC="${CC:-gcc} -mtune=cascadelake -fno-plt -fno-semantic-interposition ${EXTRA_CFLAGS:-}"
cd src
nx build main.nx --mode fast --cpu x86-64-v3 --out-dir ../bin/nx
cp ../bin/nx/main ../bin/server
echo "built $(cd .. && pwd)/bin/server"
