#!/usr/bin/env bash
# Compiles the two small foreign libraries swipl loads: the SQLite binding (sqlite4pl.c) and the epoll
# and send() wrapper (epoll4pl.c), each about 60 to 70 lines of C. server.pl itself is loaded from source at start.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p lib
swipl-ld -shared -O2 -o lib/sqlite4pl sqlite4pl.c -lsqlite3
swipl-ld -shared -O2 -o lib/epoll4pl epoll4pl.c
