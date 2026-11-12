#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ "${TINYJS_TRANSPORT:-bun}" = bun ]; then
  exec bun transport.ts
fi
if [ "$TINYJS_TRANSPORT" != node ]; then
  echo "TINYJS_TRANSPORT must be bun or node" >&2
  exit 1
fi
exec bun node_modules/tinyjs-fullstack/bin/tinyjs.js serve --host "${HOST:-0.0.0.0}" --port "${PORT:-80}"
