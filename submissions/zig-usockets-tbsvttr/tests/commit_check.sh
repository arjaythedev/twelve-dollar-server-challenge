#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sqlite=.deps/sqlite-amalgamation-3530400
# Only this test executable redirects SQLite's open function to the commit hook.
cc -O2 -I"$sqlite" -D__real_sqlite3_open_v2=sqlite3_open_v2 \
  -c tests/commit_fault.c -o .deps/commit_fault.o
zig translate-c -lc -I"$sqlite" -I.deps/pico -I.deps/usockets/src \
  -Dsqlite3_open_v2=__wrap_sqlite3_open_v2 src/c.h > .deps/c-test.zig
zig build-exe -O ReleaseSafe -lc -lm \
  .deps/sqlite3.o .deps/pico.o .deps/usockets/uSockets.a .deps/commit_fault.o \
  --dep c -Mroot=src/main.zig -Mc=.deps/c-test.zig -femit-bin=bin/commit-test
python3 tests/commit_check.py
