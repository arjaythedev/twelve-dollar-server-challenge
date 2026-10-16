#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
exec .venv/bin/uvicorn app:app --host "${HOST:-127.0.0.1}" --port "${PORT:-3000}" \
  --workers "${WORKERS:-1}" --no-access-log --log-level warning --timeout-keep-alive 75
