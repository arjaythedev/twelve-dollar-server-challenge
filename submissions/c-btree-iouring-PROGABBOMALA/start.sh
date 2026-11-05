#!/usr/bin/env bash
# Foreground. Config comes only from SQLITE_PATH, JWT_SECRET, HOST and PORT.
exec "$(dirname "$0")/bin/server"
