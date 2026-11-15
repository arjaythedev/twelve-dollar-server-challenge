#!/usr/bin/env bash
# Runs the server in the foreground. Configured only by SPEC.md env vars.
set -euo pipefail
cd "$(dirname "$0")"
exec bun src/index.ts
