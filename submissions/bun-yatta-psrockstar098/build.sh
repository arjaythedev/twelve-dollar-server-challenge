#!/usr/bin/env bash
# Runs as a normal user. Installs the pinned dependencies (yatta.js 1.0.1
# from npm, per bun.lock) and compiles the server to a standalone binary so
# start.sh needs nothing but the binary itself.
set -euo pipefail
cd "$(dirname "$0")"
[[ "$(bun --version)" == 1.4.4 ]] || { echo 'Bun 1.4.4 required; run install.sh' >&2; exit 1; }
bun install
bun build --compile src/server.ts --outfile bin/server
