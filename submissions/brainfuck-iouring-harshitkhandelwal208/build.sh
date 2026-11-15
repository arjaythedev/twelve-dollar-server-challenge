#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p bin build

# 1. Compile native Brainfuck optimizing compiler
clang -O3 -o bin/bf_compiler src/compiler/bf_compiler.c

# 2. Compile native Brainfuck server source (src/bf/server.bf) into optimized x86_64 assembly
bin/bf_compiler src/bf/server.bf build/server_bf.s
as -o build/server_bf.o build/server_bf.s

# 3. Build server binary with native Brainfuck engine and system SQLite
zig build -Doptimize=ReleaseFast -Dcpu=native
