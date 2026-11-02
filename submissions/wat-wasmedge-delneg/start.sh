#!/usr/bin/env bash
# Runs the server in the foreground. Env (SPEC.md): SQLITE_PATH, JWT_SECRET, HOST, PORT.
cd "$(dirname "$0")"
exec ./build/host ./build/app.so
