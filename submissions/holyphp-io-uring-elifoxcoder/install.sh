#!/usr/bin/env bash
# Run once as root on a clean Ubuntu 24.04.
#   build-essential  — gcc, linker, ar
#   libsqlite3-dev   — SQLite headers + library (fallback if the pinned build fails)
#   curl, unzip      — fetch and unpack the pinned SQLite amalgamation in build.sh
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends \
  build-essential ca-certificates curl unzip \
  libsqlite3-dev
