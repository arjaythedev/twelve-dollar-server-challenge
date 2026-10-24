#!/usr/bin/env bash
# Builds bin/server as a normal user: fetches the pinned SQLite amalgamation
# and liburing (checksums verified), then a PGO+LTO release build trained on
# a synthetic database. Falls back to a plain -O3 build if PGO fails.
set -euo pipefail
cd "$(dirname "$0")"
./fetch-deps.sh
make release
ls -la bin/server
