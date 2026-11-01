#!/usr/bin/env bash
# build.sh — builds bin/server as a normal user. C only; the Lean 4 spec in
# spec/ is checked separately with lake (see README.md).
#
# PGO=1 (default): profile-guided build in three steps —
#   1. compile + link instrumented (-fprofile-generate),
#   2. run `bin/server --pgo-train` (in-process: builds a small DB under
#      $TMPDIR, serves ~70k raw HTTP requests through the real parser and
#      handlers, verifies JWTs, deletes the DB) → obj/db.gcda, obj/server.gcda,
#   3. compile + link again with -fprofile-use.
# A failure in any step exits non-zero (no silent fallback). PGO=0 builds
# without a profile. PGO_KEEP_GCDA=1 keeps the .gcda files for inspection.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin obj deps
PGO="${PGO:-1}"
tmp=""; tdir=""
trap 'rm -rf ${tmp:+"$tmp"} ${tdir:+"$tdir"}' EXIT

# SQLite 3.54.1 amalgamation, pinned by SHA-256 (tarball and the two files).
SQLITE_URL=https://www.sqlite.org/2026/sqlite-autoconf-3540100.tar.gz
SQLITE_TGZ_SHA=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e
SQLITE_C_SHA=0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52
SQLITE_H_SHA=d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c

have_sqlite() {
  [ -f deps/sqlite3.c ] && [ -f deps/sqlite3.h ] &&
  echo "$SQLITE_C_SHA  deps/sqlite3.c" | sha256sum -c --status &&
  echo "$SQLITE_H_SHA  deps/sqlite3.h" | sha256sum -c --status
}
if ! have_sqlite; then
  tmp="$(mktemp -d)"
  echo "build.sh: downloading $SQLITE_URL"
  curl -fsSL --retry 3 -o "$tmp/sqlite.tgz" "$SQLITE_URL"
  echo "$SQLITE_TGZ_SHA  $tmp/sqlite.tgz" | sha256sum -c --quiet
  tar -xzf "$tmp/sqlite.tgz" -C "$tmp" sqlite-autoconf-3540100/sqlite3.c sqlite-autoconf-3540100/sqlite3.h
  cp "$tmp/sqlite-autoconf-3540100/sqlite3.c" "$tmp/sqlite-autoconf-3540100/sqlite3.h" deps/
  have_sqlite || { echo "build.sh: amalgamation checksum mismatch" >&2; exit 1; }
fi

SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_USE_ALLOCA -DSQLITE_TEMP_STORE=3 -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1
  -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_DECLTYPE -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_STRICT_SUBTYPE=1 -DSQLITE_OMIT_AUTHORIZATION
  -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE
  -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_OMIT_EXPLAIN -DSQLITE_OMIT_JSON"

CC="${CC:-gcc}"
# db.c #includes deps/sqlite3.c (one translation unit), so it gets -w.
# Built on the target machine itself, so -march=native is safe.
CFLAGS="-O3 -flto=auto -march=native -fno-plt -fomit-frame-pointer
  -fno-semantic-interposition -DNDEBUG"
# $1: extra flags (word-split). Same -o paths every time, so the .gcda names
# written in step 2 match the objects of step 3.
compile() {
  $CC $CFLAGS $1 -std=gnu11 -w $SQLITE_FLAGS -c src/db.c -o obj/db.o &&
  $CC $CFLAGS $1 -std=gnu11 -Wall -Wextra -c src/server.c -o obj/server.o &&
  $CC $CFLAGS $1 -o bin/server obj/server.o obj/db.o -luring -lm -lpthread -ldl
}

rm -f obj/*.gcda
if [ "$PGO" = 0 ]; then
  compile "" || { echo "build.sh: build failed" >&2; exit 1; }
  echo "build.sh: built bin/server (no pgo)"
  exit 0
fi

echo "build.sh: pgo 1/3: instrumented build"
compile "-fprofile-generate -fprofile-update=single" ||
  { echo "build.sh: PGO step 1 (instrumented build) failed; not falling back (PGO=0 builds without PGO)" >&2; exit 1; }

echo "build.sh: pgo 2/3: bin/server --pgo-train"
tdir="$(mktemp -d "${TMPDIR:-/tmp}/pgo-build.XXXXXX")"
TMPDIR="$tdir" JWT_SECRET=pgo-train-secret ./bin/server --pgo-train ||
  { echo "build.sh: PGO step 2 (bin/server --pgo-train) failed; not falling back (PGO=0 builds without PGO)" >&2; exit 1; }
rm -rf "$tdir"; tdir=""
for f in obj/db.gcda obj/server.gcda; do
  [ -s "$f" ] || { echo "build.sh: PGO step 2 left no $f; not falling back (PGO=0 builds without PGO)" >&2; exit 1; }
done
echo "build.sh: profile: $(cd obj && for f in *.gcda; do printf '%s (%s bytes) ' "$f" "$(wc -c < "$f")"; done)"

echo "build.sh: pgo 3/3: optimized build with the profile"
compile "-fprofile-use -fprofile-partial-training -Wno-missing-profile" ||
  { echo "build.sh: PGO step 3 (profile-use build) failed" >&2; exit 1; }
[ "${PGO_KEEP_GCDA:-0}" = 1 ] || rm -f obj/*.gcda
echo "build.sh: built bin/server (pgo)"
