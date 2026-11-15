#!/usr/bin/env bash
# Runs the server in the foreground: an IniTeX job that never reaches \end. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# TEXMFCNF points kpathsea at the three-line texmf.cnf here (the binaries package ships none); the
# transcript (.log) goes to /tmp. Lua's print and errors go to stdout/stderr. --shell-escape is what lets
# LuaTeX's Lua load a C module (package.loadlib) and turns on its luasocket; nothing here runs a shell.
trap '' PIPE
cd "$(dirname "$0")"
exec env TEXMFCNF="$PWD" luatex --ini --shell-escape --interaction=batchmode --jobname=tex-feed-server --output-directory="${TMPDIR:-/tmp}" server.tex
