#!/usr/bin/env bash
# Runs the server in the foreground. The server is src/server.sql; bin/server only runs it.
exec "$(dirname "$0")/bin/server"
