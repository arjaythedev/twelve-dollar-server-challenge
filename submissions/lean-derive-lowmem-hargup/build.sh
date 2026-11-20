#!/usr/bin/env bash
# Builds the server as a normal user:
#   1. Lean checks the specification (spec/) — its theorems are verified by the kernel — and
#      generates the application layer and its job/framing/SQE headers. The build stops if a checked-in copy
#      differs from what the specification generates.
#   2. The C compiler builds one translation unit: SQLite (pinned, checksummed), the runtime,
#      and the generated code.
set -euo pipefail
cd "$(dirname "$0")"
LEAN=/opt/lean-4.35.0/bin
SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p vendor bin build
if [ ! -f "vendor/$SQLITE/sqlite3.c" ]; then
  curl -fsSL --retry 3 -o "vendor/$SQLITE.zip" "https://sqlite.org/2026/$SQLITE.zip"
  echo "$SQLITE_SHA256  vendor/$SQLITE.zip" | sha256sum -c -
  unzip -q -o "vendor/$SQLITE.zip" -d vendor
fi

echo "== lean: check the specification and derive the application layer"
(cd spec && "$LEAN/lake" build && "$LEAN/lake" exe derive ../build)
for generated in app_gen.c job_gen.h frame_gen.h sqe_gen.h; do
if ! cmp -s "build/$generated" "gen/$generated"; then
  echo "gen/$generated is not what spec/ generates; run: (cd spec && lake exe derive ../gen)"
  diff -u "gen/$generated" "build/$generated" | head -40
  exit 1
fi
done

echo "== cc"
cc -O2 -march=native -fno-plt -pthread -o bin/server \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
  -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
  -Iruntime -Ibuild -I"vendor/$SQLITE" runtime/server.c -lm
echo "built bin/server"
