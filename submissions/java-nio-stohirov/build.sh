#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/javac ]; then JAVA_HOME=/opt/jdk; fi
JDK_BIN="${JAVA_HOME:+$JAVA_HOME/bin/}"

SQLITE=sqlite-amalgamation-3540000
SQLITE_URL="https://sqlite.org/2026/$SQLITE.zip"
SQLITE_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA -DSQLITE_ENABLE_QPSG"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

mkdir -p build
if [ "$(uname)" = Darwin ]; then
  LIB=build/libsqlite3.dylib; CFLAGS="-O2 -dynamiclib"
else
  LIB=build/libsqlite3.so; CFLAGS="-O2 -march=native -fPIC -shared"
fi
STAMP="$SQLITE $CFLAGS $(echo $SQLITE_FLAGS)"
if [ ! -f "$LIB" ] || [ "$(cat build/sqlite.stamp 2>/dev/null)" != "$STAMP" ]; then
  if [ ! -f "build/$SQLITE.zip" ] || [ "$(sha256 "build/$SQLITE.zip")" != "$SQLITE_SHA256" ]; then
    curl -fsSL "$SQLITE_URL" -o "build/$SQLITE.zip.part"
    [ "$(sha256 "build/$SQLITE.zip.part")" = "$SQLITE_SHA256" ] || { echo "sqlite checksum mismatch"; exit 1; }
    mv "build/$SQLITE.zip.part" "build/$SQLITE.zip"
  fi
  rm -rf "build/$SQLITE"
  (cd build && "${JDK_BIN}jar" xf "$SQLITE.zip")
  cc $CFLAGS $SQLITE_FLAGS -o "$LIB" "build/$SQLITE/sqlite3.c"
  echo "$STAMP" > build/sqlite.stamp
fi

rm -rf build/classes
"${JDK_BIN}javac" --release 25 -Xlint:all,-restricted,-serial -Werror -d build/classes $(find src -name '*.java')
printf 'Enable-Native-Access: ALL-UNNAMED\n' > build/manifest.txt
"${JDK_BIN}jar" --create --file build/server.jar --main-class challenge.Main --manifest build/manifest.txt -C build/classes .
