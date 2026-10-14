#!/usr/bin/env bash
# No dependencies to fetch: the server uses only Bun built-ins (Bun.serve, bun:sqlite, node:crypto).
set -euo pipefail
cd "$(dirname "$0")"
bun --version
