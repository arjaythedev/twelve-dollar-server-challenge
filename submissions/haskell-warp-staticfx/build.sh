#!/usr/bin/env bash
# Builds the app as a normal user: fetches the pinned Hackage index, builds, copies the binary to bin/.
set -euo pipefail
cd "$(dirname "$0")"
cabal update 'hackage.haskell.org,2026-10-15T11:34:24Z'
cabal build -j exe:server
mkdir -p bin && cp "$(cabal list-bin exe:server)" bin/server
