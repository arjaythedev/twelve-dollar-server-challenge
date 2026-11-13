#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/javac ]; then JAVA_HOME=/opt/jdk; fi
JDK_BIN="${JAVA_HOME:+$JAVA_HOME/bin/}"

SQLITE=sqlite-amalgamation-3540000
SQLITE_URL="https://sqlite.org/2026/$SQLITE.zip"
SQLITE_SHA256=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
SQLITE_FLAGS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_USE_ALLOCA
  -DSQLITE_ENABLE_QPSG -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1"

mkdir -p build
LIB=build/libshim.so
CFLAGS="-O3 -march=native -fPIC -shared -w"
STAMP="$SQLITE $CFLAGS $(echo $SQLITE_FLAGS) $(sha256sum csrc/shim.c | cut -d' ' -f1)"
if [ ! -f "$LIB" ] || [ "$(cat build/shim.stamp 2>/dev/null)" != "$STAMP" ]; then
  if [ ! -f "build/$SQLITE.zip" ] || [ "$(sha256sum "build/$SQLITE.zip" | cut -d' ' -f1)" != "$SQLITE_SHA256" ]; then
    curl -fsSL "$SQLITE_URL" -o "build/$SQLITE.zip.part"
    [ "$(sha256sum "build/$SQLITE.zip.part" | cut -d' ' -f1)" = "$SQLITE_SHA256" ] || { echo "sqlite checksum mismatch"; exit 1; }
    mv "build/$SQLITE.zip.part" "build/$SQLITE.zip"
  fi
  rm -rf "build/$SQLITE"
  (cd build && "${JDK_BIN}jar" xf "$SQLITE.zip")
  cc $CFLAGS $SQLITE_FLAGS -I"build/$SQLITE" -o "$LIB" csrc/shim.c -lm
  echo "$STAMP" > build/shim.stamp
fi

rm -rf build/classes
"${JDK_BIN}javac" --release 25 -Xlint:all,-restricted,-serial -Werror -d build/classes $(find src -name '*.java')
printf 'Enable-Native-Access: ALL-UNNAMED\n' > build/manifest.txt
"${JDK_BIN}jar" --create --file build/server.jar --main-class challenge.Main --manifest build/manifest.txt -C build/classes .
