#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
npm ci --no-audit --no-fund
./node_modules/.bin/tsc -p tsconfig.json
