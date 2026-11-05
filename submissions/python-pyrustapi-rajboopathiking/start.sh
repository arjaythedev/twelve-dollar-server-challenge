#!/usr/bin/env bash
# Runs the server in the foreground, configured only by the env vars in SPEC.md.
set -euo pipefail
cd "$(dirname "$0")"

# Turn off per-request stdout access logging to prevent stdout lock contention under high load
export RUSTAPI_LOG="0"

# The challenge grants a 65,000 fd limit; keep headroom for 50k keep-alive sockets.
ulimit -n 65000 2>/dev/null || true

if [ -f ".venv/bin/python" ]; then
    exec .venv/bin/python server.py
else
    exec python3 server.py
fi
