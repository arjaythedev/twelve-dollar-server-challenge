#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
export PYPY_GC_NURSERY=8M

exec  ./.venv/bin/pypy3 main.py
