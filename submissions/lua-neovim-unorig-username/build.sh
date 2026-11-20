#!/usr/bin/env bash
# Nothing to compile: Neovim's LuaJIT runs server.lua as is. This only checks that it parses.
set -euo pipefail
cd "$(dirname "$0")"
nvim --headless --clean -l /dev/stdin <<<'assert(loadfile("server.lua"))'
