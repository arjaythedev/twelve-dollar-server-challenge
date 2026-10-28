#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
[ -e /usr/lib/x86_64-linux-gnu/libsqlite3.so ] && L=-lsqlite3 || L=/usr/lib/x86_64-linux-gnu/libsqlite3.so.0
gcc -no-pie -O3 -march=native -Wall server.s -o bin/server "$L" -lcrypto
chmod +x bin/server
