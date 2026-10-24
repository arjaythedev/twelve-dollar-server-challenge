#!/usr/bin/env bash
# Builds bin/server as a normal user: fetches the pinned SQLite amalgamation
# and liburing (checksums verified), then an -O3 -march=native LTO build.
# (PGO is available as `make pgo`; it measured no gain, see docs/RESULTS.md.)
set -euo pipefail
cd "$(dirname "$0")"
./fetch-deps.sh
make release
ls -la bin/server
