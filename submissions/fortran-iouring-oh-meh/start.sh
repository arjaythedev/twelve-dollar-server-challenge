#!/usr/bin/env bash
# Runs the server in the foreground, configured by SQLITE_PATH, JWT_SECRET, HOST, PORT
# (plus FORTRAN_TABLE_OVERRIDE / FORTRAN_NBUFS_OVERRIDE / FORTRAN_WAL_HOLD_OVERRIDE,
# test-only knobs that are never set on the box).
exec "$(dirname "$0")/bin/server"
