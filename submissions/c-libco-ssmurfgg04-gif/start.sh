#!/usr/bin/env bash
# Runs the server in the foreground, configured only by SPEC.md env vars.
set -euo pipefail
cd "$(dirname "$0")"
exec bin/server
