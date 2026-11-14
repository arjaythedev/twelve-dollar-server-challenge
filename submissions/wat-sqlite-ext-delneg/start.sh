#!/usr/bin/env bash
# Runs the server in the foreground: the stock sqlite3 shell on $SQLITE_PATH, reading start.sql.
# Env (SPEC.md): SQLITE_PATH, JWT_SECRET, HOST, PORT; the WAT reads the last three itself.
cd "$(dirname "$0")"
exec ./build/sqlite3 "$SQLITE_PATH" < start.sql
