#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
bun install --frozen-lockfile
bun node_modules/tinyjs-fullstack/bin/tinyjs.js build
