#!/usr/bin/env bash
set -euo pipefail
ulimit -Sn "$(ulimit -Hn)"
cd "$(dirname "$0")"
exec .venv/bin/python server.py
