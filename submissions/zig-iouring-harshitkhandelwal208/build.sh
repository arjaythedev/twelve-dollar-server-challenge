#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p bin deps

if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL --retry 3 -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

zig build -Doptimize=ReleaseFast -Dcpu=native
