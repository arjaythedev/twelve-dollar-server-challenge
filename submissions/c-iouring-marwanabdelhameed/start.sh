#!/usr/bin/env bash
# Runs the server in the foreground. Configuration is only the environment variables of SPEC.md.
exec "$(dirname "$0")/bin/server"
