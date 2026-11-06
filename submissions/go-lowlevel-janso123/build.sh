#!/usr/bin/env bash
# Pinned local toolchains; SQLite PGO and ThinLTO default on, with opt-out flags.
set -euo pipefail
cd "$(dirname "$0")"
exec python3 ./build.py "$@"
