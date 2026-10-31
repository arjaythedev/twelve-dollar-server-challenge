#!/usr/bin/env bash
# Runs the server in the foreground, configured by SQLITE_PATH, JWT_SECRET, HOST, PORT
# (plus MOJO_TABLE_OVERRIDE, a test-only knob that is never set on the box).
exec "$(dirname "$0")/bin/server"
