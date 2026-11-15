#!/usr/bin/env bash
# Compiles the SQLite binding (lsqlite3) into a Lua C module that LuaTeX loads in-process.
# No -ltexlua53: the module takes the Lua API from the luatex process that loads it.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
gcc -O2 -fPIC -shared -I/usr/include/texlua53 -o bin/lsqlite3.so /usr/local/src/lsqlite3_v096/lsqlite3.c -lsqlite3
texluac -p server.lua
