#!/usr/bin/env bash
# Runs the server in the foreground; configured by SQLITE_PATH, JWT_SECRET, HOST and PORT.
export GOMAXPROCS=1 GODEBUG=asyncpreemptoff=1
exec "$(dirname "$0")/bin/server"
