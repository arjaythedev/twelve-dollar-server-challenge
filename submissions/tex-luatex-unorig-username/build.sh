#!/usr/bin/env bash
# Compiles two Lua C modules that LuaTeX loads in-process: the SQLite binding (lsqlite3) and evio.c (epoll).
# No -ltexlua53: each module takes the Lua API from the luatex process that loads it.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
gcc -O2 -fPIC -shared -I/usr/include/texlua53 -o bin/lsqlite3.so /usr/local/src/lsqlite3_v096/lsqlite3.c -lsqlite3
gcc -O2 -fPIC -shared -Wall -I/usr/include/texlua53 -o bin/evio.so evio.c
texluac -p server.lua
