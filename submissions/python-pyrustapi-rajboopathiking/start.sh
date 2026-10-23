#!/usr/bin/env bash
# Runs the server in the foreground, configured only by the env vars in SPEC.md.
set -euo pipefail
cd "$(dirname "$0")"

if [ -f ".venv/bin/python" ]; then
    exec .venv/bin/python server.py
else
    exec python3 server.py
fi
