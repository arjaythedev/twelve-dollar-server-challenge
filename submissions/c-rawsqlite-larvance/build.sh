#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
gcc -O3 -march=native -fno-plt -flto=auto -pthread -o bin/server -Isrc src/server.c -lm
