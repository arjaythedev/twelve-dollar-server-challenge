#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
flags=(-O2 -flto -std=gnu++20 -pthread -Wno-c99-designator)
if [[ $(uname -s) == Linux ]]; then flags+=(-fuse-ld=lld); fi
includes=(-I.deps -I.deps/sqlite-amalgamation-3530400 -I.deps/uwebsockets/src -I.deps/usockets/src)
"${CXX:-clang++}" -O2 -I.deps/sqlite-amalgamation-3530400 -c tests/commit_fault.cpp -o .deps/commit_fault.o
"${CXX:-clang++}" "${flags[@]}" "${includes[@]}" -Dsqlite3_exec=test_exec \
  -DLIBUS_NO_SSL -DUWS_NO_ZLIB -DUWS_HTTPRESPONSE_NO_WRITEMARK -DNDEBUG \
  server.cpp .deps/commit_fault.o .deps/sqlite3.o .deps/usockets/uSockets.a -lm -o bin/commit-test
python3 tests/commit_check.py
