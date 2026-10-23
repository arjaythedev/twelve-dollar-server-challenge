#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ "$(bun --version)" == 1.4.2 ]] || { echo 'Bun 1.4.2 required; run install.sh' >&2; exit 1; }
bun build --compile server.ts --outfile bin/server
