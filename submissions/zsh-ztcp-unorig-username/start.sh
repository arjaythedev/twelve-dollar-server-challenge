#!/usr/bin/env bash
# Runs the server (one zsh process) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
cd "$(dirname "$0")"
export LC_ALL=C
exec ./zsh/bin/zsh -f server.zsh
