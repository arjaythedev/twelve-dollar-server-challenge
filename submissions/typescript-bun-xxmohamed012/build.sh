#!/usr/bin/env bash
# Builds the app as a normal user: one self-contained executable, bin/server. No dependencies to download.
set -euo pipefail
cd "$(dirname "$0")"
bun build --compile --outfile bin/server server.ts
