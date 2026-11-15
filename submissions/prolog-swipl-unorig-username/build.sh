#!/usr/bin/env bash
# Compiles the SQLite binding (sqlite4pl.c, about 70 lines of C) into a foreign library that swipl loads.
# server.pl itself is loaded from source at start.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p lib
swipl-ld -shared -O2 -o lib/sqlite4pl sqlite4pl.c -lsqlite3
