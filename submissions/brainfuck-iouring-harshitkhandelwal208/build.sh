#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/bin" "$DIR/build"

# 1. Compile Brainfuck server source (src/bf/server.bf) into optimized x86_64 assembly
"$DIR/bin/bf_compiler" "$DIR/src/bf/server.bf" "$DIR/build/server_bf.s" "$DIR/build/runtime.c"
as -o "$DIR/build/server_bf.o" "$DIR/build/server_bf.s"

# 2. Build standalone high-performance server binary
gcc -O3 -flto -march=native -o "$DIR/bin/server" "$DIR/build/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto
