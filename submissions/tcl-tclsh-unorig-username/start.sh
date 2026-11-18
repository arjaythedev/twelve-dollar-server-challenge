#!/usr/bin/env bash
# Runs the server (one tclsh, built by build.sh) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# One descriptor per keep-alive client: raise this process's soft limit (the hard limit is the box's).
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)"
exec "$(dirname "$0")/tcl/bin/tclsh8.7" "$(dirname "$0")/server.tcl" "$(ulimit -n)"
