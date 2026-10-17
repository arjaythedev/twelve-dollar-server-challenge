#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
BUN="$(command -v bun || echo /usr/local/bin/bun)"
"$BUN" install --frozen-lockfile --production
"$BUN" build --compile --production --target bun --outfile bin/server src/server/main.ts
if [ "$(uname -s)" = Darwin ]; then codesign --sign - --force bin/server; fi
