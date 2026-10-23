#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source ./sqlite-build.sh
cc -O2 -I"$sqlite" -c tests/commit_fault.c -o .deps/commit_fault.o
gfortran -O3 -flto -ffree-line-length-none -J.deps -I.deps bindings.f90 unicode.f90 server.f90 \
  .deps/bridge.o .deps/commit_fault.o .deps/libsqlite3.a .deps/libmicrohttpd-1.0.2/src/microhttpd/.libs/libmicrohttpd.a \
  -Wl,--wrap=sqlite3_open_v2 -lm -ldl -lpthread -o bin/commit-test
python3 tests/commit_check.py
