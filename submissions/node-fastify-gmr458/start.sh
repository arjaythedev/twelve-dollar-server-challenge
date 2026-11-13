#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export NODE_ENV="${NODE_ENV:-production}"
exec node dist/server.js
