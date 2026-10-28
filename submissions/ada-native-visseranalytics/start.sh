#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${SQLITE_PATH:?Set SQLITE_PATH to a writable on-disk seed copy}"
: "${JWT_SECRET:?Set JWT_SECRET}"
exec ./bin/server
