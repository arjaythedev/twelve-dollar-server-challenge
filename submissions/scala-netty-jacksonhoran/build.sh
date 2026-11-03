#!/usr/bin/env bash
# Builds as a normal user: SQLite 3.53.4 from the official amalgamation (checksum-verified) into
# native/libsqlite3.so, then the Scala app with sbt. start.sh runs the classpath written here.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME=/opt/jdk-25 PATH="/opt/jdk-25/bin:$PATH"

mkdir -p native
if [ ! -f native/libsqlite3.so ]; then
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/sqlite.zip" https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  echo "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d  $tmp/sqlite.zip" | sha256sum -c -
  unzip -q -d "$tmp" "$tmp/sqlite.zip"
  # The server uses one connection from one thread, so SQLite's mutexes are compiled out.
  # The rest are the "recommended compile-time options" from sqlite.org/compile.html.
  gcc -O2 -fPIC -shared -o native/libsqlite3.so "$tmp/sqlite-amalgamation-3530400/sqlite3.c" \
    -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_USE_ALLOCA -DSQLITE_OMIT_AUTOINIT -DSQLITE_DEFAULT_FOREIGN_KEYS=0 -lm
fi

/opt/sbt/bin/sbt -batch -Dsbt.server.forcestart=false compile writeClasspath
