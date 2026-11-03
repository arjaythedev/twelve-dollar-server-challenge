#!/usr/bin/env bash
# Runs in the foreground. Configuration comes only from SPEC.md environment variables.
set -euo pipefail
cd "$(dirname "$0")"
exec ./bin/server
