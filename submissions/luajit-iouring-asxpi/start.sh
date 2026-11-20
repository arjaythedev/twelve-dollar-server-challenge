#!/usr/bin/env bash
# Runs the server in the foreground, configured by SQLITE_PATH, JWT_SECRET, HOST and PORT.
cd "$(dirname "$0")"
LUAJIT="${LUAJIT:-/usr/local/bin/luajit}"
[ -x "$LUAJIT" ] || LUAJIT="$(command -v luajit)"
exec "$LUAJIT" src/main.lua
