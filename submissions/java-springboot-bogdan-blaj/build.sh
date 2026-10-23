#!/usr/bin/env bash
# Builds native/libfeedsqlite.so from the pinned SQLite amalgamation and target/feed.jar.
# Runs as a normal user. test/run.sh calls this on every run, so both steps are skipped when up to date.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_ID=3530400  # 3.53.4
SQLITE_URL=https://www.sqlite.org/2026/sqlite-autoconf-$SQLITE_ID.tar.gz
SQLITE_SHA256=0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c
SQLITE_DEFINES=(
  -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1
)

mkdir -p vendor native

# 1. SQLite source: download (once), verify, extract sqlite3.c / sqlite3.h.
if [ "$(cat vendor/.sqlite-id 2>/dev/null || true)" != "$SQLITE_ID" ] || [ ! -f vendor/sqlite3.c ] || [ ! -f vendor/sqlite3.h ]; then
  tarball="vendor/sqlite-autoconf-$SQLITE_ID.tar.gz"
  if [ ! -f "$tarball" ] || ! echo "$SQLITE_SHA256  $tarball" | sha256sum -c --quiet - >/dev/null 2>&1; then
    echo "build: downloading $SQLITE_URL"
    curl -fsSL --retry 3 -o "$tarball.part" "$SQLITE_URL"
    mv "$tarball.part" "$tarball"
  fi
  echo "$SQLITE_SHA256  $tarball" | sha256sum -c --quiet -
  tar -xzf "$tarball" -C vendor --strip-components=1 \
    "sqlite-autoconf-$SQLITE_ID/sqlite3.c" "sqlite-autoconf-$SQLITE_ID/sqlite3.h"
  echo "$SQLITE_ID" > vendor/.sqlite-id
fi

# 2. Native library. Rebuilt when the source, the compiler or the flags change.
build_id="$SQLITE_ID $(gcc -dumpfullversion) ${SQLITE_DEFINES[*]}"
if [ ! -f native/libfeedsqlite.so ] || [ vendor/sqlite3.c -nt native/libfeedsqlite.so ] \
   || [ "$(cat native/.build-id 2>/dev/null || true)" != "$build_id" ]; then
  echo "build: compiling native/libfeedsqlite.so"
  gcc -O2 -march=native -fPIC -shared -o native/libfeedsqlite.so.tmp vendor/sqlite3.c -lm "${SQLITE_DEFINES[@]}"
  mv native/libfeedsqlite.so.tmp native/libfeedsqlite.so
  echo "$build_id" > native/.build-id
fi

# 3. Application jar. `find -newer` also sees directories, so deleting a source file triggers a rebuild too.
if [ ! -f target/feed.jar ] || [ -n "$(find pom.xml src -newer target/feed.jar -print -quit)" ]; then
  echo "build: mvn package"
  JAVA_HOME=/opt/jdk-25 /opt/maven/bin/mvn -B -q -DskipTests package
fi
