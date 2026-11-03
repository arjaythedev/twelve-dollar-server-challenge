#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: C toolchain + curl for the
# pinned SQLite download. Everything else builds from source in build.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates
gcc --version | head -1
