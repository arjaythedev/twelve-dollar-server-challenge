#!/usr/bin/env bash
# build.sh — builds bin/server as a normal user.
#   1. SQLite 3.54.1: the pinned amalgamation (sha256-checked) plus our patches (config/cursed.sh picks which)
#   2. the HTTP parser, generated from the API spec (gen/)
#   3. the server, with PGO: an instrumented build trains on a synthetic database (no seed data), then the final
#      build uses that profile. Any PGO failure falls back to a plain -O3 build. PGO=0 skips it.
# Build-time options (parsers, how cursed the SQLite side is) are documented in config/*.sh and the README.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_VERSION=3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
SQLITE_URL="https://www.sqlite.org/2026/sqlite-amalgamation-$SQLITE_VERSION.zip"
CC=${CC:-gcc}
ARCH=${ARCH:--march=native}

. config/sqlite.sh
. config/cursed.sh
. config/parsers.sh
. config/compile.sh

log() { echo "[build $(date +%T)] $*"; }
# GNU sha256sum on Linux; macOS's BSD sha256sum lacks -c --quiet, so fall back to shasum there
sha256() { if sha256sum --version > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
sha256_ok() { [ "$(sha256 "$2" | cut -d' ' -f1)" = "$1" ]; }
mkdir -p deps build bin
log "options: $CURSED_SUMMARY parsers=${PARSER_FAST:-batch}+${PARSER_SLOW:-fb}"

# 1. SQLite (cached in deps/ until the version or the patch set changes)
stamp="$SQLITE_VERSION $SQLITE_SHA256 [$SQLITE_PATCHES] $(cat patches/sqlite/*.patch | sha256 | cut -c1-16)"
if [ "$(cat deps/sqlite/STAMP 2> /dev/null)" != "$stamp" ]; then
  # reuse a cached zip only if it's there and matches; otherwise download (and the check below must pass)
  if ! { [ -f deps/sqlite.zip ] && sha256_ok "$SQLITE_SHA256" deps/sqlite.zip; }; then
    log "fetch SQLite $SQLITE_VERSION"
    curl -fsSL --retry 3 -o deps/sqlite.zip "$SQLITE_URL"
    sha256_ok "$SQLITE_SHA256" deps/sqlite.zip || { log "deps/sqlite.zip: sha256 mismatch"; exit 1; }
  fi
  rm -rf deps/sqlite "deps/sqlite-amalgamation-$SQLITE_VERSION"
  unzip -q deps/sqlite.zip -d deps
  mkdir -p deps/sqlite/a
  cp "deps/sqlite-amalgamation-$SQLITE_VERSION"/sqlite3.[ch] deps/sqlite/a/
  for p in $SQLITE_PATCHES; do
    (cd deps/sqlite && patch -s -p1 < "../../patches/sqlite/$p.patch")
    log "patched $p"
  done
  mv deps/sqlite/a/* deps/sqlite/
  rmdir deps/sqlite/a
  echo "$stamp" > deps/sqlite/STAMP
fi

# 2. the HTTP parser
generate_parser build

# 3. the server
plain() {
  log "plain -O3 build"
  compile_units build deps/sqlite $ARCH -O3 && link_server build bin/server $ARCH -O3
}
if [ "${PGO:-1}" = 0 ] || $CC --version 2> /dev/null | grep -q clang; then
  # the PGO flags below are gcc's; clang (e.g. macOS's cc/gcc) gets a plain build
  [ "${PGO:-1}" = 0 ] || log "PGO skipped: $CC is clang"
  plain
  log "done (no PGO)"
  exit 0
fi
rm -rf build/pgo build/train
mkdir -p build/train
pgo="-fprofile-dir=$PWD/build/pgo"
if log "PGO pass 1 (instrumented)" &&
  compile_units build deps/sqlite $ARCH -O3 -fprofile-generate -fprofile-update=prefer-atomic "$pgo" &&
  link_server build bin/server $ARCH -O3 -fprofile-generate "$pgo" &&
  log "train (synthetic DB)" && timeout 120 bin/server --train build/train &&
  log "PGO pass 2" &&
  compile_units build deps/sqlite $ARCH -O3 -fprofile-use -fprofile-partial-training -Wno-missing-profile "$pgo" &&
  link_server build bin/server $ARCH -O3 -fprofile-use "$pgo"; then
  log "done (PGO)"
else
  log "PGO failed: falling back"
  plain
  log "done (no PGO)"
fi
rm -rf build/train
