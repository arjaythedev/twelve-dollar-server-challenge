#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
exec bun node_modules/tinyjs-fullstack/bin/tinyjs.js serve --host "${HOST:-0.0.0.0}" --port "${PORT:-80}"
