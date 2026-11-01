#!/usr/bin/env bash
# Builds bin/server as a normal user: fetches the pinned SQLite amalgamation
# and liburing (checksums verified), then an -O3 -march=native LTO build with
# PGO: the instrumented server is trained on a synthetic database generated
# here (never the seed, rule 7) and rebuilt with the profile; if training
# fails the plain LTO build is shipped instead (see Makefile `pgo`).
set -euo pipefail
cd "$(dirname "$0")"
./fetch-deps.sh
make pgo
ls -la bin/server
