#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (optional knobs in README).
exec "$(cd "$(dirname "$0")" && pwd)/build/server.kexe"
