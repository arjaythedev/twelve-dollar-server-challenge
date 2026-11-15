#!/usr/bin/env bash
# Runs the server (one swipl process) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
exec swipl -O "$(dirname "$0")/server.pl"
