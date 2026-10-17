#!/usr/bin/env bash
# Nothing to build: no dependencies, only Node's built-in http, sqlite and crypto modules.
set -euo pipefail
cd "$(dirname "$0")"
node --check index.js
