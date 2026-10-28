#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin obj
exec gnatmake-13 -j1 -O2 -gnat2022 -gnatwa -gnatVa -fstack-protector-strong \
  -D obj -Isrc src/server.adb -o bin/server -largs -lsqlite3 -lcrypto
