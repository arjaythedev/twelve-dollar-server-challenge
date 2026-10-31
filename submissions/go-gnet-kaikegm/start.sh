#!/usr/bin/env bash
# Runs the server in the foreground, configured only by the env vars in SPEC.md.
set -euo pipefail
exec "$(dirname "$0")/bin/server"
