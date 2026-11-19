#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

# Environment variables for optimizations (from PR #41)
export GRANIAN_BACKPRESSURE=65535
export GRANIAN_RESPAWN_FAILED_WORKERS=false

# Python 3.15: enable JIT
export PYTHON_JIT=1
export PYTHONDONTWRITEBYTECODE=1
export PYTHONUNBUFFERED=1

# Launch Granian directly from the venv built by build.sh.
# uv run is not needed here and is dangerous: it may re-resolve Python to the
# managed 3.15.0a5 (alpha) build lacking PyType_FromSlots and break granian import.
exec .venv/bin/granian \
    --interface asgi \
    --loop uvloop \
    --workers 1 \
    --host "${HOST:-0.0.0.0}" \
    --port "${PORT:-80}" \
    --backlog 65535 \
    --http1-buffer-size 8192 \
    --log-level warning \
    app:app