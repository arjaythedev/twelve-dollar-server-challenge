#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
uv sync --locked --no-dev
uv run --locked --no-sync --no-dev python native/build.py
