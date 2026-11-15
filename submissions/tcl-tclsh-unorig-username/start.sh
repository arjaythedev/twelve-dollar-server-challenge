#!/usr/bin/env bash
# Runs the server (one tclsh) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
exec tclsh8.6 "$(dirname "$0")/server.tcl"
