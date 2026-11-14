#!/usr/bin/env bash
# Runs the server (a Neovim process) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
export LC_ALL=C
exec nvim --headless --clean -l "$(dirname "$0")/server.lua"
