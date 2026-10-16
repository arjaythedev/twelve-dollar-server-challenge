#!/usr/bin/env bash
# Nothing to compile: the server is nginx.conf plus Lua that OpenResty's LuaJIT loads at startup.
# This byte-compiles every Lua file as a syntax check and checks that the SQLite library loads.
set -euo pipefail
cd "$(dirname "$0")"
for f in lua/*.lua; do /usr/local/openresty/luajit/bin/luajit -b "$f" /dev/null; done
/usr/local/openresty/luajit/bin/luajit -e 'require("ffi").load("libsqlite3.so.0")'
echo "build ok"
