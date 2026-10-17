#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin

SQLITE_LIB="-lsqlite3"
if ! ld -lsqlite3 -shared -o /dev/null 2>/dev/null; then
    SQLITE_LIB="/usr/lib/x86_64-linux-gnu/libsqlite3.so.0"
fi

gcc -no-pie -O3 -Wall server.s -o bin/server "$SQLITE_LIB" -lcrypto
chmod +x bin/server