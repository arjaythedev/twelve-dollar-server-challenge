#!/usr/bin/env bash
# Foreground server; configured by SQLITE_PATH, JWT_SECRET, HOST and PORT (SPEC.md).
exec "$(dirname "$0")/bin/server"
