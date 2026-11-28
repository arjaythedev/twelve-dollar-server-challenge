#!/usr/bin/env bash
# Builds the HolyPHP compiler from the source under toolchain/, then compiles
# server.hphp into bin/server. Runs as a normal user; writes only inside this
# folder.
set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"
ROOT="$HERE/toolchain"

# Optimisation for the generated server, its runtime and the link. The
# compiler honours HPHP_OPT; a caller value still wins (portability/debug).
export HPHP_OPT="${HPHP_OPT:--O3 -march=native -flto}"
if [[ -n "${HPHP_PGO_GENERATE_DIR:-}" ]]; then
  mkdir -p "$HPHP_PGO_GENERATE_DIR"
  HPHP_OPT+=" -fprofile-generate=$HPHP_PGO_GENERATE_DIR -fprofile-update=single"
elif [[ -n "${HPHP_PGO_DIR:-}" ]]; then
  HPHP_OPT+=" -fprofile-use=$HPHP_PGO_DIR -fprofile-correction -Wno-missing-profile -Wno-error=coverage-mismatch"
fi
export HPHP_OPT

# 1. SQLite 3.46.1, pinned by checksum. The compiler embeds the amalgamation
#    (with toolchain/src/hp_btree.c appended for the B-tree cursor builtins)
#    and compiles it thread-safe with the settings in toolchain/scripts/build.sh.
SQLITE_VER=3460100
SQLITE_SHA=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
SQLITE_ZIP="sqlite-amalgamation-$SQLITE_VER.zip"
SQLITE_DIR="$ROOT/third_party/sqlite"
if [[ ! -f "$SQLITE_DIR/sqlite3.c" ]]; then
  mkdir -p "$HERE/build" "$SQLITE_DIR"
  curl -fsSL -o "$HERE/build/$SQLITE_ZIP" "https://www.sqlite.org/2024/$SQLITE_ZIP"
  echo "$SQLITE_SHA  $HERE/build/$SQLITE_ZIP" | sha256sum -c - >/dev/null
  unzip -q -o "$HERE/build/$SQLITE_ZIP" -d "$HERE/build/sqlite-src"
  cp "$HERE/build/sqlite-src/sqlite-amalgamation-$SQLITE_VER/"sqlite3{.c,.h,ext.h} "$SQLITE_DIR/"
fi

# 2. The compiler (runtime, stdlib and SQLite embedded in the binary).
(cd "$ROOT" && bash scripts/build.sh)

# 3. The server, built fresh (no object cache) with HPHP_OPT.
mkdir -p bin
HPHP_NO_CACHE=1 HPHP_WORK_DIR="$HERE/build/work" "$ROOT/hphp" build server.hphp -o bin/server
echo "built: bin/server"
