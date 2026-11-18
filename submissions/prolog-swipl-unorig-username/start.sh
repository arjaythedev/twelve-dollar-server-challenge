#!/usr/bin/env bash
# Runs the server (one swipl process) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# One descriptor per keep-alive user, so lift the soft limit (1024 by default) to the hard one.
ulimit -n "$(ulimit -Hn)"
exec swipl -O "$(dirname "$0")/server.pl"
