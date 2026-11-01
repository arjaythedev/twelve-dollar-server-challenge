#!/usr/bin/env bash
# Runs the server in the foreground. Configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT.
cd "$(dirname "$0")"
exec bin/server
