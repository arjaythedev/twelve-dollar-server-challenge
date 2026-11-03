#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
cc -O3 -std=gnu11 -flto -march=native -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-unused-function \
   src/server.c -o bin/server $(pkg-config --cflags --libs sqlite3 json-c openssl)
